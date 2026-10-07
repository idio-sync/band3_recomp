#include "midi_port.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>
#include <rex/logging.h>
#include "ThirdParty/rtmidi/RtMidi.h"
#include "midi_port_select.h"

namespace band3::input {

namespace {

// how often to look for the port, and notice it going away
constexpr std::chrono::milliseconds kScanInterval{2000};

// the ports band3's MidiPorts hold, for PickPort's `taken`: one entry for each
// open port (or one being opened)
std::mutex& TakenMutex() {
    static std::mutex mutex;
    return mutex;
}
std::vector<std::string>& Taken() {
    static std::vector<std::string> taken;
    return taken;
}

void Release(const std::string& port) {
    std::lock_guard<std::mutex> lock(TakenMutex());
    auto& taken = Taken();
    if (const auto it = std::ranges::find(taken, port); it != taken.end()) taken.erase(it);
}

// `user` is the MidiPort's log name
void LogRtMidiError(RtMidiError::Type type, const std::string& text, void* user) {
    const std::string& log_name = *static_cast<const std::string*>(user);
    if (type == RtMidiError::WARNING || type == RtMidiError::DEBUG_WARNING) {
        REXLOG_DEBUG("{}: {}", log_name, text);
    } else {
        REXLOG_WARN("{}: {}", log_name, text);
    }
}

}

MidiPort::MidiPort(std::string log_name, std::string client)
    : log_name_(std::move(log_name)), client_(std::move(client)) {}

MidiPort::~MidiPort() { Stop(); }

bool MidiPort::Start(std::string wanted, OnMessage on_message, OnPort on_port) {
    wanted_ = std::move(wanted);
    on_message_ = std::move(on_message);
    on_port_ = std::move(on_port);

    try {
        input_ = std::make_unique<RtMidiIn>();
        probe_ = std::make_unique<RtMidiIn>();
    } catch (const RtMidiError& e) {
        REXLOG_WARN("{}: MIDI input isn't available ({})", log_name_, e.getMessage());
        input_.reset();
        probe_.reset();
        return false;
    }
    input_->setErrorCallback(LogRtMidiError, &log_name_);
    probe_->setErrorCallback(LogRtMidiError, &log_name_);
    // drop sysex, clock and active sensing, which some kits send constantly
    input_->ignoreTypes(true, true, true);
    input_->setCallback(&MidiPort::Receive, this);

    REXLOG_INFO("{}: looking for {}", log_name_,
                wanted_.empty() ? std::string("a MIDI input") : "\"" + wanted_ + "\"");
    watcher_ = std::thread([this] { WatchLoop(); });
    return true;
}

void MidiPort::Stop() {
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        if (stop_) return;
        stop_ = true;
    }
    stop_cv_.notify_all();
    if (watcher_.joinable()) watcher_.join();
    if (input_) {
        input_->cancelCallback();
        if (input_->isPortOpen()) input_->closePort();
    }
    if (open_) {
        Release(*open_);
        open_.reset();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    port_.clear();
}

std::string MidiPort::port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

std::vector<std::string> MidiPort::ports() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ports_;
}

// RtMidi's thread, for every message from the open port
void MidiPort::Receive(double, std::vector<unsigned char>* message, void* user) {
    auto* self = static_cast<MidiPort*>(user);
    if (!message || !self->on_message_) return;
    self->on_message_(std::span<const uint8_t>(message->data(), message->size()));
}

void MidiPort::WatchLoop() {
    std::unique_lock<std::mutex> lock(stop_mutex_);
    while (!stop_) {
        lock.unlock();
        Scan();
        lock.lock();
        stop_cv_.wait_for(lock, kScanInterval, [this] { return stop_.load(); });
    }
}

// opens the wanted port when it appears, closes it when it goes. on_port_ is
// called holding none of MidiPort's locks: the driver takes its own in it, which
// RtMidi's thread takes too.
void MidiPort::Scan() {
    std::vector<std::string> ports;
    const unsigned count = probe_->getPortCount();
    for (unsigned i = 0; i < count; i++) ports.push_back(probe_->getPortName(i));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ports_ = ports;
    }

    if (open_) {
        if (std::ranges::find(ports, *open_) != ports.end()) return;
        REXLOG_INFO("{}: {} disconnected", log_name_, *open_);
        input_->closePort();
        Release(*open_);
        open_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            port_.clear();
        }
        if (on_port_) on_port_(std::string());
        return;
    }

    // a port that won't open is passed over for the next one that would do
    size_t from = 0;
    while (from < ports.size()) {
        const std::vector<std::string> rest(ports.begin() + static_cast<std::ptrdiff_t>(from),
                                            ports.end());
        size_t i;
        {
            std::lock_guard<std::mutex> lock(TakenMutex());
            const std::optional<size_t> pick = PickPort(rest, wanted_, Taken());
            if (!pick) return;
            i = from + *pick;
            // held from now, so another MidiPort scanning meanwhile passes it over
            Taken().push_back(ports[i]);
        }
        input_->openPort(static_cast<unsigned>(i), client_);
        if (!input_->isPortOpen()) {
            Release(ports[i]);
            from = i + 1;
            continue;
        }
        REXLOG_INFO("{}: playing {}", log_name_, ports[i]);
        open_ = ports[i];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            port_ = ports[i];
        }
        if (on_port_) on_port_(ports[i]);
        return;
    }
}

}

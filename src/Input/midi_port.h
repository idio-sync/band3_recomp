#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

class RtMidiIn;

// A MIDI input port that band3's MIDI drivers play from, read with RtMidi: a
// watcher thread lists the input ports every 2 s, opens the wanted one when it
// appears (midi_port_select.h picks it) and closes it when it goes. Sysex, clock
// and active sensing are dropped; every other message goes to a callback on
// RtMidi's thread.

namespace band3::input {

class MidiPort {
public:
    // RtMidi's thread, for every message from the open port
    using OnMessage = std::function<void(std::span<const uint8_t> message)>;
    // the watcher thread, after the port opens (its name) or closes (empty);
    // not called by Stop
    using OnPort = std::function<void(const std::string& port)>;

    // `log_name` starts its log lines ("MIDI drums"); `client` names band3's end
    // of the connection ("band3 drums")
    MidiPort(std::string log_name, std::string client);
    ~MidiPort();
    MidiPort(const MidiPort&) = delete;
    MidiPort& operator=(const MidiPort&) = delete;

    // Starts watching for the port `wanted` names (any port when empty; see
    // PickPort). Once. False when MIDI input isn't available, which it logs.
    bool Start(std::string wanted, OnMessage on_message, OnPort on_port);
    // Closes the port and stops the watcher; the callbacks aren't called after.
    void Stop();

    // the open port's name, empty while none is open
    std::string port() const;
    // every MIDI input port, as last listed
    std::vector<std::string> ports() const;

private:
    static void Receive(double, std::vector<unsigned char>* message, void* user);
    void WatchLoop();
    void Scan();

    // RtMidi's error callback is handed log_name_
    std::string log_name_;
    std::string client_;
    std::string wanted_;
    OnMessage on_message_;
    OnPort on_port_;

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    std::atomic<bool> stop_{false};
    std::thread watcher_;

    // the watcher thread opens and closes input_; probe_ only lists ports
    std::unique_ptr<RtMidiIn> input_;
    std::unique_ptr<RtMidiIn> probe_;
    // the port input_ has open: the watcher thread's, and Stop's once it's gone
    std::optional<std::string> open_;

    // for port() and ports(), from any thread
    mutable std::mutex mutex_;
    std::string port_;
    std::vector<std::string> ports_;
};

}

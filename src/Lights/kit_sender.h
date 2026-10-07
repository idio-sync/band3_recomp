#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "stagekit.h"

// The Stage Kits plugged into this PC, each brought to its lights with a
// KitSync. Without threads or devices: usb_kits.cpp finds the kits, hands
// each one's KitPort over, and calls Pump on its own thread.
namespace band3::lights {

// how a kit is sent a command
class KitPort {
public:
    virtual ~KitPort() = default;
    // false if the kit is gone
    virtual bool Send(Command command) = 0;
};

enum class KitKind { kHid, kXInput, kFake };

struct KitInfo {
    std::string key;   // what it's found by again: its HID path, its XInput slot
    std::string name;  // "Santroller Stage Kit (HID)"
    KitKind kind = KitKind::kHid;
    bool operator==(const KitInfo&) const = default;
};

class KitSender {
public:
    // a kit found: it takes the game's lights, after an all-off
    void Add(KitInfo info, std::unique_ptr<KitPort> port);
    void Remove(const std::string& key);
    bool Has(const std::string& key) const;
    std::vector<KitInfo> Kits() const;

    // the game's command, for every kit
    void Game(Command command);
    // a test command, for one kit (by key) or every kit
    void Test(Command command, const std::optional<std::string>& key);

    // Sends each kit its next command, if it has one. A kit whose send fails
    // is dropped (it was unplugged) and listed in TakeLost. True while a kit
    // still has commands to send.
    bool Pump();
    // the kits dropped since the last call
    std::vector<KitInfo> TakeLost();

private:
    struct Kit {
        KitInfo info;
        std::unique_ptr<KitPort> port;
        KitSync sync;
    };
    // the lights the game last set, for a kit found later
    StageKit game_;
    std::map<std::string, Kit> kits_;
    std::vector<KitInfo> lost_;
};

}  // namespace band3::lights

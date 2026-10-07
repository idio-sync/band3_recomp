#include "kit_sender.h"
#include <utility>

namespace band3::lights {

void KitSender::Add(KitInfo info, std::unique_ptr<KitPort> port) {
    Kit kit{std::move(info), std::move(port), {}};
    kit.sync.wanted = game_;
    std::string key = kit.info.key;
    kits_.insert_or_assign(std::move(key), std::move(kit));
}

void KitSender::Remove(const std::string& key) { kits_.erase(key); }

bool KitSender::Has(const std::string& key) const { return kits_.contains(key); }

std::vector<KitInfo> KitSender::Kits() const {
    std::vector<KitInfo> kits;
    for (const auto& [key, kit] : kits_) kits.push_back(kit.info);
    return kits;
}

void KitSender::Game(Command command) {
    game_ = ApplyStageKit(game_, command.left, command.right);
    for (auto& [key, kit] : kits_) {
        kit.sync.wanted = ApplyStageKit(kit.sync.wanted, command.left, command.right);
    }
}

void KitSender::Test(Command command, const std::optional<std::string>& key) {
    for (auto& [kit_key, kit] : kits_) {
        if (key && *key != kit_key) continue;
        kit.sync.wanted = ApplyStageKit(kit.sync.wanted, command.left, command.right);
    }
}

bool KitSender::Pump() {
    bool more = false;
    for (auto it = kits_.begin(); it != kits_.end();) {
        Kit& kit = it->second;
        if (const auto command = kit.sync.Next()) {
            if (!kit.port->Send(*command)) {
                lost_.push_back(kit.info);
                it = kits_.erase(it);
                continue;
            }
            kit.sync.Sent(*command);
            more = more || kit.sync.Next().has_value();
        }
        ++it;
    }
    return more;
}

std::vector<KitInfo> KitSender::TakeLost() { return std::exchange(lost_, {}); }

}  // namespace band3::lights

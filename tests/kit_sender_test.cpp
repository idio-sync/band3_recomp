// Checks how the Stage Kits plugged into this PC are kept to their lights
// (src/Lights/kit_sender.cpp): the game's commands and the launcher's test
// ones, kits found and lost.

#include <doctest/doctest.h>
#include <memory>
#include <vector>
#include "src/Lights/kit_sender.h"

using namespace band3::lights;

namespace {

// a kit that keeps what it's sent, and can be unplugged
struct Recorder {
    std::vector<Command> sent;
    bool unplugged = false;
};

class FakePort final : public KitPort {
public:
    explicit FakePort(std::shared_ptr<Recorder> recorder) : recorder_(std::move(recorder)) {}
    bool Send(Command command) override {
        if (recorder_->unplugged) return false;
        recorder_->sent.push_back(command);
        return true;
    }

private:
    std::shared_ptr<Recorder> recorder_;
};

std::shared_ptr<Recorder> AddKit(KitSender& sender, const std::string& key) {
    auto recorder = std::make_shared<Recorder>();
    sender.Add({key, "kit " + key, KitKind::kFake}, std::make_unique<FakePort>(recorder));
    return recorder;
}

void PumpAll(KitSender& sender) {
    for (int i = 0; i < 64 && sender.Pump(); i++) {}
    REQUIRE_FALSE(sender.Pump());
}

}

TEST_CASE("a kit found is sent all-off, then the game's lights") {
    KitSender sender;
    sender.Game({0x0F, kRed});
    sender.Game({0, kFogOn});
    auto kit = AddKit(sender, "a");
    PumpAll(sender);
    CHECK(kit->sent == std::vector<Command>{kAllOffCommand, {0, kFogOn}, {0x0F, kRed}});
}

TEST_CASE("every kit is sent the game's commands") {
    KitSender sender;
    auto a = AddKit(sender, "a");
    auto b = AddKit(sender, "b");
    PumpAll(sender);
    a->sent.clear();
    b->sent.clear();

    sender.Game({0x55, kBlue});
    PumpAll(sender);
    CHECK(a->sent == std::vector<Command>{{0x55, kBlue}});
    CHECK(b->sent == std::vector<Command>{{0x55, kBlue}});
}

TEST_CASE("Pump sends each kit one command at a time") {
    KitSender sender;
    auto kit = AddKit(sender, "a");
    sender.Game({0x01, kRed});
    sender.Game({0x02, kGreen});
    CHECK(sender.Pump());
    CHECK(kit->sent.size() == 1);
    CHECK(sender.Pump());
    CHECK(kit->sent.size() == 2);
    // the last one sent, nothing is left
    CHECK_FALSE(sender.Pump());
    CHECK(kit->sent.size() == 3);
}

TEST_CASE("a test command goes to the kit it names") {
    KitSender sender;
    auto a = AddKit(sender, "a");
    auto b = AddKit(sender, "b");
    PumpAll(sender);
    a->sent.clear();
    b->sent.clear();

    sender.Test({kPatternAll, kYellow}, std::string("b"));
    PumpAll(sender);
    CHECK(a->sent.empty());
    CHECK(b->sent == std::vector<Command>{{kPatternAll, kYellow}});
}

TEST_CASE("a test command without a kit goes to every kit") {
    KitSender sender;
    auto a = AddKit(sender, "a");
    auto b = AddKit(sender, "b");
    PumpAll(sender);
    a->sent.clear();
    b->sent.clear();

    sender.Test({0, kStrobeSlow}, std::nullopt);
    PumpAll(sender);
    CHECK(a->sent == std::vector<Command>{{0, kStrobeSlow}});
    CHECK(b->sent == std::vector<Command>{{0, kStrobeSlow}});
}

TEST_CASE("a test command isn't the game's: a kit found later doesn't take it") {
    KitSender sender;
    auto a = AddKit(sender, "a");
    sender.Test({kPatternAll, kRed}, std::nullopt);
    PumpAll(sender);
    auto b = AddKit(sender, "b");
    PumpAll(sender);
    CHECK(b->sent == std::vector<Command>{kAllOffCommand});
}

TEST_CASE("the game's next command changes only what it sets on a tested kit") {
    KitSender sender;
    auto kit = AddKit(sender, "a");
    sender.Test({kPatternAll, kRed}, std::nullopt);
    PumpAll(sender);
    kit->sent.clear();
    sender.Game({0x0F, kBlue});
    PumpAll(sender);
    CHECK(kit->sent == std::vector<Command>{{0x0F, kBlue}});
}

TEST_CASE("a kit whose send fails is dropped and reported lost") {
    KitSender sender;
    auto a = AddKit(sender, "a");
    auto b = AddKit(sender, "b");
    PumpAll(sender);
    a->unplugged = true;
    sender.Game({0x01, kRed});
    PumpAll(sender);
    CHECK_FALSE(sender.Has("a"));
    CHECK(sender.Has("b"));
    const auto lost = sender.TakeLost();
    REQUIRE(lost.size() == 1);
    CHECK(lost[0].key == "a");
    CHECK(sender.TakeLost().empty());
}

TEST_CASE("a kit found again starts over with all-off") {
    KitSender sender;
    auto first = AddKit(sender, "a");
    sender.Game({0x01, kRed});
    PumpAll(sender);
    sender.Remove("a");
    CHECK(sender.Kits().empty());
    auto again = AddKit(sender, "a");
    PumpAll(sender);
    CHECK(again->sent == std::vector<Command>{kAllOffCommand, {0x01, kRed}});
}

TEST_CASE("Kits lists the kits by key") {
    KitSender sender;
    AddKit(sender, "b");
    AddKit(sender, "a");
    const auto kits = sender.Kits();
    REQUIRE(kits.size() == 2);
    CHECK(kits[0].key == "a");
    CHECK(kits[1].name == "kit b");
}

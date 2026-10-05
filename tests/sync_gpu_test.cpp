// Checks what the sync-only GPU's graphics system decides outside the SDK:
// emulated_gpu's rules at startup (src/Render/sync_gpu/emulated_gpu_mode.h),
// and its threads' vblank period, watchdog and log summary
// (src/Render/sync_gpu/sync_monitor.h).

#include <doctest/doctest.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include "src/Render/sync_gpu/emulated_gpu_mode.h"
#include "src/Render/sync_gpu/sync_monitor.h"
#include "src/Render/sync_gpu/xenos_defs.h"

using namespace band3::render::sync_gpu;
using namespace std::chrono_literals;

namespace {

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// SetNativeOnly for a test, put back after
struct NativeOnlyFor {
    bool was = NativeOnly();
    explicit NativeOnlyFor(bool on) { SetNativeOnly(on); }
    ~NativeOnlyFor() { SetNativeOnly(was); }
};

CurrentWait Waiting(std::chrono::nanoseconds elapsed, uint32_t addr = 0x1A2B3C42) {
    CurrentWait w;
    w.active = true;
    w.opcode = pm4::WAIT_REG_MEM;
    w.memory = true;
    w.poll_addr = addr;
    w.ref = 1;
    w.mask = 0xFFFFFFFF;
    w.function = 3;
    w.last_value = 0;
    w.elapsed = elapsed;
    return w;
}

}  // namespace

TEST_CASE("emulated_gpu is on or off, and only off goes without the emulated GPU") {
    CHECK(ParseEmulatedGpu("on") == std::optional<bool>(true));
    CHECK(ParseEmulatedGpu("off") == std::optional<bool>(false));
    CHECK_FALSE(ParseEmulatedGpu("").has_value());
    CHECK_FALSE(ParseEmulatedGpu("Off").has_value());

    // on, or anything the setting wouldn't take: as before, nothing to say,
    // whatever the build can present with
    for (const char* v : {"on", "", "maybe"}) {
        for (const bool presentable : {true, false}) {
            INFO(v << (presentable ? " presentable" : " not presentable"));
            const StartupGpuPlan plan = PlanStartupGpu(v, "xenos", presentable);
            CHECK_FALSE(plan.native_only);
            CHECK(plan.log.empty());
        }
    }

    const StartupGpuPlan off = PlanStartupGpu("off", "", true);
    CHECK(off.native_only);
    REQUIRE(off.log.size() == 1);
    CHECK(Has(off.log[0], "emulated_gpu off: no emulated GPU this run"));
    CHECK(Has(off.log[0], "F8 is inert"));

    // a plugin named (band3.toml, --gpu_plugin) isn't loaded, and it says so
    const StartupGpuPlan named = PlanStartupGpu("off", "xenos", true);
    CHECK(named.native_only);
    REQUIRE(named.log.size() == 2);
    CHECK(named.log[1] == "emulated_gpu off: gpu_plugin xenos isn't loaded");
}

TEST_CASE("emulated_gpu off is ignored on a build with nothing else to present with") {
    // neither Direct3D 12 nor Vulkan: the emulated GPU stays, the plugin named
    // with it, and the log says why
    for (const char* plugin : {"", "xenos"}) {
        INFO(plugin);
        const StartupGpuPlan plan = PlanStartupGpu("off", plugin, false);
        CHECK_FALSE(plan.native_only);
        REQUIRE(plan.log.size() == 1);
        CHECK(plan.log[0] ==
              "emulated_gpu off isn't available on this platform yet; running with the "
              "emulated GPU");
    }
}

TEST_CASE("without the emulated GPU the renderer is native, and says so if it wasn't") {
    CHECK_FALSE(ForceRendererNative(false, "emulated").has_value());
    CHECK_FALSE(ForceRendererNative(true, "native").has_value());
    const auto line = ForceRendererNative(true, "emulated");
    REQUIRE(line.has_value());
    CHECK(*line == "emulated_gpu off: renderer was emulated, native for this run");
}

TEST_CASE("a new emulated_gpu value is a change only against this run's mode") {
    {
        NativeOnlyFor run(false);
        CHECK_FALSE(EmulatedGpuChanged("on"));
        CHECK(EmulatedGpuChanged("off"));
        CHECK_FALSE(EmulatedGpuChanged("nonsense"));
    }
    {
        NativeOnlyFor run(true);
        CHECK(NativeOnly());
        CHECK(EmulatedGpuChanged("on"));
        CHECK_FALSE(EmulatedGpuChanged("off"));
    }
}

TEST_CASE("the vblank comes at the guest's refresh rate, or every millisecond free-running") {
    CHECK(VblankPeriodNs(60, false) == 16'666'667);
    CHECK(VblankPeriodNs(120, false) == 8'333'333);
    CHECK(VblankPeriodNs(59.94, false) == 16'683'350);
    // unset or nonsense: the console's 60
    CHECK(VblankPeriodNs(0, false) == 16'666'667);
    CHECK(VblankPeriodNs(-5, false) == 16'666'667);
    CHECK(VblankPeriodNs(1e9, false) == 16'666'667);
    // the frame cap paces the game: whatever the rate
    CHECK(VblankPeriodNs(60, true) == kFreeRunningVblankNs);
    CHECK(VblankPeriodNs(144, true) == 1'000'000);
}

TEST_CASE("opcodes and wait functions are named for the log") {
    CHECK(Pm4OpcodeName(pm4::WAIT_REG_MEM) == "WAIT_REG_MEM");
    CHECK(Pm4OpcodeName(pm4::EVENT_WRITE_SHD) == "EVENT_WRITE_SHD");
    CHECK(Pm4OpcodeName(pm4::XE_SWAP) == "XE_SWAP");
    CHECK(Pm4OpcodeName(0x7E) == "7E");
    CHECK(std::string(WaitFunctionName(3)) == "==");
    CHECK(std::string(WaitFunctionName(0)) == "never");
    CHECK(std::string(WaitFunctionName(15)) == "always");
}

TEST_CASE("the summary counts what changed since the last one, opcodes most first") {
    SyncCpStats before, now;
    before.packets.add(100);
    now.packets.add(1100);
    now.type0.add(400);
    now.type3.add(600);
    now.opcodes[pm4::SET_CONSTANT].add(300);
    now.opcodes[pm4::DRAW_INDX].add(200);
    now.opcodes[pm4::EVENT_WRITE_SHD].add(200);
    now.opcodes[pm4::INTERRUPT].add(60);
    before.opcodes[pm4::INTERRUPT].add(10);
    now.draws_skipped.add(200);
    now.waits.add(70);
    now.stalled_waits.add(60);
    now.wait_ns_total.add(250'000'000);
    now.wait_ns_max.raise_to(16'000'000);
    now.interrupts.add(60);
    now.swaps.add(60);
    now.fences.add(200);
    now.unknown_register_writes.add(2);
    now.unknown_register_reads.add(1);
    // the stalled waits: 55 yielding (0x20), 5 sleeping (0x100)
    now.stalled_by_band[kWaitYield].add(55);
    now.wait_ns_by_band[kWaitYield].add(200'000'000);
    now.polls_by_band[kWaitYield].add(90000);
    now.stalled_by_band[kWaitSleep].add(5);
    now.wait_ns_by_band[kWaitSleep].add(50'000'000);
    now.polls_by_band[kWaitSleep].add(9);
    // 0x100 seen first; `before` had seen it alone, 3 times
    before.wait_value_count.set(1);
    before.wait_values[0].set(0x100);
    before.stalled_by_value[0].add(3);
    now.wait_value_count.set(2);
    now.wait_values[0].set(0x100);
    now.stalled_by_value[0].add(8);
    now.wait_values[1].set(0x20);
    now.stalled_by_value[1].add(55);

    const SyncCpDelta d = DeltaOf(now, before);
    CHECK(d.packets == 1000);
    CHECK(d.unknown_registers == 3);
    CHECK(d.wait_ms == doctest::Approx(250));
    CHECK(d.wait_max_ms == doctest::Approx(16));
    REQUIRE(d.opcodes.size() == 4);
    CHECK(d.opcodes[0] == std::pair<uint32_t, uint64_t>{pm4::SET_CONSTANT, 300});
    // a tie keeps the opcodes' order
    CHECK(d.opcodes[1].first == pm4::DRAW_INDX);
    CHECK(d.opcodes[2].first == pm4::EVENT_WRITE_SHD);
    CHECK(d.opcodes[3] == std::pair<uint32_t, uint64_t>{pm4::INTERRUPT, 50});
    CHECK(d.stalled_by_band[kWaitYield] == 55);
    CHECK(d.wait_ms_by_band[kWaitSleep] == doctest::Approx(50));
    CHECK(d.polls_by_band[kWaitYield] == 90000);
    // the intervals that stalled in this time, most first
    REQUIRE(d.wait_values.size() == 2);
    CHECK(d.wait_values[0] == std::pair<uint32_t, uint64_t>{0x20, 55});
    CHECK(d.wait_values[1] == std::pair<uint32_t, uint64_t>{0x100, 5});

    const std::string line = SummaryLine(d, 10, 600, 3);
    CHECK(Has(line, "longest yet 16.0 ms); stalled by wait interval: yield 55 (200.0 ms, 90000 "
                    "polls), sleep 5 (50.0 ms, 9 polls), long_sleep 0 (0.0 ms, 0 polls); "
                    "intervals 0x20 55, 0x100 5; interrupts"));
    CHECK(Has(line, "sync gpu: the last 10 s: 1000 packets (type 0 400, type 1 0, type 3 600)"));
    CHECK(Has(line, "opcodes SET_CONSTANT 300, DRAW_INDX 200, EVENT_WRITE_SHD 200 and 1 more;"));
    CHECK(Has(line, "draws skipped 200; waits 70 (60 stalled, 250.0 ms in all, longest yet 16.0 ms)"));
    CHECK(Has(line, "interrupts 60, swaps 60, vblanks 600, fences 200"));
    CHECK(Has(line, "unknown registers 3"));

    CHECK(Has(SummaryLine(DeltaOf(before, before), 10, 0), "opcodes none;"));
    CHECK(Has(SummaryLine(DeltaOf(before, before), 10, 0), "intervals none;"));
}

TEST_CASE("the watchdog reports a wait once it has gone on 2 s, once, and when it ends") {
    StallWatch watch;
    CHECK(watch.Check(CurrentWait{}, 0, 0).empty());
    CHECK(watch.Check(Waiting(1s), 0x10, 0x20).empty());

    const std::vector<std::string> stall = watch.Check(Waiting(2s), 0x10, 0x20);
    REQUIRE(stall.size() == 1);
    CHECK(Has(stall[0], "sync gpu: watchdog: the command processor has waited 2.0 s in "
                        "WAIT_REG_MEM for memory 1A2B3C40 (endian 2) & FFFFFFFF == 00000001 "
                        "(last read 00000000); ring read index 10, write index 20"));
    // the same wait going on: said once
    CHECK(watch.Check(Waiting(3s), 0x10, 0x20).empty());
    CHECK(watch.Check(Waiting(9s), 0x10, 0x20).empty());

    // it ended
    const std::vector<std::string> ended = watch.Check(CurrentWait{}, 0x20, 0x20);
    REQUIRE(ended.size() == 1);
    CHECK(Has(ended[0], "the wait for memory 1A2B3C40 (endian 2) ended after 9 s or more"));
    CHECK(watch.Check(CurrentWait{}, 0x20, 0x20).empty());

    // another wait on the same word, already long by the next look: the last
    // one ended, and this one is reported
    CHECK(watch.Check(Waiting(5s), 0x20, 0x30).size() == 1);
    const std::vector<std::string> next = watch.Check(Waiting(2500ms), 0x20, 0x30);
    REQUIRE(next.size() == 2);
    CHECK(Has(next[0], "ended after 5 s or more"));
    CHECK(Has(next[1], "has waited 2.5 s"));
}

TEST_CASE("the watchdog names a register wait by its register") {
    CurrentWait w = Waiting(3s, 0x1951);
    w.memory = false;
    w.function = 5;
    w.last_value = 7;
    w.wait = 0x40;
    CHECK(Has(DescribeStall(w, 1, 2), "for register 1951 & FFFFFFFF >= 00000001 (last read 00000007)"));
    // its wait interval, and whether that yields or sleeps between polls
    CHECK(Has(DescribeStall(w, 1, 2), "write index 2; wait interval 0x40 (yield)."));
    CHECK(WaitBandOf(0xFF) == kWaitYield);
    CHECK(WaitBandOf(0x100) == kWaitSleep);
    CHECK(WaitBandOf(0xFFF) == kWaitSleep);
    CHECK(WaitBandOf(0x1000) == kWaitLongSleep);
    CHECK(std::string(WaitBandName(kWaitLongSleep)) == "long_sleep");
}

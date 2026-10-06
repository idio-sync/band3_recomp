// Checks the renderer setting's rules (src/Render/renderer_mode.h): its three
// values, the migration of the retired emulated_gpu, what startup runs for
// each value, what a change of the setting and F8 do while band3 runs, and
// when the launcher's Play restarts band3 for it.

#include <doctest/doctest.h>
#include <optional>
#include <string>
#include "src/Render/renderer_mode.h"
#include "src/renderer_default.h"

using namespace band3::render;

namespace {

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

OldSetting Unset(std::string value) { return {std::move(value), SettingSource::kUnset}; }
OldSetting InConfig(std::string value) { return {std::move(value), SettingSource::kConfig}; }
OldSetting OnCommandLine(std::string value) {
    return {std::move(value), SettingSource::kCommandLine};
}

// a run as startup plans it for `renderer`
RendererState RunFor(const char* renderer, bool presentable = true) {
    return StartState(PlanStartupGpu(renderer, "", presentable), presentable);
}

}  // namespace

TEST_CASE("renderer is native, emulated or both") {
    CHECK(ParseRenderer("native") == RendererMode::kNative);
    CHECK(ParseRenderer("emulated") == RendererMode::kEmulated);
    CHECK(ParseRenderer("both") == RendererMode::kBoth);
    CHECK_FALSE(ParseRenderer("").has_value());
    CHECK_FALSE(ParseRenderer("Native").has_value());
    CHECK_FALSE(ParseRenderer("on").has_value());
    for (const RendererMode m : {RendererMode::kNative, RendererMode::kEmulated, RendererMode::kBoth}) {
        CHECK(ParseRenderer(RendererName(m)) == m);
    }
    CHECK(std::string(RendererLabel(RendererMode::kNative)) == "Native");
    CHECK(std::string(RendererLabel(RendererMode::kEmulated)) == "Emulated");
    CHECK(std::string(RendererLabel(RendererMode::kBoth)) == "Native + emulated (debug)");
    // the default is one of them
    CHECK(ParseRenderer(band3::settings::kDefaultRenderer).has_value());
}

TEST_CASE("an unset emulated_gpu leaves renderer alone") {
    for (const char* r : {"native", "emulated", "both"}) {
        INFO(r);
        const RendererMigration m = MigrateEmulatedGpu(Unset(""), InConfig(r));
        CHECK_FALSE(m.renderer.has_value());
        CHECK(m.log.empty());
    }
    // a value the alias wouldn't take counts as unset too
    CHECK_FALSE(MigrateEmulatedGpu(InConfig("maybe"), Unset("native")).renderer.has_value());
}

TEST_CASE("emulated_gpu off becomes renderer native, whatever the old renderer said") {
    for (const char* r : {"native", "emulated"}) {
        INFO(r);
        const RendererMigration m = MigrateEmulatedGpu(InConfig("off"), InConfig(r));
        REQUIRE(m.renderer.has_value());
        CHECK(*m.renderer == "native");
        CHECK(m.source == SettingSource::kConfig);
        CHECK(Has(m.log, "emulated_gpu = off"));
        CHECK(Has(m.log, "renderer = native"));
    }
    // renderer unset: its default doesn't matter either
    CHECK(*MigrateEmulatedGpu(InConfig("off"), Unset("emulated")).renderer == "native");
}

TEST_CASE("emulated_gpu on with renderer native becomes both, with emulated stays emulated") {
    CHECK(*MigrateEmulatedGpu(InConfig("on"), InConfig("native")).renderer == "both");
    CHECK(*MigrateEmulatedGpu(InConfig("on"), InConfig("emulated")).renderer == "emulated");
    // renderer unset: as its default was then (native on Windows: A/B)
    CHECK(*MigrateEmulatedGpu(InConfig("on"), Unset("native")).renderer == "both");
    CHECK(*MigrateEmulatedGpu(InConfig("on"), Unset("emulated")).renderer == "emulated");
}

TEST_CASE("emulated_gpu is read with renderer from the same place or a weaker one") {
    // both on the command line: the old pair, migrated, kept as the command line's
    const RendererMigration cli = MigrateEmulatedGpu(OnCommandLine("on"), OnCommandLine("native"));
    REQUIRE(cli.renderer.has_value());
    CHECK(*cli.renderer == "both");
    CHECK(cli.source == SettingSource::kCommandLine);
    // the command line's emulated_gpu over band3.toml's renderer
    const RendererMigration over = MigrateEmulatedGpu(OnCommandLine("off"), InConfig("emulated"));
    CHECK(*over.renderer == "native");
    CHECK(over.source == SettingSource::kCommandLine);
    // a renderer the command line set wins over band3.toml's emulated_gpu,
    // which is ignored, and says so
    const RendererMigration wins = MigrateEmulatedGpu(InConfig("off"), OnCommandLine("emulated"));
    CHECK_FALSE(wins.renderer.has_value());
    CHECK(Has(wins.log, "ignored"));
    CHECK(Has(wins.log, "renderer = emulated"));
}

TEST_CASE("a renderer already both was migrated: a stale emulated_gpu beside it is ignored") {
    // no build with emulated_gpu wrote both, so it doesn't flip back to native
    // at every start
    for (const char* v : {"on", "off"}) {
        INFO(v);
        const RendererMigration m = MigrateEmulatedGpu(InConfig(v), InConfig("both"));
        CHECK_FALSE(m.renderer.has_value());
        CHECK(Has(m.log, "ignored"));
    }
    // both as a default isn't a choice (no build has that default)
    CHECK(*MigrateEmulatedGpu(InConfig("off"), Unset("both")).renderer == "native");
}

TEST_CASE("the native renderer's anisotropy: its own setting, else the emulated GPU's") {
    // its own, whatever the emulated GPU's says
    CHECK(NativeAnisotropy(5, std::nullopt) == 5);
    CHECK(NativeAnisotropy(0, 3) == 0);
    // -1: the emulated GPU's where it runs, so both pictures match
    CHECK(NativeAnisotropy(-1, 4) == 4);
    CHECK(NativeAnisotropy(-1, -1) == -1);
    // native alone: no anisotropic_override, the game's own
    CHECK(NativeAnisotropy(-1, std::nullopt) == -1);
    // out of range is the game's own
    CHECK(NativeAnisotropy(9, std::nullopt) == -1);
    CHECK(NativeAnisotropy(-1, 7) == -1);
}

TEST_CASE("startup: native runs without the emulated GPU, emulated and both with it") {
    const StartupGpuPlan native = PlanStartupGpu("native", "", true);
    CHECK(native.mode == RendererMode::kNative);
    CHECK(native.native_only);
    REQUIRE(native.log.size() == 1);
    CHECK(Has(native.log[0], "no emulated GPU this run"));

    const StartupGpuPlan emulated = PlanStartupGpu("emulated", "", true);
    CHECK(emulated.mode == RendererMode::kEmulated);
    CHECK_FALSE(emulated.native_only);
    REQUIRE(emulated.log.size() == 1);
    CHECK(Has(emulated.log[0], "the emulated GPU draws the window"));

    const StartupGpuPlan both = PlanStartupGpu("both", "", true);
    CHECK(both.mode == RendererMode::kBoth);
    CHECK_FALSE(both.native_only);
    REQUIRE(both.log.size() == 1);
    CHECK(Has(both.log[0], "F8 switches"));

    // a plugin named (band3.toml, --gpu_plugin) isn't loaded natively, and it says so
    const StartupGpuPlan named = PlanStartupGpu("native", "xenos", true);
    CHECK(named.native_only);
    REQUIRE(named.log.size() == 2);
    CHECK(named.log[1] == "renderer native: gpu_plugin xenos isn't loaded");
    CHECK(PlanStartupGpu("both", "xenos", true).log.size() == 1);
}

TEST_CASE("startup: a build with nothing to present with on its own runs native as emulated") {
    // neither Direct3D 12 nor Vulkan (CanPresentNativeOnly): the emulated GPU
    // stays, the plugin named with it, and the log says why
    for (const char* plugin : {"", "xenos"}) {
        INFO(plugin);
        const StartupGpuPlan plan = PlanStartupGpu("native", plugin, false);
        CHECK(plan.mode == RendererMode::kEmulated);
        CHECK_FALSE(plan.native_only);
        REQUIRE(plan.log.size() == 1);
        CHECK(plan.log[0] == kNotPresentableHere);
    }
    CHECK(PlanStartupGpu("emulated", "", false).mode == RendererMode::kEmulated);
    CHECK(PlanStartupGpu("both", "", false).mode == RendererMode::kBoth);
}

TEST_CASE("startup: a value the setting wouldn't take runs as the build's default") {
    const StartupGpuPlan plan = PlanStartupGpu("nonsense", "", true);
    CHECK(plan.mode == *ParseRenderer(band3::settings::kDefaultRenderer));
}

TEST_CASE("the picture shown at start: native for native and both, emulated for emulated") {
    CHECK(RunFor("native").show_native);
    CHECK(RunFor("native").native_only);
    CHECK_FALSE(RunFor("emulated").show_native);
    CHECK(RunFor("both").show_native);
    CHECK(RunFor("both").live == RendererMode::kBoth);
    CHECK_FALSE(RunFor("native", false).show_native);
}

TEST_CASE("F8 switches the picture in both, and only logs elsewhere") {
    RendererState run = RunFor("both");
    RendererStep step = OnSwitchKey(run);
    CHECK_FALSE(step.next.show_native);
    REQUIRE(step.log.has_value());
    CHECK(Has(*step.log, "emulated"));
    step = OnSwitchKey(step.next);
    CHECK(step.next.show_native);

    for (const char* r : {"native", "emulated"}) {
        INFO(r);
        run = RunFor(r);
        step = OnSwitchKey(run);
        CHECK(step.next.show_native == run.show_native);
        CHECK(step.next.live == run.live);
        REQUIRE(step.log.has_value());
        CHECK(Has(*step.log, "Native + emulated (debug)"));
        CHECK(Has(*step.log, "renderer = both"));
    }
}

TEST_CASE("emulated and both switch at once while the emulated GPU runs") {
    RendererState run = RunFor("emulated");
    RendererStep step = OnRendererSetting(run, "both");
    CHECK(step.next.live == RendererMode::kBoth);
    CHECK(step.next.show_native);
    step = OnRendererSetting(step.next, "emulated");
    CHECK(step.next.live == RendererMode::kEmulated);
    CHECK_FALSE(step.next.show_native);

    // F8 to the emulated picture in both, then emulated: stays emulated
    run = OnSwitchKey(RunFor("both")).next;
    CHECK(OnRendererSetting(run, "emulated").next.show_native == false);
}

TEST_CASE("changes between native and the others wait for a restart") {
    // native-only: nothing else can be shown
    RendererState run = RunFor("native");
    for (const char* v : {"emulated", "both"}) {
        INFO(v);
        const RendererStep step = OnRendererSetting(run, v);
        CHECK(step.next.live == RendererMode::kNative);
        CHECK(step.next.show_native);
        REQUIRE(step.log.has_value());
        CHECK(Has(*step.log, "next start"));
    }
    CHECK_FALSE(OnRendererSetting(run, "native").log.has_value());

    // with the emulated GPU: native keeps what's shown
    for (const char* r : {"emulated", "both"}) {
        INFO(r);
        run = RunFor(r);
        const RendererStep step = OnRendererSetting(run, "native");
        CHECK(step.next.live == run.live);
        CHECK(step.next.show_native == run.show_native);
        REQUIRE(step.log.has_value());
        CHECK(Has(*step.log, "next start"));
    }
    // in both, native shows the native picture meanwhile
    run = OnSwitchKey(RunFor("both")).next;
    CHECK_FALSE(run.show_native);
    CHECK(OnRendererSetting(run, "native").next.show_native);
    CHECK(OnRendererSetting(run, "native").next.live == RendererMode::kBoth);
    // ... unless this build can't run native at all
    const RendererStep never = OnRendererSetting(RunFor("emulated", false), "native");
    REQUIRE(never.log.has_value());
    CHECK(Has(*never.log, "isn't available"));
}

TEST_CASE("Play restarts band3 for a renderer only when startup would pick another GPU") {
    const RendererState native = RunFor("native");
    CHECK_FALSE(RendererRestartNeeded(native, "native"));
    CHECK(RendererRestartNeeded(native, "emulated"));
    CHECK(RendererRestartNeeded(native, "both"));
    for (const char* r : {"emulated", "both"}) {
        INFO(r);
        const RendererState run = RunFor(r);
        CHECK(RendererRestartNeeded(run, "native"));
        CHECK_FALSE(RendererRestartNeeded(run, "emulated"));
        CHECK_FALSE(RendererRestartNeeded(run, "both"));
    }
    // a build that can't run native: it would come back the same
    CHECK_FALSE(RendererRestartNeeded(RunFor("emulated", false), "native"));
    CHECK_FALSE(RendererRestartNeeded(native, "nonsense"));
}

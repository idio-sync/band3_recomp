#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"

// Rock Band 3 Deluxe. Its code caves are declared in band3_rb3dx.toml; this
// covers what a config row can't.
//
// Deluxe's main calls App::RunWithoutDebugging with `bcl 20,lt` (branch
// always and link) where TU5 has a plain call. The recompiler doesn't
// implement bcl and generates a throw there, so the game ends as soon as
// App's constructor finishes loading. When main holds that instruction, main
// runs here instead; otherwise the recompiled main runs as it is.

extern "C" void __imp__rb3_main(PPCContext& ctx, uint8_t* base);
REX_EXTERN(App___ct);
REX_EXTERN(App__RunWithoutDebugging);
REX_EXTERN(App_dt);

namespace {

constexpr uint32_t kMainRunCall = 0x82272E90;
// bcl 20,lt,0x82270080
constexpr uint32_t kDeluxeRunCall = 0x4280D1F1;
// main's frame, with the App at r31+80 (r31 is the new r1)
constexpr uint32_t kMainFrame = 112;
constexpr uint32_t kMainApp = 80;

}  // namespace

extern "C" REX_FUNC(rb3_main) {
    if (REX_LOAD_U32(kMainRunCall) != kDeluxeRunCall) {
        __imp__rb3_main(ctx, base);
        return;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        REXLOG_INFO("rb3dx: running Deluxe's main (bcl call to App::RunWithoutDebugging)");
    }

    const uint32_t caller_sp = ctx.r1.u32;
    const uint64_t caller_r31 = ctx.r31.u64;
    const uint32_t sp = caller_sp - kMainFrame;
    REX_STORE_U32(sp, caller_sp);  // back chain, as stwu leaves it
    ctx.r1.u64 = sp;
    ctx.r31.u64 = sp;
    const uint32_t app = sp + kMainApp;

    // App app(argc, argv);
    ctx.r5.u64 = ctx.r4.u64;
    ctx.r4.u64 = ctx.r3.u64;
    ctx.r3.u64 = app;
    ctx.lr = 0x82272E8C;
    App___ct(ctx, base);

    // app.RunWithoutDebugging();
    ctx.r3.u64 = app;
    ctx.lr = 0x82272E94;
    App__RunWithoutDebugging(ctx, base);

    // ~App()
    ctx.r3.u64 = app;
    ctx.lr = 0x82272E9C;
    App_dt(ctx, base);

    ctx.r3.u64 = 0;
    ctx.r1.u64 = caller_sp;
    ctx.r31.u64 = caller_r31;
}

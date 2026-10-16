// Serializes the game's archive block cache (TheBlockMgr, and TheHDCache it
// drives) across threads.
//
// BlockMgr has no lock. The main thread and a second guest thread read the
// archive at once as the main hub first loads (both through
// ArkFile::ReadDone -> BlockMgr::Poll), and BlockMgr::ReadBlock reads the
// current request (BlockMgr +20) before starting its read and again after
// (to stamp its order at +12): when the other thread's Poll has finished and
// cleared that request in between, ReadBlock writes to guest 0x0000000C. The
// SDK's handler returns to the same store, which faults again, hundreds of
// thousands of times a second, with a log line each, until the harness or
// the player gives up: about one boot in 25 on a Steam Deck, since at least
// 2026-10-13.
//
// So each of BlockMgr's functions that code outside it calls runs under one
// recursive lock. Nothing in them waits on another thread: HDCache has no
// thread of its own (BlockMgr::Poll drives it) and CDRead reads the disc
// file synchronously.
//
// TEST ONLY: BAND3_BLOCKMGR_STRESS=<ms> sleeps that long in HDCache::ReadAsync,
// between ReadBlock's two reads, and BAND3_BLOCKMGR_NOLOCK=1 leaves the lock
// out, to show the race and the fix.

#include <rex/hook.h>
#include <rex/types.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

extern "C" void __imp__BlockMgr__Poll(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BlockMgr__AddTask(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BlockMgr__GetAssociatedBlocks(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BlockMgr__GetBlockData(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BlockMgr__KillBlockRequests(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BlockMgr__SpinUp(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BlockMgr__MarkDiscRead(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__HDCache__ReadAsync(PPCContext& ctx, uint8_t* base);

namespace {

std::recursive_mutex g_mutex;

bool Env(const char* name) { return std::getenv(name) != nullptr; }

template <void (*Fn)(PPCContext&, uint8_t*)>
void Locked(PPCContext& ctx, uint8_t* base) {
    static const bool off = Env("BAND3_BLOCKMGR_NOLOCK");
    if (off) {
        Fn(ctx, base);
        return;
    }
    std::lock_guard lock(g_mutex);
    Fn(ctx, base);
}

}  // namespace

extern "C" REX_FUNC(BlockMgr__Poll) { Locked<__imp__BlockMgr__Poll>(ctx, base); }
extern "C" REX_FUNC(BlockMgr__AddTask) { Locked<__imp__BlockMgr__AddTask>(ctx, base); }
extern "C" REX_FUNC(BlockMgr__GetAssociatedBlocks) {
    Locked<__imp__BlockMgr__GetAssociatedBlocks>(ctx, base);
}
extern "C" REX_FUNC(BlockMgr__GetBlockData) { Locked<__imp__BlockMgr__GetBlockData>(ctx, base); }
extern "C" REX_FUNC(BlockMgr__KillBlockRequests) {
    Locked<__imp__BlockMgr__KillBlockRequests>(ctx, base);
}
extern "C" REX_FUNC(BlockMgr__SpinUp) { Locked<__imp__BlockMgr__SpinUp>(ctx, base); }
extern "C" REX_FUNC(BlockMgr__MarkDiscRead) { Locked<__imp__BlockMgr__MarkDiscRead>(ctx, base); }

extern "C" REX_FUNC(HDCache__ReadAsync) {
    static const char* env = std::getenv("BAND3_BLOCKMGR_STRESS");
    static const int ms = env ? std::atoi(env) : 0;
    __imp__HDCache__ReadAsync(ctx, base);
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

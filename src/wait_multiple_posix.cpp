// rex::thread::WaitMultiple, in place of the SDK's on Linux.
//
// The SDK's POSIX WaitMultiple (src/core/threading_posix.cpp, as of nightly
// 20260925) polls its handles, sleeping
// duration_cast<milliseconds>(deadline - now) capped at 1 ms between polls:
// within the last millisecond that's 0, and sleep_for(0) doesn't sleep. An
// alertable wait goes round in slices of exactly 1 ms, to dispatch user
// callbacks (APCs) between them, so every slice is its last millisecond and
// the wait never sleeps at all. The audio worker waits that way on its
// clients' semaphores all the time: on a Steam Deck it spun 98% of the time
// it waited, about 60% of a core of the four, with 22 million mutex trylocks
// a second (Windows' WaitForMultipleObjects sleeps).
//
// The executable's definition is the one the SDK's runtime library binds to
// (it calls WaitMultiple through its GOT; cmake/linux_exports.map exports
// _Z*). This one keeps the SDK's polling, a millisecond apart, but sleeps
// between polls: user callbacks are dispatched by an alertable Wait on the
// first handle with no timeout (which dispatches them, then times out
// without looking at the handle), and the handles polled by the SDK's own
// WaitMultiple, not alertable and with no timeout (one look, wait_all
// included). A single handle, or no timeout, is the SDK's as it was: its
// single-handle wait sleeps on a condition variable.

#ifdef __linux__

#include <dlfcn.h>

#include <rex/thread.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace rex::thread {

namespace {

using WaitMultipleFn = std::pair<WaitResult, size_t> (*)(WaitHandle**, size_t, bool, bool,
                                                         std::chrono::milliseconds);

WaitMultipleFn SdkWaitMultiple() {
    static const WaitMultipleFn sdk = [] {
        auto f = reinterpret_cast<WaitMultipleFn>(dlsym(
            RTLD_NEXT,
            "_ZN3rex6thread12WaitMultipleEPPNS0_10WaitHandleEmbbNSt6chrono8durationIlSt5ratioILl1ELl1000EEEE"));
        if (!f) {
            // nothing to wait with; logging may not be up yet
            std::fprintf(stderr, "band3: the SDK's rex::thread::WaitMultiple is missing (%s)\n",
                         dlerror());
            std::abort();
        }
        return f;
    }();
    return sdk;
}

constexpr auto kPollInterval = std::chrono::milliseconds(1);

}  // namespace

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
    const WaitMultipleFn sdk = SdkWaitMultiple();
    if (wait_handle_count <= 1 || timeout <= std::chrono::milliseconds::zero())
        return sdk(wait_handles, wait_handle_count, wait_all, is_alertable, timeout);
    using Clock = std::chrono::steady_clock;
    const bool forever = timeout == std::chrono::milliseconds::max();
    const Clock::time_point deadline = forever ? Clock::time_point::max() : Clock::now() + timeout;
    while (true) {
        if (is_alertable && Wait(wait_handles[0], true, std::chrono::milliseconds::zero()) ==
                                WaitResult::kUserCallback)
            return {WaitResult::kUserCallback, 0};
        const auto polled =
            sdk(wait_handles, wait_handle_count, wait_all, false, std::chrono::milliseconds::zero());
        if (polled.first != WaitResult::kTimeout) return polled;
        const Clock::time_point now = Clock::now();
        if (!forever && now >= deadline) return {WaitResult::kTimeout, 0};
        std::this_thread::sleep_for(
            forever ? Clock::duration(kPollInterval)
                    : std::min<Clock::duration>(kPollInterval, deadline - now));
    }
}

}  // namespace rex::thread

#endif

// rex::thread::TimerQueue::TimerThreadMain, in place of the SDK's on Linux.
//
// The SDK's timer queue (src/core/timer_queue.cpp, as of nightly 20260925)
// waits for new timers and the next one's due time in disruptorplus's
// spin_wait_strategy: it spins, then yields, sleeping a millisecond only
// every 20th time round. With nothing due it never stops: about a tenth of a
// Steam Deck's core, whose CPU and GPU share one power budget. Its thread is
// started at the runtime library's static init, so it can't be left out; but
// the loop is TimerThreadMain, which the library calls through its GOT, so the
// executable's definition (cmake/linux_exports.map exports _Z*) is the one the
// thread runs. This one does what the SDK's does, but looks for new timers
// without waiting (last_published_after) and sleeps until the next one is due,
// a quarter of a millisecond at most, so a timer queued meanwhile for sooner
// is started at most that late (Windows' own timers are no finer than 1 ms).
//
// It reads the SDK's TimerQueue, so the class below is the SDK's, member for
// member; only TimerThreadMain is defined here (another member defined in the
// executable would replace the SDK's too). Before taking over it checks that
// this is the SDK's timer_queue_, that its ring buffer and claim strategy have
// the SDK's 512 slots, and that its dispatch_thread_, the last member, is
// this thread; if any isn't so, the SDK's own loop runs instead.
// Not on Windows, whose DLLs keep calling their own.

#ifdef __linux__

#include <dlfcn.h>
#include <pthread.h>

#include <disruptorplus/disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/disruptorplus/ring_buffer.hpp>
#include <disruptorplus/disruptorplus/sequence_barrier.hpp>
#include <disruptorplus/disruptorplus/spin_wait_strategy.hpp>

#include <rex/thread/timer_queue.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <forward_list>
#include <memory>
#include <thread>

namespace dp = disruptorplus;

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

// The SDK's, as declared there; members not defined here
class TimerQueue {
 public:
  using clock = WaitItem::clock;

  TimerQueue();
  ~TimerQueue();
  void TimerThreadMain(std::stop_token stop_token);
  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item);
  std::jthread::id dispatch_thread_id() const;

 private:
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  dp::spin_wait_strategy wait_strategy_;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim_strategy_;
  dp::sequence_barrier<dp::spin_wait_strategy> consumed_;
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;

  bool LooksLikeTheSdks() const;
};

namespace {

constexpr auto kLongestSleep = std::chrono::microseconds(250);

using ThreadMainFn = void (*)(TimerQueue*, std::stop_token);

}  // namespace

bool TimerQueue::LooksLikeTheSdks() const {
  const void* sdk_queue = dlsym(RTLD_DEFAULT, "_ZN3rex6thread12timer_queue_E");
  if (sdk_queue != this) return false;
  if (buffer_.size() != kWaitCount || claim_strategy_.buffer_size() != kWaitCount) return false;
  // the constructor assigns dispatch_thread_ once the thread has started,
  // microseconds after
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (dispatch_thread_.get_id() != std::this_thread::get_id()) {
    if (std::chrono::steady_clock::now() > give_up) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void TimerQueue::TimerThreadMain(std::stop_token stop_token) {
  // the SDK's name is too long for Linux's 15 characters
  pthread_setname_np(pthread_self(), "rex timers");
  if (!LooksLikeTheSdks()) {
    std::fprintf(stderr,
                 "band3: the SDK's TimerQueue isn't laid out as band3 expects; running its "
                 "own timer loop\n");
    const auto sdk = reinterpret_cast<ThreadMainFn>(
        dlsym(RTLD_NEXT, "_ZN3rex6thread10TimerQueue15TimerThreadMainESt10stop_token"));
    if (sdk) sdk(this, std::move(stop_token));
    return;
  }

  dp::sequence_t next_sequence = 0;
  const auto comp = [](const std::shared_ptr<WaitItem>& left,
                       const std::shared_ptr<WaitItem>& right) {
    return left->due_ < right->due_;
  };
  while (!stop_token.stop_requested()) {
    // new wait items into the sorted wait queue
    const dp::sequence_t available = claim_strategy_.last_published_after(next_sequence - 1);
    if (available != next_sequence - 1) {
      std::forward_list<std::shared_ptr<WaitItem>> wait_items;
      do {
        wait_items.push_front(std::move(buffer_[next_sequence]));
      } while (next_sequence++ != available);
      consumed_.publish(available);
      wait_items.sort(comp);
      wait_queue_.merge(wait_items, comp);
    }

    // the SDK's: invoke what's due, rescheduling the recurring
    {
      std::forward_list<std::shared_ptr<WaitItem>> wait_items;
      while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
        auto wait_item = std::move(wait_queue_.front());
        wait_queue_.pop_front();
        auto state = WaitItem::State::kIdle;
        if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                      std::memory_order_acq_rel)) {
          wait_item->callback_(wait_item->userdata_);
          if (wait_item->interval_ != clock::duration::zero() &&
              wait_item->state_.load(std::memory_order_acquire) !=
                  WaitItem::State::kInCallbackSelfDisarmed) {
            wait_item->due_ += wait_item->interval_;
            wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
            wait_item->state_.notify_all();
            wait_items.push_front(std::move(wait_item));
          } else {
            wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
            wait_item->state_.notify_all();
          }
        }
      }
      wait_items.sort(comp);
      wait_queue_.merge(wait_items, comp);
    }

    // until the next is due, or a new one may have come
    const clock::time_point now = clock::now();
    clock::time_point wake = now + kLongestSleep;
    if (!wait_queue_.empty()) wake = std::min(wake, wait_queue_.front()->due_);
    if (wake > now) std::this_thread::sleep_until(wake);
  }
}

}  // namespace rex::thread

#endif

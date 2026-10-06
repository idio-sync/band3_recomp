#include "renderer_switch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace band3::render {

namespace {

std::mutex g_mutex;
RendererState g_state;
std::atomic<bool> g_show_native{false};
std::map<int, std::function<void(bool)>> g_listeners;
int g_next_listener = 1;

// takes the step, logs it, and tells the listeners if the picture changed
void Apply(const RendererStep& step) {
    bool changed = false;
    std::vector<std::function<void(bool)>> listeners;
    {
        std::lock_guard lock(g_mutex);
        changed = step.next.show_native != g_state.show_native;
        g_state = step.next;
        g_show_native.store(step.next.show_native, std::memory_order_release);
        if (changed) {
            for (const auto& [id, listener] : g_listeners) listeners.push_back(listener);
        }
    }
    if (step.log) REXLOG_INFO("{}", *step.log);
    // outside the lock: a listener (the native present's Follow) may take a while
    for (const auto& listener : listeners) listener(step.next.show_native);
}

}  // namespace

void StartRendererSwitch(const StartupGpuPlan& plan, bool presentable) {
    {
        std::lock_guard lock(g_mutex);
        g_state = StartState(plan, presentable);
        g_show_native.store(g_state.show_native, std::memory_order_release);
    }
    rex::cvar::RegisterChangeCallback("renderer", [](std::string_view, std::string_view v) {
        Apply(OnRendererSetting(CurrentRenderer(), v));
    });
}

RendererState CurrentRenderer() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

bool ShowsNativePicture() { return g_show_native.load(std::memory_order_acquire); }

void PressRendererSwitch() { Apply(OnSwitchKey(CurrentRenderer())); }

bool RendererRestartNeeded(std::string_view setting) {
    return RendererRestartNeeded(CurrentRenderer(), setting);
}

int AddShownPictureListener(std::function<void(bool native)> listener) {
    std::lock_guard lock(g_mutex);
    const int id = g_next_listener++;
    g_listeners.emplace(id, std::move(listener));
    return id;
}

void RemoveShownPictureListener(int id) {
    std::lock_guard lock(g_mutex);
    g_listeners.erase(id);
}

}  // namespace band3::render

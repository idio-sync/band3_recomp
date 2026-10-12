#include "app_quit.h"

#include <mutex>

namespace band3::app {

namespace {
std::mutex g_mutex;
std::function<void()> g_quit;
}

void SetQuit(std::function<void()> quit) {
    std::lock_guard lock(g_mutex);
    g_quit = std::move(quit);
}

bool Quit() {
    std::function<void()> quit;
    {
        std::lock_guard lock(g_mutex);
        quit = g_quit;
    }
    if (!quit) return false;
    quit();
    return true;
}

}

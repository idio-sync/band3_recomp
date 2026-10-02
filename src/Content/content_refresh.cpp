#include "content_refresh.h"

namespace band3::content {

std::string RefreshScript(std::string_view screen) {
    // the main hub's enter: it refreshes each time it's entered
    if (screen == "main_hub_screen") return "{if {content_mgr refresh_done} {content_mgr start_refresh}}";
    // the Music Library's own storage_changed, which closes a song's details
    // first; it refreshes unless one is going already
    if (screen == "song_select_screen") return "{song_select_panel storage_changed}";
    return {};
}

std::string RefreshPlanner::Next(uint64_t generation, uint64_t listed, std::string_view screen,
                                 bool in_game) {
    if (generation <= listed || in_game) return {};
    if (generation == tried_generation_ && screen == tried_screen_) return {};
    std::string script = RefreshScript(screen);
    // the screen is remembered either way: leaving a safe one for another
    // screen and coming back is another try
    tried_generation_ = generation;
    tried_screen_ = std::string(screen);
    return script;
}

}

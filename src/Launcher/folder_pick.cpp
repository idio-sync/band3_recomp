#include "folder_pick.h"
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <mutex>

namespace band3::launcher {

struct FolderPicker::Pick {
    std::mutex mutex;
    bool done = false;
    std::optional<std::string> folder;
    std::string error;
};

namespace {

void SDLCALL OnPicked(void* userdata, const char* const* files, int) {
    auto* holder = static_cast<std::shared_ptr<FolderPicker::Pick>*>(userdata);
    {
        FolderPicker::Pick& pick = **holder;
        std::lock_guard lock(pick.mutex);
        pick.done = true;
        if (!files) {
            pick.error = SDL_GetError();
        } else if (files[0]) {
            pick.folder = files[0];
        }
    }
    delete holder;
}

}

void FolderPicker::Start(std::string target, const std::string& from) {
    // a dialog that never answered (gamescope may not show it) doesn't block
    // another: its answer goes to the pick it was for, which is dropped
    pick_ = std::make_shared<Pick>();
    target_ = std::move(target);
    // the callback's own reference, which it deletes
    auto* holder = new std::shared_ptr<Pick>(pick_);
    SDL_ShowOpenFolderDialog(OnPicked, holder, nullptr, from.empty() ? nullptr : from.c_str(),
                             false);
}

std::optional<FolderPicker::Answer> FolderPicker::Take() {
    if (!pick_) return std::nullopt;
#ifndef _WIN32
    // the dialog goes through the xdg desktop portal off Windows, which may
    // answer only while SDL's events are pumped, and nothing pumps them
    // before the game runs. Unverified on Linux; harmless without video.
    SDL_PumpEvents();
#endif
    Answer answer{.target = target_};
    {
        std::lock_guard lock(pick_->mutex);
        if (!pick_->done) return std::nullopt;
        answer.folder = pick_->folder;
        answer.error = pick_->error;
    }
    pick_.reset();
    return answer;
}

}

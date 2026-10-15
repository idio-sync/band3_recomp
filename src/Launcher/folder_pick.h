#pragma once
#include <memory>
#include <optional>
#include <string>

// The system's folder dialog, which answers on a thread of its own: start one
// for a setting, then take its answer on the UI thread. For mouse and
// keyboard players; a controller has the screens' own folder lists.

namespace band3::launcher {

class FolderPicker {
public:
    struct Answer {
        // what it was started for (a setting's name)
        std::string target;
        // the folder picked; nullopt when the dialog was cancelled or failed
        std::optional<std::string> folder;
        // why the dialog couldn't be shown, if it couldn't
        std::string error;
    };

    // opens the dialog at `from`, in place of one that never answered
    void Start(std::string target, const std::string& from);
    // the answer, once there is one; call it once a frame
    std::optional<Answer> Take();
    bool Open() const { return pick_ != nullptr; }

    // what the dialog's callback shares with the picker, which may be gone
    struct Pick;

private:
    std::shared_ptr<Pick> pick_;
    std::string target_;
};

}

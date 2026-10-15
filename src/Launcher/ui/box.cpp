#include "box.h"

namespace band3::launcher::ui {

std::optional<int> StepBox(Box& box, Action action) {
    const int count = static_cast<int>(box.options.size());
    if (count == 0) return std::nullopt;
    switch (action) {
    case Action::kUp: box.selected = MoveSelection(box.selected, -1, count, true); break;
    case Action::kDown: box.selected = MoveSelection(box.selected, 1, count, true); break;
    case Action::kSelect: return box.selected;
    case Action::kBack:
        if (box.cancel >= 0 && box.cancel < count) return box.cancel;
        break;
    default: break;
    }
    return std::nullopt;
}

}

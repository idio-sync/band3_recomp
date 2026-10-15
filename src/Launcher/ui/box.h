#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "navigation.h"

// A box over a screen, as Rock Band 3's pause container asks things: a title
// in caps, a few lines, then its choices in a centred menu. It has the input
// until a choice is picked (widgets.h's DrawBox draws it).

namespace band3::launcher::ui {

struct Box {
    std::string title;
    std::vector<std::string> lines;
    std::vector<std::string> options;
    int selected = 0;
    // the choice B picks; -1 when the box must be answered
    int cancel = -1;
    // called with the choice picked, after the box is closed
    std::function<void(int)> chosen;
};

// moves the box's selection for `action`, wrapping; the choice A (or B, for
// its cancel) picks, if one is
std::optional<int> StepBox(Box& box, Action action);

}

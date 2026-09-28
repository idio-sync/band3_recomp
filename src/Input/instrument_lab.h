#pragma once
#include <cstdint>
#include <rex/ui/imgui_dialog.h>

namespace band3::input {

// The Instrument Lab: plays the virtual instrument with the mouse and shows the
// exact report and capabilities it sends, to check how RB3 reads each
// instrument. Toggled with bind_instrument_lab (F6).
class InstrumentLabDialog : public rex::ui::ImGuiDialog {
public:
    explicit InstrumentLabDialog(rex::ui::ImGuiDrawer* imgui_drawer)
        : rex::ui::ImGuiDialog(imgui_drawer) {}

    void Toggle() { visible_ = !visible_; }

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    bool visible_ = false;
    int velocity_ = 100;
};

}

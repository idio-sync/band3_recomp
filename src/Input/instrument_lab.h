#pragma once
#include <cstdint>
#include <rex/ui/imgui_dialog.h>
#include "virtual_instrument.h"

namespace band3::input {

// The Instrument Lab (bind_instrument_lab, F6). Its tabs:
// - Virtual instrument: plays the virtual instrument with the mouse and shows
//   the exact report and capabilities it sends, to check how RB3 reads each
//   instrument
// - Connected instruments: each instrument the HID driver has open, its raw
//   reports next to what band3 sends, and a button to save a capture
// - MIDI drums: the MIDI port being played and what each recent note played
// - Pro instruments: per player, the instrument type RB3 sees and the Pro Keys /
//   Pro Guitar bytes band3 hands it
// - Microphones: what records each mic slot, how far the game has got with it,
//   and which mic the game gives each player
// - Lag: the extra lag the game builds in for each player's controller type
class InstrumentLabDialog : public rex::ui::ImGuiDialog {
public:
    explicit InstrumentLabDialog(rex::ui::ImGuiDrawer* imgui_drawer)
        : rex::ui::ImGuiDialog(imgui_drawer) {}

    void Toggle() { visible_ = !visible_; }

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    void DrawVirtualInstrument(InstrumentInputs& in);

    bool visible_ = false;
    int velocity_ = 100;
};

}

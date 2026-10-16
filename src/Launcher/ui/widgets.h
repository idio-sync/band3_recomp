#pragma once
#include <span>
#include <string>
#include <string_view>
#include <imgui.h>
#include "box.h"
#include "skin.h"
#include "theme.h"

// The redesigned launcher's pieces, drawn after Rock Band 3's menus: each
// with the game's texture when the skin has it and the theme's colours when
// it doesn't, so the layout is the same either way. Rectangles are in screen
// pixels; sizes inside them scale with the page (launcher_style.h's Px).
// They draw only: what's selected, and what the mouse does, is the screen's.

namespace band3::launcher::ui {

// the look of an item in a menu list
enum class ItemLook {
    kNormal,
    kSelected,  // on the yellow bar
    kMarked,    // the current one of a list without focus: a faint bar
    kFaint,     // a list without focus
    kOff,       // can't be picked now
};

enum class BandKind {
    kSection,  // a teal heading over a section's rows
    kAction,   // a graphite band that does something when picked (RB3's SETLISTS / BATTLES)
};

enum class ChipKind { kWarn, kGood, kInfo, kBad, kChanged };

// what a help bar item's button is, drawn as its fret colour's dash or, for
// keyboard players, the key that does it
// kBack: the controller's own Back (View) button, beside Start; not B
enum class HelpButton { kA, kB, kX, kY, kBumpers, kStart, kBack };
struct HelpItem {
    HelpButton button;
    std::string label;
};
// DrawHelpBar's answer when the mouse is over its PLAY button
inline constexpr int kHelpPlay = 1000;

// a settings row
struct RowLook {
    std::string_view label;
    std::string_view value;
    bool selected = false;
    // dimmed (a setting that doesn't apply now)
    bool dim = false;
    // locked: dimmed, and a grey bar rather than yellow when selected
    bool locked = false;
    // differs from its default: an orange dot
    bool changed = false;
    // a list value's arrows either side, while it's selected
    bool arrows = false;
    // the value is wrong (a port in use): red, with a red dash
    bool error = false;
    // an ON value, in RB3's yellow-green
    bool on = false;
    // the value in caps (ON and OFF, buttons)
    bool value_caps = false;
    // the row does something when picked (Open, Play, Apply), rather than
    // holding a value: its value is drawn as a button, framed, with a >
    bool action = false;
    // the value's colour off the selection bar, when not the usual (a status)
    ImU32 value_color = 0;
};

// the screen's background: navy, lighter towards the menu, the ornament's
// grain and the header's streaks when the skin has them, darker at the edges
void DrawBackground(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin);
// the background over the running game (the in-game settings): the game
// shows through, darkened and tinted navy, with the grain over it
void DrawGameDim(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin);
// a screen's big faded title, its top-left at `pos`
void DrawTitle(ImDrawList* dl, ImVec2 pos, std::string_view text, float size = text::kTitle);
// the grey strip under a title
void DrawStrip(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin);
// the yellow selection bar
void DrawHighlight(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin, float rounding = 0,
                   float alpha = 1);
// a menu item's caps with its top-left at `pos`, at least `width` wide;
// returns its rectangle's bottom-right
ImVec2 DrawMenuItem(ImDrawList* dl, ImVec2 pos, std::string_view text, float size, ItemLook look,
                    const Skin& skin, float width = 0);
// a section's heading or an action band, with optional caps at its right
void DrawBand(ImDrawList* dl, ImVec2 min, ImVec2 max, std::string_view text, BandKind kind,
              const Skin& skin, bool selected = false, std::string_view right = {}, bool dim = false);
void DrawRow(ImDrawList* dl, ImVec2 min, ImVec2 max, const RowLook& row, const Skin& skin);
// a framed panel (the detail panel, a box): the pause container's frame over
// a dark wallpaper
void DrawPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin);
// a chip of caps (a note under a description); returns its size
ImVec2 DrawChip(ImDrawList* dl, ImVec2 pos, std::string_view text, ChipKind kind, float max_width = 0);
// a key's name on a rounded cap `height` tall; returns its width
float DrawKeyCap(ImDrawList* dl, ImVec2 pos, std::string_view text, float height, bool dark = false);
// the fading rule under a heading
void DrawRule(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin);
// one of RB3's instrument icons (a 4x4 sheet: guitar, bass, drums, keys; the
// pro ones; vocals)
void DrawIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin, int column, int row);
// the help bar along the bottom: `items` in fixed slots, a PLAY button at the
// right when `play`. Returns the item the mouse is over (its index, or
// kHelpPlay), -1 for none.
int DrawHelpBar(ImDrawList* dl, ImVec2 min, ImVec2 max, std::span<const HelpItem> items, bool keyboard,
                bool play, const Skin& skin);
// a box (box.h) centred over `min`-`max`, the screen under it darkened;
// returns the choice the mouse is over, -1 for none
int DrawBox(ImDrawList* dl, ImVec2 min, ImVec2 max, const Box& box, const Skin& skin);
// a count in an orange pill (a category's settings changed), its right edge
// at `right`; returns its width
float DrawCount(ImDrawList* dl, float right, float center_y, int count);

}

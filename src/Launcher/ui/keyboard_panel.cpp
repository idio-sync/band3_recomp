#include "keyboard_panel.h"
#include <imgui_internal.h>
#include <cmath>
#include <string>
#include "src/Launcher/launcher_style.h"
#include "widgets.h"

namespace band3::launcher::ui {

namespace {

std::string Utf8(unsigned int c) {
    std::string out;
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
    return out;
}

bool Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

}

OnScreenKeyboard::Result DrawKeyboardPanel(ImDrawList* dl, const Page& page, OnScreenKeyboard& keyboard,
                                           const KeyboardPanel& look, const Skin& skin, bool clicks) {
    const ImGuiIO& io = ImGui::GetIO();
    float y = look.y;
    const float x = look.x, width = look.width;
    for (const std::string& line : WrapText(kCaps(text::kPanelHeading), look.heading, Px(width))) {
        PaintText(dl, page.At(x, y), kCaps(text::kPanelHeading), color::kText, line);
        y += text::kPanelHeading + 4;
    }
    DrawRule(dl, page.At(x, y + 6), page.At(x + width, y + 10), skin);
    y += 22;

    // the entry, its end in sight where the typing is
    const ImVec2 emin = page.At(x, y), emax = page.At(x + width, y + 40);
    dl->AddRectFilled(emin, emax, IM_COL32(0, 0, 0, 140));
    dl->AddLine(emin, ImVec2(emax.x, emin.y), IM_COL32(255, 255, 255, 178), Px(2));
    dl->AddLine(ImVec2(emin.x, emax.y), emax, IM_COL32(255, 255, 255, 178), Px(2));
    std::string shown = keyboard.Text();
    if (look.secret) {
        shown.clear();
        for (const char c : keyboard.Text()) {
            if (!Continuation(c)) shown += "\xE2\x80\xA2";
        }
    }
    const TextStyle entry = kPlain(22);
    while (!shown.empty() && TextWidth(entry, shown) > Px(width - 24)) {
        size_t cut = 1;
        while (cut < shown.size() && Continuation(shown[cut])) cut++;
        shown.erase(0, cut);
    }
    const float tw = PaintText(dl, page.At(x + 10, y + 9), entry, color::kText, shown);
    if (std::fmod(NowSeconds(), 1.0) < 0.6) {
        const ImVec2 c = page.At(x + 12, y + 9);
        dl->AddRectFilled(ImVec2(c.x + tw, c.y), ImVec2(c.x + tw + Px(2), c.y + Px(22)), color::kYellow);
    }
    y += 52;

    // the keys
    OnScreenKeyboard::Result result = OnScreenKeyboard::Result::kNone;
    const float gap = 2;
    const float key = (width - gap * (OnScreenKeyboard::kColumns - 1)) / OnScreenKeyboard::kColumns;
    const bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    for (int i = 0; i < keyboard.KeyCount(); i++) {
        const OnScreenKeyboard::KeyInfo k = keyboard.Key(i);
        const float kx = x + float(k.column) * (key + gap);
        const float ky = y + float(k.row) * (key + gap);
        const ImVec2 min = page.At(kx, ky);
        const ImVec2 max = page.At(kx + key * float(k.span) + gap * float(k.span - 1), ky + key);
        const bool selected = i == keyboard.Selected();
        if (selected) {
            DrawHighlight(dl, min, max, skin, Px(3));
        } else {
            dl->AddRectFilled(min, max, IM_COL32(255, 255, 255, 16), Px(3));
            dl->AddRect(min, max, IM_COL32(255, 255, 255, 30), Px(3), 0, std::max(1.0f, Px(1)));
        }
        std::string label = k.label;
        // the case key says what it switches to
        if (k.span > 1 && label == "abc/ABC") label = keyboard.Upper() ? "abc" : "ABC";
        const TextStyle style =
            k.span > 1 ? kCaps(text::kChip + 2) : (keyboard.Upper() ? kCaps(22) : kPlain(22));
        const float lw = TextWidth(style, label);
        const float lh = k.span > 1 ? text::kChip + 2 : 22;
        PaintText(dl, ImVec2((min.x + max.x - lw) * 0.5f, (min.y + max.y) * 0.5f - Px(lh) * 0.5f), style,
                  selected ? color::kOnYellow : color::kText, label);
        if (ImGui::IsMouseHoveringRect(min, max)) {
            if (moved) keyboard.Select(i);
            if (clicks && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) result = keyboard.Press(i);
        }
    }
    y += 8 * (key + gap) + 10;
    for (const std::string& line : WrapText(kPlain(text::kBody), look.hint, Px(width))) {
        PaintText(dl, page.At(x, y), kPlain(text::kBody), color::kMuted, line);
        y += text::kBodyLine;
    }
    return result;
}

OnScreenKeyboard::Result TypeIntoKeyboard(OnScreenKeyboard& keyboard, ImGuiIO& io, bool& edited) {
    // the SDK starts the system's text input for the frame that asks for it
    ImGui::GetCurrentContext()->PlatformImeData.WantTextInput = true;
    edited = false;
    for (const ImWchar c : io.InputQueueCharacters) {
        if (c >= 0x20 && c != 0x7F) {
            keyboard.Type(Utf8(c));
            edited = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
        keyboard.Backspace();
        edited = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        return OnScreenKeyboard::Result::kDone;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return OnScreenKeyboard::Result::kCancelled;
    return OnScreenKeyboard::Result::kNone;
}

}

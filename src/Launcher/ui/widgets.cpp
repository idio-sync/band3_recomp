#include "widgets.h"
#include <algorithm>
#include <cmath>
#include "src/Launcher/launcher_style.h"

namespace band3::launcher::ui {

namespace {

ImU32 WithAlpha(ImU32 color, float alpha) {
    const ImU32 a = ImU32(std::clamp(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha, 0.0f, 255.0f));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// a horizontal gradient, left to right
void Gradient(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 left, ImU32 right) {
    dl->AddRectFilledMultiColor(min, max, left, right, right, left);
}

// a vertical gradient, top to bottom
void VGradient(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 top, ImU32 bottom) {
    dl->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
}

// a texture's border drawn around a rectangle, its middle left out: the outer
// `slice` texels of each side stretched to `border` pixels
void NineSliceBorder(ImDrawList* dl, ImTextureRef tex, ImVec2 tex_size, ImVec2 min, ImVec2 max,
                     float slice, float border, ImU32 tint) {
    const float xs[4] = {min.x, min.x + border, max.x - border, max.x};
    const float ys[4] = {min.y, min.y + border, max.y - border, max.y};
    const float us[4] = {0, slice / tex_size.x, 1 - slice / tex_size.x, 1};
    const float vs[4] = {0, slice / tex_size.y, 1 - slice / tex_size.y, 1};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            if (row == 1 && col == 1) continue;
            dl->AddImage(tex, ImVec2(xs[col], ys[row]), ImVec2(xs[col + 1], ys[row + 1]),
                         ImVec2(us[col], vs[row]), ImVec2(us[col + 1], vs[row + 1]), tint);
        }
    }
}

ImU32 ChipColor(ChipKind kind) {
    switch (kind) {
    case ChipKind::kGood: return color::kGood;
    case ChipKind::kInfo: return color::kInfo;
    case ChipKind::kBad: return color::kBad;
    case ChipKind::kChanged: return color::kChanged;
    case ChipKind::kWarn: break;
    }
    return color::kWarn;
}

ImU32 DashColor(HelpButton button) {
    switch (button) {
    case HelpButton::kA: return color::kFretGreen;
    case HelpButton::kB: return color::kFretRed;
    case HelpButton::kX: return color::kFretBlue;
    case HelpButton::kY: return color::kFretYellow;
    case HelpButton::kBumpers:
    case HelpButton::kStart: break;
    }
    return color::kFretOrange;
}

const char* KeyName(HelpButton button) {
    switch (button) {
    case HelpButton::kA: return "Enter";
    case HelpButton::kB: return "Esc";
    case HelpButton::kX: return "/";
    case HelpButton::kY: return "R";
    case HelpButton::kBumpers: return "Tab";
    case HelpButton::kStart: break;
    }
    return "F5";
}

}

void DrawBackground(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin) {
    const ImVec2 size(max.x - min.x, max.y - min.y);
    dl->AddRectFilled(min, max, color::kNavyDeep);
    VGradient(dl, min, max, color::kNavy, color::kNavyDeep);
    // a light behind the menu, and a wider, fainter one over the page
    if (skin.HasGlow()) {
        dl->AddImage(skin.Glow(), ImVec2(min.x - size.x * 0.25f, min.y - size.y * 0.25f),
                     ImVec2(min.x + size.x * 0.95f, min.y + size.y * 1.05f), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(20, 34, 110, 140));
        dl->AddImage(skin.Glow(), ImVec2(min.x - size.x * 0.12f, min.y + size.y * 0.05f),
                     ImVec2(min.x + size.x * 0.52f, min.y + size.y * 0.92f), ImVec2(0, 0), ImVec2(1, 1),
                     color::kNavyGlow);
    }
    if (skin.Has(SkinPart::kGrain)) {
        const float tile = Px(384);
        dl->AddImage(skin.Texture(SkinPart::kGrain), min, max, ImVec2(0, 0),
                     ImVec2(size.x / tile, size.y / tile), IM_COL32(110, 130, 230, 34));
    }
    if (skin.Has(SkinPart::kStreaks)) {
        const ImTextureRef streaks = skin.Texture(SkinPart::kStreaks);
        const float top = Px(300), bottom = Px(180);
        dl->AddImage(streaks, ImVec2(min.x - size.x * 0.1f, min.y - Px(30)),
                     ImVec2(max.x + size.x * 0.1f, min.y - Px(30) + top), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, 96));
        // fainter and mirrored, low on the page
        dl->AddImage(streaks, ImVec2(min.x - size.x * 0.1f, max.y - Px(40) - bottom),
                     ImVec2(max.x + size.x * 0.1f, max.y - Px(40)), ImVec2(1, 0), ImVec2(0, 1),
                     IM_COL32(255, 255, 255, 50));
    }
    // darker towards the edges
    const ImU32 dark = IM_COL32(0, 0, 0, 170), clear = IM_COL32(0, 0, 0, 0);
    const float edge = std::min(size.x, size.y) * 0.22f;
    VGradient(dl, min, ImVec2(max.x, min.y + edge), dark, clear);
    VGradient(dl, ImVec2(min.x, max.y - edge), max, clear, dark);
    Gradient(dl, min, ImVec2(min.x + edge, max.y), dark, clear);
    Gradient(dl, ImVec2(max.x - edge, min.y), max, clear, dark);
}

void DrawGameDim(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin) {
    const ImVec2 size(max.x - min.x, max.y - min.y);
    VGradient(dl, min, max, IM_COL32(4, 8, 34, 234), IM_COL32(2, 4, 20, 246));
    if (skin.Has(SkinPart::kGrain)) {
        const float tile = Px(384);
        dl->AddImage(skin.Texture(SkinPart::kGrain), min, max, ImVec2(0, 0),
                     ImVec2(size.x / tile, size.y / tile), IM_COL32(110, 130, 230, 26));
    }
}

void DrawTitle(ImDrawList* dl, ImVec2 pos, std::string_view text, float size) {
    PaintText(dl, pos, kCaps(size), color::kTitle, text);
}

void DrawStrip(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin) {
    if (skin.Has(SkinPart::kStrip)) {
        dl->AddImage(skin.Texture(SkinPart::kStrip), min, max, ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(140, 140, 140, 255));
    } else {
        Gradient(dl, min, max, IM_COL32(52, 52, 56, 255), IM_COL32(108, 108, 112, 255));
    }
    // darker at its left, as the Music Library's
    Gradient(dl, min, ImVec2(min.x + (max.x - min.x) * 0.85f, max.y), IM_COL32(0, 0, 0, 64),
             IM_COL32(0, 0, 0, 0));
    dl->AddLine(ImVec2(min.x, max.y), max, IM_COL32(0, 0, 0, 140), std::max(1.0f, Px(1)));
}

void DrawHighlight(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin, float rounding,
                   float alpha) {
    if (skin.Has(SkinPart::kHighlight)) {
        dl->AddImageRounded(skin.Texture(SkinPart::kHighlight), min, max, ImVec2(0, 0), ImVec2(1, 1),
                            WithAlpha(IM_COL32_WHITE, alpha), rounding);
        return;
    }
    // the bar's gloss: darker olive ends, bright yellow middle
    const float mid = min.x + (max.x - min.x) * 0.55f;
    Gradient(dl, min, ImVec2(mid, max.y), WithAlpha(IM_COL32(150, 146, 18, 255), alpha),
             WithAlpha(IM_COL32(232, 224, 31, 255), alpha));
    Gradient(dl, ImVec2(mid, min.y), max, WithAlpha(IM_COL32(232, 224, 31, 255), alpha),
             WithAlpha(IM_COL32(128, 124, 16, 255), alpha));
    dl->AddRectFilled(min, ImVec2(max.x, min.y + (max.y - min.y) * 0.32f),
                      WithAlpha(IM_COL32(255, 255, 255, 34), alpha));
}

ImVec2 DrawMenuItem(ImDrawList* dl, ImVec2 pos, std::string_view text, float size, ItemLook look,
                    const Skin& skin, float width) {
    const TextStyle style = kCaps(size);
    const float w = std::max(width, TextWidth(style, text));
    const ImVec2 max(pos.x + w, pos.y + Px(size));
    ImU32 col = color::kText;
    if (look == ItemLook::kSelected) {
        DrawHighlight(dl, ImVec2(pos.x - Px(14), pos.y), ImVec2(max.x + Px(14), max.y), skin, Px(6));
        col = color::kOnYellow;
    } else if (look == ItemLook::kMarked) {
        DrawHighlight(dl, ImVec2(pos.x - Px(14), pos.y), ImVec2(max.x + Px(4), max.y), skin, Px(4), 0.28f);
    } else if (look == ItemLook::kFaint) {
        col = WithAlpha(color::kText, 0.55f);
    } else if (look == ItemLook::kOff) {
        col = WithAlpha(color::kText, 0.38f);
    }
    PaintText(dl, pos, style, col, text, width > 0 ? width : 0);
    return max;
}

void DrawBand(ImDrawList* dl, ImVec2 min, ImVec2 max, std::string_view text, BandKind kind,
              const Skin& skin, bool selected, std::string_view right) {
    const float w = max.x - min.x;
    ImU32 col = IM_COL32(238, 246, 248, 255);
    if (selected) {
        DrawHighlight(dl, min, max, skin);
        dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(255, 255, 255, 190), Px(2));
        dl->AddLine(ImVec2(min.x, max.y - Px(1)), ImVec2(max.x, max.y - Px(1)), IM_COL32(255, 255, 255, 190),
                    Px(2));
        col = color::kOnYellow;
    } else if (kind == BandKind::kAction) {
        // graphite, not RB3's red: red is Back's and the errors' colour here
        Gradient(dl, min, ImVec2(min.x + w * 0.7f, max.y), IM_COL32(64, 70, 92, 210),
                 IM_COL32(64, 70, 92, 120));
        Gradient(dl, ImVec2(min.x + w * 0.7f, min.y), max, IM_COL32(64, 70, 92, 120),
                 IM_COL32(64, 70, 92, 50));
        dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(170, 178, 204, 110), std::max(1.0f, Px(1)));
    } else {
        Gradient(dl, min, ImVec2(min.x + w * 0.7f, max.y), color::kTeal, IM_COL32(70, 150, 165, 90));
        Gradient(dl, ImVec2(min.x + w * 0.7f, min.y), max, IM_COL32(70, 150, 165, 90),
                 IM_COL32(70, 150, 165, 38));
        dl->AddLine(min, ImVec2(max.x, min.y), color::kTealEdge, std::max(1.0f, Px(1)));
    }
    const float text_y = min.y + ((max.y - min.y) - Px(text::kBand)) * 0.5f;
    float right_w = 0;
    if (!right.empty()) {
        const TextStyle right_style = kCaps(text::kChip + 2);
        right_w = TextWidth(right_style, right);
        PaintText(dl, ImVec2(max.x - Px(14) - right_w, min.y + ((max.y - min.y) - Px(text::kChip + 2)) * 0.5f),
                 right_style, selected ? color::kOnYellow : IM_COL32(230, 240, 245, 190), right);
    }
    PaintText(dl, ImVec2(min.x + Px(14), text_y), kCaps(text::kBand), col, text,
             w - Px(28) - (right_w > 0 ? right_w + Px(16) : 0));
}

void DrawRow(ImDrawList* dl, ImVec2 min, ImVec2 max, const RowLook& row, const Skin& skin) {
    const float h = max.y - min.y;
    if (row.selected && row.locked) {
        Gradient(dl, min, max, IM_COL32(150, 150, 160, 140), IM_COL32(110, 110, 120, 102));
        dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(255, 255, 255, 115), Px(2));
        dl->AddLine(ImVec2(min.x, max.y - Px(1)), ImVec2(max.x, max.y - Px(1)), IM_COL32(255, 255, 255, 115),
                    Px(2));
    } else if (row.selected) {
        DrawHighlight(dl, min, max, skin);
        dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(255, 255, 255, 190), Px(2));
        dl->AddLine(ImVec2(min.x, max.y - Px(1)), ImVec2(max.x, max.y - Px(1)), IM_COL32(255, 255, 255, 190),
                    Px(2));
    } else {
        Gradient(dl, min, max, IM_COL32(0, 0, 0, 107), IM_COL32(0, 0, 0, 64));
        dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(255, 255, 255, 10), std::max(1.0f, Px(1)));
    }
    const bool on_yellow = row.selected && !row.locked;
    const float fade = row.dim || row.locked ? 0.42f : 1.0f;
    const ImU32 label_col = WithAlpha(on_yellow ? color::kOnYellow : IM_COL32(244, 244, 244, 255),
                                      row.selected && row.locked ? 0.8f : fade);

    // the value at the right, with arrows either side when it's a list being changed
    const TextStyle value_style = row.value_caps ? kCaps(text::kValue) : kPlain(text::kValue);
    ImU32 value_col = color::kSoft;
    if (row.error) value_col = color::kBad;
    else if (row.on) value_col = color::kOn;
    else if (row.value_color) value_col = row.value_color;
    if (on_yellow) value_col = color::kOnYellow;
    value_col = WithAlpha(value_col, row.selected && row.locked ? 0.8f : fade);
    const float arrow = Px(10);
    const float right = max.x - (row.arrows ? Px(22) + arrow + Px(8) : Px(22));
    const float max_value = (max.x - min.x) * 0.48f;
    const float value_w = std::min(TextWidth(value_style, row.value), max_value);
    const float value_x = right - value_w;
    const float value_y = min.y + (h - Px(text::kValue)) * 0.5f;
    PaintText(dl, ImVec2(value_x, value_y), value_style, value_col, row.value, max_value);
    if (row.arrows) {
        const float cy = min.y + h * 0.5f, half = Px(8);
        const ImU32 a = color::kOnYellow;
        const float lx = value_x - Px(8);
        dl->AddTriangleFilled(ImVec2(lx - arrow, cy), ImVec2(lx, cy - half), ImVec2(lx, cy + half), a);
        const float rx = right + Px(8);
        dl->AddTriangleFilled(ImVec2(rx + arrow, cy), ImVec2(rx, cy - half), ImVec2(rx, cy + half), a);
    }
    if (row.error) {
        const ImVec2 dash(value_x - Px(22), min.y + h * 0.5f - Px(3));
        dl->AddRectFilled(dash, ImVec2(dash.x + Px(14), dash.y + Px(6)),
                          on_yellow ? IM_COL32(107, 16, 16, 255) : color::kBad);
    }
    if (row.changed) {
        dl->AddCircleFilled(ImVec2(max.x - Px(9), min.y + h * 0.5f), Px(3),
                            on_yellow ? IM_COL32(90, 50, 5, 255) : color::kChanged);
    }
    const float label_room = value_x - min.x - Px(20) - Px(24);
    PaintText(dl, ImVec2(min.x + Px(20), min.y + (h - Px(text::kRow)) * 0.5f), kPlain(text::kRow),
             label_col, row.label, std::max(label_room, Px(60)));
}

void DrawPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin) {
    const float inset = Px(9);
    const ImVec2 fmin(min.x + inset, min.y + inset), fmax(max.x - inset, max.y - inset);
    if (skin.Has(SkinPart::kFill)) {
        const float tile = Px(256);
        dl->AddImage(skin.Texture(SkinPart::kFill), fmin, fmax, ImVec2(0, 0),
                     ImVec2((fmax.x - fmin.x) / tile, (fmax.y - fmin.y) / tile));
    }
    VGradient(dl, fmin, fmax, IM_COL32(4, 8, 22, 204), IM_COL32(2, 4, 14, 224));
    if (skin.Has(SkinPart::kFrame)) {
        // the container's grey frame, tinted the overshell's blue
        NineSliceBorder(dl, skin.Texture(SkinPart::kFrame), skin.Size(SkinPart::kFrame), min, max,
                        skin.Size(SkinPart::kFrame).x * (52.0f / 512.0f), Px(26),
                        IM_COL32(120, 175, 255, 255));
    } else {
        dl->AddRect(ImVec2(min.x + Px(4), min.y + Px(4)), ImVec2(max.x - Px(4), max.y - Px(4)),
                    color::kFrameBlue, Px(3), 0, Px(2));
        dl->AddRect(ImVec2(min.x + Px(9), min.y + Px(9)), ImVec2(max.x - Px(9), max.y - Px(9)),
                    IM_COL32(90, 150, 220, 90), Px(2), 0, std::max(1.0f, Px(1)));
    }
}

ImVec2 DrawChip(ImDrawList* dl, ImVec2 pos, std::string_view text, ChipKind kind, float max_width) {
    const TextStyle style = kCaps(text::kChip);
    const ImU32 col = ChipColor(kind);
    const float pad_x = Px(9), pad_y = Px(4);
    float w = TextWidth(style, text);
    if (max_width > 0) w = std::min(w, max_width - pad_x * 2);
    const ImVec2 max(pos.x + w + pad_x * 2, pos.y + Px(text::kChip) + pad_y * 2);
    dl->AddRectFilled(pos, max, WithAlpha(col, 0.14f), Px(3));
    dl->AddRect(pos, max, WithAlpha(col, 0.48f), Px(3), 0, std::max(1.0f, Px(1)));
    PaintText(dl, ImVec2(pos.x + pad_x, pos.y + pad_y), style, col, text, w + 0.5f);
    return ImVec2(max.x - pos.x, max.y - pos.y);
}

float DrawKeyCap(ImDrawList* dl, ImVec2 pos, std::string_view text, float height, bool dark) {
    const TextStyle style = kCaps(text::kKey);
    const float w = std::max(height, TextWidth(style, text) + Px(12));
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + height),
                      dark ? IM_COL32(23, 23, 10, 217) : IM_COL32(226, 226, 226, 255), height * 0.5f);
    const float tw = TextWidth(style, text);
    PaintText(dl, ImVec2(pos.x + (w - tw) * 0.5f, pos.y + (height - Px(text::kKey)) * 0.5f), style,
             dark ? IM_COL32(243, 239, 154, 255) : IM_COL32(21, 21, 21, 255), text);
    return w;
}

void DrawRule(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin) {
    if (skin.Has(SkinPart::kLineFade)) {
        dl->AddImage(skin.Texture(SkinPart::kLineFade), min, max, ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, 128));
        return;
    }
    const float y = (min.y + max.y) * 0.5f;
    Gradient(dl, ImVec2(min.x, y - Px(1.5f)), ImVec2(max.x, y + Px(1.5f)), IM_COL32(255, 255, 255, 0),
             IM_COL32(220, 220, 225, 150));
}

void DrawIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, const Skin& skin, int column, int row) {
    if (!skin.Has(SkinPart::kIcons)) {
        dl->AddCircle(ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), (max.x - min.x) * 0.42f,
                      color::kSoft, 0, Px(2));
        return;
    }
    dl->AddImage(skin.Texture(SkinPart::kIcons), min, max, ImVec2(column / 4.0f, row / 4.0f),
                 ImVec2((column + 1) / 4.0f, (row + 1) / 4.0f));
}

int DrawHelpBar(ImDrawList* dl, ImVec2 min, ImVec2 max, std::span<const HelpItem> items, bool keyboard,
                bool play, const Skin& skin) {
    VGradient(dl, min, max, IM_COL32(6, 8, 28, 209), IM_COL32(4, 5, 18, 242));
    DrawRule(dl, ImVec2(min.x, min.y - Px(4)), ImVec2(max.x, min.y + Px(4)), skin);

    const TextStyle label = kCaps(text::kHelp);
    const float h = max.y - min.y;
    const float cy = min.y + h * 0.5f;
    // fixed slots, as the overshell's, as wide as the widest item needs
    float slot = Px(196);
    for (const HelpItem& item : items) {
        const float mark = keyboard ? TextWidth(kCaps(text::kKey), KeyName(item.button)) + Px(24) : Px(18);
        slot = std::max(slot, mark + Px(10) + TextWidth(label, item.label) + Px(36));
    }
    int hovered = -1;
    for (size_t i = 0; i < items.size(); i++) {
        const HelpItem& item = items[i];
        float x = min.x + Px(56) + slot * float(i);
        const float start = x;
        if (keyboard) {
            x += DrawKeyCap(dl, ImVec2(x, cy - Px(11)), KeyName(item.button), Px(22));
        } else {
            dl->AddRectFilled(ImVec2(x, cy - Px(3.5f)), ImVec2(x + Px(18), cy + Px(3.5f)),
                              DashColor(item.button), Px(1));
            x += Px(18);
        }
        x += Px(10);
        x += PaintText(dl, ImVec2(x, cy - Px(text::kHelp) * 0.5f), label, color::kText, item.label);
        if (ImGui::IsMouseHoveringRect(ImVec2(start - Px(6), min.y), ImVec2(x + Px(6), max.y))) {
            hovered = int(i);
        }
    }
    if (play) {
        const TextStyle play_style = kCaps(25);
        const char* key = keyboard ? "F5" : "Start";
        const float key_w = std::max(Px(22), TextWidth(kCaps(text::kKey), key) + Px(12));
        const float w = Px(20) + Px(13) + Px(10) + TextWidth(play_style, "Play") + Px(10) + key_w + Px(16);
        const ImVec2 bmin(max.x - Px(28) - w, cy - Px(19)), bmax(max.x - Px(28), cy + Px(19));
        DrawHighlight(dl, bmin, bmax, skin, Px(5));
        float x = bmin.x + Px(20);
        dl->AddTriangleFilled(ImVec2(x, cy - Px(8)), ImVec2(x + Px(13), cy), ImVec2(x, cy + Px(8)),
                              color::kOnYellow);
        x += Px(13) + Px(10);
        x += PaintText(dl, ImVec2(x, cy - Px(25) * 0.5f), play_style, color::kOnYellow, "Play") + Px(10);
        DrawKeyCap(dl, ImVec2(x, cy - Px(11)), key, Px(22), true);
        if (ImGui::IsMouseHoveringRect(bmin, bmax)) hovered = kHelpPlay;
    }
    return hovered;
}

int DrawBox(ImDrawList* dl, ImVec2 min, ImVec2 max, const Box& box, const Skin& skin) {
    dl->AddRectFilled(min, max, IM_COL32(0, 0, 0, 150));
    const TextStyle title = kCaps(text::kBoxTitle);
    const TextStyle body = kPlain(text::kBody);
    const TextStyle option = kCaps(28);
    const float w = Px(560), inner = w - Px(70);
    std::vector<std::string> title_lines = WrapText(title, box.title, inner);
    std::vector<std::string> body_lines;
    for (const std::string& line : box.lines) {
        for (std::string& wrapped : WrapText(body, line, inner)) body_lines.push_back(std::move(wrapped));
    }
    const float title_h = Px(text::kBoxTitle + 2) * float(title_lines.size());
    const float body_h = Px(text::kBodyLine) * float(body_lines.size());
    const float h = Px(34) + title_h + (body_lines.empty() ? 0 : Px(10) + body_h) + Px(20) +
                    Px(42) * float(box.options.size()) + Px(24);
    const ImVec2 bmin(std::round((min.x + max.x - w) * 0.5f),
                      std::round(std::max(min.y + Px(30), (min.y + max.y - h) * 0.5f)));
    const ImVec2 bmax(bmin.x + w, bmin.y + h);
    DrawPanel(dl, bmin, bmax, skin);

    const float cx = (bmin.x + bmax.x) * 0.5f;
    float y = bmin.y + Px(34);
    for (const std::string& line : title_lines) {
        PaintText(dl, ImVec2(cx - TextWidth(title, line) * 0.5f, y), title, color::kText, line);
        y += Px(text::kBoxTitle + 2);
    }
    if (!body_lines.empty()) {
        y += Px(10);
        for (const std::string& line : body_lines) {
            PaintText(dl, ImVec2(cx - TextWidth(body, line) * 0.5f, y), body, color::kBody,
                      line);
            y += Px(text::kBodyLine);
        }
    }
    y += Px(20);
    int hovered = -1;
    for (size_t i = 0; i < box.options.size(); i++) {
        const ImVec2 omin(bmin.x + Px(30), y), omax(bmax.x - Px(30), y + Px(40));
        const bool selected = int(i) == box.selected;
        if (selected) DrawHighlight(dl, omin, omax, skin, Px(3));
        const std::string& label = box.options[i];
        PaintText(dl, ImVec2(cx - std::min(TextWidth(option, label), inner) * 0.5f, y + Px(6)), option,
                  selected ? color::kOnYellow : color::kText, label, inner);
        if (ImGui::IsMouseHoveringRect(omin, omax)) hovered = int(i);
        y += Px(42);
    }
    return hovered;
}

float DrawCount(ImDrawList* dl, float right, float center_y, int count) {
    const std::string text = std::to_string(count);
    const TextStyle style = kCaps(text::kKey);
    const float h = Px(20), w = std::max(h, TextWidth(style, text) + Px(10));
    const ImVec2 min(right - w, center_y - h * 0.5f), max(right, center_y + h * 0.5f);
    dl->AddRectFilled(min, max, color::kChanged, h * 0.5f);
    PaintText(dl, ImVec2(min.x + (w - TextWidth(style, text)) * 0.5f, center_y - Px(text::kKey) * 0.5f),
              style, IM_COL32(30, 16, 2, 255), text);
    return w;
}

}

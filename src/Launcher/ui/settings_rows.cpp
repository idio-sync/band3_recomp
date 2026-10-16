#include "settings_rows.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include "src/Audio/usb_mic.h"
#include "src/Input/midi_keys.h"
#include "src/Launcher/device_names.h"
#include "src/paths.h"

namespace band3::launcher::ui {

namespace {

std::string Number(double v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", v);
    return buffer;
}

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

Row ControlRow(Control control, std::string label) {
    return Row{.kind = RowKind::kControl, .control = control, .label = std::move(label)};
}

ValueType TypeOf(const SettingsModel& model, std::string_view cvar) {
    const CvarFacts* facts = model.Facts(cvar);
    return facts ? facts->type : ValueType::kString;
}

// the setting's saved slots, as many as there are singers
std::vector<std::string> MicSlots(const SettingsModel& model, const Setting& s) {
    std::vector<std::string> slots = audio::usb_mic::ParseDeviceList(model.Value(s.cvar));
    slots.resize(audio::usb_mic::kSingers);
    return slots;
}

// a window size as the SDK reads resolution: a preset, or WxH
std::optional<std::pair<int, int>> SizeOf(std::string_view value) {
    constexpr std::pair<std::string_view, std::pair<int, int>> kPresets[] = {
        {"720p", {1280, 720}}, {"1080p", {1920, 1080}}, {"1440p", {2560, 1440}},
        {"4k", {3840, 2160}},  {"2160p", {3840, 2160}},
    };
    const std::string lower = Lower(value);
    for (const auto& [name, size] : kPresets) {
        if (lower == name) return size;
    }
    const size_t x = lower.find('x');
    if (x == std::string::npos) return std::nullopt;
    int w = 0, h = 0;
    const char* end = lower.data() + lower.size();
    const auto [wend, wec] = std::from_chars(lower.data(), lower.data() + x, w);
    const auto [hend, hec] = std::from_chars(lower.data() + x + 1, end, h);
    if (wec != std::errc() || hec != std::errc() || wend != lower.data() + x || hend != end ||
        w <= 0 || h <= 0) {
        return std::nullopt;
    }
    return std::pair<int, int>(w, h);
}

std::string SizeLabel(int w, int h) { return std::to_string(w) + " x " + std::to_string(h); }

const char* WindowModeValue(WindowMode mode) {
    switch (mode) {
    case WindowMode::kWindowed: return "windowed";
    case WindowMode::kBorderless: return "borderless";
    case WindowMode::kExclusive: return "exclusive";
    }
    return "windowed";
}

// the MIDI port a driver opens with its setting empty, for "First port (...)"
std::string FirstPortLabel(const SettingsModel& model, const Setting& s, const DeviceLists& lists) {
    std::vector<std::string> ports;
    for (const MidiPort& p : lists.midi_ports) ports.push_back(p.port);
    // the kit's driver starts first, so the keyboard's passes over the kit's port
    const bool keys = s.cvar == "midi_keys_device";
    const std::string drums_saved = model.Value("midi_drums_device");
    const MidiDriverSetting drivers[] = {
        {keys ? AsBool(model.Value("midi_drums")) : true,
         keys ? std::string_view(drums_saved) : std::string_view()},
        {true, ""},
    };
    std::string label = "First port";
    if (const auto first = PickMidiPorts(ports, drivers)[keys ? 1 : 0]) {
        label += " (" + lists.midi_ports[*first].name + ")";
    }
    return label;
}

// the monitor whose sizes the window size row lists: the chosen one, or the
// one the window is on for the default
const Monitor* SizesMonitor(const SettingsModel& model, const DeviceLists& lists) {
    if (lists.monitors.empty()) return nullptr;
    const int chosen = static_cast<int>(AsInt(model.Value("monitor")).value_or(0));
    if (chosen >= 1 && static_cast<size_t>(chosen) <= lists.monitors.size()) {
        return &lists.monitors[static_cast<size_t>(chosen - 1)];
    }
    const Monitor* monitor = nullptr;
    for (const Monitor& m : lists.monitors) {
        if (m.has_window || (!monitor && m.primary)) monitor = &m;
    }
    return monitor ? monitor : &lists.monitors.front();
}

std::vector<ChoiceItem> SettingChoices(const SettingsModel& model, const Setting& s,
                                       const DeviceLists& lists) {
    const std::string value = model.Value(s.cvar);
    const std::string fallback = model.EffectiveDefault(s.cvar);
    const ValueType type = TypeOf(model, s.cvar);
    std::vector<ChoiceItem> out;
    auto add = [&](std::string label, std::string v) {
        ChoiceItem item{.label = std::move(label), .value = std::move(v)};
        item.current = SameValue(type, value, item.value);
        item.is_default = SameValue(type, fallback, item.value);
        out.push_back(std::move(item));
    };
    // a value none of the choices is: itself, last
    auto add_typed = [&](std::string label) {
        if (value.empty() || std::ranges::any_of(out, &ChoiceItem::current)) return;
        out.push_back({.label = std::move(label), .value = value, .current = true});
    };

    switch (s.widget) {
    case Widget::kCheckbox:
        add("Off", "false");
        add("On", "true");
        break;
    case Widget::kCombo:
    case Widget::kComboText:
        for (const Choice& c : s.choices) {
            if (model.Offered(c) || SameValue(type, value, c.value)) {
                add(std::string(c.label), std::string(c.value));
            }
        }
        add_typed(value);
        break;
    case Widget::kMonitor:
        if (lists.monitors.empty()) {
            for (const Choice& c : s.choices) add(std::string(c.label), std::string(c.value));
        } else {
            add("Default", "0");
            for (size_t i = 0; i < lists.monitors.size(); i++) {
                const Monitor& m = lists.monitors[i];
                std::string label = m.name.empty() ? "Monitor " + std::to_string(i + 1) : m.name;
                if (m.primary) label += " (primary)";
                add(std::move(label), std::to_string(i + 1));
            }
        }
        add_typed("Monitor " + value + " (not connected)");
        break;
    case Widget::kResolution: {
        const auto size = SizeOf(value);
        // by size, however the value is written (1080p or 1920x1080)
        auto add_size = [&](int w, int h, std::string label, std::string v) {
            const bool listed = std::ranges::any_of(out, [&](const ChoiceItem& item) {
                const auto other = SizeOf(item.value);
                return other && other->first == w && other->second == h;
            });
            if (listed) return;
            ChoiceItem item{.label = std::move(label), .value = std::move(v)};
            item.current = size && size->first == w && size->second == h;
            out.push_back(std::move(item));
        };
        out.push_back({.label = "Default", .value = "", .current = value.empty(),
                       .is_default = true});
        if (const Monitor* monitor = SizesMonitor(model, lists)) {
            for (const DisplayMode& mode : monitor->modes) {
                const bool desktop = mode.width == monitor->current.width &&
                                     mode.height == monitor->current.height;
                add_size(mode.width, mode.height,
                         SizeLabel(mode.width, mode.height) + (desktop ? " (desktop)" : ""),
                         std::to_string(mode.width) + "x" + std::to_string(mode.height));
            }
        }
        for (const Choice& c : s.choices) {
            if (const auto preset = SizeOf(c.value)) {
                add_size(preset->first, preset->second, std::string(c.label), std::string(c.value));
            }
        }
        if (size) add_size(size->first, size->second, SizeLabel(size->first, size->second), value);
        add_typed(value);
        break;
    }
    case Widget::kMidiPort: {
        add(FirstPortLabel(model, s, lists), "");
        std::vector<std::string> names;
        for (const MidiPort& p : lists.midi_ports) names.push_back(p.name);
        const auto saved = value.empty() ? std::nullopt : FindSavedDevice(names, value);
        for (size_t i = 0; i < names.size(); i++) {
            ChoiceItem item{.label = names[i], .value = names[i]};
            item.current = saved == i;
            out.push_back(std::move(item));
        }
        add_typed(value + " (not connected)");
        break;
    }
    case Widget::kWindowMode: {
        const WindowMode mode = model.GetWindowMode();
        for (const auto& [m, label] : {std::pair{WindowMode::kWindowed, "Windowed"},
                                       std::pair{WindowMode::kBorderless, "Borderless"},
                                       std::pair{WindowMode::kExclusive, "Exclusive"}}) {
            out.push_back({.label = label, .value = WindowModeValue(m), .current = m == mode});
        }
        break;
    }
    default: break;
    }
    return out;
}

std::string SettingValue(const SettingsModel& model, const Setting& s, const DeviceLists& lists) {
    const std::string value = model.Value(s.cvar);
    const std::string unit = s.unit.empty() ? std::string() : " " + std::string(s.unit);
    auto chosen = [&]() -> std::optional<std::string> {
        for (const ChoiceItem& c : SettingChoices(model, s, lists)) {
            if (c.current) return c.label;
        }
        return std::nullopt;
    };
    switch (s.widget) {
    case Widget::kCheckbox: return AsBool(value) ? "On" : "Off";
    case Widget::kCombo:
    case Widget::kComboText:
    case Widget::kMonitor:
    case Widget::kResolution:
    case Widget::kMidiPort:
    case Widget::kWindowMode: return chosen().value_or(value);
    case Widget::kIntStepper:
    case Widget::kIntSlider: {
        const int index = model.ChoiceIndex(s);
        if (index >= 0) return std::string(s.choices[static_cast<size_t>(index)].label);
        return value + unit;
    }
    case Widget::kMidiNote: {
        const int note = static_cast<int>(AsInt(value).value_or(0));
        return std::to_string(note) + " (" + input::midi_keys::NoteName(note) + ")";
    }
    case Widget::kFloatSlider: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.2f", AsFloat(value).value_or(0));
        // "1.25x" reads as a speed; other units stand apart
        return std::string(buffer) + (s.unit == "x" ? std::string("x") : unit);
    }
    case Widget::kFloatInput: return Number(AsFloat(value).value_or(0)) + unit;
    case Widget::kPercentSlider:
        return std::to_string(std::lround(AsFloat(value).value_or(0) * 100)) + "%";
    case Widget::kText: return value.empty() ? "Not set" : value;
    case Widget::kPassword: return value.empty() ? "Not set" : "Set";
    case Widget::kPath: {
        if (!value.empty()) return value;
        // the folder the default is, where it's known
        const std::string folder = model.EffectiveDefault(s.cvar);
        return folder.empty() ? "Default" : folder;
    }
    case Widget::kFolderList: {
        const auto folders = paths::SplitList(value);
        if (folders.empty()) return "None";
        if (folders.size() == 1) return folders.front();
        return std::to_string(folders.size()) + " folders";
    }
    case Widget::kMicSlots:
    case Widget::kJoypadLag:
    case Widget::kNone: break;
    }
    return value;
}

bool Matches(const SettingsModel& model, const Row& row, std::string_view section,
             const std::string& query) {
    std::string text = row.label + "\n" + std::string(section);
    if (row.setting) {
        // the plain description; the registry's, technical and long, only
        // for a row without one (Advanced's)
        const std::string_view about = row.setting->about.empty()
                                           ? model.Description(row.setting->cvar)
                                           : row.setting->about;
        text += "\n" + std::string(row.setting->label) + "\n" + std::string(about) + "\n" +
                std::string(row.setting->cvar);
    }
    return Lower(text).find(query) != std::string::npos;
}

}

std::vector<Section> CategoryRows(const SettingsModel& model, Category category,
                                  const RowOptions& options) {
    const PageFeatures features = FeaturesFor(options.where);
    std::vector<Section> out;
    for (const std::string_view name : SectionsOf(model.Table(), category)) {
        Section section{.name = std::string(name), .note = model.SectionNote(category, name)};
        for (const Setting& s : model.Table()) {
            if (s.category != category || s.section != name || !model.Visible(s)) continue;
            if (s.widget == Widget::kMicSlots) {
                for (int i = 0; i < audio::usb_mic::kSingers; i++) {
                    section.rows.push_back({.kind = RowKind::kMicSlot, .setting = &s, .index = i,
                                            .label = "Mic " + std::to_string(i + 1)});
                }
            } else if (s.widget == Widget::kJoypadLag) {
                const auto types = LagEditorTypes();
                for (size_t i = 0; i < types.size(); i++) {
                    section.rows.push_back({.kind = RowKind::kLagType, .setting = &s,
                                            .index = static_cast<int>(i),
                                            .label = std::string(types[i].label)});
                }
            } else {
                section.rows.push_back({.kind = RowKind::kSetting, .setting = &s,
                                        .label = model.Label(s)});
            }
        }

        if (category == Category::kGame && name == "Folders") {
            section.rows.push_back(ControlRow(Control::kSettingsFile, "Settings file"));
        }
        // under the last of the rows it sets
        if (category == Category::kDisplay && name == "Smoothness") {
            size_t at = section.rows.size();
            for (size_t i = 0; i < section.rows.size(); i++) {
                const Setting* s = section.rows[i].setting;
                if (s && (s->cvar == "frame_cap" || s->cvar == "native_present_pacing")) at = i + 1;
            }
            if (at <= section.rows.size()) {
                section.rows.insert(section.rows.begin() + static_cast<std::ptrdiff_t>(at),
                                    ControlRow(Control::kLowestDelay, "Lowest delay"));
            }
        }
        if (category == Category::kControllers && name == "Instruments" && features.device_tester) {
            section.rows.push_back(ControlRow(Control::kTestControllers, "Test controllers"));
        }
        if (category == Category::kVideos && name == "Playback") {
            section.rows.insert(section.rows.begin(),
                                ControlRow(Control::kVideosList, "Music Videos screen"));
        }
        // rows locked for this run say so under the band, not only once picked
        if (!section.note) {
            std::string locked;
            Lock why = Lock::kNone;
            for (const Row& r : section.rows) {
                if (!r.setting || r.kind == RowKind::kControl || !model.IsLocked(r.setting->cvar)) continue;
                if (why == Lock::kNone) why = model.LockOf(r.setting->cvar);
                if (r.kind != RowKind::kSetting || locked.find(r.label) != std::string::npos) continue;
                locked += (locked.empty() ? "" : ", ") + r.label;
            }
            if (!locked.empty()) section.note = std::string(LockReason(why)) + " for this run: " + locked + ".";
        }
        if (!section.rows.empty() || section.note) out.push_back(std::move(section));
    }

    if (category == Category::kLights) {
        out.push_back(Section{.name = "Try the lights",
                              .note = model.SectionNote(category, "Try the lights"),
                              .rows = {ControlRow(Control::kLightsDevice, "Kit to test"),
                                       ControlRow(Control::kLightsCheck, "Colour check"),
                                       ControlRow(Control::kFog, "Fog"),
                                       ControlRow(Control::kStrobe, "Strobe"),
                                       ControlRow(Control::kLightsColour, "LED colour"),
                                       ControlRow(Control::kLeds, "Which LED"),
                                       ControlRow(Control::kPattern, "Pattern"),
                                       ControlRow(Control::kChase, "Chase"),
                                       ControlRow(Control::kLightsOff, "All off")}});
    }
    if (category == Category::kAdvanced && options.sdk_settings) {
        out.push_back(Section{.name = "SDK",
                              .rows = {ControlRow(Control::kSdkSettings, "Every SDK setting")}});
    }
    return out;
}

std::vector<Section> SearchRows(const SettingsModel& model, std::string_view query,
                                const RowOptions& options) {
    std::string wanted = Lower(query);
    while (!wanted.empty() && wanted.back() == ' ') wanted.pop_back();
    while (!wanted.empty() && wanted.front() == ' ') wanted.erase(wanted.begin());
    std::vector<Section> out;
    if (wanted.empty()) return out;
    for (const Category category : Categories()) {
        Section found{.name = std::string(CategoryName(category))};
        for (Section& section : CategoryRows(model, category, options)) {
            for (Row& row : section.rows) {
                if (Matches(model, row, section.name, wanted)) found.rows.push_back(std::move(row));
            }
        }
        if (!found.rows.empty()) out.push_back(std::move(found));
    }
    return out;
}

std::string ValueText(const SettingsModel& model, const Row& row, const DeviceLists& lists) {
    switch (row.kind) {
    case RowKind::kSetting: return row.setting ? SettingValue(model, *row.setting, lists) : "";
    case RowKind::kMicSlot: {
        const std::vector<std::string> slots = MicSlots(model, *row.setting);
        const std::string& slot = slots[static_cast<size_t>(row.index)];
        // with every slot empty, the first records the system's default
        // microphone; once another is named, an empty slot is unused
        const bool all_empty = std::ranges::all_of(slots, &std::string::empty);
        if (slot.empty()) return row.index == 0 && all_empty ? "System default" : "None";
        if (const auto found = FindSavedDevice(lists.mics, slot)) return lists.mics[*found];
        return slot + " (not connected)";
    }
    case RowKind::kLagType: {
        const LagType& type = LagEditorTypes()[static_cast<size_t>(row.index)];
        const auto lag = LagFor(model.Value(row.setting->cvar), type.type);
        return lag ? Number(*lag) + " ms" : "Game's own";
    }
    case RowKind::kControl: break;
    }
    return {};
}

std::vector<ChoiceItem> Choices(const SettingsModel& model, const Row& row,
                                const DeviceLists& lists) {
    switch (row.kind) {
    case RowKind::kSetting: return row.setting ? SettingChoices(model, *row.setting, lists)
                                               : std::vector<ChoiceItem>{};
    case RowKind::kMicSlot: {
        const std::vector<std::string> slots = MicSlots(model, *row.setting);
        const std::string& slot = slots[static_cast<size_t>(row.index)];
        const bool all_empty = std::ranges::all_of(slots, &std::string::empty);
        std::vector<ChoiceItem> out;
        out.push_back({.label = row.index == 0 && all_empty ? "System default" : "None",
                       .value = "", .current = slot.empty(), .is_default = true});
        const auto saved = slot.empty() ? std::nullopt : FindSavedDevice(lists.mics, slot);
        for (size_t i = 0; i < lists.mics.size(); i++) {
            out.push_back({.label = lists.mics[i], .value = MicSlotValue(lists.mics[i]),
                           .current = saved == i});
        }
        if (!slot.empty() && !saved) {
            out.push_back({.label = slot + " (not connected)", .value = slot, .current = true});
        }
        return out;
    }
    case RowKind::kLagType:
    case RowKind::kControl: break;
    }
    return {};
}

std::optional<std::string> Stepped(const SettingsModel& model, const Row& row, int delta,
                                   const DeviceLists& lists) {
    if (row.kind == RowKind::kControl || !row.setting || delta == 0) return std::nullopt;
    if (row.kind == RowKind::kLagType) {
        const LagType& type = LagEditorTypes()[static_cast<size_t>(row.index)];
        const auto lag = LagFor(model.Value(row.setting->cvar), type.type);
        // the game's own comes before 0
        if (!lag) return delta > 0 ? std::optional<std::string>("0") : std::nullopt;
        const double next = std::round(*lag) + delta;
        if (next < 0) return std::string();
        if (next > 1000) return std::nullopt;
        return Number(next);
    }

    const std::vector<ChoiceItem> choices = Choices(model, row, lists);
    if (!choices.empty()) {
        const auto current = std::ranges::find_if(choices, &ChoiceItem::current);
        if (current == choices.end()) return choices.front().value;
        const auto index = (current - choices.begin()) + delta;
        if (index < 0 || index >= static_cast<std::ptrdiff_t>(choices.size())) return std::nullopt;
        return choices[static_cast<size_t>(index)].value;
    }

    const Setting& s = *row.setting;
    const std::string value = model.Value(s.cvar);
    const Range range = s.range.value_or(Range{});
    const bool bounded = range.max > range.min;
    switch (s.widget) {
    case Widget::kIntStepper:
    case Widget::kIntSlider:
    case Widget::kMidiNote: {
        const int64_t step = std::max<int64_t>(1, static_cast<int64_t>(range.step));
        const int64_t v = AsInt(value).value_or(static_cast<int64_t>(range.min));
        const int64_t next = v + delta * step;
        if (bounded && (next < range.min || next > range.max)) return std::nullopt;
        return std::to_string(next);
    }
    case Widget::kFloatSlider:
    case Widget::kPercentSlider:
    case Widget::kFloatInput: {
        const double step = range.step > 0 ? range.step : 0.1;
        const double v = AsFloat(value).value_or(range.min);
        // on the range's steps, so 1.0 goes to 1.05 rather than 1.0499999
        const double next = std::round((v + delta * step) / step) * step;
        if (bounded && (next < range.min - step / 2 || next > range.max + step / 2)) {
            return std::nullopt;
        }
        return Number(bounded ? std::clamp(next, range.min, range.max) : next);
    }
    default: break;
    }
    return std::nullopt;
}

bool ApplyValue(SettingsModel& model, const Row& row, std::string_view value) {
    if (!row.setting) return false;
    const Setting& s = *row.setting;
    switch (row.kind) {
    case RowKind::kMicSlot: {
        std::vector<std::string> slots = MicSlots(model, s);
        slots[static_cast<size_t>(row.index)] = std::string(value);
        return model.Set(s.cvar, JoinMicSlots(slots));
    }
    case RowKind::kLagType: {
        const LagType& type = LagEditorTypes()[static_cast<size_t>(row.index)];
        const auto ms = AsFloat(value);
        return model.Set(s.cvar, WithLag(model.Value(s.cvar), type.type,
                                         ms ? std::optional<float>(static_cast<float>(*ms))
                                            : std::nullopt));
    }
    case RowKind::kSetting:
        if (s.widget == Widget::kWindowMode) {
            if (value == "exclusive") return model.SetWindowMode(WindowMode::kExclusive);
            if (value == "borderless") return model.SetWindowMode(WindowMode::kBorderless);
            return model.SetWindowMode(WindowMode::kWindowed);
        }
        return model.Set(s.cvar, value);
    case RowKind::kControl: break;
    }
    return false;
}

bool RowChanged(const SettingsModel& model, const Row& row) {
    if (!row.setting) return false;
    switch (row.kind) {
    case RowKind::kSetting: return model.IsChanged(row.setting->cvar);
    case RowKind::kMicSlot:
        return !MicSlots(model, *row.setting)[static_cast<size_t>(row.index)].empty();
    case RowKind::kLagType:
        return LagFor(model.Value(row.setting->cvar),
                      LagEditorTypes()[static_cast<size_t>(row.index)].type)
            .has_value();
    case RowKind::kControl: break;
    }
    return false;
}

bool ResetRow(SettingsModel& model, const Row& row) {
    if (!row.setting) return false;
    if (row.kind == RowKind::kSetting) return model.Reset(row.setting->cvar);
    return ApplyValue(model, row, "");
}

bool OpensList(const Row& row) {
    if (row.kind == RowKind::kMicSlot) return true;
    if (row.kind != RowKind::kSetting || !row.setting) return false;
    switch (row.setting->widget) {
    case Widget::kMonitor:
    case Widget::kResolution:
    case Widget::kMidiPort:
    case Widget::kComboText:
    case Widget::kFolderList: return true;
    case Widget::kCombo: return row.setting->choices.size() > 6;
    default: return false;
    }
}

bool TypesText(const Row& row) {
    if (row.kind != RowKind::kSetting || !row.setting) return false;
    switch (row.setting->widget) {
    case Widget::kText:
    case Widget::kPassword:
    case Widget::kIntStepper:
    case Widget::kFloatInput: return true;
    default: return false;
    }
}

}

#include "xinput_caps.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <rex/logging.h>

namespace band3::input {

namespace {

// XInputGetCapabilitiesEx's answer (xinput1_4.dll's ordinal 108), as SDL
// reads it for the ids in its GUIDs
struct CapabilitiesEx {
    XINPUT_CAPABILITIES caps;
    WORD vendor;
    WORD product;
    WORD version;
    WORD unknown1;
    DWORD unknown2;
};

struct XInputCapsApi {
    using GetCapabilitiesEx = DWORD(WINAPI*)(DWORD, DWORD, DWORD, CapabilitiesEx*);
    using GetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
    GetCapabilitiesEx get_capabilities_ex = nullptr;
    GetCapabilities get_capabilities = nullptr;
};

// from whichever XInput the PC has; only xinput1_4 has the Ex read
const XInputCapsApi& Api() {
    static const XInputCapsApi api = [] {
        XInputCapsApi found;
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            HMODULE module = LoadLibraryW(name);
            if (!module) continue;
            found.get_capabilities_ex = reinterpret_cast<XInputCapsApi::GetCapabilitiesEx>(
                reinterpret_cast<void*>(GetProcAddress(module, MAKEINTRESOURCEA(108))));
            found.get_capabilities = reinterpret_cast<XInputCapsApi::GetCapabilities>(
                reinterpret_cast<void*>(GetProcAddress(module, "XInputGetCapabilities")));
            if (found.get_capabilities) return found;
            found = {};
        }
        return found;
    }();
    return api;
}

Caps360 FromWindows(const XINPUT_CAPABILITIES& in) {
    Caps360 caps;
    caps.sub_type = in.SubType;
    caps.flags = in.Flags;
    caps.gamepad.buttons = in.Gamepad.wButtons;
    caps.gamepad.left_trigger = in.Gamepad.bLeftTrigger;
    caps.gamepad.right_trigger = in.Gamepad.bRightTrigger;
    caps.gamepad.thumb_lx = in.Gamepad.sThumbLX;
    caps.gamepad.thumb_ly = in.Gamepad.sThumbLY;
    caps.gamepad.thumb_rx = in.Gamepad.sThumbRX;
    caps.gamepad.thumb_ry = in.Gamepad.sThumbRY;
    return caps;
}

std::vector<WindowsPad> ReadWindowsPads() {
    const XInputCapsApi& api = Api();
    std::vector<WindowsPad> pads;
    if (!api.get_capabilities) return pads;
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; slot++) {
        if (api.get_capabilities_ex) {
            CapabilitiesEx ex{};
            if (api.get_capabilities_ex(1, slot, 0, &ex) == ERROR_SUCCESS) {
                pads.push_back({ex.vendor, ex.product, FromWindows(ex.caps)});
                continue;
            }
        }
        XINPUT_CAPABILITIES caps{};
        if (api.get_capabilities(slot, 0, &caps) == ERROR_SUCCESS) {
            pads.push_back({0, 0, FromWindows(caps)});
        }
    }
    return pads;
}

std::mutex g_mutex;
// each kit's answer, by GUID: a model's capabilities, which don't change
std::map<std::string, Caps360, std::less<>> g_found;
// the kits whose answer couldn't be found, logged once each
std::set<std::string, std::less<>> g_missed;

}

std::optional<Caps360> WindowsDrumCaps(std::string_view guid, const Caps360& reported) {
    const auto ids = XInputIdsFromGuid(guid);
    if (!ids || ids->subtype != kSubtypeDrums) return std::nullopt;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_found.find(guid);
    if (found == g_found.end()) {
        const std::vector<WindowsPad> pads = ReadWindowsPads();
        const auto match = MatchWindowsCaps(*ids, pads);
        if (!match) {
            if (g_missed.emplace(guid).second) {
                REXLOG_WARN("Input: Windows' XInput has no capabilities for the drum kit "
                            "{:04X}:{:04X} ({} XInput pads), so RB3 gets SDL's and reads it "
                            "as an RB1 kit",
                            ids->vendor, ids->product, pads.size());
            }
            return std::nullopt;
        }
        const Gamepad360& g = match->gamepad;
        REXLOG_INFO("Input: the drum kit {:04X}:{:04X} reports flags {:04X} and sticks "
                    "{:04X} {:04X} {:04X} {:04X} through Windows' XInput, so RB3 reads it "
                    "as an {} kit",
                    ids->vendor, ids->product, match->flags, static_cast<uint16_t>(g.thumb_lx),
                    static_cast<uint16_t>(g.thumb_ly), static_cast<uint16_t>(g.thumb_rx),
                    static_cast<uint16_t>(g.thumb_ry), IsRb2Drums(*match) ? "RB2-or-later" : "RB1");
        found = g_found.emplace(std::string(guid), *match).first;
    }
    Caps360 caps = reported;
    caps.flags = found->second.flags;
    caps.gamepad = found->second.gamepad;
    return caps;
}

}

#else

namespace band3::input {

std::optional<Caps360> WindowsDrumCaps(std::string_view, const Caps360&) { return std::nullopt; }

}

#endif

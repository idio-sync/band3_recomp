#include "usb_driver_check.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <iterator>
#include "hid_instrument_types.h"

namespace band3::input {

namespace {

// UTF-16 as plain ASCII, which hardware ids, instance ids and service names are
std::string Ascii(const wchar_t* text) {
    std::string out;
    for (const wchar_t* c = text; *c; c++) out += *c < 0x80 ? static_cast<char>(*c) : '?';
    return out;
}

// one of a device's string properties; the first string of a multi-string one
std::string Property(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD property) {
    wchar_t buffer[512] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, property, nullptr,
                                           reinterpret_cast<BYTE*>(buffer),
                                           sizeof(buffer) - sizeof(wchar_t), nullptr)) {
        return {};
    }
    return Ascii(buffer);
}

std::string InstanceId(HDEVINFO set, SP_DEVINFO_DATA& device) {
    wchar_t buffer[512] = {};
    if (!SetupDiGetDeviceInstanceIdW(set, &device, buffer, static_cast<DWORD>(std::size(buffer) - 1),
                                     nullptr)) {
        return {};
    }
    return Ascii(buffer);
}

}

std::vector<UsbInstrumentEntry> ListKnownUsbInstruments() {
    std::vector<UsbInstrumentEntry> found;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &device); i++) {
        const auto ids = ParseUsbHardwareId(Property(set, device, SPDRP_HARDWAREID));
        if (!ids) continue;
        for (const auto& known : KnownHidInstruments()) {
            if (known.vendor != ids->vendor || known.product != ids->product) continue;
            found.push_back({*ids, known.name, Property(set, device, SPDRP_SERVICE),
                             InstanceId(set, device)});
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

}

#else

namespace band3::input {

std::vector<UsbInstrumentEntry> ListKnownUsbInstruments() { return {}; }

}

#endif

#include "usb_driver_check.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <algorithm>
#include "hid_instrument_types.h"

namespace band3::input {

namespace {

// one of a device's string properties, as plain ASCII (hardware ids and
// service names are); the first string of a multi-string one
std::string Property(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD property) {
    wchar_t buffer[512] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, property, nullptr,
                                           reinterpret_cast<BYTE*>(buffer),
                                           sizeof(buffer) - sizeof(wchar_t), nullptr)) {
        return {};
    }
    std::string out;
    for (const wchar_t* c = buffer; *c; c++) out += *c < 0x80 ? static_cast<char>(*c) : '?';
    return out;
}

}

std::vector<WrongDriverInstrument> FindInstrumentsOnOtherDrivers() {
    std::vector<WrongDriverInstrument> found;
    // ids with an entry under HID's driver (or a composite parent): a composite
    // dongle's other interfaces may sit on other drivers without harm
    std::vector<UsbIds> readable;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &device); i++) {
        const auto ids = ParseUsbHardwareId(Property(set, device, SPDRP_HARDWAREID));
        if (!ids) continue;
        for (const auto& known : KnownHidInstruments()) {
            if (known.vendor != ids->vendor || known.product != ids->product) continue;
            std::string service = Property(set, device, SPDRP_SERVICE);
            if (IsHidReadableService(service)) {
                readable.push_back(*ids);
            } else {
                found.push_back({*ids, known.name, std::move(service)});
            }
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    std::erase_if(found, [&](const WrongDriverInstrument& instrument) {
        return std::ranges::find(readable, instrument.ids) != readable.end();
    });
    return found;
}

}

#else

namespace band3::input {

std::vector<WrongDriverInstrument> FindInstrumentsOnOtherDrivers() { return {}; }

}

#endif

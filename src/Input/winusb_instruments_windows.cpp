#include "winusb_instruments.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <winusb.h>
#include <algorithm>
#include <cstring>
#include <optional>
#include <system_error>
#include <vector>
#include <rex/logging.h>
#include "hid_instrument_types.h"
#include "ps3_instruments.h"

namespace band3::input {

namespace {

// HID's class requests (HID 1.11, 7.2)
constexpr UCHAR kHidSetReport = 0x09;
constexpr USHORT kFeatureReport = 0x0300;
constexpr UCHAR kClassToInterface = 0x21;

std::string ErrorText(DWORD code) {
    return std::system_category().message(static_cast<int>(code)) + " (" + std::to_string(code) +
           ")";
}

std::wstring Wide(const std::string& ascii) { return {ascii.begin(), ascii.end()}; }

// the interface GUID Zadig (libwdi) registered for the device, from its
// Device Parameters key: DeviceInterfaceGUIDs, or an older INF's
// DeviceInterfaceGUID
std::optional<GUID> InterfaceGuid(DEVINST devinst) {
    HKEY key = nullptr;
    if (CM_Open_DevNode_Key(devinst, KEY_READ, 0, RegDisposition_OpenExisting, &key,
                            CM_REGISTRY_HARDWARE) != CR_SUCCESS) {
        return std::nullopt;
    }
    wchar_t value[256] = {};
    LSTATUS status = ERROR_FILE_NOT_FOUND;
    for (const wchar_t* name : {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"}) {
        DWORD size = sizeof(value) - 2 * sizeof(wchar_t);
        status = RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size);
        if (status == ERROR_SUCCESS) break;
    }
    RegCloseKey(key);
    GUID guid{};
    if (status != ERROR_SUCCESS || FAILED(IIDFromString(value, &guid))) return std::nullopt;
    return guid;
}

// the path CreateFile opens the device's WinUSB interface by
std::wstring InterfacePath(const std::string& instance, std::string& error) {
    std::wstring id = Wide(instance);
    DEVINST devinst = 0;
    if (CM_Locate_DevNodeW(&devinst, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        error = "Windows no longer lists it";
        return {};
    }
    const auto guid = InterfaceGuid(devinst);
    if (!guid) {
        error = "its registry key has no WinUSB interface GUID";
        return {};
    }
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<GUID*>(&*guid), id.data(),
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        length <= 1) {
        error = "it has no WinUSB interface";
        return {};
    }
    std::vector<wchar_t> list(length);
    if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&*guid), id.data(), list.data(), length,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        !list[0]) {
        error = "it has no WinUSB interface";
        return {};
    }
    // the first of the list's strings
    return list.data();
}

// a HID feature report sent as SET_REPORT, `report` led by its report ID,
// which goes in the request rather than the data, as HidD_SetFeature does
bool SetFeatureReport(WINUSB_INTERFACE_HANDLE usb, UCHAR interface_number,
                      const uint8_t* report, size_t size) {
    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = kClassToInterface;
    setup.Request = kHidSetReport;
    setup.Value = static_cast<USHORT>(kFeatureReport | report[0]);
    setup.Index = interface_number;
    setup.Length = static_cast<USHORT>(size - 1);
    std::vector<UCHAR> data(report + 1, report + size);
    ULONG sent = 0;
    return WinUsb_ControlTransfer(usb, setup, data.data(), static_cast<ULONG>(data.size()), &sent,
                                  nullptr) &&
           sent == data.size();
}

// as the HID path's SendPs3ProEnable (hid_instruments.cpp): the five short
// reports, or the one long one if they aren't accepted
void SendPs3ProEnable(WINUSB_INTERFACE_HANDLE usb, UCHAR interface_number, const std::string& name) {
    std::vector<uint8_t> whole{0x00};
    bool sent = true;
    for (const auto& report : Ps3ProEnableReports()) {
        whole.insert(whole.end(), report.begin() + 1, report.end());
        if (sent && !SetFeatureReport(usb, interface_number, report.data(), report.size())) {
            sent = false;
        }
    }
    if (sent) {
        REXLOG_INFO("HID instruments: asked {} to send its keys or frets (through WinUSB)", name);
        return;
    }
    const std::string short_error = ErrorText(GetLastError());
    if (SetFeatureReport(usb, interface_number, whole.data(), whole.size())) {
        REXLOG_INFO("HID instruments: asked {} to send its keys or frets (through WinUSB, as one "
                    "report)",
                    name);
        return;
    }
    REXLOG_WARN("HID instruments: couldn't ask {} to send its keys or frets through WinUSB ({}; "
                "{}), so only its buttons may work",
                name, short_error, ErrorText(GetLastError()));
}

// a dongle's reports from its interrupt-in pipe
class WinUsbSource final : public ReportSource {
public:
    WinUsbSource(HANDLE file, WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, USHORT packet_size)
        : file_(file), usb_(usb), pipe_(pipe), packet_(std::max<USHORT>(packet_size, 64)) {}
    ~WinUsbSource() override {
        WinUsb_AbortPipe(usb_, pipe_);
        WinUsb_Free(usb_);
        CloseHandle(file_);
    }

    int Read(uint8_t* buffer, size_t size, int timeout_ms) override {
        if (timeout_ms != timeout_ms_) {
            ULONG timeout = static_cast<ULONG>(std::max(timeout_ms, 1));
            WinUsb_SetPipePolicy(usb_, pipe_, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
            timeout_ms_ = timeout_ms;
        }
        ULONG got = 0;
        if (!WinUsb_ReadPipe(usb_, pipe_, packet_.data(), static_cast<ULONG>(packet_.size()), &got,
                             nullptr)) {
            return GetLastError() == ERROR_SEM_TIMEOUT ? 0 : -1;
        }
        const size_t n = std::min<size_t>(got, size);
        std::memcpy(buffer, packet_.data(), n);
        return static_cast<int>(n);
    }

private:
    const HANDLE file_;
    const WINUSB_INTERFACE_HANDLE usb_;
    const UCHAR pipe_;
    // a whole packet, or WinUSB fails the read on overflow
    std::vector<UCHAR> packet_;
    int timeout_ms_ = -1;
};

}

WinUsbInstrument OpenWinUsbInstrument(const UsbInstrumentEntry& entry, std::string& error) {
    const std::wstring path = InterfacePath(entry.instance, error);
    if (path.empty()) return {};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        error = code == ERROR_ACCESS_DENIED
                    ? "another program has it open (RPCS3?); band3 tries again while it runs"
                    : ErrorText(code);
        return {};
    }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) {
        error = "WinUSB didn't start on it: " + ErrorText(GetLastError());
        CloseHandle(file);
        return {};
    }
    const auto fail = [&](std::string why) {
        error = std::move(why);
        WinUsb_Free(usb);
        CloseHandle(file);
        return WinUsbInstrument{};
    };

    WinUsbInstrument out{entry.ids.vendor, entry.ids.product, 0, nullptr};
    USB_DEVICE_DESCRIPTOR descriptor{};
    ULONG got = 0;
    if (WinUsb_GetDescriptor(usb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                             reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor), &got) &&
        got == sizeof(descriptor)) {
        out.vendor = descriptor.idVendor;
        out.product = descriptor.idProduct;
        out.release = descriptor.bcdDevice;
    }

    USB_INTERFACE_DESCRIPTOR settings{};
    if (!WinUsb_QueryInterfaceSettings(usb, 0, &settings)) {
        return fail("couldn't read its interface: " + ErrorText(GetLastError()));
    }
    std::optional<WINUSB_PIPE_INFORMATION> in_pipe;
    for (UCHAR i = 0; i < settings.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (WinUsb_QueryPipe(usb, 0, i, &pipe) && pipe.PipeType == UsbdPipeTypeInterrupt &&
            USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            in_pipe = pipe;
            break;
        }
    }
    if (!in_pipe) return fail("it has no interrupt-in pipe to read reports from");
    UCHAR on = TRUE;
    WinUsb_SetPipePolicy(usb, in_pipe->PipeId, AUTO_CLEAR_STALL, sizeof(on), &on);

    const auto instrument = IdentifyHidInstrument(out.vendor, out.product, out.release);
    if (instrument && NeedsPs3ProEnable(out.vendor, *instrument)) {
        SendPs3ProEnable(usb, settings.bInterfaceNumber, entry.name);
    }
    out.source = std::make_unique<WinUsbSource>(file, usb, in_pipe->PipeId,
                                                in_pipe->MaximumPacketSize);
    return out;
}

}

#else

namespace band3::input {

WinUsbInstrument OpenWinUsbInstrument(const UsbInstrumentEntry&, std::string& error) {
    error = "WinUSB is Windows only";
    return {};
}

}

#endif

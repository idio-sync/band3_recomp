#pragma once
#include <cstddef>
#include <cstdint>

// Where one instrument's raw reports come from, for the instrument driver
// (hid_instruments.h): a HID dongle through SDL, or an Xbox One instrument
// through GameInput (gameinput_instruments.h).

namespace band3::input {

class ReportSource {
public:
    virtual ~ReportSource() = default;

    // Waits up to `timeout_ms` for a report and copies it into `buffer`
    // (truncated to `size`): its length, 0 if none came, or -1 once the
    // device is gone.
    virtual int Read(uint8_t* buffer, size_t size, int timeout_ms) = 0;
};

}

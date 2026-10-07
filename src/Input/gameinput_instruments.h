#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "hid_instrument_types.h"
#include "report_source.h"

// Xbox One instruments (KnownGipInstruments in hid_instrument_types.h), found
// and read through GameInput's raw device reports, for the instrument driver
// (hid_instruments.h). Windows only, and only with GameInput 3 or newer
// installed (Microsoft's GameInput redistributable; many games install it):
// older versions don't hand over GIP devices' raw reports. Wireless
// instruments connect through an Xbox Wireless Adapter for Windows.
//
// A report is the report's ID followed by its data (gip_instruments.h). A
// device that keeps sending the same report is read once: guitars don't move
// the reading's timestamp for an axis, so only the bytes tell a change.

namespace band3::input {

struct GipInstrument {
    // tells the device apart while it stays connected
    std::string path;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t release = 0;
    HidInstrumentType instrument = HidInstrumentType::kXboxOneGuitar;
    std::unique_ptr<ReportSource> source;
};

class GipWatch {
public:
    virtual ~GipWatch() = default;

    // the known instruments that connected since the last call, each ready to read
    virtual std::vector<GipInstrument> TakeArrivals() = 0;
};

// Starts watching for Xbox One instruments; nullptr, having logged why, where
// GameInput 3 isn't there (or off Windows, or in a build without it).
std::unique_ptr<GipWatch> StartGipWatch();

}

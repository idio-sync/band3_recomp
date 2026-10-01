#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/Input/instrument_kind.h"

// The test harness's names for what the virtual instrument can press, and the
// instruments it can be.

namespace band3::test {

std::string SetInput(input::InstrumentKind kind, input::InstrumentInputs& in,
                     std::string_view name, uint8_t value, uint8_t fret = 0);
std::string SetAxis(input::InstrumentKind kind, input::InstrumentInputs& in,
                    std::string_view name, float value);
std::vector<std::string> SplitInputs(std::string_view list);
std::optional<input::InstrumentKind> ParseInstrumentName(std::string_view name);
const char* InstrumentName(input::InstrumentKind kind);

}

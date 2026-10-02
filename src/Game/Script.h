#pragma once
#include <cstdint>
#include <string>

struct PPCContext;

namespace band3 {

// Runs DTA through RockCentralGateway::ExecuteConfig, as RB3Enhanced does
// (its ExecuteDTA), e.g. "{do {push_back {find $syscfg modifiers modifiers} (mod_x)}}".
// Calls the game, so it runs on the game thread only, from a hook.
void RunScript(PPCContext& ctx, uint8_t* base, const std::string& script);

}

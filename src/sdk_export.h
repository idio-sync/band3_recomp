#pragma once

#include <rex/hook.h>
#include <cstdint>

namespace band3 {

// One of the SDK's own exports, from its runtime library (rexruntime.dll,
// librexruntime.so), for an override of band3's to hand the call on to: the
// process's own lookup of the name finds band3's. Null, logged, if missing.
// Called as (*fn)(ctx, base), which tools/compile_check doesn't take for a
// guest call.
using SdkFunction = void(PPCContext&, uint8_t*);
SdkFunction* SdkExport(const char* name);

}  // namespace band3

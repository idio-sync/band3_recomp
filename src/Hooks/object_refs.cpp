#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/types.h>
#include <cstring>
#include "generated/band3_init.h"
#include "src/sdk_export.h"

// ObDereferenceObject(object pointer) lets go of the object whose guest copy
// that is. The SDK's finds the object from a handle it keeps in the copy's
// header, and given a pointer whose header has none, it makes an object of its
// own for it, keeps that one's handle there and lets it go at once. RB3's
// song scan dereferences its cross-title enumerator's guest copy once a
// package (an enumerator's header has no handle), so from the second package
// on, each dereference let go of whatever had been given that handle since:
// with songs and loose files in the game folder, a character file the main
// menu was reading was closed under it (its next read failed with
// STATUS_INVALID_HANDLE and the game read on in stale buffers).
// band3 hands the SDK only a dereference whose header names the object the
// pointer is the guest copy of, and drops any other.

namespace {

using rex::system::XObject;

// whether the SDK's dereference of pointer would find the object it is the
// guest copy of: a handle after the SDK's signature ("XER\0") in its header
bool HeaderNamesItsObject(uint8_t* base, uint32_t pointer) {
    static constexpr uint8_t kSignature[4] = {'R', 'E', 'X', 0};  // 0x584552 as the SDK reads it
    if (!pointer || std::memcmp(base + pointer + 8, kSignature, sizeof(kSignature)) != 0) {
        return false;
    }
    auto object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(REX_LOAD_U32(pointer + 12));
    return object && object->guest_object() == pointer;
}

}  // namespace

extern "C" REX_FUNC(__imp__ObDereferenceObject) {
    static band3::SdkFunction* const sdk = band3::SdkExport("__imp__ObDereferenceObject");
    const uint32_t pointer = ctx.r3.u32;
    if (!HeaderNamesItsObject(base, pointer)) {
        REXLOG_DEBUG("object refs: dropped a dereference of {:08X}, whose header names no "
                     "object of its own",
                     pointer);
        return;
    }
    if (sdk) sdk(ctx, base);
}

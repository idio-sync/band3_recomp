#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/settings.h"

// The autosave setting. Every autosave the game asks for (after a song, a
// setlist edit, a change to the song cache) goes through
// SaveLoadManager::IsReasonToAutosave, from AutoSave and AutoSaveNow; only when
// it answers yes does the save manager start saving on its next poll. With
// autosave off it answers no, so profiles are only saved when you save from the
// options menu. For testing with autoplay without it reaching your profile.
// rb3-xenon, src/band3/meta_band/SaveLoadManager.cpp.

extern "C" void __imp__SaveLoadManager__IsReasonToAutosave(PPCContext& ctx, uint8_t* base);

namespace {

// so the log says it once per game run, not on every request
bool g_reported_off = false;

}

// bool SaveLoadManager::IsReasonToAutosave(this)
extern "C" REX_FUNC(SaveLoadManager__IsReasonToAutosave)
{
    if (REXCVAR_GET(autosave)) {
        __imp__SaveLoadManager__IsReasonToAutosave(ctx, base);
        if (ctx.r3.u32 & 0xFF) REXLOG_DEBUG("autosave: the game is saving");
        return;
    }
    if (!g_reported_off) {
        REXLOG_INFO("autosave is off: the game asked to save and won't (save from the "
                    "options menu to keep progress)");
        g_reported_off = true;
    }
    ctx.r3.u64 = 0;
}

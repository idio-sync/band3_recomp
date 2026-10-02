#include <rex/logging.h>
#include <rex/ui/flags.h>
#include "generated/band3_init.h"
#include "src/Hooks/frame_pacing.h"
#include "src/settings.h"

extern "C" void __imp__BoxMapLighting__ApplyQueuedLights(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndMat__Load(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__OutfitConfig__CompressTextures(PPCContext& ctx, uint8_t* base);

extern "C" REX_FUNC(BoxMapLighting__ApplyQueuedLights)
{
    if (REXCVAR_GET(disable_approximate_lights)) {
        return;
    }
    __imp__BoxMapLighting__ApplyQueuedLights(ctx, base);
}

extern "C" REX_FUNC(RndMat__Load)
{
    uint32_t this_addr = ctx.r3.u32;
    __imp__RndMat__Load(ctx, base);
    if (REXCVAR_GET(disable_hair_shader)) {
        uint32_t shader = REX_LOAD_U32(this_addr + 0x118);
        if (shader == 2) {
			// set shader variation to kShaderVariationNone
            REX_STORE_U32(this_addr + 0x118, 0);
        }
    }
	
    if (REXCVAR_GET(fullbright)) {
		// force useEnviron to be 0
        REX_STORE_U8(this_addr + 0x99, 0);
    }
}

extern "C" void __imp__ProcCounter__ProcCommands(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(ProcCounter__ProcCommands)
{
    if (REXCVAR_GET(disable_even_odd_rendering)) {
        ctx.r3.u64 = 7;
        return;
    }
    __imp__ProcCounter__ProcCommands(ctx, base);
}

// ProcCounter's fields SetEmulateFPS sets
constexpr uint32_t kProcCounter_Count = 4;     // frames into the period
constexpr uint32_t kProcCounter_Period = 8;    // frames between world frames
constexpr uint32_t kProcCounter_OddHalf = 12;  // its odd half-frame, alternated
constexpr uint32_t kProcCounter_Fps = 16;      // the rate it was set for

// ProcCommands calls this every frame with the venue's emulate_fps; the game
// counts the period as if it ran at 60 (frame_pacing.h). At another
// refresh_rate, or with background_fps set, the period is counted from the
// game's real rate instead, stored as SetEmulateFPS would.
extern "C" void __imp__ProcCounter__SetEmulateFPS(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(ProcCounter__SetEmulateFPS)
{
    static uint32_t s_counter = 0;
    static int32_t s_half_frames = 0;  // what was stored, 0 when the game set it
    const uint32_t counter = ctx.r3.u32;
    const int32_t fps = ctx.r4.s32;
    const double game_hz = REXCVAR_GET(video_mode_refresh_rate);
    const int32_t background_fps = REXCVAR_GET(background_fps);

    const bool at_60 = !(game_hz > 0) || game_hz == 60;
    if (fps <= 0 || (at_60 && background_fps == 0)) {
        // the game's own; one stored here is set again (it keeps a period
        // while the rate it was set for doesn't change, 0 included), so the
        // rate is one no venue asks for
        if (s_half_frames && s_counter == counter) {
            REX_STORE_U32(counter + kProcCounter_Fps, 0x80000000u);
            s_half_frames = 0;
        }
        __imp__ProcCounter__SetEmulateFPS(ctx, base);
        return;
    }

    const int32_t half_frames = band3::pacing::WorldHalfFrames(game_hz, fps, background_fps);
    if (counter != s_counter || half_frames != s_half_frames ||
        static_cast<int32_t>(REX_LOAD_U32(counter + kProcCounter_Fps)) != fps) {
        const int32_t period = half_frames >> 1;
        REX_STORE_U32(counter + kProcCounter_Fps, static_cast<uint32_t>(fps));
        REX_STORE_U32(counter + kProcCounter_Period, static_cast<uint32_t>(period));
        REX_STORE_U32(counter + kProcCounter_OddHalf, static_cast<uint32_t>(half_frames & 1));
        if (static_cast<int32_t>(REX_LOAD_U32(counter + kProcCounter_Count)) >= period)
            REX_STORE_U32(counter + kProcCounter_Count, 0);
        if (half_frames != s_half_frames)
            REXLOG_INFO("Background: the world every {} frames, {:.1f} fps at {} Hz "
                        "(venue {} fps, background_fps {})",
                        half_frames / 2.0, 2 * game_hz / half_frames, game_hz, fps,
                        background_fps);
        s_counter = counter;
        s_half_frames = half_frames;
    }
    ctx.r3.u64 = static_cast<uint32_t>(fps);
}

extern "C" REX_FUNC(OutfitConfig__CompressTextures)
{
    if (!REXCVAR_GET(compress_character_textures)) {
        return;
    }
    __imp__OutfitConfig__CompressTextures(ctx, base);
}

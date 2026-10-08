#pragma once

#include <cstdint>
#include <optional>

// band3's answers to the game's occlusion queries, in place of the GPUs'
// constant (the sync-only GPU's native_query_sample_count, the emulated GPU's
// query_occlusion_fake_sample_count, 1000 each). RB3's lens flares draw at
// their area test's samples / (rect w x h) of their strength
// (RndFlare::DrawShowing), so 1000 left a 128x128 flare at 6% and a 32x32 one
// full; a 360 answers each test with what its draws cover where nothing is in
// front of them.
//
// query_answers.cpp follows D3DQuery_Issue's begins and ends on the game's
// render thread and adds up the pixels each D3DDevice_DrawVerticesUP between
// them covers (query_coverage.h; gpu_skip.cpp's hook, before it decides
// whether the emulated GPU draws it); D3DQuery_GetData (query_log.cpp) hands
// the game that count once the GPU has answered, so the game's one-frame
// latency is the GPU's. A query with another kind of draw, or a primitive
// type not counted, keeps the GPU's answer. With either GPU.
//
// Nothing is occluded: the counts are what the draws cover, not what passes
// the scene's depth. The native renderer draws a frame on its own thread
// after the game has moved on, and may skip one, so its depth can't answer by
// the next frame's DxRnd::DoPointTests the way a 360's GPU does.

namespace band3::render {

// native_query_sample_count -1 (the default): band3 answers. 0 or more: the
// GPUs' constants.
bool QueryAnswersOn();

// What the sync-only GPU writes at a query's end for the setting: its count,
// or with band3 answering the 1000 the game got before, which only queries
// band3 didn't follow keep
inline int32_t SyncCpSampleCount(int32_t setting) { return setting < 0 ? 1000 : setting; }

// D3DQuery_Issue's hook: `query` (an occlusion one is followed) and its
// D3DISSUE_BEGIN (2) and D3DISSUE_END (1) flags
void NoteQueryIssue(uint8_t* base, uint32_t query, uint32_t flags);

// gpu_skip.cpp's emitter hooks, on the game's render thread: a
// D3DDevice_DrawVerticesUP's primitive type, vertex count, vertices and
// stride (guest r4..r7), and any other draw
void NoteQueryDrawUp(uint8_t* base, uint32_t prim, uint32_t count, uint32_t data,
                     uint32_t stride);
void NoteQueryOtherDraw();

// At D3DQuery_GetData's return (`hr` its r3): with band3 answering and an
// answer for `query`, writes it over the passed samples the GPU's count left
// at `data` and returns that count; nullopt if it left it
std::optional<uint32_t> ApplyQueryAnswer(uint8_t* base, uint32_t query, uint32_t data,
                                         uint32_t size, uint32_t hr);

}  // namespace band3::render

// Auto-sync's ear (src/Video/auto_sync.h): the song's own audio, as the game
// decodes it. StandardStream::ConsumeData (rb3-xenon synth/StandardStream.cpp)
// takes each block the song's Vorbis reader decodes, every channel (stem)
// apart and before the mixer, at its sample position: after it, the samples
// it took are mixed to mono and handed on. The song's stream is the first of
// several channels at a music rate to start at sample 0 once a song with a
// music video begins (menus' and the music library's previews are other
// streams; a preview starts mid-song). The reader decodes a couple of seconds
// ahead of what plays, which positions by sample don't mind.

#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <bit>
#include <cstdint>
#include <mutex>
#include <vector>

#include "generated/band3_init.h"
#include "src/Video/auto_sync.h"

namespace {

// StandardStream (synth/StandardStream.h)
constexpr uint32_t kStream_ChannelsBegin = 0x20;  // std::vector<StreamReceiver*>
constexpr uint32_t kStream_ChannelsEnd = 0x24;
constexpr uint32_t kStream_SampleRate = 0x2c;
constexpr uint32_t kStream_CurrentSamp = 0xa4;
constexpr uint32_t kStream_InfoFloatSamples = 0xe9;  // else big-endian 16-bit
constexpr uint32_t kStream_VirtualChans = 0xec;
// a song's stream: several stems at a music rate (menus' are 28 kHz)
constexpr int kMinChannels = 4;
constexpr int kMinRate = 32000;

std::mutex g_mutex;
uint64_t g_song = ~0ull;   // auto_sync's song count the stream below is for
uint32_t g_stream = 0;     // the song's StandardStream, once found
int32_t g_next = 0;        // the sample the next block should start at
std::vector<float> g_mono;

}

extern "C" void __imp__StandardStream__ConsumeData(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(StandardStream__ConsumeData) {
    const uint32_t stream = ctx.r3.u32;
    const uint32_t v = ctx.r4.u32;  // void*[channels]
    const int32_t before = int32_t(REX_LOAD_U32(stream + kStream_CurrentSamp));
    __imp__StandardStream__ConsumeData(ctx, base);
    const int32_t taken = ctx.r3.s32;
    if (taken <= 0 || !band3::video::SongAudioWanted()) return;

    const uint64_t song = band3::video::SongGeneration();
    const int channels = int((REX_LOAD_U32(stream + kStream_ChannelsEnd) -
                              REX_LOAD_U32(stream + kStream_ChannelsBegin)) / 4) -
                         int(REX_LOAD_U32(stream + kStream_VirtualChans));
    const int rate = int(REX_LOAD_U32(stream + kStream_SampleRate));
    std::lock_guard lock(g_mutex);
    if (song != g_song) {
        g_song = song;
        g_stream = 0;
    }
    if (!g_stream) {
        if (before != 0 || channels < kMinChannels || rate < kMinRate) return;
        g_stream = stream;
        g_next = 0;
        band3::video::SongAudioBegin(song, rate);
    }
    if (stream != g_stream) return;
    if (before != g_next) {
        // it jumped (a restart, practice): from the top again, else no more
        if (before != 0) return;
        band3::video::SongAudioBegin(song, rate);
    }
    const bool floats = REX_LOAD_U8(stream + kStream_InfoFloatSamples) != 0;
    g_mono.assign(size_t(taken), 0.0f);
    for (int c = 0; c < channels; c++) {
        const uint32_t buf = REX_LOAD_U32(v + c * 4);
        if (!buf) continue;
        for (int32_t s = 0; s < taken; s++) {
            g_mono[size_t(s)] +=
                floats ? std::bit_cast<float>(REX_LOAD_U32(buf + s * 4))
                       : float(int16_t(REX_LOAD_U16(buf + s * 2))) * (1.0f / 32768.0f);
        }
    }
    for (float& x : g_mono) x /= float(channels);
    band3::video::SongAudio(song, g_mono);
    g_next = before + taken;
}

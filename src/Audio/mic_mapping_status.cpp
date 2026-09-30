#include "mic_mapping_status.h"
#include <mutex>

namespace band3::audio {

namespace {

std::mutex g_mutex;
MicMapping g_mapping;

}

bool RecordMicMapping(std::vector<MicMappingPlayer> players, std::vector<MicMappingMic> mics) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool changed = !g_mapping.seen || players != g_mapping.players || mics != g_mapping.mics;
    g_mapping.seen = true;
    g_mapping.refreshes++;
    g_mapping.players = std::move(players);
    g_mapping.mics = std::move(mics);
    return changed;
}

MicMapping GetMicMapping() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_mapping;
}

}

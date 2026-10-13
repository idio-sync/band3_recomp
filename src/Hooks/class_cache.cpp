#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// IsASubclass(Symbol child, Symbol parent): whether class child is parent or
// derives from it. Each call walks the class's superclasses in the system
// config (RecurseSuperClassesSearch: SystemConfig objects, the class's
// array, its superclasses array, a linear FindArray at every step), and
// scripts that iterate a directory's objects of a class ask it of every
// object: at a song's start, when the game screen enters, it was 38% of a
// ~90 ms game frame. The answer depends only on the two symbols (interned,
// so their string pointers name them) and the config, which doesn't change
// once the game runs, so band3 keeps each answer.

namespace {

std::mutex g_mutex;
std::unordered_map<uint64_t, bool> g_answers;  // under g_mutex
// past this many pairs (a few hundred classes ever meet), start over rather
// than grow without bound
constexpr size_t kMaxAnswers = 65536;

}  // namespace

extern "C" void __imp__IsASubclass(PPCContext& ctx, uint8_t* base);

// child in r3, parent in r4 (each a Symbol's string pointer); the answer in r3
extern "C" REX_FUNC(IsASubclass) {
    const uint64_t key = (uint64_t(ctx.r3.u32) << 32) | ctx.r4.u32;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto it = g_answers.find(key); it != g_answers.end()) {
            ctx.r3.u64 = it->second;
            return;
        }
    }
    __imp__IsASubclass(ctx, base);
    const bool answer = (ctx.r3.u32 & 0xFF) != 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_answers.size() >= kMaxAnswers) g_answers.clear();
    g_answers.emplace(key, answer);
}

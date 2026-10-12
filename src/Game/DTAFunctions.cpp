#include "DataArray.h"
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <bit>
#include <optional>
#include <string>
#include <unordered_map>
#include <cstring>

#include "generated/band3_init.h"
#include "src/app_quit.h"
#include "src/build_tag.h"
#include "src/Net/events.h"
#include "src/Game/SongCache.h"
#include "src/Net/local_address.h"
#include "src/relaunch.h"
#include "src/settings.h"
#include "SongMgr.h"

extern "C" void DataNode__Evaluate(PPCContext& ctx, uint8_t* base);

// symbol --> func handler mapping for custom dta functions
static std::unordered_map<uint32_t, PPCFunc*> g_custom_dta_funcs;

// registers a custom DTA function by name
static void RegisterDTAFunc(PPCContext& ctx, uint8_t* base,
                            const char* name, PPCFunc* handler) {
    band3::Symbol sym(ctx, base, name);
    uint32_t sym_value = sym.value(base);

    if (!sym_value) {
        REXLOG_ERROR("RegisterDTAFunc: Symbol construction returned null for '{}'", name);
        return;
    }

    g_custom_dta_funcs[sym_value] = handler;
    REXLOG_INFO("Registered custom DTA function '{}' (sym={:08X})", name, sym_value);
}

// hook for DataArray::Execute, we check if the first node is a symbol (which is a rough indicator that we are trying to call a function) and pass it through to our handler
// kind of a hack but rexglue doesn't like calling indirect guest functions outside of the normal game code range or something and I can't figure out a cleaner way to do this
extern "C" void __imp__DataArray__Execute(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(DataArray__Execute) {
    uint32_t args_addr = ctx.r4.u32;
    uint32_t nodes_ptr = REX_LOAD_U32(args_addr);
    uint32_t first_type = REX_LOAD_U32(nodes_ptr + 4);
    uint32_t first_value = REX_LOAD_U32(nodes_ptr);

    if (first_type == band3::kDataSymbol) {
        auto it = g_custom_dta_funcs.find(first_value);
        if (it != g_custom_dta_funcs.end()) {
            it->second(ctx, base);
            return;
        }
    }

    __imp__DataArray__Execute(ctx, base);
}

static void ExitHandler(PPCContext& ctx, uint8_t* base) {
    REXLOG_INFO("Game requested exit, closing band3 as its window's close button does");
    // as the window's close button (app_quit.h); the title alone before the
    // window exists
    if (!band3::app::Quit()) rex::system::kernel_state()->TerminateTitle();
}

// evaluates argument `index` of a DTA call and returns it as a string if it is a
// symbol or string, otherwise nullptr
static const char* ArgString(PPCContext& ctx, uint8_t* base, uint32_t args_addr, int index) {
    auto* args = reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(args_addr));
    if (index >= static_cast<short>(args->mSize)) return nullptr;

    PPCContext eval = ctx;
    eval.r3.u64 = args->mNodes + index * sizeof(band3::DataNode);
    DataNode__Evaluate(eval, base);
    auto* n = reinterpret_cast<const band3::DataNode*>(REX_RAW_ADDR(eval.r3.u32));

    uint32_t str = 0;
    if (n->type == band3::kDataSymbol) str = n->value;
    else if (n->type == band3::kDataString) str = REX_LOAD_U32(n->value);
    return str ? reinterpret_cast<const char*>(REX_RAW_ADDR(str)) : nullptr;
}

// {rb3e_send_event_string id data}: sends mod data (used by RB3 Deluxe) as an
// RB3E network event, like RB3E's own command; returns 1 if sent, 0 if rejected
static void SendEventStringHandler(PPCContext& ctx, uint8_t* base) {
    uint32_t ret = ctx.r3.u32;
    uint32_t args = ctx.r4.u32;
    band3::events::ModData mod{};
    int32_t result = 0;

    const char* id = ArgString(ctx, base, args, 1);
    const char* data = ArgString(ctx, base, args, 2);
    if (!id || strlen(id) > sizeof(mod.identify_value) ||
        !data || strlen(data) > sizeof(mod.string)) {
        REXLOG_WARN("rb3e_send_event_string: expects an id of up to 10 chars and data of up to 240");
    } else {
        memcpy(mod.identify_value, id, strlen(id));
        memcpy(mod.string, data, strlen(data));
        band3::events::Send(band3::events::kDxData, &mod, sizeof(mod));
        result = 1;
    }

    REX_STORE_U32(ret, result);
    REX_STORE_U32(ret + 4, band3::kDataInt);
    ctx.r3.u64 = ret;
}

// RB3Enhanced's other script functions (source/DTAFunctions.c in its repo),
// which RB3 Deluxe calls when it finds RB3E.

// the node argument `index` evaluates to, or nullptr past the end
static const band3::DataNode* ArgNode(PPCContext& ctx, uint8_t* base, uint32_t args_addr,
                                      int index) {
    auto* args = reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(args_addr));
    if (index >= static_cast<short>(args->mSize)) return nullptr;
    PPCContext eval = ctx;
    eval.r3.u64 = args->mNodes + index * sizeof(band3::DataNode);
    DataNode__Evaluate(eval, base);
    return reinterpret_cast<const band3::DataNode*>(REX_RAW_ADDR(eval.r3.u32));
}

static void Return(PPCContext& ctx, uint8_t* base, uint32_t value, band3::DataType type) {
    uint32_t ret = ctx.r3.u32;
    REX_STORE_U32(ret, value);
    REX_STORE_U32(ret + 4, type);
    ctx.r3.u64 = ret;
}

static void ReturnInt(PPCContext& ctx, uint8_t* base, int32_t value) {
    Return(ctx, base, static_cast<uint32_t>(value), band3::kDataInt);
}

// the text as a symbol, as RB3E returns strings; `fallback` when it can't be
// one (empty, or past Symbol's 255 characters)
static void ReturnSymbol(PPCContext& ctx, uint8_t* base, const std::string& text,
                         const char* fallback) {
    const char* name = (text.empty() || text.size() > 255) ? fallback : text.c_str();
    Return(ctx, base, band3::Symbol(ctx, base, name).value(base), band3::kDataSymbol);
}

// {print_debug value}: logs it; returns 1
static void PrintDebugHandler(PPCContext& ctx, uint8_t* base) {
    const band3::DataNode* n = ArgNode(ctx, base, ctx.r4.u32, 1);
    if (!n) {
        REXLOG_INFO("print_debug: (nothing)");
    } else if (n->type == band3::kDataInt) {
        REXLOG_INFO("print_debug: {}", static_cast<int32_t>(static_cast<uint32_t>(n->value)));
    } else if (n->type == band3::kDataFloat) {
        REXLOG_INFO("print_debug: {}", std::bit_cast<float>(static_cast<uint32_t>(n->value)));
    } else if (n->type == band3::kDataSymbol && n->value) {
        REXLOG_INFO("print_debug: {}", reinterpret_cast<const char*>(REX_RAW_ADDR(n->value)));
    } else if (n->type == band3::kDataString && n->value) {
        const uint32_t str = REX_LOAD_U32(n->value);
        REXLOG_INFO("print_debug: {}",
                    str ? reinterpret_cast<const char*>(REX_RAW_ADDR(str)) : "");
    } else {
        REXLOG_INFO("print_debug: <type {}> {:08X}", static_cast<uint32_t>(n->type),
                    static_cast<uint32_t>(n->value));
    }
    ReturnInt(ctx, base, 1);
}

// {rb3e_api_version}: RB3E's DTA API version, which Deluxe checks before
// using any of these; band3 answers as the version it follows (1, the song
// track and gem queries below)
static void ApiVersionHandler(PPCContext& ctx, uint8_t* base) { ReturnInt(ctx, base, 1); }

// {rb3e_build_tag}: RB3E's is its git describe; band3's the same, named, as
// the Liveless Rooms server gets it
static void BuildTagHandler(PPCContext& ctx, uint8_t* base) {
    ReturnSymbol(ctx, base, std::string("band3 ") + band3::BuildTag(), "unknown");
}

// RB3E's build's commit; band3's build doesn't record one
static void CommitHandler(PPCContext& ctx, uint8_t* base) {
    ReturnSymbol(ctx, base, "unknown", "unknown");
}

// {rb3e_is_emulator}: 1 on Xenia and Dolphin, RB3E's "not a console"; band3
// isn't one either.
static void IsEmulatorHandler(PPCContext& ctx, uint8_t* base) { ReturnInt(ctx, base, 1); }

// {rb3e_relaunch_game}: starts band3 again and closes this one, as RB3E
// relaunches the game's executable; returns 0 only when it couldn't
static void RelaunchGameHandler(PPCContext& ctx, uint8_t* base) {
    ReturnInt(ctx, base, 0);
    if (band3::relaunch::StartAgain()) {
        // doesn't return on a guest thread
        rex::system::kernel_state()->TerminateTitle();
    }
}

// {rb3e_delete_songcache}: deletes the song cache the game mounted, at the
// next start (the game has it open); 1 when there was one, as on RB3E
static void DeleteSongCacheHandler(PPCContext& ctx, uint8_t* base) {
    ReturnInt(ctx, base, band3::song_cache::RequestDelete() ? 1 : 0);
}

// {rb3e_get_song_count}: the songs in the library, as the Music Library counts
// them (RB3E counts every song's metadata it loaded, which is close)
static void GetSongCountHandler(PPCContext& ctx, uint8_t* base) {
    ReturnInt(ctx, base, static_cast<int32_t>(band3::songs::RankedIds(ctx, base).size()));
}

// {rb3e_get_<field> song_id}: the song's title, artist and so on as a symbol,
// or rb3e_no_<field> for an ID no song has
static void SongFieldHandler(PPCContext& ctx, uint8_t* base,
                             std::string band3::songs::Song::*field, const char* missing,
                             const char* name) {
    const band3::DataNode* n = ArgNode(ctx, base, ctx.r4.u32, 1);
    if (!n || n->type != band3::kDataInt) {
        REXLOG_WARN("{}: expects a song ID", name);
        ReturnSymbol(ctx, base, missing, missing);
        return;
    }
    const auto id = static_cast<int32_t>(static_cast<uint32_t>(n->value));
    const std::optional<band3::songs::Song> song = band3::songs::Get(ctx, base, id);
    if (!song) {
        REXLOG_WARN("{}: no song has ID {}", name, id);
        ReturnSymbol(ctx, base, missing, missing);
        return;
    }
    ReturnSymbol(ctx, base, (*song).*field, missing);
}

static void GetSongNameHandler(PPCContext& ctx, uint8_t* base) {
    SongFieldHandler(ctx, base, &band3::songs::Song::title, "rb3e_no_song_name",
                     "rb3e_get_song_name");
}
static void GetArtistHandler(PPCContext& ctx, uint8_t* base) {
    SongFieldHandler(ctx, base, &band3::songs::Song::artist, "rb3e_no_artist",
                     "rb3e_get_artist");
}
static void GetAlbumHandler(PPCContext& ctx, uint8_t* base) {
    SongFieldHandler(ctx, base, &band3::songs::Song::album, "rb3e_no_album", "rb3e_get_album");
}
static void GetGenreHandler(PPCContext& ctx, uint8_t* base) {
    SongFieldHandler(ctx, base, &band3::songs::Song::genre, "rb3e_no_genre", "rb3e_get_genre");
}
static void GetOriginHandler(PPCContext& ctx, uint8_t* base) {
    SongFieldHandler(ctx, base, &band3::songs::Song::origin, "rb3e_no_origin",
                     "rb3e_get_origin");
}

// {rb3e_set_venue venue}: forces the venue for this session, as forced_venue
// does; returns 1
static void SetVenueHandler(PPCContext& ctx, uint8_t* base) {
    const char* venue = ArgString(ctx, base, ctx.r4.u32, 1);
    // RB3E's limit: its config keeps 29 characters
    if (!venue || strlen(venue) > 29) {
        REXLOG_WARN("rb3e_set_venue: expects a venue name of up to 29 characters");
    } else {
        REXLOG_INFO("rb3e_set_venue: forcing venue {} for this session", venue);
        band3::settings::SetSessionVenue(venue);
    }
    ReturnInt(ctx, base, 1);
}

static void ReturnFloat(PPCContext& ctx, uint8_t* base, float value) {
    Return(ctx, base, std::bit_cast<uint32_t>(value), band3::kDataFloat);
}

// the speed argument of rb3e_change_music_speed and rb3e_change_track_speed,
// an int or a float; 1 for anything else or a speed that isn't above 0, as
// RB3E falls back to 1 (and a track speed of 0 would divide by it)
static double SpeedArg(PPCContext& ctx, uint8_t* base, const char* name) {
    const band3::DataNode* n = ArgNode(ctx, base, ctx.r4.u32, 1);
    double speed = 0;
    if (n && n->type == band3::kDataFloat) {
        speed = std::bit_cast<float>(static_cast<uint32_t>(n->value));
    } else if (n && n->type == band3::kDataInt) {
        speed = static_cast<int32_t>(static_cast<uint32_t>(n->value));
    }
    if (!(speed > 0)) {
        REXLOG_WARN("{}: expects a speed above 0; using 1", name);
        return 1.0;
    }
    return speed;
}

// {rb3e_change_music_speed speed}: the song speed for this session (the
// song_speed setting keeps its value), from the next song; returns 1
static void ChangeMusicSpeedHandler(PPCContext& ctx, uint8_t* base) {
    const double speed = SpeedArg(ctx, base, "rb3e_change_music_speed");
    REXLOG_INFO("rb3e_change_music_speed: {:.2f} for this session", speed);
    band3::settings::SetSessionSongSpeed(speed);
    ReturnInt(ctx, base, 1);
}

// {rb3e_change_track_speed speed}: likewise for the track speed
static void ChangeTrackSpeedHandler(PPCContext& ctx, uint8_t* base) {
    const double speed = SpeedArg(ctx, base, "rb3e_change_track_speed");
    REXLOG_INFO("rb3e_change_track_speed: {:.2f} for this session", speed);
    band3::settings::SetSessionTrackSpeed(speed);
    ReturnInt(ctx, base, 1);
}

static void GetMusicSpeedHandler(PPCContext& ctx, uint8_t* base) {
    ReturnFloat(ctx, base, static_cast<float>(band3::settings::SongSpeed()));
}

static void GetTrackSpeedHandler(PPCContext& ctx, uint8_t* base) {
    ReturnFloat(ctx, base, static_cast<float>(band3::settings::TrackSpeed()));
}

// {rb3e_local_ip}: this PC's address on the local network, which Deluxe shows
// with the web server's port for its party mode
static void LocalIpHandler(PPCContext& ctx, uint8_t* base) {
    ReturnSymbol(ctx, base, band3::net::LocalAddress(), "(not connected)");
}

// RB3E API 1's queries of the loaded song's tracks and gems. Each reads the
// song TheSongDB holds (none outside a song), the way the game's own code does.
// Layouts, from band3's code:
//   SongDB:      +0x4 SongData*
//   SongData:    +0x44 vector<TrackInfo*>, +0xB0 vector<GameGemDB*> (one per
//                track; SongData::GetGemListByDiff)
//   TrackInfo:   +0x10 TrackType (its constructor)
//   GameGemDB:   +0x0 vector<GameGemList*>, one per difficulty
//   GameGemList: +0x4 vector<GameGem>, 0x44 bytes each (its push_back)
//   GameGem:     +0x0 ms (float), +0x4 tick, +0x8 duration ms (u16),
//                +0xA duration ticks (u16), +0xC slots (MergeChordGems ORs them)
// The game's vectors are begin, end, capacity.
namespace gems {
constexpr uint32_t kTheSongDB = 0x82E023F8;  // SongDB*
constexpr uint32_t kSongData = 0x4;
constexpr uint32_t kTrackInfos = 0x44;
constexpr uint32_t kGemDBs = 0xB0;
constexpr uint32_t kTrackType = 0x10;
constexpr uint32_t kGemListGems = 0x4;
constexpr uint32_t kGemSize = 0x44;
constexpr uint32_t kGemMs = 0x0;
constexpr uint32_t kGemTick = 0x4;
constexpr uint32_t kGemDurationMs = 0x8;
constexpr uint32_t kGemDurationTicks = 0xA;
constexpr uint32_t kGemSlots = 0xC;

// RB3E's track types (TrackType) that have a 17-fret fallback
constexpr int32_t kRealGuitar = 6;
constexpr int32_t kRealGuitar22Fret = 7;
constexpr int32_t kRealBass = 8;
constexpr int32_t kRealBass22Fret = 9;

// the vector at `vec`: its first element and how many `size`-byte elements
// it holds (0 for an empty or broken one)
struct Vector {
    uint32_t begin = 0;
    int32_t count = 0;
};
static Vector LoadVector(uint8_t* base, uint32_t vec, uint32_t size) {
    const uint32_t begin = REX_LOAD_U32(vec);
    const uint32_t end = REX_LOAD_U32(vec + 4);
    if (!begin || end < begin) return {};
    return {begin, static_cast<int32_t>((end - begin) / size)};
}

// the loaded SongData, or 0 (logged under `name`) when there's none
static uint32_t SongData(uint8_t* base, const char* name) {
    const uint32_t song_db = REX_LOAD_U32(gems::kTheSongDB);
    const uint32_t song_data = song_db ? REX_LOAD_U32(song_db + gems::kSongData) : 0;
    if (!song_data) REXLOG_DEBUG("{}: no song data loaded", name);
    return song_data;
}

// the song's TrackInfo pointers; none outside a song, and RB3E's cap of 32
// against a bad vector
static Vector TrackInfos(uint8_t* base, uint32_t song_data) {
    const Vector infos = LoadVector(base, song_data + gems::kTrackInfos, 4);
    return infos.count <= 32 ? infos : Vector{};
}

// the index of the song's first track of `type`, or -1; 22-fret pro guitar
// and bass fall back to the 17-fret track, as RB3E's do (the game charts one)
static int32_t FindTrack(uint8_t* base, uint32_t song_data, int32_t type) {
    const Vector infos = TrackInfos(base, song_data);
    for (int32_t i = 0; i < infos.count; ++i) {
        const uint32_t info = REX_LOAD_U32(infos.begin + i * 4);
        if (info && static_cast<int32_t>(REX_LOAD_U32(info + gems::kTrackType)) == type) return i;
    }
    if (type == kRealGuitar22Fret) return FindTrack(base, song_data, kRealGuitar);
    if (type == kRealBass22Fret) return FindTrack(base, song_data, kRealBass);
    return -1;
}
}  // namespace gems

// argument `index` as an int, or nullopt when it isn't one
static std::optional<int32_t> IntArg(PPCContext& ctx, uint8_t* base, int index) {
    const band3::DataNode* n = ArgNode(ctx, base, ctx.r4.u32, index);
    if (!n || n->type != band3::kDataInt) return std::nullopt;
    return static_cast<int32_t>(static_cast<uint32_t>(n->value));
}

// the gems of {name tracktype difficulty ...}: their first and how many, or
// a count of -1 when there's no such list
static gems::Vector GemListArgs(PPCContext& ctx, uint8_t* base, const char* name) {
    const gems::Vector none{0, -1};
    const std::optional<int32_t> type = IntArg(ctx, base, 1);
    const std::optional<int32_t> difficulty = IntArg(ctx, base, 2);
    if (!type || !difficulty) {
        REXLOG_WARN("{}: expects <tracktype> <difficulty> as ints", name);
        return none;
    }
    const uint32_t song_data = gems::SongData(base, name);
    if (!song_data) return none;

    const int32_t track = gems::FindTrack(base, song_data, *type);
    const gems::Vector dbs = gems::LoadVector(base, song_data + gems::kGemDBs, 4);
    if (track < 0 || track >= dbs.count) {
        REXLOG_DEBUG("{}: the song has no track of type {}", name, *type);
        return none;
    }
    const uint32_t db = REX_LOAD_U32(dbs.begin + track * 4);
    const gems::Vector lists = db ? gems::LoadVector(base, db, 4) : gems::Vector{};
    if (*difficulty < 0 || *difficulty >= lists.count) {
        REXLOG_DEBUG("{}: difficulty {} is out of range (the track has {})", name, *difficulty,
                     lists.count);
        return none;
    }
    const uint32_t list = REX_LOAD_U32(lists.begin + *difficulty * 4);
    if (!list) return none;
    return gems::LoadVector(base, list + gems::kGemListGems, gems::kGemSize);
}

// the gem {name tracktype difficulty gemid ...} names, or 0
static uint32_t GemArgs(PPCContext& ctx, uint8_t* base, const char* name) {
    const gems::Vector list = GemListArgs(ctx, base, name);
    if (list.count < 0) return 0;
    const std::optional<int32_t> id = IntArg(ctx, base, 3);
    if (!id) {
        REXLOG_WARN("{}: expects <tracktype> <difficulty> <gemid> as ints", name);
        return 0;
    }
    if (*id < 0 || *id >= list.count) {
        REXLOG_DEBUG("{}: gem {} is out of range (the list has {})", name, *id, list.count);
        return 0;
    }
    return list.begin + *id * gems::kGemSize;
}

// the optional 5th argument of the gem time queries: 1 asks for when the gem
// ends (its start plus its sustain) instead of when it starts
static bool WantsGemEnd(PPCContext& ctx, uint8_t* base) {
    const std::optional<int32_t> end = IntArg(ctx, base, 4);
    return end && *end == 1;
}

// {rb3e_get_game_gem_time_ms tracktype difficulty gemid [end]}: the gem's
// time in ms, or -1.0
static void GetGameGemTimeMsHandler(PPCContext& ctx, uint8_t* base) {
    const uint32_t gem = GemArgs(ctx, base, "rb3e_get_game_gem_time_ms");
    if (!gem) {
        ReturnFloat(ctx, base, -1.0f);
        return;
    }
    float ms = std::bit_cast<float>(REX_LOAD_U32(gem + gems::kGemMs));
    if (WantsGemEnd(ctx, base)) ms += REX_LOAD_U16(gem + gems::kGemDurationMs);
    ReturnFloat(ctx, base, ms);
}

// {rb3e_get_game_gem_time_tick tracktype difficulty gemid [end]}: likewise in
// ticks, or -1
static void GetGameGemTimeTickHandler(PPCContext& ctx, uint8_t* base) {
    const uint32_t gem = GemArgs(ctx, base, "rb3e_get_game_gem_time_tick");
    if (!gem) {
        ReturnInt(ctx, base, -1);
        return;
    }
    int32_t tick = static_cast<int32_t>(REX_LOAD_U32(gem + gems::kGemTick));
    if (WantsGemEnd(ctx, base)) tick += REX_LOAD_U16(gem + gems::kGemDurationTicks);
    ReturnInt(ctx, base, tick);
}

// {rb3e_get_game_gem_count tracktype difficulty}: the list's gems, or -1
static void GetGameGemCountHandler(PPCContext& ctx, uint8_t* base) {
    ReturnInt(ctx, base, GemListArgs(ctx, base, "rb3e_get_game_gem_count").count);
}

// {rb3e_get_game_gem_slots tracktype difficulty gemid}: the gem's lanes, keys
// or strings, a bit each, or -1
static void GetGameGemSlotsHandler(PPCContext& ctx, uint8_t* base) {
    const uint32_t gem = GemArgs(ctx, base, "rb3e_get_game_gem_slots");
    ReturnInt(ctx, base, gem ? static_cast<int32_t>(REX_LOAD_U32(gem + gems::kGemSlots)) : -1);
}

// {rb3e_get_track_count}: the loaded song's tracks, or -1 outside a song
static void GetTrackCountHandler(PPCContext& ctx, uint8_t* base) {
    const uint32_t song_data = gems::SongData(base, "rb3e_get_track_count");
    ReturnInt(ctx, base, song_data ? gems::TrackInfos(base, song_data).count : -1);
}

// {rb3e_get_track_type trackindex}: that track's type (RB3E's TrackType), or -1
static void GetTrackTypeHandler(PPCContext& ctx, uint8_t* base) {
    const std::optional<int32_t> index = IntArg(ctx, base, 1);
    if (!index) {
        REXLOG_WARN("rb3e_get_track_type: expects <trackindex> as an int");
        ReturnInt(ctx, base, -1);
        return;
    }
    const uint32_t song_data = gems::SongData(base, "rb3e_get_track_type");
    const gems::Vector infos = song_data ? gems::TrackInfos(base, song_data) : gems::Vector{};
    const uint32_t info =
        *index >= 0 && *index < infos.count ? REX_LOAD_U32(infos.begin + *index * 4) : 0;
    if (!info) {
        REXLOG_DEBUG("rb3e_get_track_type: track {} is out of range (the song has {})", *index,
                     infos.count);
        ReturnInt(ctx, base, -1);
        return;
    }
    ReturnInt(ctx, base, static_cast<int32_t>(REX_LOAD_U32(info + gems::kTrackType)));
}

// {rb3e_has_track_type tracktype}: 1 when the loaded song has a track of that
// type, with the gem queries' 22-fret fallback, else 0
static void HasTrackTypeHandler(PPCContext& ctx, uint8_t* base) {
    const std::optional<int32_t> type = IntArg(ctx, base, 1);
    if (!type) {
        REXLOG_WARN("rb3e_has_track_type: expects <tracktype> as an int");
        ReturnInt(ctx, base, 0);
        return;
    }
    const uint32_t song_data = gems::SongData(base, "rb3e_has_track_type");
    ReturnInt(ctx, base, song_data && gems::FindTrack(base, song_data, *type) >= 0 ? 1 : 0);
}

// register our custom DTA funcs after the game itself inits most of the DTA functions
extern "C" void __imp__DataInitFuncs(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(DataInitFuncs) {
    __imp__DataInitFuncs(ctx, base);
	
	// override exit to properly terminate the title
	// this will usually just crash things but this way we can properly handle this so in the future we can add a proper "Exit Game" button to main menu
	RegisterDTAFunc(ctx, base, "exit", ExitHandler);
	
	// RB3Enhanced's command for mods to send their own network events
	RegisterDTAFunc(ctx, base, "rb3e_send_event_string", SendEventStringHandler);

	// and the rest of RB3E's
	RegisterDTAFunc(ctx, base, "print_debug", PrintDebugHandler);
	RegisterDTAFunc(ctx, base, "rb3e_api_version", ApiVersionHandler);
	RegisterDTAFunc(ctx, base, "rb3e_build_tag", BuildTagHandler);
	RegisterDTAFunc(ctx, base, "rb3e_commit", CommitHandler);
	RegisterDTAFunc(ctx, base, "rb3e_is_emulator", IsEmulatorHandler);
	RegisterDTAFunc(ctx, base, "rb3e_relaunch_game", RelaunchGameHandler);
	RegisterDTAFunc(ctx, base, "rb3e_delete_songcache", DeleteSongCacheHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_song_count", GetSongCountHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_song_name", GetSongNameHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_artist", GetArtistHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_album", GetAlbumHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_genre", GetGenreHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_origin", GetOriginHandler);
	RegisterDTAFunc(ctx, base, "rb3e_set_venue", SetVenueHandler);
	RegisterDTAFunc(ctx, base, "rb3e_local_ip", LocalIpHandler);
	RegisterDTAFunc(ctx, base, "rb3e_change_music_speed", ChangeMusicSpeedHandler);
	RegisterDTAFunc(ctx, base, "rb3e_change_track_speed", ChangeTrackSpeedHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_music_speed", GetMusicSpeedHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_track_speed", GetTrackSpeedHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_game_gem_time_ms", GetGameGemTimeMsHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_game_gem_time_tick", GetGameGemTimeTickHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_game_gem_count", GetGameGemCountHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_game_gem_slots", GetGameGemSlotsHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_track_count", GetTrackCountHandler);
	RegisterDTAFunc(ctx, base, "rb3e_get_track_type", GetTrackTypeHandler);
	RegisterDTAFunc(ctx, base, "rb3e_has_track_type", HasTrackTypeHandler);

	// custom functions should go here
}

#include "DataArray.h"
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <bit>
#include <optional>
#include <string>
#include <unordered_map>
#include <cstring>

#include "generated/band3_init.h"
#include "src/Net/events.h"
#include "src/Net/local_address.h"
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
    REXLOG_INFO("Game requested exit, terminating title properly");
	
	// the proper way to terminate the title accoridng to Rexglue SDK
    rex::system::kernel_state()->TerminateTitle();
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
// which RB3 Deluxe calls when it finds RB3E. All but the music and track
// speed ones, which come with the speed setting itself.

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
// using any of these; band3 answers as the version it follows
static void ApiVersionHandler(PPCContext& ctx, uint8_t* base) { ReturnInt(ctx, base, 0); }

static void BuildTagHandler(PPCContext& ctx, uint8_t* base) {
    ReturnSymbol(ctx, base, band3::events::kBuildTag, "unknown");
}

// RB3E's build's commit; band3's build doesn't record one
static void CommitHandler(PPCContext& ctx, uint8_t* base) {
    ReturnSymbol(ctx, base, "unknown", "unknown");
}

// {rb3e_is_emulator}: 1 on Xenia and Dolphin, where RB3E leaves out what only
// works on a console (relaunching the game, its content APIs). band3 isn't
// one either.
static void IsEmulatorHandler(PPCContext& ctx, uint8_t* base) { ReturnInt(ctx, base, 1); }

// {rb3e_relaunch_game}: RB3E restarts default.xex; band3 can't restart itself
// yet, so it says so and returns 0, as RB3E does when it can't
static void RelaunchGameHandler(PPCContext& ctx, uint8_t* base) {
    REXLOG_WARN("rb3e_relaunch_game: band3 can't relaunch the game yet; restart it yourself");
    ReturnInt(ctx, base, 0);
}

// {rb3e_delete_songcache}: RB3E deletes the song cache it saw mounted; band3
// doesn't track it yet, so nothing is deleted and it returns 0 (failed)
static void DeleteSongCacheHandler(PPCContext& ctx, uint8_t* base) {
    REXLOG_WARN("rb3e_delete_songcache: band3 can't delete the song cache yet");
    ReturnInt(ctx, base, 0);
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

// {rb3e_local_ip}: this PC's address on the local network, which Deluxe shows
// with the web server's port for its party mode
static void LocalIpHandler(PPCContext& ctx, uint8_t* base) {
    ReturnSymbol(ctx, base, band3::net::LocalAddress(), "(not connected)");
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

	// custom functions should go here
}

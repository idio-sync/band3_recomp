#include <rex/system/kernel_state.h>
#include <rex/logging.h>
#include <atomic>
#include <cstring>
#include <vector>
#include <string_view>
#include <set>
#include "generated/band3_init.h"
#include "config.h"
#include "settings.h"
#include "src/Input/instruments.h"
#include "src/Net/events.h"
#include "src/Net/home_assistant.h"
#include "src/Game/Modifiers.h"
#include "src/Game/Symbol.h"
#include "src/Test/game_state.h"
#include "src/Test/test_server.h"
#include "src/Video/music_video.h"
#include "src/Video/video_venue.h"
#include <random>
#include <cstdio>

static std::set<size_t> g_consumed_args;

namespace {

const char* SubtypeName(uint8_t subtype) {
    using namespace band3::input;
    switch (subtype) {
    case kSubtypeGamepad: return "gamepad";
    case kSubtypeGuitar: return "guitar";
    case kSubtypeGuitarAlternate: return "guitar";
    case kSubtypeDrums: return "drums";
    case kSubtypeGuitarBass: return "bass";
    case kSubtypeKeytar: return "keytar";
    case kSubtypeProGuitar: return "pro guitar";
    default: return "unknown";
    }
}

// Logs each subtype the first time RB3 reads it, and what it plays as, so a
// report from a new setup (a Steam Deck, Steam Input, SDL's view of an Xbox 360
// instrument) says whether the instrument's own type reached the game.
// ReadSingleXinputJoypad runs every poll, so each value is logged once.
std::atomic<uint64_t> g_logged_subtypes[4];

void LogSubtypeOnce(uint8_t subtype, long played_as) {
    const uint64_t bit = uint64_t{1} << (subtype % 64);
    if (g_logged_subtypes[subtype / 64].fetch_or(bit) & bit) return;
    if (played_as == subtype) {
        REXLOG_INFO("A controller reports type {} ({}); kept", subtype, SubtypeName(subtype));
    } else {
        REXLOG_INFO("A controller reports type {} ({}); playing as {} ({}), from controller_type",
                    subtype, SubtypeName(subtype), played_as,
                    SubtypeName(static_cast<uint8_t>(played_as)));
    }
}

}

// r11 is the subtype ReadSingleXinputJoypad just read from the device's
// capabilities. Only devices RB3 wouldn't take as an instrument (gamepads, the
// keyboard) are overridden, so instruments that report their own type keep it.
void ControllerHook(PPCRegister& r11) {
    const auto subtype = static_cast<uint8_t>(r11.u64);
    long overrideType = band3::settings::Startup().controller_type;
    if (overrideType == -1 || band3::input::IsRb3InstrumentSubtype(subtype)) {
        LogSubtypeOnce(subtype, subtype);
        return;
    }
    LogSubtypeOnce(subtype, overrideType);
    r11.u64 = overrideType;
}

void UpdateArkHook(PPCRegister& r4) {
	//REXLOG_INFO("Patching update ark path to game dir");
    r4.u64 = 0x82089B50;
}

void SongCountHook(PPCRegister& r3) {
    r3.u64 = 8000; //8000 is apparently what rb3e sets, so use that
}

//replace calls to 822703D0 (bad) with 822703A8 (good)
extern "C" REX_FUNC(App__Run)
{
	REXLOG_INFO("Patching debugger trap");
	RunFunc_AppRunWithoutDebugging(ctx, base);
}

// OptionBool(char* optionName, bool default) - check host cmd line aargs for boolean options
// these do not needa value, just being on the command line implies they are true
extern "C" REX_FUNC(OptionBool)
{
	const char* optionName = reinterpret_cast<const char*>(
		base + static_cast<uint32_t>(ctx.r3.u64));
	bool defaultVal = ctx.r4.u64 != 0;

	std::string dashOption = std::string("-") + optionName;
	const auto& args = band3::GetArgs();
	for (size_t i = 0; i < args.size(); i++) {
		if (g_consumed_args.count(i)) continue;
		if (args[i] == dashOption) {
			REXLOG_INFO("OptionBool(\"{}\") = {} (found)", optionName, !defaultVal);
			g_consumed_args.insert(i);
			ctx.r3.u64 = !defaultVal;
			return;
		}
	}

	ctx.r3.u64 = defaultVal;
}

// OptionStr(char* option, char* default) - check host cmd line args for string options
extern "C" REX_FUNC(OptionStr)
{
	const char* option = reinterpret_cast<const char*>(
		base + static_cast<uint32_t>(ctx.r3.u64));
	uint32_t defaultPtr = static_cast<uint32_t>(ctx.r4.u64);

	std::string dashOption = std::string("-") + option;
	REXLOG_INFO("Checking for string arg {}", option);
	const auto& args = band3::GetArgs();
	for (size_t i = 0; i < args.size(); i++) {
		if (g_consumed_args.count(i)) continue;
		if (args[i] == dashOption && i + 1 < args.size()) {
			const std::string& value = args[i + 1];
			auto* mem = rex::system::kernel_memory();
			uint32_t len = static_cast<uint32_t>(value.size() + 1);
			uint32_t str_guest = mem->SystemHeapAlloc(len, 1);
			std::memcpy(base + str_guest, value.c_str(), len);
			REXLOG_INFO("OptionStr(\"{}\") = \"{}\" (found)", option, value);
			g_consumed_args.insert(i);
			g_consumed_args.insert(i + 1);
			ctx.r3.u64 = str_guest;
			return;
		}
	}

	// PreInitSystem asks for "define" until it gets null, and makes each answer a
	// macro the game's scripts see. With rb3e_mode, once the command line's
	// -define values run out, the answers go on with RB3E's (its DefinesHook in
	// source/rb3enhanced.c), which Rock Band 3 Deluxe checks to use RB3E. Not
	// RB3E_EMULATOR: Deluxe reads a Dolphin path and drops its reboot options
	// with it.
	static constexpr const char* kRb3eDefines[] = {"RB3E", "RB3E_HAS_VERSION"};
	static size_t rb3e_defines_given = 0;
	if (std::strcmp(option, "define") == 0 && band3::settings::Startup().rb3e_mode &&
	    rb3e_defines_given < std::size(kRb3eDefines)) {
		const char* define = kRb3eDefines[rb3e_defines_given++];
		const uint32_t len = static_cast<uint32_t>(std::strlen(define) + 1);
		const uint32_t str_guest = rex::system::kernel_memory()->SystemHeapAlloc(len, 1);
		std::memcpy(base + str_guest, define, len);
		REXLOG_INFO("OptionStr(\"define\") = \"{}\" (rb3e_mode)", define);
		ctx.r3.u64 = str_guest;
		return;
	}

	ctx.r3.u64 = defaultPtr;
}

// Rnd::PreInit - override vsync from the rnd_sync setting
extern "C" void __imp__Rnd__PreInit(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(Rnd__PreInit)
{
	uint32_t rnd_this = static_cast<uint32_t>(ctx.r3.u64);
	__imp__Rnd__PreInit(ctx, base);

	long sync = band3::settings::Startup().rnd_sync;
	if (sync >= 0) {
		// TODO:
		// get a proper Rnd structure instead of this pointer math
		auto* ptr = reinterpret_cast<rex::be<uint32_t>*>(base + rnd_this + 0xf0);
		*ptr = static_cast<uint32_t>(sync);
		REXLOG_INFO("Rnd::PreInit: sync set to {}", sync);
	}
}

// file checksum patch, just return true always
extern "C" REX_FUNC(StreamChecksum__ValidateChecksum)
{
	ctx.r3.u64 = 1;
}
// file checksum patch, just return
extern "C" REX_FUNC(PlatformMgr__SetDiskError)
{
	return;
}

// nuke metamusic calls, if enabled
extern "C" void __imp__MetaMusic__Load(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(MetaMusic__Load)
{
    if (band3::settings::Startup().disable_metamusic) return;
    __imp__MetaMusic__Load(ctx, base);
}
extern "C" void __imp__MetaMusic__Poll(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(MetaMusic__Poll)
{
    if (band3::settings::Startup().disable_metamusic) return;
    __imp__MetaMusic__Poll(ctx, base);
}
extern "C" void __imp__MetaMusic__Start(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(MetaMusic__Start)
{
    if (band3::settings::Startup().disable_metamusic) return;
    __imp__MetaMusic__Start(ctx, base);
}
extern "C" void __imp__MetaMusic__Loaded(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(MetaMusic__Loaded)
{
    if (band3::settings::Startup().disable_metamusic) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__MetaMusic__Loaded(ctx, base);
}

// no more demos!
extern "C" void __imp__SongMgr__IsDemo(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(SongMgr__IsDemo)
{
    ctx.r3.u64 = 0;
    return;
}

//Set venue from the forced_venue setting, read each time so it can be changed mid-game
extern "C" void __imp__MetaPerformer__SetVenue(PPCContext& ctx, uint8_t* base);
REX_EXTERN(MetaPerformer__Song);

static std::mt19937& VenueRng() {
    static std::mt19937 rng{ std::random_device{}() };
    return rng;
}

// The song the venue is for: MetaPerformer::Song(Symbol* out, MetaPerformer*),
// the setlist's next (SelectRandomVenue reads the setlist too); empty if none
static std::string UpcomingSong(const PPCContext& ctx, uint8_t* base) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    const uint32_t out = call.r1.u32 + 0x80;
    call.r3.u64 = out;
    call.r4.u64 = ctx.r3.u32;  // SetVenue's MetaPerformer
    MetaPerformer__Song(call, base);
    const uint32_t name = REX_LOAD_U32(out);
    return name ? std::string(reinterpret_cast<const char*>(base + name)) : std::string();
}

// music_video_venue_chance: whether this venue becomes a video venue, for the
// song's music video (src/Video/video_venue.h)
static bool VideoVenueForSong(const PPCContext& ctx, uint8_t* base, bool black_background) {
    band3::video::VideoVenueInputs in;
    in.black_background = black_background;
    in.music_videos = band3::video::MusicVideosOn();
    in.chance = REXCVAR_GET(music_video_venue_chance);
    if (!in.music_videos || in.chance <= 0 || black_background) return false;
    const std::string song = UpcomingSong(ctx, base);
    in.has_video = !song.empty() && band3::video::HasMusicVideo(song);
    if (!in.has_video) return false;
    in.roll = std::uniform_real_distribution<double>(0.0, 1.0)(VenueRng());
    const bool pick = band3::video::PickVideoVenue(in);
    REXLOG_INFO("{} has a music video: {} ({}% of the time)", song,
                pick ? "a video venue" : "the game's venue this time", in.chance);
    return pick;
}
// reports the venue actually being set (r4 = venue Symbol) as an RB3E event
// and to GameState, for the test harness and Home Assistant
static void SetVenueAndReport(PPCContext& ctx, uint8_t* base) {
    if (band3::events::Enabled() && ctx.r4.u32)
        band3::events::SendString(band3::events::kVenueName,
                                  reinterpret_cast<const char*>(base + ctx.r4.u32));
    if ((band3::test::Enabled() || band3::ha::Configured()) && ctx.r4.u32)
        band3::test::GameState::Get().SetVenue(reinterpret_cast<const char*>(base + ctx.r4.u32));
    __imp__MetaPerformer__SetVenue(ctx, base);
}
extern "C" REX_FUNC(MetaPerformer__SetVenue)
{
    std::string forced = band3::settings::ForcedVenue();
    // drawn as it is, unless the black background below makes it a black one
    band3::video::SetBlackVenue(false);

    if (forced.empty() || forced == "false") {
        // RB3E's black background modifier: venue "none", the track over black;
        // a forced venue still wins, as on RB3E
        const bool black = band3::modifiers::Active(ctx, base, "mod_black_background");
        if (black && REXCVAR_GET(black_background_lights)) {
            // not "none", which has no lights: a video venue drawn black with
            // no band (src/Hooks/music_video.cpp), whose lights go on
            REXLOG_INFO("Black background modifier: a video venue drawn black, for its lights");
            band3::video::SetBlackVenue(true);
        } else {
            if (black) {
                if (const uint32_t none = band3::modifiers::Intern(ctx, base, "none")) {
                    REXLOG_INFO("Black background modifier: no venue");
                    ctx.r4.u64 = none;
                }
            }
            // a song with a music video: a video venue, picked as forced_venue
            // video picks one
            if (!VideoVenueForSong(ctx, base, black)) {
                SetVenueAndReport(ctx, base);
                return;
            }
        }
        forced = "video";
    }

    static const char* small_club[] = { "01","02","03","04","05","06","10","11","13","14","15" };
    static const char* big_club[]   = { "01","02","04","15","17" };
    static const char* arena[]      = { "01","04","06","07","10","11","12" };
    static const char* festival[]   = { "01","02" };
    static const char* video[]      = { "01","02","03","04","05","06","07" };

    std::mt19937& rng = VenueRng();

    auto pick = [&rng](const char* const* list, size_t n) -> const char* {
        std::uniform_int_distribution<size_t> dist(0, n - 1);
        return list[dist(rng)];
    };

    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
        return s;
    };

    std::string choice;
    {
        std::vector<std::string_view> items;
        std::string_view sv(forced);

        while (!sv.empty()) {
            size_t comma = sv.find(',');
            std::string_view part = (comma == std::string_view::npos) ? sv : sv.substr(0, comma);
            part = trim(part);
            if (!part.empty() && part != "false")
                items.push_back(part);
            if (comma == std::string_view::npos) break;
            sv.remove_prefix(comma + 1);
        }

        if (items.empty()) {
            SetVenueAndReport(ctx, base);
            return;
        }

        std::uniform_int_distribution<size_t> dist(0, items.size() - 1);
        choice.assign(items[dist(rng)]);
    }

    std::string resolved = choice;

    struct VenueGroup { const char* name; const char* const* list; size_t count; };
    static const VenueGroup groups[] = {
        { "small_club", small_club, std::size(small_club) },
        { "big_club",   big_club,   std::size(big_club)   },
        { "arena",      arena,      std::size(arena)      },
        { "festival",   festival,   std::size(festival)   },
        { "video",      video,      std::size(video)      },
    };

    for (const auto& g : groups) {
        if (choice == g.name) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s_%s", g.name, pick(g.list, g.count));
            resolved = buf;
            break;
        }
    }

    // Symbols compare by pointer, so the venue has to be interned in the game's table
    uint32_t venue_sym = band3::Symbol(ctx, base, resolved.c_str()).value(base);
    if (!venue_sym) {
        SetVenueAndReport(ctx, base);
        return;
    }

    REXLOG_INFO("Forcing venue to \"{}\"", resolved);
    ctx.r4.u64 = venue_sym;
    SetVenueAndReport(ctx, base);
}

//force this import so dlc can load through its roundabout way
REX_EXTERN(__imp__XamContentAggregateCreateEnumerator);
//I'll be the roundabout
//The words will make you out and out
//You spend the day your wayyyyy
//Call it morning driving through the south in and out the valleyyyyyyyyyyyy
[[gnu::used]] static volatile auto imp_XamContentAggregateCreateEnumerator = &__imp__XamContentAggregateCreateEnumerator;

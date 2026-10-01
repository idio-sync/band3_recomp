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
#include "src/Game/Symbol.h"
#include "src/Test/game_state.h"
#include "src/Test/test_server.h"
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
// reports the venue actually being set (r4 = venue Symbol) as an RB3E event
// and to the test harness
static void SetVenueAndReport(PPCContext& ctx, uint8_t* base) {
    if (band3::events::Enabled() && ctx.r4.u32)
        band3::events::SendString(band3::events::kVenueName,
                                  reinterpret_cast<const char*>(base + ctx.r4.u32));
    if (band3::test::Enabled() && ctx.r4.u32)
        band3::test::GameState::Get().SetVenue(reinterpret_cast<const char*>(base + ctx.r4.u32));
    __imp__MetaPerformer__SetVenue(ctx, base);
}
extern "C" REX_FUNC(MetaPerformer__SetVenue)
{
    const std::string forced = band3::settings::ForcedVenue();

    if (forced.empty() || forced == "false") {
        SetVenueAndReport(ctx, base);
        return;
    }

    static const char* small_club[] = { "01","02","03","04","05","06","10","11","13","14","15" };
    static const char* big_club[]   = { "01","02","04","15","17" };
    static const char* arena[]      = { "01","04","06","07","10","11","12" };
    static const char* festival[]   = { "01","02" };
    static const char* video[]      = { "01","02","03","04","05","06","07" };

    static std::mt19937 rng{ std::random_device{}() };

    auto pick = [](const char* const* list, size_t n) -> const char* {
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

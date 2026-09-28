#include "config.h"
#include "ThirdParty/inih/INIReader.h"
#include <rex/logging.h>

#ifdef _WIN32
#include <cstdlib>
#include <windows.h>
#else
#include <cstdlib>
#include <fstream>
#endif

namespace band3 {

static Config g_config;
static std::vector<std::string> g_args;
static bool g_args_initialized = false;

const Config& GetConfig() { return g_config; }

void LoadConfig(const char* path) {
    // inih keeps parsing past a bad line, so only a missing file loses the settings;
    // either way the defaults below still apply and the args are still injected
    INIReader reader(path);
    if (reader.ParseError() == -1) {
        REXLOG_WARN("Failed to open {}, using defaults", path);
    } else if (reader.ParseError() != 0) {
        REXLOG_WARN("{}: {}; the other settings still apply", path, reader.ParseErrorMessage());
    }

    g_config.controller_type =
        reader.GetInteger("controller", "type", g_config.controller_type);
    g_config.input_backend =
        reader.Get("controller", "input_backend", g_config.input_backend);
    g_config.sync =
        reader.GetInteger("rnd", "sync", g_config.sync);
    g_config.refresh_rate =
        reader.GetInteger("rnd", "refresh_rate", g_config.refresh_rate);
    g_config.forced_venue =
        reader.Get("venue", "forced_venue", g_config.forced_venue);
    g_config.fullscreen =
        reader.GetBoolean("window", "fullscreen", g_config.fullscreen);
    g_config.width =
        reader.GetInteger("window", "width", g_config.width);
    g_config.height =
        reader.GetInteger("window", "height", g_config.height);
    g_config.fast_start =
        reader.GetBoolean("game", "fast_start", g_config.fast_start);
    g_config.disable_metamusic =
        reader.GetBoolean("game", "disable_metamusic", g_config.disable_metamusic);
    // [game] is where band3_config.ini documents it; [paths] kept for older inis
    g_config.game_data_root =
        reader.Get("game", "game_data_root",
                   reader.Get("paths", "game_data_root", g_config.game_data_root));
    g_config.lang =
        reader.Get("game", "lang", g_config.lang);
    g_config.disable_approximate_lights =
        reader.GetBoolean("graphics", "disable_approximate_lights", g_config.disable_approximate_lights);
    g_config.disable_hair_shader =
        reader.GetBoolean("graphics", "disable_hair_shader", g_config.disable_hair_shader);
    g_config.compress_character_textures =
        reader.GetBoolean("graphics", "compress_character_textures", g_config.compress_character_textures);
    g_config.fullbright =
        reader.GetBoolean("graphics", "fullbright", g_config.fullbright);
    g_config.disable_even_odd_rendering =
        reader.GetBoolean("graphics", "disable_even_odd_rendering", g_config.disable_even_odd_rendering);
    g_config.username =
        reader.Get("profile", "username", g_config.username);
    g_config.events_enabled =
        reader.GetBoolean("events", "enabled", g_config.events_enabled);
    g_config.events_target =
        reader.Get("events", "target", g_config.events_target);
    g_config.events_port =
        reader.GetInteger("events", "port", g_config.events_port);
    g_config.discord_enabled =
        reader.GetBoolean("discord", "enabled", g_config.discord_enabled);
    g_config.main_heap_size =
        reader.GetInteger("memory", "main_heap_size", g_config.main_heap_size);
    g_config.char_heap_size =
        reader.GetInteger("memory", "char_heap_size", g_config.char_heap_size);
    g_config.max_queued_frames =
        reader.GetInteger("audio", "max_queued_frames", g_config.max_queued_frames);
    g_config.debug_overlay =
        reader.GetBoolean("debug", "overlay", g_config.debug_overlay);
    g_config.native_math =
        reader.GetBoolean("debug", "native_math", g_config.native_math);
    g_config.native_camera_shake =
        reader.GetBoolean("debug", "native_camera_shake", g_config.native_camera_shake);
    g_config.log_level =
        reader.Get("debug", "log_level", g_config.log_level);

    // Inject config-driven args into the command line
    GetArgs();
    if (g_config.fast_start) {
        g_args.push_back("-fast");
    }
    if (!g_config.lang.empty()) {
        g_args.push_back("-lang");
        g_args.push_back(g_config.lang);
    }

    // hard defines for various usecases
    // Rock Band 3 DX identifier
    g_args.push_back("-define");
    g_args.push_back("MHX_PC");
}

std::string ReadForcedVenue(const char* path) {
    INIReader reader(path);
    return reader.Get("venue", "forced_venue", g_config.forced_venue);
}

const std::vector<std::string>& GetArgs() {
    if (!g_args_initialized) {
        g_args_initialized = true;
#ifdef _WIN32
        for (int i = 0; i < __argc; i++) {
            int len = WideCharToMultiByte(CP_UTF8, 0, __wargv[i], -1,
                                          nullptr, 0, nullptr, nullptr);
            std::string s(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, __wargv[i], -1,
                                s.data(), len, nullptr, nullptr);
            g_args.push_back(std::move(s));
        }
#else
		// quality code alert
        std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
        if (cmdline) {
            std::string arg;
            while (std::getline(cmdline, arg, '\0')) {
                g_args.push_back(std::move(arg));
            }
        }
#endif
    }
    return g_args;
}

}

#include "config.h"
#include "settings.h"
#include "paths.h"
#include "ThirdParty/inih/INIReader.h"
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <cstdlib>
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace band3 {

static std::vector<std::string> g_args;
static bool g_args_initialized = false;
static std::filesystem::path g_game_data_root;

namespace {

// where each ini key lands; the SDK's own cvars (window, refresh rate, audio,
// log level, input backend) take the ini's values directly
struct IniSetting {
    const char* section;
    const char* key;
    const char* cvar;
    // the ini documents 0 as "don't override" for these
    bool zero_is_unset = false;
};

constexpr IniSetting kIniSettings[] = {
    {"controller", "type", "controller_type"},
    {"controller", "input_backend", "input_backend"},
    {"rnd", "sync", "rnd_sync"},
    {"rnd", "refresh_rate", "video_mode_refresh_rate", true},
    {"venue", "forced_venue", "forced_venue"},
    {"window", "fullscreen", "fullscreen"},
    {"window", "width", "window_width", true},
    {"window", "height", "window_height", true},
    {"game", "fast_start", "fast_start"},
    {"game", "disable_metamusic", "disable_metamusic"},
    {"game", "lang", "lang"},
    {"graphics", "disable_approximate_lights", "disable_approximate_lights"},
    {"graphics", "disable_hair_shader", "disable_hair_shader"},
    {"graphics", "fullbright", "fullbright"},
    {"graphics", "compress_character_textures", "compress_character_textures"},
    {"graphics", "disable_even_odd_rendering", "disable_even_odd_rendering"},
    {"profile", "username", "username"},
    {"events", "enabled", "events_enabled"},
    {"events", "target", "events_target"},
    {"events", "port", "events_port"},
    {"discord", "enabled", "discord_enabled"},
    {"http", "enabled", "http_enabled"},
    {"http", "port", "http_port"},
    {"http", "address", "http_address"},
    {"http", "allow_cors", "http_allow_cors"},
    {"http", "allow_scripts", "http_allow_scripts"},
    {"audio", "max_queued_frames", "audio_maxqframes"},
    {"memory", "main_heap_size", "main_heap_size"},
    {"memory", "char_heap_size", "char_heap_size"},
    {"debug", "overlay", "debug_overlay"},
    {"debug", "log_level", "log_level"},
    {"debug", "native_math", "native_math"},
    {"debug", "native_camera_shake", "native_camera_shake"},
    {"debug", "log_shake_timing", "log_shake_timing"},
};

// inih keeps quotes, and the ini has always shown paths in them
std::string Unquote(std::string value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// read through a path, not a narrow file name, so a non-ASCII folder works;
// a missing file reads as an empty ini
INIReader ReadIni() {
    std::ifstream file(LegacyIniPath(), std::ios::binary);
    if (!file) return INIReader("", 0);
    std::stringstream text;
    text << file.rdbuf();
    const std::string s = text.str();
    return INIReader(s.c_str(), s.size());
}

}

const std::filesystem::path& LegacyIniPath() {
    static const std::filesystem::path path = [] {
        std::error_code ec;
        auto found = paths::FindFile(
            {std::filesystem::current_path(ec), rex::filesystem::GetExecutableFolder()},
            kLegacyIniPath);
        return found.empty() ? std::filesystem::path(kLegacyIniPath) : found;
    }();
    return path;
}

std::filesystem::path IniAnchor() {
    std::error_code ec;
    auto path = std::filesystem::absolute(LegacyIniPath(), ec);
    return std::filesystem::is_regular_file(path, ec) ? path.parent_path()
                                                       : std::filesystem::current_path(ec);
}

std::string ReadIniString(const char* key) {
    return Unquote(ReadIni().Get("game", key, ""));
}

std::string ReadIniGameDataRoot() {
    INIReader reader = ReadIni();
    // [game] is where band3_config.ini documents it; [paths] kept for older inis
    std::string root = Unquote(
        reader.Get("game", "game_data_root", reader.Get("paths", "game_data_root", "")));
    return root.empty() ? "assets" : root;
}

void ApplyLegacyIni() {
    const std::string path = rex::path_to_utf8(LegacyIniPath());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(LegacyIniPath(), ec)) {
        REXLOG_DEBUG("No {}, using band3.toml and defaults", path);
        return;
    }
    // inih keeps parsing past a bad line, so only a missing file loses the settings
    INIReader reader = ReadIni();
    if (reader.ParseError() != 0) {
        REXLOG_WARN("{}: {}; the other settings still apply", path, reader.ParseErrorMessage());
    }

    int applied = 0;
    int overridden = 0;
    for (const auto& s : kIniSettings) {
        std::string value = Unquote(reader.Get(s.section, s.key, ""));
        // an empty value is how the ini leaves a setting at its default
        if (value.empty() || (s.zero_is_unset && value == "0")) continue;

        // band3.toml, the environment, the command line and the Steam Deck
        // defaults all win over the ini
        if (rex::cvar::GetFlagSource(s.cvar) != rex::cvar::Source::kDefault) {
            overridden++;
            continue;
        }

        const auto* info = rex::cvar::GetFlagInfo(s.cvar);
        if (!info) {
            REXLOG_WARN("{}: [{}] {} has no matching setting '{}'", path, s.section, s.key, s.cvar);
            continue;
        }
        // the ini takes inih's true/yes/on/1 spellings
        if (info->type == rex::cvar::FlagType::Boolean) {
            value = reader.GetBoolean(s.section, s.key, false) ? "true" : "false";
        }

        if (!rex::cvar::SetFlagByName(s.cvar, value)) {
            REXLOG_WARN("{}: [{}] {} = {} is not valid, keeping {}", path, s.section, s.key,
                        value, rex::cvar::GetFlagByName(s.cvar));
            continue;
        }
        applied++;
    }

    // loading the ini is not a change waiting on a restart
    rex::cvar::ClearPendingRestartFlags();

    REXLOG_INFO("{}: applied {} settings", path, applied);
    if (overridden > 0) {
        REXLOG_INFO("{}: {} settings are set elsewhere (band3.toml, the command line or the "
                    "Steam Deck defaults) "
                    "and override it", path, overridden);
    }
}

void AddSettingArgs() {
    GetArgs();
    if (REXCVAR_GET(fast_start)) {
        g_args.push_back("-fast");
    }
    const std::string& lang = REXCVAR_GET(lang);
    if (!lang.empty()) {
        g_args.push_back("-lang");
        g_args.push_back(lang);
    }

    // hard defines for various usecases
    // Rock Band 3 DX identifier
    g_args.push_back("-define");
    g_args.push_back("MHX_PC");
}

const std::filesystem::path& GameDataRoot() { return g_game_data_root; }
void SetGameDataRoot(std::filesystem::path root) { g_game_data_root = std::move(root); }

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

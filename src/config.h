#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace band3 {

struct Config {
    long controller_type = 7;
    std::string input_backend;
    long sync = -1;
    long refresh_rate = 0;
    std::string forced_venue = "false";
    bool fullscreen = false;
    long width = 1280;
    long height = 720;
    bool fast_start = false;
    std::string lang;
    bool disable_metamusic = false;
    bool disable_approximate_lights = true;
    bool disable_hair_shader = false;
    bool fullbright = false;
    bool compress_character_textures = false;
    bool disable_even_odd_rendering = false;
    long main_heap_size = 0;
    long char_heap_size = 0;
    std::string username;
    bool events_enabled = false;
    std::string events_target = "255.255.255.255";
    long events_port = 21070;
    bool discord_enabled = false;
    long max_queued_frames = 3;
    bool debug_overlay = true;
    bool native_math = true;
    bool native_camera_shake = true;
    std::string log_level = "info";
    std::string game_data_root = "assets";
};

const Config& GetConfig();
void LoadConfig(const char* path = "band3_config.ini");

// re-reads only [venue] forced_venue, so it can be changed while the game runs;
// falls back to the value loaded at startup
std::string ReadForcedVenue(const char* path = "band3_config.ini");

const std::vector<std::string>& GetArgs();

}

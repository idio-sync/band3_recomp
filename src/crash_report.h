#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

// What crash_trace.cpp writes around its stacks, so a player's report says
// which run and build it came from and the player hears about it: each run's
// reports go to their own file in band3's logs folder, under a header naming
// the build, and a fatal one leaves last_crash.txt behind, which the next
// start turns into a notice (ReadLastCrash in band3_app.h). Kept to plain data
// so the unit tests can check it.

namespace band3::crash_report {

// a UTC time, as the report prints it
struct Utc {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

// "2026-10-06 10:05:33"
inline std::string FormatUtc(const Utc& t) {
    char out[32];
    std::snprintf(out, sizeof(out), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day,
                  t.hour, t.minute, t.second);
    return out;
}

// a run's reports: "crash-20261006-100533-1234.txt", by when it started (UTC)
// and its process id, so one folder holds many runs' apart
inline std::string ReportFileName(const Utc& started, uint32_t pid) {
    char out[64];
    std::snprintf(out, sizeof(out), "crash-%04d%02d%02d-%02d%02d%02d-%u.txt", started.year,
                  started.month, started.day, started.hour, started.minute, started.second, pid);
    return out;
}

// what a report file's first line says about its run
struct Run {
    std::string build;           // band3::BuildTag()
    uint32_t exe_timestamp = 0;  // band3.exe's link time stamp, as band3.map's "Timestamp is"
    Utc started;
    uint32_t pid = 0;
    std::string renderer;  // the renderer setting
    std::string gpu;       // the Direct3D 12 device's adapter, "" unknown
};

// "=== band3 v1.2-3-gabc | band3.exe 6ac5002d | started 2026-10-06 10:05:33 UTC
// | pid 1234 | renderer native | GPU AMD Radeon RX 6800 ===". tools/symbolize.py
// reads the build and the time stamp, to find the band3.map that resolves the
// frames and to check it's the one.
inline std::string FormatHeader(const Run& run) {
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%08x", run.exe_timestamp);
    std::string text = "=== band3 " + (run.build.empty() ? std::string("unknown") : run.build) +
                       " | band3.exe " + stamp + " | started " + FormatUtc(run.started) +
                       " UTC | pid " + std::to_string(run.pid);
    if (!run.renderer.empty()) text += " | renderer " + run.renderer;
    if (!run.gpu.empty()) text += " | GPU " + run.gpu;
    return text + " ===\n";
}

// what ended a run, as last_crash.txt records it
enum class Kind {
    kAbort,      // abort(), the SDK's way out of most errors
    kGpuHang,    // abort() with the Direct3D 12 device removed
    kTerminate,  // std::terminate, an uncaught exception
};

inline const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::kGpuHang: return "gpu-hang";
        case Kind::kTerminate: return "terminate";
        case Kind::kAbort: break;
    }
    return "abort";
}

inline std::optional<Kind> ParseKind(std::string_view name) {
    for (Kind k : {Kind::kAbort, Kind::kGpuHang, Kind::kTerminate})
        if (name == KindName(k)) return k;
    return std::nullopt;
}

// last_crash.txt: the run that ended, for the next start's notice
struct LastCrash {
    std::string report;  // the report file, UTF-8
    Kind kind = Kind::kAbort;
    std::string renderer;
    std::string build;
};

// key=value lines, one each
inline std::string FormatLastCrash(const LastCrash& c) {
    return "report=" + c.report + "\nkind=" + KindName(c.kind) + "\nrenderer=" + c.renderer +
           "\nbuild=" + c.build + "\n";
}

// nothing unless it names the report and a kind it knows
inline std::optional<LastCrash> ParseLastCrash(std::string_view text) {
    LastCrash c;
    bool have_report = false, have_kind = false;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "report") {
            c.report = value;
            have_report = !value.empty();
        } else if (key == "kind") {
            if (auto k = ParseKind(value)) {
                c.kind = *k;
                have_kind = true;
            }
        } else if (key == "renderer") {
            c.renderer = value;
        } else if (key == "build") {
            c.build = value;
        }
    }
    if (!have_report || !have_kind) return std::nullopt;
    return c;
}

// what the next start tells the player: where the report is and, for a GPU
// hang while band3's own renderer was drawing, that the emulated GPU may avoid
// it
inline std::string Notice(const LastCrash& c) {
    std::string text = "band3 closed unexpectedly the last time it ran";
    if (!c.build.empty()) text += " (build " + c.build + ")";
    text += ".\n\nWhat happened is written in:\n" + c.report +
            "\n\nIf you report the problem, please include that file and the log beside it.";
    if (c.kind == Kind::kGpuHang && c.renderer != "emulated") {
        text += "\n\nThe graphics card stopped responding";
        if (!c.renderer.empty()) text += " with renderer = " + c.renderer;
        text += ". If it keeps happening, setting renderer = emulated (the launcher's Graphics "
                "tab, or band3.toml) avoids band3's native renderer.";
    }
    return text;
}

}  // namespace band3::crash_report

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// which binary wrote a report, as its header says it: band3.exe's link time
// stamp, which band3.map's "Timestamp is" gives too
inline std::string PeExeId(uint32_t timestamp) {
    char out[32];
    std::snprintf(out, sizeof(out), "band3.exe %08x", timestamp);
    return out;
}

// or, on Linux, the executable's GNU build id ("" unknown)
inline std::string ElfExeId(std::string_view build_id) {
    return "band3 build-id " + (build_id.empty() ? std::string("unknown") : std::string(build_id));
}

// what a report file's first line says about its run
struct Run {
    std::string build;  // band3::BuildTag()
    std::string exe;    // PeExeId or ElfExeId
    Utc started;
    uint32_t pid = 0;
    std::string renderer;  // the renderer setting
    std::string gpu;       // the Direct3D 12 device's adapter, "" unknown
};

// "=== band3 v1.2-3-gabc | band3.exe 6ac5002d | started 2026-10-06 10:05:33 UTC
// | pid 1234 | renderer native | GPU AMD Radeon RX 6800 ===". tools/symbolize.py
// reads the build and the binary's id, to find the band3.map (or Linux
// executable) that resolves the frames and to check it's the one.
inline std::string FormatHeader(const Run& run) {
    std::string text = "=== band3 " + (run.build.empty() ? std::string("unknown") : run.build) +
                       " | " + run.exe + " | started " + FormatUtc(run.started) +
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
    kException,  // an exception nothing handled: an access violation, a stack overflow
};

inline const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::kGpuHang: return "gpu-hang";
        case Kind::kTerminate: return "terminate";
        case Kind::kException: return "exception";
        case Kind::kAbort: break;
    }
    return "abort";
}

inline std::optional<Kind> ParseKind(std::string_view name) {
    for (Kind k : {Kind::kAbort, Kind::kGpuHang, Kind::kTerminate, Kind::kException})
        if (name == KindName(k)) return k;
    return std::nullopt;
}

// an exception nothing handled, from its EXCEPTION_RECORD: "access violation
// (0xC0000005), write of 0x0000000000000010", "stack overflow (0xC00000FD)"
inline std::string DescribeException(uint32_t code, uint32_t params, uint64_t info0,
                                     uint64_t info1) {
    const char* name = "exception";
    switch (code) {
        case 0xC0000005: name = "access violation"; break;
        case 0xC0000006: name = "in-page error"; break;
        case 0xC000001D: name = "illegal instruction"; break;
        case 0xC0000094: name = "integer divide by zero"; break;
        case 0xC0000096: name = "privileged instruction"; break;
        case 0xC00000FD: name = "stack overflow"; break;
        case 0xC0000374: name = "heap corruption"; break;
        case 0x80000003: name = "breakpoint"; break;
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%s (0x%08X)", name, code);
    std::string out = text;
    // what it tried and where: ExceptionInformation[0] and [1]
    if ((code == 0xC0000005 || code == 0xC0000006) && params >= 2) {
        const char* op = info0 == 0   ? "read"
                         : info0 == 1 ? "write"
                         : info0 == 8 ? "execute"
                                      : "access";
        std::snprintf(text, sizeof(text), ", %s of 0x%016llX", op,
                      static_cast<unsigned long long>(info1));
        out += text;
    }
    return out;
}

// Of a folder's minidumps (crash-<start>-<pid>.dmp, which sort by when their
// run started), the ones to delete so `keep` stay: the oldest.
inline std::vector<std::string> DumpsToRemove(std::vector<std::string> names, size_t keep) {
    std::sort(names.begin(), names.end());
    if (names.size() <= keep) return {};
    names.resize(names.size() - keep);
    return names;
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

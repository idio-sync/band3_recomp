// Checks src/crash_report.h: the report file's name and header, which tell
// runs and builds apart (tools/symbolize.py reads the header), and
// last_crash.txt, which the next start turns into the player's notice.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/crash_report.h"

using namespace band3::crash_report;

namespace {

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

const Utc kStarted{2026, 10, 6, 9, 5, 3};

}  // namespace

TEST_CASE("a run's report file is named by when it started and its process") {
    CHECK(ReportFileName(kStarted, 1234) == "crash-20261006-090503-1234.txt");
    CHECK(FormatUtc(kStarted) == "2026-10-06 09:05:03");
}

TEST_CASE("the header names the build and band3.exe's time stamp as band3.map gives it") {
    Run run;
    run.build = "v1.2-3-gabc1234-dirty";
    run.exe = PeExeId(0x6ac5002d);
    run.started = kStarted;
    run.pid = 1234;
    run.renderer = "native";
    run.gpu = "AMD Radeon RX 6800";
    CHECK(FormatHeader(run) ==
          "=== band3 v1.2-3-gabc1234-dirty | band3.exe 6ac5002d | started 2026-10-06 09:05:03 UTC"
          " | pid 1234 | renderer native | GPU AMD Radeon RX 6800 ===\n");
}

TEST_CASE("the header leaves out what isn't known") {
    Run run;
    run.exe = PeExeId(0x1);
    run.started = kStarted;
    run.pid = 7;
    const std::string header = FormatHeader(run);
    CHECK(Has(header, "band3 unknown |"));
    CHECK(Has(header, "band3.exe 00000001"));
    CHECK_FALSE(Has(header, "renderer"));
    CHECK_FALSE(Has(header, "GPU"));
}

TEST_CASE("on Linux the header names the executable by its build id") {
    Run run;
    run.build = "v1.2";
    run.exe = ElfExeId("0a1b2c");
    run.started = kStarted;
    run.pid = 9;
    CHECK(FormatHeader(run) ==
          "=== band3 v1.2 | band3 build-id 0a1b2c | started 2026-10-06 09:05:03 UTC | pid 9 "
          "===\n");
    CHECK(ElfExeId("") == "band3 build-id unknown");
}

TEST_CASE("last_crash.txt reads back as it was written") {
    LastCrash c;
    c.report = "C:\\Games\\band3\\logs\\crash-20261006-090503-1234.txt";
    c.kind = Kind::kGpuHang;
    c.renderer = "native";
    c.build = "v1.2";
    const auto back = ParseLastCrash(FormatLastCrash(c));
    REQUIRE(back);
    CHECK(back->report == c.report);
    CHECK(back->kind == Kind::kGpuHang);
    CHECK(back->renderer == "native");
    CHECK(back->build == "v1.2");
}

TEST_CASE("last_crash.txt takes CRLF lines and needs a report and a known kind") {
    const auto crlf = ParseLastCrash("report=a.txt\r\nkind=terminate\r\n");
    REQUIRE(crlf);
    CHECK(crlf->report == "a.txt");
    CHECK(crlf->kind == Kind::kTerminate);
    CHECK_FALSE(ParseLastCrash("kind=abort\n"));
    CHECK_FALSE(ParseLastCrash("report=a.txt\n"));
    CHECK_FALSE(ParseLastCrash("report=a.txt\nkind=meltdown\n"));
    CHECK_FALSE(ParseLastCrash(""));
}

TEST_CASE("the notice says where the report is") {
    LastCrash c;
    c.report = "C:\\band3\\logs\\crash-1.txt";
    c.kind = Kind::kAbort;
    c.renderer = "native";
    c.build = "v1.2";
    const std::string notice = Notice(c);
    CHECK(Has(notice, "(build v1.2)"));
    CHECK(Has(notice, "C:\\band3\\logs\\crash-1.txt"));
    CHECK_FALSE(Has(notice, "renderer = emulated"));
}

TEST_CASE("after a GPU hang the notice suggests the emulated renderer, unless it was on") {
    LastCrash c;
    c.report = "r.txt";
    c.kind = Kind::kGpuHang;
    c.renderer = "native";
    CHECK(Has(Notice(c), "with renderer = native"));
    CHECK(Has(Notice(c), "setting renderer = emulated"));
    c.renderer = "both";
    CHECK(Has(Notice(c), "setting renderer = emulated"));
    c.renderer = "emulated";
    CHECK_FALSE(Has(Notice(c), "renderer = emulated"));
}

TEST_CASE("an unhandled exception is named, with what an access violation tried and where") {
    CHECK(DescribeException(0xC0000005, 2, 1, 0x10) ==
          "access violation (0xC0000005), write of 0x0000000000000010");
    CHECK(DescribeException(0xC0000005, 2, 0, 0) ==
          "access violation (0xC0000005), read of 0x0000000000000000");
    CHECK(DescribeException(0xC0000005, 2, 8, 0x1234) ==
          "access violation (0xC0000005), execute of 0x0000000000001234");
    CHECK(DescribeException(0xC0000005, 0, 1, 0x10) == "access violation (0xC0000005)");
    CHECK(DescribeException(0xC00000FD, 0, 0, 0) == "stack overflow (0xC00000FD)");
    CHECK(DescribeException(0x12345678, 0, 0, 0) == "exception (0x12345678)");
}

TEST_CASE("the oldest minidumps go, so the newest few stay") {
    const std::vector<std::string> names = {
        "crash-20261006-100000-3.dmp", "crash-20261004-090000-1.dmp",
        "crash-20261005-120000-2.dmp"};
    CHECK(DumpsToRemove(names, 5).empty());
    CHECK(DumpsToRemove(names, 3).empty());
    const std::vector<std::string> oldest = {"crash-20261004-090000-1.dmp",
                                             "crash-20261005-120000-2.dmp"};
    CHECK(DumpsToRemove(names, 1) == oldest);
    CHECK(DumpsToRemove(names, 0).size() == 3);
}

TEST_CASE("an exception crash reads back from last_crash.txt") {
    const auto c = ParseLastCrash("report=r.txt\nkind=exception\n");
    REQUIRE(c);
    CHECK(c->kind == Kind::kException);
}

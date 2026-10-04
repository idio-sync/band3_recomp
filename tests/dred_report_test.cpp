// Checks src/dred_report.h: how crash_trace.cpp words Direct3D 12's Device
// Removed Extended Data after a GPU hang, from the auto-breadcrumbs (each
// command list's ops and how many the GPU finished) and the page fault's
// allocations.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/dred_report.h"

using namespace band3::dred;

namespace {

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// ops 3 DRAWINSTANCED, 4 DRAWINDEXEDINSTANCED, 15 RESOURCEBARRIER
CommandList List(std::string queue, std::string list, uint32_t done, std::vector<uint32_t> ops) {
    return {std::move(queue), std::move(list), done, std::move(ops)};
}

}  // namespace

TEST_CASE("breadcrumb ops are named as d3d12.h names them, gaps and all") {
    CHECK(std::string(OpName(0)) == "SETMARKER");
    CHECK(std::string(OpName(4)) == "DRAWINDEXEDINSTANCED");
    CHECK(std::string(OpName(15)) == "RESOURCEBARRIER");
    CHECK(std::string(OpName(48)) == "SETPROGRAM");
    // 49..51 are unused, 52 is PROCESSFRAMES2
    CHECK(std::string(OpName(52)) == "PROCESSFRAMES2");
    CHECK(std::string(OpName(49)) == "op 49");
    CHECK(std::string(OpName(1000)) == "op 1000");
}

TEST_CASE("allocation types are named from 19 up, unknown ones by number") {
    CHECK(std::string(AllocationTypeName(19)) == "COMMAND_QUEUE");
    CHECK(std::string(AllocationTypeName(34)) == "RESOURCE");
    CHECK(std::string(AllocationTypeName(49)) == "VIDEO_ENCODER_HEAP");
    CHECK(std::string(AllocationTypeName(26)) == "type 26");
    CHECK(std::string(AllocationTypeName(0xffffffffu)) == "INVALID");
}

TEST_CASE("no breadcrumbs says so") {
    const std::string text = FormatBreadcrumbs({});
    CHECK(Has(text, "no command lists"));
}

TEST_CASE("finished lists are only counted") {
    const std::string text = FormatBreadcrumbs({List("q", "a", 3, {15, 3, 15}),
                                                List("q", "b", 2, {4, 15})});
    CHECK(Has(text, "2 command lists"));
    CHECK(Has(text, "2 finished"));
    CHECK_FALSE(Has(text, "list a"));
    CHECK_FALSE(Has(text, "list b"));
}

TEST_CASE("a list the GPU stopped partway through shows the ops around the unfinished one") {
    // 20 ops, 11 done: [11] is the one that didn't finish
    std::vector<uint32_t> ops(20, 15);
    ops[10] = 3;
    ops[11] = 4;
    ops[12] = 6;
    const std::string text =
        FormatBreadcrumbs({List("q", "a", 20, std::vector<uint32_t>(20, 15)),
                           List("the SDK's direct queue", "cl 0x1234", 11, ops)},
                          /*context=*/2);
    CHECK(Has(text, "1 finished"));
    CHECK(Has(text, "1 stopped partway"));
    CHECK(Has(text, "queue the SDK's direct queue, list cl 0x1234: stopped at [11] of 20 ops"));
    CHECK(Has(text, "   [10] DRAWINSTANCED\n"));
    CHECK(Has(text, "-> [11] DRAWINDEXEDINSTANCED <- unfinished"));
    CHECK(Has(text, "   [12] DISPATCH\n"));
    // two either side, no more
    CHECK(Has(text, "   [9] RESOURCEBARRIER\n"));
    CHECK(Has(text, "   [13] RESOURCEBARRIER\n"));
    CHECK_FALSE(Has(text, "[8] "));
    CHECK_FALSE(Has(text, "[14] "));
}

TEST_CASE("a list with nothing done may be waiting or stuck on its first op") {
    const std::string text = FormatBreadcrumbs({List("q", "a", 0, {4, 15, 3})});
    CHECK(Has(text, "1 not started"));
    CHECK(Has(text, "queue q, list a: nothing done of 3 ops (waiting, or stuck at [0] DRAWINDEXEDINSTANCED)"));
}

TEST_CASE("the shown lists are capped, partway ones first") {
    std::vector<CommandList> lists;
    for (int i = 0; i < 5; i++) lists.push_back(List("q", "w" + std::to_string(i), 0, {3}));
    lists.push_back(List("q", "p", 1, {3, 4}));
    const std::string text = FormatBreadcrumbs(lists, 2, /*max_shown=*/3);
    CHECK(Has(text, "list p: stopped at [1] of 2 ops"));
    CHECK(text.find("list p:") < text.find("list w0:"));
    CHECK(Has(text, "list w1:"));
    CHECK_FALSE(Has(text, "list w2:"));
    CHECK(Has(text, "3 more not shown"));
}

TEST_CASE("a page fault gives its address and the allocations there") {
    PageFault fault;
    fault.va = 0x12345678abcull;
    fault.existing = {{"scene target", 34}, {"", 25}};
    fault.freed = {{"old output", 34}};
    const std::string text = FormatPageFault(fault);
    CHECK(Has(text, "page fault at 0x12345678ABC"));
    CHECK(Has(text, "live there: RESOURCE 'scene target', HEAP (unnamed)"));
    CHECK(Has(text, "freed there recently: RESOURCE 'old output'"));
}

TEST_CASE("no page fault says so") {
    const std::string text = FormatPageFault({});
    CHECK(Has(text, "no page fault"));
}

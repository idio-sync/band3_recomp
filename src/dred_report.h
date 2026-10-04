#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Experimental: the wording of Direct3D 12's Device Removed Extended Data
// (DRED), which crash_trace.cpp turns on before the SDK makes its device and
// logs when band3 aborts on a removed one (a GPU hang, DXGI_ERROR_DEVICE_HUNG):
// from the auto-breadcrumbs, which command lists the GPU had in hand and the op
// in each it didn't finish, and from the page fault, if there was one, what
// lived at the address. Kept to plain data, apart from d3d12.h, so the unit
// tests can check it.

namespace band3::dred {

// a D3D12_AUTO_BREADCRUMB_NODE: the queue and list (named by the caller),
// how many of its ops the GPU finished (*pLastBreadcrumbValue), and the ops
// (pCommandHistory, D3D12_AUTO_BREADCRUMB_OP values)
struct CommandList {
    std::string queue;
    std::string list;
    uint32_t done = 0;
    std::vector<uint32_t> ops;
};

// a D3D12_DRED_ALLOCATION_NODE: its object's name ("" unnamed) and
// D3D12_DRED_ALLOCATION_TYPE
struct Allocation {
    std::string name;
    uint32_t type = 0;
};

// D3D12_DRED_PAGE_FAULT_OUTPUT: va 0 is no fault
struct PageFault {
    uint64_t va = 0;
    std::vector<Allocation> existing;
    std::vector<Allocation> freed;
};

// d3d12.h's D3D12_AUTO_BREADCRUMB_OP names, without the prefix; "op N" past
// them
inline const char* OpName(uint32_t op) {
    static const char* const kNames[] = {
        "SETMARKER",
        "BEGINEVENT",
        "ENDEVENT",
        "DRAWINSTANCED",
        "DRAWINDEXEDINSTANCED",
        "EXECUTEINDIRECT",
        "DISPATCH",
        "COPYBUFFERREGION",
        "COPYTEXTUREREGION",
        "COPYRESOURCE",
        "COPYTILES",
        "RESOLVESUBRESOURCE",
        "CLEARRENDERTARGETVIEW",
        "CLEARUNORDEREDACCESSVIEW",
        "CLEARDEPTHSTENCILVIEW",
        "RESOURCEBARRIER",
        "EXECUTEBUNDLE",
        "PRESENT",
        "RESOLVEQUERYDATA",
        "BEGINSUBMISSION",
        "ENDSUBMISSION",
        "DECODEFRAME",
        "PROCESSFRAMES",
        "ATOMICCOPYBUFFERUINT",
        "ATOMICCOPYBUFFERUINT64",
        "RESOLVESUBRESOURCEREGION",
        "WRITEBUFFERIMMEDIATE",
        "DECODEFRAME1",
        "SETPROTECTEDRESOURCESESSION",
        "DECODEFRAME2",
        "PROCESSFRAMES1",
        "BUILDRAYTRACINGACCELERATIONSTRUCTURE",
        "EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO",
        "COPYRAYTRACINGACCELERATIONSTRUCTURE",
        "DISPATCHRAYS",
        "INITIALIZEMETACOMMAND",
        "EXECUTEMETACOMMAND",
        "ESTIMATEMOTION",
        "RESOLVEMOTIONVECTORHEAP",
        "SETPIPELINESTATE1",
        "INITIALIZEEXTENSIONCOMMAND",
        "EXECUTEEXTENSIONCOMMAND",
        "DISPATCHMESH",
        "ENCODEFRAME",
        "RESOLVEENCODEROUTPUTMETADATA",
        "BARRIER",
        "BEGIN_COMMAND_LIST",
        "DISPATCHGRAPH",
        "SETPROGRAM",
    };
    if (op < std::size(kNames)) return kNames[op];
    if (op == 52) return "PROCESSFRAMES2";
    thread_local char other[24];
    std::snprintf(other, sizeof(other), "op %u", op);
    return other;
}

// d3d12.h's D3D12_DRED_ALLOCATION_TYPE names, which start at 19 and skip 26,
// 31 and 33; "type N" for those
inline const char* AllocationTypeName(uint32_t type) {
    static const char* const kNames[] = {
        "COMMAND_QUEUE",            // 19
        "COMMAND_ALLOCATOR",        // 20
        "PIPELINE_STATE",           // 21
        "COMMAND_LIST",             // 22
        "FENCE",                    // 23
        "DESCRIPTOR_HEAP",          // 24
        "HEAP",                     // 25
        nullptr,                    // 26
        "QUERY_HEAP",               // 27
        "COMMAND_SIGNATURE",        // 28
        "PIPELINE_LIBRARY",         // 29
        "VIDEO_DECODER",            // 30
        nullptr,                    // 31
        "VIDEO_PROCESSOR",          // 32
        nullptr,                    // 33
        "RESOURCE",                 // 34
        "PASS",                     // 35
        "CRYPTOSESSION",            // 36
        "CRYPTOSESSIONPOLICY",      // 37
        "PROTECTEDRESOURCESESSION", // 38
        "VIDEO_DECODER_HEAP",       // 39
        "COMMAND_POOL",             // 40
        "COMMAND_RECORDER",         // 41
        "STATE_OBJECT",             // 42
        "METACOMMAND",              // 43
        "SCHEDULINGGROUP",          // 44
        "VIDEO_MOTION_ESTIMATOR",   // 45
        "VIDEO_MOTION_VECTOR_HEAP", // 46
        "VIDEO_EXTENSION_COMMAND",  // 47
        "VIDEO_ENCODER",            // 48
        "VIDEO_ENCODER_HEAP",       // 49
    };
    if (type == 0xffffffffu) return "INVALID";
    if (type >= 19 && type - 19 < std::size(kNames) && kNames[type - 19]) return kNames[type - 19];
    thread_local char other[24];
    std::snprintf(other, sizeof(other), "type %u", type);
    return other;
}

// The breadcrumbs, a line per command list the GPU hadn't finished: first
// those it stopped partway through, each with `context` ops either side of
// the unfinished one, then those it had done nothing of (waiting their turn
// on the queue, or stuck at their first op: DRED can't tell those apart).
// At most `max_shown` lists; the finished ones only counted.
inline std::string FormatBreadcrumbs(const std::vector<CommandList>& lists, size_t context = 6,
                                     size_t max_shown = 8) {
    size_t finished = 0;
    std::vector<const CommandList*> partway, waiting;
    for (const CommandList& l : lists) {
        if (l.done >= l.ops.size()) finished++;
        else if (l.done > 0) partway.push_back(&l);
        else waiting.push_back(&l);
    }
    if (lists.empty()) return "DRED breadcrumbs: no command lists recorded\n";
    std::string out = "DRED breadcrumbs: " + std::to_string(lists.size()) + " command lists, " +
                      std::to_string(finished) + " finished, " + std::to_string(partway.size()) +
                      " stopped partway, " + std::to_string(waiting.size()) + " not started\n";
    size_t shown = 0;
    for (const CommandList* l : partway) {
        if (shown == max_shown) break;
        shown++;
        out += "  queue " + l->queue + ", list " + l->list + ": stopped at [" +
               std::to_string(l->done) + "] of " + std::to_string(l->ops.size()) + " ops\n";
        const size_t first = l->done > context ? l->done - context : 0;
        const size_t end = std::min(l->ops.size(), size_t(l->done) + context + 1);
        for (size_t i = first; i < end; i++) {
            out += i == l->done ? "  -> [" : "     [";
            out += std::to_string(i) + "] " + OpName(l->ops[i]);
            out += i == l->done ? " <- unfinished\n" : "\n";
        }
    }
    for (const CommandList* l : waiting) {
        if (shown == max_shown) break;
        shown++;
        out += "  queue " + l->queue + ", list " + l->list + ": nothing done of " +
               std::to_string(l->ops.size()) + " ops (waiting, or stuck at [0] " +
               OpName(l->ops[0]) + ")\n";
    }
    const size_t hidden = partway.size() + waiting.size() - shown;
    if (hidden) out += "  ... " + std::to_string(hidden) + " more not shown\n";
    return out;
}

// The page fault: its address, and the allocations that were there, live or
// freed shortly before
inline std::string FormatPageFault(const PageFault& fault) {
    if (!fault.va) return "DRED: no page fault\n";
    char head[64];
    std::snprintf(head, sizeof(head), "DRED: page fault at 0x%llX\n",
                  static_cast<unsigned long long>(fault.va));
    std::string out = head;
    auto list = [&](const char* what, const std::vector<Allocation>& allocations) {
        out += std::string("  ") + what + ": ";
        if (allocations.empty()) out += "none";
        for (size_t i = 0; i < allocations.size(); i++) {
            if (i) out += ", ";
            out += AllocationTypeName(allocations[i].type);
            out += allocations[i].name.empty() ? " (unnamed)" : " '" + allocations[i].name + "'";
        }
        out += "\n";
    };
    list("live there", fault.existing);
    list("freed there recently", fault.freed);
    return out;
}

}  // namespace band3::dred

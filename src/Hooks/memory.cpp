#include <rex/logging.h>
#include <cstdint>
#include <cstring>
#include "generated/band3_init.h"
#include "src/settings.h"
#include "src/Game/heap_report.h"
#include "src/Game/DataNode.h"
#include "src/Game/DataArray.h"

extern "C" void __imp__AddHeap(PPCContext& ctx, uint8_t* base);

extern "C" REX_FUNC(AddHeap)
{
    uint32_t arr_addr = ctx.r5.u32;

	// look at the DataArray in r5 and determine which heap we are adding to override the size
	// TODO: this does not handle pools from mem.dta, so big_hunk is not changeable, would be nice if we could also adjust this in the future
    if (arr_addr) {
        auto* arr = reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(arr_addr));
        uint32_t nodes_addr = arr->mNodes;
        auto* first = reinterpret_cast<const band3::DataNode*>(REX_RAW_ADDR(nodes_addr));
        if (first->type == band3::kDataSymbol) {
            const char* name = reinterpret_cast<const char*>(REX_RAW_ADDR(first->value));
            // the main heap as big as a heap can be unless main_heap_size is set
            // (src/Game/heap_report.h)
            const int32_t setting = strcmp(name, "main") == 0
                                        ? band3::settings::Startup().main_heap_size
                                    : strcmp(name, "char") == 0
                                        ? band3::settings::Startup().char_heap_size
                                        : 0;
            const auto size = static_cast<uint32_t>(
                band3::heap_report::HeapSize(name, setting, ctx.r4.u32));
            if (setting > band3::heap_report::kMaxHeapSize) {
                REXLOG_WARN("{}_heap_size {} is more than a heap can be; {} instead", name,
                            setting, band3::heap_report::kMaxHeapSize);
            }
            if (size != ctx.r4.u32) {
                REXLOG_INFO("Overriding {} heap size: {:#x} -> {:#x} ({} bytes{})", name,
                            ctx.r4.u32, size, size, setting > 0 ? "" : ", band3's default");
                ctx.r4.u32 = size;
            }
        }
    }

    __imp__AddHeap(ctx, base);
}

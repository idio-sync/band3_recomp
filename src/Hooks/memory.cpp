#include <rex/logging.h>
#include <cstdint>
#include <cstring>
#include "generated/band3_init.h"
#include "src/settings.h"
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
            const int32_t main_heap = band3::settings::Startup().main_heap_size;
            const int32_t char_heap = band3::settings::Startup().char_heap_size;
            if (main_heap > 0 && strcmp(name, "main") == 0) {
                REXLOG_INFO("Overriding main heap size: {:#x} -> {:#x}", ctx.r4.u32, main_heap);
                ctx.r4.u32 = static_cast<uint32_t>(main_heap);
            } else if (char_heap > 0 && strcmp(name, "char") == 0) {
                REXLOG_INFO("Overriding char heap size: {:#x} -> {:#x}", ctx.r4.u32, char_heap);
                ctx.r4.u32 = static_cast<uint32_t>(char_heap);
            }
        }
    }

    __imp__AddHeap(ctx, base);
}

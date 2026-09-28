#include <rex/hook.h>
#include <rex/types.h>
#include <rex/logging.h>
#include <filesystem>
#include <system_error>
#include "src/config.h"

using namespace rex::ppc;

extern "C" void __imp__NewFile(PPCContext& ctx, uint8_t* base);

// handles the .. folders in the ARK
static std::filesystem::path SanitizePath(const char* cc) {
    std::filesystem::path result;
    try {
        for (const auto& part : std::filesystem::path(cc)) {
            if (part == "..") {
                result /= "(..)";
            } else {
                result /= part;
            }
        }
    } catch (...) {
        return std::filesystem::path(cc);
    }
    return result;
}

extern "C" REX_FUNC(NewFile) {
    uint32_t cc_addr = ctx.r3.u32;
    uint32_t flags = ctx.r4.u32;

    if (cc_addr && cc_addr < 0xFFFF0000) {
        const char* cc = reinterpret_cast<const char*>(base + cc_addr);

		// bit of sanity checking for filenames, just in case
        if (cc[0] >= 0x20 && cc[0] <= 0x7E) {
            try {
                std::error_code ec;
                std::filesystem::path sanitized = SanitizePath(cc);

                bool exists = std::filesystem::exists(sanitized, ec);

                const std::filesystem::path root(band3::GameDataRoot());
                if (!exists && !root.empty() && sanitized.begin() != sanitized.end() &&
                    *sanitized.begin() != *root.begin()) {
                    exists = std::filesystem::exists(root / sanitized, ec);
                }

                // ARK opens are nearly every file, so only loose-file overrides log at info
                if (exists) {
                    REXLOG_INFO("NewFile: {} [flags={:#x}]", cc, flags);
                    ctx.r4.u64 = flags | 0x10000;
                } else {
                    REXLOG_DEBUG("NewFile: {} (ARK) [flags={:#x}]", cc, flags);
                }
            } catch (...) {
                REXLOG_WARN("NewFile: exception processing path at {:08X}", cc_addr);
            }
        }
    }

    __imp__NewFile(ctx, base);
}

// rex::memory::AllocFixed, in place of the SDK's on Linux.
//
// The SDK's (librexruntime.so, 0.10.0.15) maps with MAP_FIXED_NOREPLACE; when
// that fails with EEXIST on a commit, it reads all of /proc/self/maps to check
// the range is covered by mappings already there, then mprotects it. Guest
// memory is one mapping made at startup, so every commit of the game's heaps
// (BaseHeap::AllocRange) takes that path: the kernel prints every mapping of
// the process (thousands, with the GPU driver's) under the mmap lock, and the
// game thread waits. On the Steam Deck that's 2-3 ms of a song frame here and
// there, enough to miss a refresh. msync(MS_ASYNC) answers the same question
// (ENOMEM if any page of the range isn't mapped) without changing anything,
// so this one does what the SDK's does, with that in place of the file.
// The library calls AllocFixed through its GOT, so the executable's definition
// (cmake/linux_exports.map exports _Z*) is the one it runs, as with
// timer_queue_posix.cpp. Not on Windows, whose DLLs keep calling their own.

#ifdef __linux__

#include <rex/memory/utils.h>

#include <sys/mman.h>

#include <cerrno>
#include <cstdint>

namespace rex::memory {

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
    // the SDK's table, indexed by access - 1
    static constexpr int kProt[7] = {PROT_READ, 0, PROT_READ | PROT_WRITE, 0,
                                     PROT_READ | PROT_EXEC, 0,
                                     PROT_READ | PROT_WRITE | PROT_EXEC};
    const unsigned index = unsigned(access) - 1;
    const int prot = index < 7 ? kProt[index] : 0;
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS | (base_address ? MAP_FIXED_NOREPLACE : 0);
    void* result = mmap(base_address, length,
                        allocation_type == AllocationType::kReserve ? 0 : prot, flags, -1, 0);
    if (result != MAP_FAILED) return result;
    // committing pages of a mapping already there: only if it covers them all
    if (!base_address || !length || errno != EEXIST ||
        (int(allocation_type) & ~int(AllocationType::kReserve)) != int(AllocationType::kCommit))
        return nullptr;
    if (uintptr_t(base_address) + length < uintptr_t(base_address)) return nullptr;
    if (msync(base_address, length, MS_ASYNC) != 0) return nullptr;
    return mprotect(base_address, length, prot) == 0 ? base_address : nullptr;
}

}  // namespace rex::memory

#endif

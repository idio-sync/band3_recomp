// Checks src/guest_memory_file.h: the guest memory files a process maps are
// read from its /proc maps, unlinked ones and other shm files left out, and
// what's removed is this process's own and those no process maps.

#include <doctest/doctest.h>
#include "src/guest_memory_file.h"

using namespace band3::guest_memory_file;

TEST_CASE("the guest memory files a maps text maps") {
    const char* maps =
        "100000000-220000000 rw-s 00000000 00:1a 61 /dev/shm/xenia_memory_10882990350863\n"
        "220000000-240000000 rw-s 20000000 00:1a 61 /dev/shm/xenia_memory_10882990350863\n"
        "7f00-7f10 rw-s 00000000 00:1a 62 /dev/shm/xenia_memory_99 (deleted)\n"
        "7f10-7f20 rw-s 00000000 00:1a 63 /dev/shm/u1000-Shm_1051a01\n"
        "7f20-7f30 r-xp 00000000 103:08 64 /usr/lib/libc.so.6\n"
        "7f30-7f40 rw-p 00000000 00:00 0 \n"
        "7f40-7f50 rw-s 00000000 00:1a 65 /dev/shm/xenia_memory_\n";
    CHECK(MappedGuestMemoryFiles(maps) ==
          std::set<std::string>{"/dev/shm/xenia_memory_10882990350863"});
    CHECK(MappedGuestMemoryFiles("").empty());
    // no newline at the end
    CHECK(MappedGuestMemoryFiles("1-2 rw-s 0 00:1a 61 /dev/shm/xenia_memory_7").size() == 1);
}

TEST_CASE("its own file and those no process maps are removed") {
    const std::vector<std::string> files = {"/dev/shm/xenia_memory_1", "/dev/shm/xenia_memory_2",
                                            "/dev/shm/xenia_memory_3"};
    // 1 is this process's, 2 another running game's, 3 left by a run that ended
    const std::set<std::string> own = {"/dev/shm/xenia_memory_1"};
    const std::set<std::string> mapped = {"/dev/shm/xenia_memory_1", "/dev/shm/xenia_memory_2"};
    CHECK(FilesToRemove(files, own, mapped) ==
          std::vector<std::string>{"/dev/shm/xenia_memory_1", "/dev/shm/xenia_memory_3"});
    CHECK(FilesToRemove({}, own, mapped).empty());
}

TEST_CASE("only xenia_memory_ files with a name after the prefix count") {
    CHECK(IsGuestMemoryName("xenia_memory_123"));
    CHECK_FALSE(IsGuestMemoryName("xenia_memory_"));
    CHECK_FALSE(IsGuestMemoryName("u1000-Shm_1051a01"));
    CHECK_FALSE(IsGuestMemoryName("xenia_memor"));
}

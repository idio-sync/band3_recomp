// Checks src/Render/sync_gpu/sync_cp.h, the sync-only GPU's command
// processor, against Xenia's CommandProcessor semantics it follows: packets
// written into a primary ring in a byte vector standing in for guest
// physical memory, big-endian as the guest writes them, and what the
// processor leaves in its registers and in memory (in the byte order each
// packet's endian bits ask for).

#include <doctest/doctest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "src/Render/gamma_ramp.h"
#include "src/Render/sync_gpu/sync_cp.h"

using namespace band3::render;
using namespace band3::render::sync_gpu;

namespace {

struct TestMemory final : GuestMemory {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000, 0);

    uint8_t* TranslatePhysical(uint32_t address, uint32_t size) override {
        if (uint64_t(address) + size > bytes.size()) return nullptr;
        return bytes.data() + address;
    }
    uint8_t* at(uint32_t a) { return bytes.data() + a; }
    uint32_t be(uint32_t a) const { return LoadBE32(bytes.data() + a); }
    void set_be(uint32_t a, uint32_t v) { StoreBE32(bytes.data() + a, v); }
    uint32_t host(uint32_t a) const { return LoadHost32(bytes.data() + a); }
    std::vector<uint8_t> span(uint32_t a, uint32_t n) const {
        return {bytes.begin() + a, bytes.begin() + a + n};
    }
};

constexpr uint32_t kRing = 0x1000;  // the primary ring, 2048 dwords
constexpr uint32_t kRingLog2 = 10;  // in qwords: 1 << 13 bytes

uint32_t T0(uint32_t base, uint32_t count, bool one_reg = false) {
    return (count - 1) << 16 | (one_reg ? 0x8000u : 0u) | base;
}
uint32_t T1(uint32_t reg1, uint32_t reg2) { return 1u << 30 | reg2 << 11 | reg1; }
constexpr uint32_t kT2 = 2u << 30;
uint32_t T3(uint32_t opcode, uint32_t count, bool predicated = false) {
    return 3u << 30 | (count - 1) << 16 | opcode << 8 | (predicated ? 1u : 0u);
}
uint32_t Mmio(uint32_t reg) { return 0x7FC80000 + reg * 4; }

// a register nothing special happens on, for "the packets after it ran"
constexpr uint32_t kMarker = 0x2000;

struct Fixture {
    TestMemory mem;
    std::vector<std::string> logs;
    SyncCommandProcessor cp;

    explicit Fixture(SyncCpHooks hooks = {}, SyncCpConfig config = {})
        : cp(mem, WithLog(std::move(hooks)), config) {
        cp.InitializeRingBuffer(kRing, kRingLog2);
    }
    SyncCpHooks WithLog(SyncCpHooks hooks) {
        hooks.log = [this](const std::string& line) { logs.push_back(line); };
        return hooks;
    }
    // the words from the write index on, and the write pointer after them
    void Write(const std::vector<uint32_t>& words) {
        const uint32_t capacity = cp.primary_buffer_size() / 4;
        uint32_t w = cp.write_index() % capacity;
        for (uint32_t v : words) {
            mem.set_be(cp.primary_buffer_ptr() + w * 4, v);
            w = (w + 1) % capacity;
        }
        cp.UpdateWritePointer(w);
    }
    void Run(const std::vector<uint32_t>& words) {
        Write(words);
        cp.ExecutePending();
    }
    size_t LogsWith(const std::string& text) const {
        size_t n = 0;
        for (const auto& l : logs) n += l.find(text) != std::string::npos;
        return n;
    }
};

std::vector<uint8_t> Bytes(std::initializer_list<int> b) { return {b.begin(), b.end()}; }

}  // namespace

TEST_CASE("the ring's size is in qwords, as Xenia reads VdInitializeRingBuffer's") {
    Fixture f;
    f.cp.InitializeRingBuffer(0x4000, 6);
    CHECK(f.cp.primary_buffer_size() == 512);  // 1 << (6 + 3) bytes
    CHECK(f.cp.read_index() == 0);
}

TEST_CASE("a type-0 packet writes a run of registers, or one register over and over") {
    Fixture f;
    f.Run({T0(0x2100, 3), 11, 22, 33, T0(0x2200, 3, true), 44, 55, 66});
    CHECK(f.cp.ReadRegister(0x2100) == 11);
    CHECK(f.cp.ReadRegister(0x2101) == 22);
    CHECK(f.cp.ReadRegister(0x2102) == 33);
    CHECK(f.cp.ReadRegister(0x2200) == 66);
    CHECK(f.cp.ReadRegister(0x2201) == 0);
    CHECK(f.cp.stats().type0.get() == 2);
    CHECK(f.cp.read_index() == 8);
}

TEST_CASE("a type-1 packet writes two registers; type 2 and zero words do nothing") {
    Fixture f;
    f.Run({0, kT2, T1(0x123, 0x456), 0xAAAA, 0xBBBB});
    CHECK(f.cp.ReadRegister(0x123) == 0xAAAA);
    CHECK(f.cp.ReadRegister(0x456) == 0xBBBB);
    CHECK(f.cp.stats().type1.get() == 1);
    CHECK(f.cp.stats().type2.get() == 1);
    CHECK(f.cp.stats().packets.get() == 3);
}

TEST_CASE("a type-0 packet longer than its buffer drops the rest of the buffer") {
    Fixture f;
    // in an indirect buffer of 3 dwords: a type-0 of 4 values after one good
    // packet; the primary ring goes on after the buffer
    f.mem.set_be(0x3000, T0(0x2300, 1));
    f.mem.set_be(0x3004, 7);
    f.mem.set_be(0x3008, T0(0x2301, 4));
    f.Run({T3(pm4::INDIRECT_BUFFER, 2), 0x3000, 3, T0(kMarker, 1), 1});
    CHECK(f.cp.ReadRegister(0x2300) == 7);
    CHECK(f.cp.ReadRegister(0x2301) == 0);
    CHECK(f.cp.stats().bad_packets.get() == 1);
    CHECK(f.LogsWith("type-0 packet 00032301 overflows it (0 of 4 dwords) in the indirect "
                     "buffer") == 1);
    CHECK(f.cp.ReadRegister(kMarker) == 1);
}

TEST_CASE("SET_CONSTANT writes into the block its type names") {
    Fixture f;
    f.Run({T3(pm4::SET_CONSTANT, 3), 0 << 16 | 5, 0xA1, 0xA2,     // ALU: 0x4000
           T3(pm4::SET_CONSTANT, 2), 1 << 16 | 6, 0xB1,           // fetch: 0x4800
           T3(pm4::SET_CONSTANT, 2), 2 << 16 | 1, 0xC1,           // bool: 0x4900
           T3(pm4::SET_CONSTANT, 2), 3 << 16 | 2, 0xD1,           // loop: 0x4908
           T3(pm4::SET_CONSTANT, 2), 4 << 16 | 0x1F0, 0xE1,       // registers: 0x2000
           T3(pm4::SET_CONSTANT, 3), 5 << 16 | 0x10, 0xF1, 0xF2,  // no such type: skipped
           T0(kMarker, 1), 1});
    CHECK(f.cp.ReadRegister(0x4005) == 0xA1);
    CHECK(f.cp.ReadRegister(0x4006) == 0xA2);
    CHECK(f.cp.ReadRegister(0x4806) == 0xB1);
    CHECK(f.cp.ReadRegister(0x4901) == 0xC1);
    CHECK(f.cp.ReadRegister(0x490A) == 0xD1);
    CHECK(f.cp.ReadRegister(0x21F0) == 0xE1);
    CHECK(f.cp.ReadRegister(0x10) == 0);
    CHECK(f.cp.ReadRegister(kMarker) == 1);
}

TEST_CASE("SET_CONSTANT2, SET_SHADER_CONSTANTS, LOAD_ALU_CONSTANT and REG_RMW write registers") {
    Fixture f;
    f.mem.set_be(0x5000, 0x01020304);
    f.mem.set_be(0x5004, 0x05060708);
    f.Run({T3(pm4::SET_CONSTANT2, 3), 0xABCD2400, 1, 2,  // index in the low 16 bits
           T3(pm4::SET_SHADER_CONSTANTS, 2), 0x2500, 3,
           // from memory, big-endian, into the ALU constants at 0x4000 + 0x10
           T3(pm4::LOAD_ALU_CONSTANT, 3), 0x40005000, 0 << 16 | 0x10, 2,
           T0(0x0600, 3), 0xF0F0, 0x00FF, 0x0F00,
           // (13-bit indices) 0x600 & 0xFF | 0x1000
           T3(pm4::REG_RMW, 3), 0x0600, 0xFF, 0x1000,
           // 0x601 & reg 0x601 | reg 0x602
           T3(pm4::REG_RMW, 3), 0xC0000000 | 0x0601, 0x0601, 0x0602});
    CHECK(f.cp.ReadRegister(0x2400) == 1);
    CHECK(f.cp.ReadRegister(0x2401) == 2);
    CHECK(f.cp.ReadRegister(0x2500) == 3);
    CHECK(f.cp.ReadRegister(0x4010) == 0x01020304);
    CHECK(f.cp.ReadRegister(0x4011) == 0x05060708);
    CHECK(f.cp.ReadRegister(0x0600) == 0x10F0);
    CHECK(f.cp.ReadRegister(0x0601) == 0x0FFF);
}

TEST_CASE("indirect buffers run where they point, nested, and the ring goes on after") {
    Fixture f;
    // 0x3000: a register, then a buffer at 0x3100 (by a GPU address with the
    // CPU's high bits, which CpuToGpu drops)
    f.mem.set_be(0x3000, T0(0x2700, 1));
    f.mem.set_be(0x3004, 0x11);
    f.mem.set_be(0x3008, T3(pm4::INDIRECT_BUFFER_PFD, 2));
    f.mem.set_be(0x300C, 0xA0003100);
    f.mem.set_be(0x3010, 2);
    f.mem.set_be(0x3100, T0(0x2701, 1));
    f.mem.set_be(0x3104, 0x22);
    f.Run({T3(pm4::INDIRECT_BUFFER, 2), 0x3000, 5, T0(kMarker, 1), 1});
    CHECK(f.cp.ReadRegister(0x2700) == 0x11);
    CHECK(f.cp.ReadRegister(0x2701) == 0x22);
    CHECK(f.cp.ReadRegister(kMarker) == 1);
    CHECK(f.cp.stats().indirect_buffers.get() == 2);
}

TEST_CASE("EVENT_WRITE_SHD writes the fence in the byte order of its address") {
    Fixture f;
    f.Run({T3(pm4::EVENT_WRITE_SHD, 3), 4, 0x4000 | 0, 0x11223344,
           T3(pm4::EVENT_WRITE_SHD, 3), 4, 0x4010 | 1, 0x11223344,
           T3(pm4::EVENT_WRITE_SHD, 3), 4, 0x4020 | 2, 0x11223344,
           T3(pm4::EVENT_WRITE_SHD, 3), 0x45, 0x4030 | 3, 0x11223344});
    CHECK(f.mem.span(0x4000, 4) == Bytes({0x44, 0x33, 0x22, 0x11}));  // none: the host's
    CHECK(f.mem.span(0x4010, 4) == Bytes({0x33, 0x44, 0x11, 0x22}));  // 8in16
    CHECK(f.mem.span(0x4020, 4) == Bytes({0x11, 0x22, 0x33, 0x44}));  // 8in32: big-endian
    CHECK(f.mem.span(0x4030, 4) == Bytes({0x22, 0x11, 0x44, 0x33}));  // 16in32
    CHECK(f.cp.ReadRegister(reg::VGT_EVENT_INITIATOR) == 5);
    CHECK(f.cp.stats().fences.get() == 4);
}

TEST_CASE("EVENT_WRITE_SHD's counter flag writes the swap and vblank counter") {
    Fixture f;
    f.cp.IncrementCounter();
    f.cp.IncrementCounter();
    f.Run({T3(pm4::XE_SWAP, 4), 0x53574150, 0, 0, 0,
           T3(pm4::EVENT_WRITE_SHD, 3), 0x80000000 | 4, 0x4000 | 2, 0x99});
    CHECK(f.cp.counter() == 3);
    CHECK(f.mem.be(0x4000) == 3);
}

TEST_CASE("MEM_WRITE stores each word in its address's byte order") {
    Fixture f;
    f.Run({T3(pm4::MEM_WRITE, 4), 0x5000 | 2, 0xAABBCCDD, 1, 2,
           T3(pm4::MEM_WRITE, 2), 0x5100, 0xAABBCCDD});
    CHECK(f.mem.be(0x5000) == 0xAABBCCDD);
    CHECK(f.mem.be(0x5004) == 1);
    CHECK(f.mem.be(0x5008) == 2);
    CHECK(f.mem.host(0x5100) == 0xAABBCCDD);
}

TEST_CASE("REG_TO_MEM copies a register to memory") {
    Fixture f;
    f.Run({T0(0x2800, 1), 0x12345678, T3(pm4::REG_TO_MEM, 2), 0x2800, 0x5200 | 2});
    CHECK(f.mem.be(0x5200) == 0x12345678);
}

TEST_CASE("COND_WRITE writes memory or a register when its compare holds") {
    Fixture f;
    f.mem.set_be(0x5300, 0x1234);
    f.Run({// memory == 0x34 under the mask: write memory
           T3(pm4::COND_WRITE, 6), 0x100 | 0x10 | 3, 0x5300 | 2, 0x34, 0xFF, 0x5310 | 2, 0xAA,
           // memory != 0x1234: doesn't hold, nothing written
           T3(pm4::COND_WRITE, 6), 0x100 | 0x10 | 4, 0x5300 | 2, 0x1234, ~0u, 0x5320 | 2, 0xBB,
           // register >= 2: write a register
           T0(0x2900, 1), 3,
           T3(pm4::COND_WRITE, 6), 5, 0x2900, 2, ~0u, 0x2901, 0xCC});
    CHECK(f.mem.be(0x5310) == 0xAA);
    CHECK(f.mem.be(0x5320) == 0);
    CHECK(f.cp.ReadRegister(0x2901) == 0xCC);
}

TEST_CASE("WAIT_REG_MEM waits for each compare function on memory and on a register") {
    struct Case {
        uint32_t function, ref, waiting, released;
    };
    // < <= == != >= >, a value under the mask that waits and one that doesn't
    const Case cases[] = {{1, 5, 5, 4}, {2, 5, 6, 5}, {3, 5, 4, 5},
                          {4, 5, 5, 6}, {5, 5, 4, 5}, {6, 5, 5, 6}};
    for (bool memory : {true, false}) {
        for (const Case& c : cases) {
            CAPTURE(memory);
            CAPTURE(c.function);
            int polls = 0;
            Fixture* fp = nullptr;
            SyncCpHooks hooks;
            // the third unmatched poll changes the value: the loop sees it
            hooks.wait = [&](uint32_t) {
                if (++polls != 3) return;
                // bits outside the mask that the compare must ignore
                const uint32_t v = 0xAB00 | c.released;
                if (memory) fp->mem.set_be(0x6000, v);
                else fp->cp.WriteRegister(0x2A00, v);
            };
            Fixture f(std::move(hooks));
            fp = &f;
            const uint32_t v = 0xAB00 | c.waiting;
            if (memory) f.mem.set_be(0x6000, v);
            else f.cp.WriteRegister(0x2A00, v);
            f.Run({T3(pm4::WAIT_REG_MEM, 5), (memory ? 0x10u : 0u) | c.function,
                   memory ? 0x6000u | 2 : 0x2A00u, c.ref, 0xFF, 0x100,
                   T0(kMarker, 1), 1});
            CHECK(polls == 3);
            CHECK(f.cp.ReadRegister(kMarker) == 1);
            CHECK(f.cp.stats().stalled_waits.get() == 1);
            CHECK_FALSE(f.cp.current_wait().active);
        }
    }
}

TEST_CASE("WAIT_REG_MEM's always doesn't wait, and never waits until a stop") {
    int polls = 0;
    Fixture* fp = nullptr;
    SyncCpHooks hooks;
    hooks.wait = [&](uint32_t) {
        if (++polls == 3) fp->cp.RequestStop();
    };
    Fixture f(std::move(hooks));
    fp = &f;
    f.Run({T3(pm4::WAIT_REG_MEM, 5), 0x10 | 7, 0x6000, 1, ~0u, 0,
           T0(0x2B00, 1), 1,
           T3(pm4::WAIT_REG_MEM, 5), 0x10 | 0, 0x6000, 0, 0, 0,
           T0(kMarker, 1), 1});
    CHECK(f.cp.ReadRegister(0x2B00) == 1);
    CHECK(polls == 3);
    CHECK(f.cp.ReadRegister(kMarker) == 0);  // the stop dropped the rest
    CHECK(f.cp.stats().waits.get() == 2);
    CHECK(f.cp.stats().bad_packets.get() == 0);  // a stop isn't a bad packet
    CHECK_FALSE(f.cp.ExecutePending());
}

namespace {

// Runs the ring on a thread of its own until the processor waits, calls
// release, and checks the ring then runs to its end. The default wait hook:
// a wait of 0x100 sleeps 1 ms a poll.
void RunReleasedByAnotherThread(Fixture& f, const std::function<void()>& release,
                                bool expect_memory, uint32_t expect_addr) {
    using namespace std::chrono_literals;
    std::thread cp_thread([&] { f.cp.ExecutePending(); });
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!f.cp.current_wait().active && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const CurrentWait w = f.cp.current_wait();
    CHECK(w.active);
    CHECK(w.memory == expect_memory);
    CHECK(w.poll_addr == expect_addr);
    CHECK(f.cp.ReadRegister(kMarker) == 0);
    std::this_thread::sleep_for(5ms);
    CHECK(f.cp.current_wait().elapsed >= 5ms);
    release();
    deadline = std::chrono::steady_clock::now() + 5s;
    while (f.cp.read_index() != f.cp.write_index() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    f.cp.RequestStop();  // only if the release wasn't seen
    cp_thread.join();
    CHECK(f.cp.ReadRegister(kMarker) == 1);
    CHECK(f.cp.stats().stalled_waits.get() == 1);
    CHECK(f.cp.stats().wait_ns_max.get() >= 5'000'000);
    CHECK_FALSE(f.cp.current_wait().active);
}

}  // namespace

TEST_CASE("a WAIT_REG_MEM on memory is released by the guest's write from another thread") {
    Fixture f;
    // the vsync wait's shape: a big-endian word until it's 1
    f.Write({T3(pm4::WAIT_REG_MEM, 5), 0x10 | 3, 0x6100 | 2, 1, ~0u, 0x100,
             T0(kMarker, 1), 1});
    RunReleasedByAnotherThread(
        f, [&] { StoreHost32Release(f.mem.at(0x6100), std::byteswap(1u)); }, true,
        0x6100 | 2);
}

TEST_CASE("a WAIT_REG_MEM on a register is released by an MMIO write from another thread") {
    Fixture f;
    f.Write({T3(pm4::WAIT_REG_MEM, 5), 5, 0x2C00, 3, ~0u, 0x100, T0(kMarker, 1), 1});
    RunReleasedByAnotherThread(f, [&] { f.cp.MmioWrite(Mmio(0x2C00), 3); }, false, 0x2C00);
}

TEST_CASE("scratch registers the mask enables are written back, big-endian") {
    Fixture f;
    f.Run({T0(reg::SCRATCH_UMSK, 2), 0b100010, 0x7000,  // UMSK, then SCRATCH_ADDR
           T0(reg::SCRATCH_REG0, 2), 0x11111111, 0xCAFEBABE});
    CHECK(f.mem.be(0x7000) == 0);  // reg 0 not enabled
    CHECK(f.mem.be(0x7004) == 0xCAFEBABE);
    CHECK(f.cp.ReadRegister(reg::SCRATCH_REG0) == 0x11111111);
    // and by MMIO
    f.cp.MmioWrite(Mmio(reg::SCRATCH_REG0 + 5), 0x5555AAAA);
    CHECK(f.mem.be(0x7014) == 0x5555AAAA);
}

TEST_CASE("the gamma ramps start linear, as Xenia's") {
    Fixture f;
    const GammaRamp g = f.cp.DisplayGamma();
    CHECK(g.mode == GammaRamp::kTable);
    CHECK(TableChannel(g.table[255], 0) == 1023);
    CHECK(TableChannel(g.table[0], 2) == 0);
    uint8_t lut[3][256];
    GammaLut(g, lut);
    CHECK(IsIdentity(lut));
}

TEST_CASE("DC_LUT_30_COLOR fills the 256-entry table, auto-incrementing the index") {
    Fixture f;
    // as D3D writes it: mode, index, write enable, then the entries
    std::vector<uint32_t> words = {T0(reg::DC_LUT_RW_MODE, 2), 0, 0,
                                   T0(reg::DC_LUT_WRITE_EN_MASK, 1), 7,
                                   T0(reg::DC_LUT_30_COLOR, 256, true)};
    auto entry = [](uint32_t i) {
        const uint32_t red = i * 4, green = 1023 - i * 4, blue = i * 2;
        return red << 20 | green << 10 | blue;
    };
    for (uint32_t i = 0; i < 256; i++) words.push_back(entry(i));
    f.Run(words);
    CHECK(f.cp.ReadRegister(reg::DC_LUT_RW_INDEX) == 0);  // 8 bits: wrapped
    GammaRamp g = f.cp.DisplayGamma();
    CHECK(g.mode == GammaRamp::kTable);
    for (uint32_t i : {0u, 1u, 128u, 255u}) {
        CAPTURE(i);
        CHECK(g.table[i] == entry(i));
    }
    // through gamma_ramp.h, as the capture applies it
    uint8_t lut[3][256];
    GammaLut(g, lut);
    CHECK(lut[0][128] == Unorm10To8(512));
    CHECK(lut[1][128] == Unorm10To8(1023 - 512));
    CHECK(lut[2][100] == Unorm10To8(200));

    // the write enable mask (blue, green, red from bit 0) keeps the others
    f.cp.MmioWrite(Mmio(reg::DC_LUT_RW_INDEX), 10);
    f.cp.MmioWrite(Mmio(reg::DC_LUT_WRITE_EN_MASK), 0b100);
    f.cp.MmioWrite(Mmio(reg::DC_LUT_30_COLOR), 0x3FFFFFFF);
    g = f.cp.DisplayGamma();
    CHECK(TableChannel(g.table[10], 0) == 1023);
    CHECK(TableChannel(g.table[10], 1) == TableChannel(entry(10), 1));
    CHECK(TableChannel(g.table[10], 2) == TableChannel(entry(10), 2));
    CHECK(f.cp.ReadRegister(reg::DC_LUT_RW_INDEX) == 11);
}

TEST_CASE("DC_LUT_SEQ_COLOR writes an entry a component at a time, red first") {
    Fixture f;
    f.Run({T0(reg::DC_LUT_RW_MODE, 2), 0, 5, T0(reg::DC_LUT_WRITE_EN_MASK, 1), 0b011,
           // red (not enabled), green, blue as 16 bits with 6 low zero bits;
           // then the first component of entry 6
           T0(reg::DC_LUT_SEQ_COLOR, 4, true), 100 << 6, 200 << 6 | 0x3F, 300 << 6, 0});
    const GammaRamp g = f.cp.DisplayGamma();
    CHECK(TableChannel(g.table[5], 0) == 5 * 0x3FF / 0xFF);  // red kept
    CHECK(TableChannel(g.table[5], 1) == 200);
    CHECK(TableChannel(g.table[5], 2) == 300);
    CHECK(f.cp.ReadRegister(reg::DC_LUT_RW_INDEX) == 6);
    // a write of the index starts over at red
    f.Run({T0(reg::DC_LUT_RW_INDEX, 1), 7, T0(reg::DC_LUT_SEQ_COLOR, 3, true), 0, 1 << 6, 2 << 6});
    CHECK(TableChannel(f.cp.DisplayGamma().table[7], 1) == 1);
    CHECK(TableChannel(f.cp.DisplayGamma().table[7], 2) == 2);
}

TEST_CASE("DC_LUT_PWL_DATA fills the PWL ramp, three components a step") {
    Fixture f;
    std::vector<uint32_t> words = {T0(reg::DC_LUT_RW_MODE, 2), 1, 0,
                                   T0(reg::DC_LUT_WRITE_EN_MASK, 1), 7,
                                   T0(reg::DC_LUT_PWL_DATA, 128 * 3, true)};
    // half the input: step i's base i * 4, delta 4, in 10.6 fixed point; the
    // low 6 bits written are dropped
    for (uint32_t i = 0; i < 128; i++)
        for (int c = 0; c < 3; c++) words.push_back((4u << 6 | 0x3F) << 16 | (i * 4) << 6 | 0x3F);
    f.Run(words);
    CHECK(f.cp.ReadRegister(reg::DC_LUT_RW_INDEX) == 0);  // 7 bits: wrapped
    const GammaRamp g = f.cp.DisplayGamma();
    CHECK(g.mode == GammaRamp::kPwl);
    CHECK(g.pwl[3][1] == ((4u << 6) << 16 | (12u << 6)));
    for (int c = 0; c < 3; c++) {
        CHECK(PwlChannel(g, 512, c) == 256);
        CHECK(PwlChannel(g, 1023, c) == 511);
    }
    uint8_t lut[3][256];
    GammaLut(g, lut);
    CHECK(lut[0][255] == Unorm10To8(511));
}

TEST_CASE("EVENT_WRITE_ZPD clears a query's counts at its begin and reports them at its end") {
    SyncCpConfig config;
    config.fake_sample_count = 7;
    Fixture f({}, config);
    constexpr uint32_t kCounts = 0x8000;
    f.Run({T0(reg::RB_SAMPLE_COUNT_ADDR, 1), kCounts});
    // begin: whatever was there is cleared
    std::memset(f.mem.at(kCounts), 0x5A, 32);
    f.Run({T3(pm4::EVENT_WRITE_ZPD, 1), 0x15});
    CHECK(f.mem.span(kCounts, 32) == std::vector<uint8_t>(32, 0));
    CHECK(f.cp.ReadRegister(reg::VGT_EVENT_INITIATOR) == 0x15);
    // end: D3D marks ZPass_A and B with 0xFFFFFEED, big-endian
    f.mem.set_be(kCounts + 16, 0xFFFFFEED);
    f.mem.set_be(kCounts + 20, 0xFFFFFEED);
    f.Run({T3(pm4::EVENT_WRITE_ZPD, 1), 0x15});
    CHECK(f.mem.host(kCounts + 0) == 7);   // Total_A, little-endian
    CHECK(f.mem.host(kCounts + 16) == 7);  // ZPass_A
    CHECK(f.mem.host(kCounts + 20) == 0);
    CHECK(f.mem.host(kCounts + 4) == 0);
    // older D3Ds mark ZFail_A and B
    f.mem.set_be(kCounts + 8, 0xFFFFFEED);
    f.mem.set_be(kCounts + 12, 0xFFFFFEED);
    f.cp.SetFakeSampleCount(1000);
    f.Run({T3(pm4::EVENT_WRITE_ZPD, 1), 0x15});
    CHECK(f.mem.host(kCounts + 16) == 1000);
    CHECK(f.mem.host(kCounts + 8) == 0);
    // a negative count leaves the queries alone
    f.cp.SetFakeSampleCount(-1);
    f.mem.set_be(kCounts + 16, 0xFFFFFEED);
    f.Run({T3(pm4::EVENT_WRITE_ZPD, 1), 0x15});
    CHECK(f.mem.be(kCounts + 16) == 0xFFFFFEED);
    CHECK(f.cp.stats().sample_count_writes.get() == 3);
}

TEST_CASE("EVENT_WRITE_EXT fakes the whole screen as the extents") {
    Fixture f;
    f.Run({T3(pm4::EVENT_WRITE_EXT, 2), 0x1A, 0x8100 | 1});
    CHECK(f.mem.span(0x8100, 12) ==
          Bytes({0, 0, 0x04, 0, 0, 0, 0x04, 0, 0, 0, 0, 1}));
    CHECK(f.cp.ReadRegister(reg::VGT_EVENT_INITIATOR) == 0x1A);
}

TEST_CASE("VIZ_QUERY's end reports the query visible") {
    Fixture f;
    f.Run({T3(pm4::VIZ_QUERY, 1), 3, T3(pm4::VIZ_QUERY, 1), 0x100 | 3,
           T3(pm4::VIZ_QUERY, 1), 0x100 | 33});
    CHECK(f.cp.ReadRegister(reg::PA_SC_VIZ_QUERY_STATUS_0) == 1u << 3);
    CHECK(f.cp.ReadRegister(reg::PA_SC_VIZ_QUERY_STATUS_1) == 1u << 1);
    CHECK(f.cp.ReadRegister(reg::VGT_EVENT_INITIATOR) == kEventVizQueryEnd);
}

TEST_CASE("the ring wraps, a packet's body included") {
    Fixture f;
    f.cp.InitializeRingBuffer(kRing, 2);  // 32 bytes: 8 dwords
    f.cp.EnableReadPointerWriteBack(0x8200, 6);
    f.Run({T0(0x2D00, 1), 1, T0(0x2D01, 1), 2, T0(0x2D02, 1), 3});
    CHECK(f.cp.read_index() == 6);
    // header at 6, values at 7, 0, 1
    f.Run({T0(0x2D10, 3), 10, 20, 30});
    CHECK(f.cp.write_index() == 2);
    CHECK(f.cp.read_index() == 2);
    CHECK(f.cp.ReadRegister(0x2D02) == 3);
    CHECK(f.cp.ReadRegister(0x2D10) == 10);
    CHECK(f.cp.ReadRegister(0x2D11) == 20);
    CHECK(f.cp.ReadRegister(0x2D12) == 30);
    CHECK(f.mem.be(0x8200) == 2);
}

TEST_CASE("the read pointer is written back big-endian, once enabled") {
    Fixture f;
    f.Run({T0(0x2E00, 1), 1});
    CHECK(f.mem.be(0x8300) == 0);
    f.cp.EnableReadPointerWriteBack(0x8300, 6);
    f.Run({T0(0x2E01, 2), 1, 2, kT2});
    CHECK(f.cp.read_index() == 6);
    CHECK(f.mem.span(0x8300, 4) == Bytes({0, 0, 0, 6}));
    CHECK_FALSE(f.cp.ExecutePending());  // nothing new
}

TEST_CASE("unknown opcodes and draws are skipped with their bodies, and counted") {
    Fixture f;
    f.Run({T3(0x7E, 3), T0(0x2F00, 1), 0xBAD, 0xBAD,  // a body that looks like packets
           T3(0x7E, 1), 0,
           T3(pm4::WAIT_REG_EQ, 3), 0x2F00, 1, 0,
           T3(pm4::DRAW_INDX, 3), 0, 0x12345678, 0,
           T3(pm4::DRAW_INDX_2, 1), 0,
           T0(kMarker, 1), 1});
    CHECK(f.cp.ReadRegister(0x2F00) == 0);
    CHECK(f.cp.ReadRegister(kMarker) == 1);
    CHECK(f.cp.stats().unknown_opcodes.get() == 3);
    CHECK(f.cp.stats().opcodes[0x7E].get() == 2);
    CHECK(f.cp.stats().draws_skipped.get() == 2);
    CHECK(f.LogsWith("opcode 7E") == 1);  // logged once
    CHECK(f.LogsWith("opcode 52") == 1);
}

TEST_CASE("INTERRUPT calls the hook with source 1 for each cpu in the mask") {
    std::vector<std::pair<uint32_t, uint32_t>> calls;
    SyncCpHooks hooks;
    hooks.interrupt = [&](uint32_t source, uint32_t cpu) { calls.emplace_back(source, cpu); };
    Fixture f(std::move(hooks));
    f.Run({T3(pm4::INTERRUPT, 1), 0b1000101});  // bit 6: no such hardware thread
    const std::vector<std::pair<uint32_t, uint32_t>> expected = {{1, 0}, {1, 2}};
    CHECK(calls == expected);
    CHECK(f.cp.stats().interrupts.get() == 2);
}

TEST_CASE("XE_SWAP calls the hook with its words and bumps the counter") {
    std::vector<SwapPacket> swaps;
    std::vector<uint32_t> first_body;
    SyncCpHooks hooks;
    hooks.swap = [&](const SwapPacket& s) {
        if (swaps.empty()) first_body.assign(s.body.begin(), s.body.end());
        swaps.push_back(s);
    };
    Fixture f(std::move(hooks));
    std::vector<uint32_t> words = {T3(pm4::XE_SWAP, 63), 0x53574150, 0x1F000000, 1280, 720};
    words.resize(64, 0);
    words[63] = 0xE0F;
    f.Run(words);
    // a predicated swap is never run
    f.Run({T3(pm4::XE_SWAP, 4, true), 0x53574150, 0, 0, 0});
    REQUIRE(swaps.size() == 1);
    CHECK(swaps[0].magic == 0x53574150);
    CHECK(swaps[0].frontbuffer_ptr == 0x1F000000);
    CHECK(swaps[0].width == 1280);
    CHECK(swaps[0].height == 720);
    CHECK(swaps[0].index == 0);
    CHECK(first_body.size() == 63);
    CHECK(first_body[62] == 0xE0F);
    CHECK(f.cp.first_swap_body() == first_body);
    CHECK(f.cp.counter() == 1);
    CHECK(f.cp.stats().swaps.get() == 1);
    CHECK(f.cp.stats().predicated_skipped.get() == 1);
    CHECK(f.LogsWith("first XE_SWAP, 63 dwords: 53574150 1F000000") == 1);
}

TEST_CASE("predicated packets run only while a selected bin is in the mask") {
    Fixture f;
    f.Run({T3(pm4::MEM_WRITE, 2, true), 0x8400 | 2, 1,  // the bins start all on
           T3(pm4::SET_BIN_SELECT, 2), 0, 0,
           T3(pm4::MEM_WRITE, 2, true), 0x8404 | 2, 2,
           T3(pm4::SET_BIN_SELECT_LO, 1), 0x10, T3(pm4::SET_BIN_MASK_LO, 1), 0x30,
           T3(pm4::MEM_WRITE, 2, true), 0x8408 | 2, 3});
    CHECK(f.mem.be(0x8400) == 1);
    CHECK(f.mem.be(0x8404) == 0);
    CHECK(f.mem.be(0x8408) == 3);
}

TEST_CASE("MMIO: the write pointer, the 5 constant registers, unknown registers") {
    int woken = 0;
    SyncCpHooks hooks;
    hooks.write_pointer_updated = [&] { woken++; };
    Fixture f(std::move(hooks));
    f.mem.set_be(kRing, T0(kMarker, 1));
    f.mem.set_be(kRing + 4, 1);
    f.cp.MmioWrite(Mmio(reg::CP_RB_WPTR), 2);
    CHECK(woken == 1);
    CHECK(f.cp.write_index() == 2);
    CHECK(f.cp.ExecutePending());
    CHECK(f.cp.ReadRegister(kMarker) == 1);

    CHECK(f.cp.MmioRead(Mmio(0x0F00)) == 0x08100748);
    CHECK(f.cp.MmioRead(Mmio(0x0F01)) == 0x0000200E);
    CHECK(f.cp.MmioRead(Mmio(0x194C)) == 0x000002D0);
    CHECK(f.cp.MmioRead(Mmio(0x1951)) == 1);
    CHECK(f.cp.MmioRead(Mmio(0x1961)) == 0x050002D0);
    f.cp.MmioWrite(Mmio(0x2F10), 0x77);
    CHECK(f.cp.MmioRead(Mmio(0x2F10)) == 0x77);
    CHECK(f.cp.stats().unknown_register_reads.get() == 0);  // no table: all known

    const uint32_t known[] = {kMarker, 0x2F10};
    f.cp.SetKnownRegisters(known);
    f.cp.MmioRead(Mmio(0x2F11));
    f.cp.MmioRead(Mmio(0x2F11));
    f.cp.MmioWrite(Mmio(0x2F12), 5);
    f.cp.MmioRead(Mmio(0x2F10));
    CHECK(f.cp.stats().unknown_register_reads.get() == 2);
    CHECK(f.cp.stats().unknown_register_writes.get() == 1);
    CHECK(f.LogsWith("unknown register 2F11") == 1);
    CHECK(f.LogsWith("unknown register 2F12") == 1);
}

TEST_CASE("memory a packet names that isn't there is counted, not touched") {
    Fixture f;
    f.Run({T3(pm4::MEM_WRITE, 2), 0x00FFFFF0 | 2, 1,
           T3(pm4::INDIRECT_BUFFER, 2), 0x00FFF000, 4,
           T0(kMarker, 1), 1});
    CHECK(f.cp.stats().bad_addresses.get() == 2);
    CHECK(f.cp.ReadRegister(kMarker) == 1);
}

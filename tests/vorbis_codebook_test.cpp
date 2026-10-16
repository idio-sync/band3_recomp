// Checks native_vorbis's codebook decoder (src/Audio/vorbis_codebook.h): that
// it does, bit for bit, what the game's recompiled decode_packed_entry_number,
// vorbis_book_decode and vector decoders do, against hashes of the recompiled
// code's results on books, packets and floats any platform reproduces exactly;
// and that it is libvorbis 1.2.0's decoder on libogg 1.1.3's bit reader,
// against a plain port of those, on books built as vorbis_book_init_decode
// builds them and on packets cut short at every bit.

#include <doctest/doctest.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>
#include "src/Audio/vorbis_codebook.h"

using namespace band3::audio;

namespace {

// Books built the way libvorbis 1.2.0's vorbis_book_init_decode builds them,
// laid out in a fake big-endian guest memory, and a run of the five decoders
// over one: the same code made the hashes of the recompiled functions below.
namespace tb {

struct Lcg {
    uint32_t state;
    uint32_t Next() { return state = state * 1664525u + 1013904223u; }
    // [0, n)
    uint32_t Below(uint32_t n) { return uint32_t((uint64_t(Next()) * n) >> 32); }
};

uint32_t BitReverse(uint32_t x) {
    x = ((x >> 16) & 0x0000ffffu) | ((x << 16) & 0xffff0000u);
    x = ((x >> 8) & 0x00ff00ffu) | ((x << 8) & 0xff00ff00u);
    x = ((x >> 4) & 0x0f0f0f0fu) | ((x << 4) & 0xf0f0f0f0u);
    x = ((x >> 2) & 0x33333333u) | ((x << 2) & 0xccccccccu);
    return ((x >> 1) & 0x55555555u) | ((x << 1) & 0xaaaaaaaau);
}

int ILog(uint32_t v) {
    int ret = 0;
    while (v) {
        ret++;
        v >>= 1;
    }
    return ret;
}

// libvorbis 1.2.0's _make_words (sharedbook.c): the codewords of the used
// entries, LSb first; empty for an overpopulated tree
std::vector<uint32_t> MakeWords(const std::vector<int>& l, long sparsecount) {
    const long n = long(l.size());
    long count = 0;
    uint32_t marker[33] = {};
    std::vector<uint32_t> r(size_t(sparsecount ? sparsecount : n));
    for (long i = 0; i < n; i++) {
        const long length = l[size_t(i)];
        if (length > 0) {
            uint32_t entry = marker[length];
            if (length < 32 && (entry >> length)) return {};
            r[size_t(count++)] = entry;
            for (long j = length; j > 0; j--) {
                if (marker[j] & 1) {
                    if (j == 1)
                        marker[1]++;
                    else
                        marker[j] = marker[j - 1] << 1;
                    break;
                }
                marker[j]++;
            }
            for (long j = length + 1; j < 33; j++) {
                if ((marker[j] >> 1) == entry) {
                    entry = marker[j];
                    marker[j] = marker[j - 1] << 1;
                } else {
                    break;
                }
            }
        } else if (sparsecount == 0) {
            count++;
        }
    }
    for (long i = 0, c = 0; i < n; i++) {
        uint32_t temp = 0;
        for (long j = 0; j < l[size_t(i)]; j++) {
            temp <<= 1;
            temp |= (r[size_t(c)] >> j) & 1;
        }
        if (sparsecount) {
            if (l[size_t(i)]) r[size_t(c++)] = temp;
        } else {
            r[size_t(c++)] = temp;
        }
    }
    return r;
}

// a codebook as vorbis_book_init_decode (1.2.0) fills it, host side
struct Book {
    int dim = 0;
    int entries = 0;
    int used = 0;
    std::vector<uint32_t> codelist;
    std::vector<int32_t> dec_index;
    std::vector<int8_t> dec_codelengths;
    std::vector<uint32_t> firsttable;
    int firsttablen = 0;
    int maxlength = 0;
    std::vector<float> values;  // used * dim, in sorted order (_book_unquantize's)
};

// lengths: per entry, 0 for unused; values: per entry, dim each
bool InitDecode(const std::vector<int>& lengths, int dim, const std::vector<float>& values,
                Book& c) {
    c = {};
    c.entries = int(lengths.size());
    int n = 0;
    for (int len : lengths) n += len > 0;
    c.used = n;
    c.dim = dim;
    if (n == 0) return false;
    std::vector<uint32_t> codes = MakeWords(lengths, n);
    if (codes.empty()) return false;
    for (auto& w : codes) w = BitReverse(w);
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    // sort32a: by the reversed codeword, unsigned (ties can't happen in a valid tree)
    std::stable_sort(order.begin(), order.end(),
                     [&](int x, int y) { return codes[size_t(x)] < codes[size_t(y)]; });
    std::vector<int> sortindex(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) sortindex[size_t(order[size_t(i)])] = i;
    c.codelist.assign(size_t(n), 0);
    for (int i = 0; i < n; i++) c.codelist[size_t(sortindex[size_t(i)])] = codes[size_t(i)];
    c.values.assign(size_t(n) * size_t(dim), 0.0f);
    c.dec_index.assign(size_t(n), 0);
    c.dec_codelengths.assign(size_t(n), 0);
    for (int k = 0, i = 0; i < c.entries; i++) {
        if (lengths[size_t(i)] > 0) {
            const int s = sortindex[size_t(k++)];
            c.dec_index[size_t(s)] = i;
            c.dec_codelengths[size_t(s)] = int8_t(lengths[size_t(i)]);
            for (int d = 0; d < dim; d++) {
                c.values[size_t(s) * size_t(dim) + size_t(d)] =
                    values[size_t(i) * size_t(dim) + size_t(d)];
            }
        }
    }
    c.firsttablen = ILog(uint32_t(c.used)) - 4;
    if (c.firsttablen < 5) c.firsttablen = 5;
    if (c.firsttablen > 8) c.firsttablen = 8;
    const int tabn = 1 << c.firsttablen;
    c.firsttable.assign(size_t(tabn), 0);
    c.maxlength = 0;
    for (int i = 0; i < n; i++) {
        if (c.maxlength < c.dec_codelengths[size_t(i)]) c.maxlength = c.dec_codelengths[size_t(i)];
        if (c.dec_codelengths[size_t(i)] <= c.firsttablen) {
            const uint32_t orig = BitReverse(c.codelist[size_t(i)]);
            for (int j = 0; j < (1 << (c.firsttablen - c.dec_codelengths[size_t(i)])); j++) {
                c.firsttable[orig | (uint32_t(j) << c.dec_codelengths[size_t(i)])] = uint32_t(i + 1);
            }
        }
    }
    const uint32_t mask = 0xfffffffeu << (31 - c.firsttablen);
    long lo = 0, hi = 0;
    for (int i = 0; i < tabn; i++) {
        const uint32_t word = uint32_t(i) << (32 - c.firsttablen);
        if (c.firsttable[BitReverse(word)] == 0) {
            while ((lo + 1) < n && c.codelist[size_t(lo + 1)] <= word) lo++;
            while (hi < n && word >= (c.codelist[size_t(hi)] & mask)) hi++;
            unsigned long loval = (unsigned long)lo;
            unsigned long hival = (unsigned long)(n - hi);
            if (loval > 0x7fff) loval = 0x7fff;
            if (hival > 0x7fff) hival = 0x7fff;
            c.firsttable[BitReverse(word)] = uint32_t(0x80000000ul | (loval << 15) | hival);
        }
    }
    return true;
}

// codeword lengths of a random complete tree of `leaves` leaves, none longer
// than maxlen: split random leaves, now and then the deepest one, so some get
// long
std::vector<int> RandomTree(Lcg& rng, int leaves, int maxlen) {
    std::vector<int> depth{0};
    while (int(depth.size()) < leaves) {
        size_t pick;
        if (rng.Below(4) == 0) {
            pick = size_t(std::max_element(depth.begin(), depth.end()) - depth.begin());
        } else {
            pick = rng.Below(uint32_t(depth.size()));
        }
        if (depth[pick] >= maxlen) {
            // find any leaf that can still split
            pick = size_t(std::min_element(depth.begin(), depth.end()) - depth.begin());
            if (depth[pick] >= maxlen) break;
        }
        depth[pick]++;
        depth.push_back(depth[pick]);
    }
    // shuffle which entry gets which length
    for (size_t i = depth.size(); i > 1; i--) std::swap(depth[i - 1], depth[rng.Below(uint32_t(i))]);
    return depth;
}

// a float every platform makes the same: small multiples, now and then a
// denormal, a negative zero, a large one
float RandomValue(Lcg& rng) {
    const uint32_t pick = rng.Below(16);
    if (pick == 0) return std::bit_cast<float>(rng.Next() & 0x807FFFFFu);  // denormal or zero
    if (pick == 1) return -0.0f;
    if (pick == 2) return std::bit_cast<float>((rng.Next() & 0x83FFFFFFu) | 0x40000000u);
    return float(int32_t(rng.Below(65)) - 32) * 0.125f +
           float(int32_t(rng.Next() >> 8) - 0x800000) * (1.0f / 16777216.0f);
}

// big-endian stores into a fake guest memory
void Put32(uint8_t* mem, uint32_t address, uint32_t v) {
    const uint32_t raw = std::byteswap(v);
    std::memcpy(mem + address, &raw, 4);
}
uint32_t Get32(const uint8_t* mem, uint32_t address) {
    uint32_t raw;
    std::memcpy(&raw, mem + address, 4);
    return std::byteswap(raw);
}

// where a Book goes in the fake guest memory: the 44-byte codebook at `at`,
// its tables after it
struct Placed {
    uint32_t book;
    uint32_t end;
};
Placed Place(uint8_t* mem, uint32_t at, const Book& c) {
    uint32_t next = at + 64;
    auto table = [&](size_t bytes) {
        const uint32_t a = next;
        next += uint32_t((bytes + 15) & ~size_t(15)) + 16;
        return a;
    };
    const uint32_t values = table(c.values.size() * 4);
    const uint32_t codelist = table(c.codelist.size() * 4);
    const uint32_t index = table(c.dec_index.size() * 4);
    const uint32_t lengths = table(c.dec_codelengths.size());
    const uint32_t first = table(c.firsttable.size() * 4);
    for (size_t i = 0; i < c.values.size(); i++)
        Put32(mem, values + 4 * uint32_t(i), std::bit_cast<uint32_t>(c.values[i]));
    for (size_t i = 0; i < c.codelist.size(); i++) Put32(mem, codelist + 4 * uint32_t(i), c.codelist[i]);
    for (size_t i = 0; i < c.dec_index.size(); i++)
        Put32(mem, index + 4 * uint32_t(i), uint32_t(c.dec_index[i]));
    for (size_t i = 0; i < c.dec_codelengths.size(); i++)
        mem[lengths + i] = uint8_t(c.dec_codelengths[i]);
    for (size_t i = 0; i < c.firsttable.size(); i++) Put32(mem, first + 4 * uint32_t(i), c.firsttable[i]);
    const uint32_t words[11] = {uint32_t(c.dim), uint32_t(c.entries), uint32_t(c.used), 0, values,
                                codelist, index, lengths, first, uint32_t(c.firsttablen),
                                uint32_t(c.maxlength)};
    for (int i = 0; i < 11; i++) Put32(mem, at + 4 * uint32_t(i), words[i]);
    return {at, next};
}

}

namespace tb {

// guest addresses in the fake memory (all below 16 MB)
constexpr uint32_t kBook = 0x1000;
constexpr uint32_t kOpb = 0x0800;
constexpr uint32_t kChannelList = 0x0900;
constexpr uint32_t kPacket = 0x100000;
constexpr uint32_t kFloats = 0x200000;
constexpr uint32_t kChannelStride = 0x8000;  // 8192 floats a channel
constexpr int kChannels = 8;
constexpr uint32_t kMemory = 0x400000;

enum Kind { kDecode, kBookDecode, kDecodevAdd, kDecodevsAdd, kDecodevvAdd, kKinds };

struct Step {
    Kind kind;
    bool reset;  // put the reader at endbyte/endbit first
    uint32_t endbyte, endbit;
    int32_t n, offset, ch;
};

struct Scenario {
    Book book;
    uint32_t storage = 0;
    std::vector<Step> steps;
};

// what one decoder does to the fake guest memory at `mem`
struct Ops {
    virtual ~Ops() = default;
    virtual int64_t Decode(Kind kind, uint8_t* mem, const Step& s) = 0;
};

void PutReader(uint8_t* mem, uint32_t storage, uint32_t endbyte, uint32_t endbit) {
    Put32(mem, kOpb + 0, endbyte);
    Put32(mem, kOpb + 4, endbit);
    Put32(mem, kOpb + 8, kPacket);
    Put32(mem, kOpb + 12, kPacket + endbyte);
    Put32(mem, kOpb + 16, storage);
}

// a book, a packet, the floats and the steps from one seed. false: the seed's
// lengths made no book (overpopulated can't happen; one used entry can)
bool MakeScenario(uint32_t seed, uint8_t* mem, Scenario& sc) {
    Lcg rng{seed * 0x9E3779B9u + 1u};
    // the book
    static const int kSizes[][2] = {{2, 8}, {9, 64}, {65, 600}, {600, 3000}};
    const auto& size = kSizes[rng.Below(4)];
    const int leaves = size[0] + int(rng.Below(uint32_t(size[1] - size[0] + 1)));
    static const int kMaxLen[] = {8, 12, 16, 24, 32, 32};
    const int maxlen = std::max(kMaxLen[rng.Below(6)], ILog(uint32_t(leaves)));
    std::vector<int> tree = RandomTree(rng, leaves, maxlen);
    std::vector<int> lengths;
    const uint32_t shape = rng.Below(6);
    for (int len : tree) {
        // sparse: unused entries between the used ones
        if (shape <= 1) {
            while (rng.Below(3) == 0) lengths.push_back(0);
        }
        // underpopulated: some codewords nobody has
        if (shape == 2 && rng.Below(5) == 0) {
            lengths.push_back(0);
            continue;
        }
        lengths.push_back(len);
    }
    static const int kDims[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 2, 4, 8};
    const int dim = kDims[rng.Below(14)];
    std::vector<float> values(lengths.size() * size_t(dim));
    for (float& v : values) v = RandomValue(rng);
    if (!InitDecode(lengths, dim, values, sc.book)) return false;

    std::memset(mem, 0, kMemory);
    Place(mem, kBook, sc.book);
    // the packet: random bits, and random bytes past its end too
    sc.storage = 1 + rng.Below(rng.Below(4) == 0 ? 64 : 6000);
    for (uint32_t i = 0; i < sc.storage + 64; i++) mem[kPacket + i] = uint8_t(rng.Next() >> 24);
    for (uint32_t i = 0; i < uint32_t(kChannels) * kChannelStride; i++) {
        Put32(mem, kFloats + 4 * i, std::bit_cast<uint32_t>(RandomValue(rng)));
    }
    for (int c = 0; c < kChannels; c++) {
        Put32(mem, kChannelList + 4 * uint32_t(c), kFloats + uint32_t(c) * kChannelStride);
    }

    // the steps: a reader somewhere in the packet, often its last bytes, then
    // calls until the packet runs out
    sc.steps.clear();
    for (int run = 0; run < 6; run++) {
        Step s{};
        s.reset = true;
        const uint32_t tail = std::min<uint32_t>(sc.storage, 1 + rng.Below(12));
        s.endbyte = rng.Below(2) ? sc.storage - tail : rng.Below(sc.storage);
        s.endbit = rng.Below(8);
        const int calls = 1 + int(rng.Below(40));
        for (int k = 0; k < calls; k++) {
            s.kind = Kind(rng.Below(kKinds));
            s.n = int32_t(rng.Below(rng.Below(4) == 0 ? 200 : 40));
            s.ch = 1 + int32_t(rng.Below(kChannels));
            s.offset = int32_t(rng.Below(64));
            sc.steps.push_back(s);
            s.reset = false;
        }
    }
    return true;
}

uint64_t Fnv(uint64_t h, uint32_t w) {
    for (int b = 0; b < 4; b++) {
        h ^= (w >> (8 * b)) & 0xFF;
        h *= 0x100000001b3ull;
    }
    return h;
}

// runs a scenario's steps; returns an FNV-1a of every result, the reader after
// every step and the floats at the end
uint64_t RunScenario(const Scenario& sc, uint8_t* mem, Ops& ops) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const Step& s : sc.steps) {
        if (s.reset) PutReader(mem, sc.storage, s.endbyte, s.endbit);
        const int64_t r = ops.Decode(s.kind, mem, s);
        h = Fnv(h, uint32_t(uint64_t(r)));
        h = Fnv(h, uint32_t(uint64_t(r) >> 32));
        for (uint32_t o : {0u, 4u, 12u}) h = Fnv(h, Get32(mem, kOpb + o));
    }
    for (uint32_t i = 0; i < uint32_t(kChannels) * kChannelStride; i++) h = Fnv(h, Get32(mem, kFloats + 4 * i));
    return h;
}

}

// libogg 1.1.3's mask table, as bitwise.c writes it
const uint32_t kMask[33] = {
    0x00000000, 0x00000001, 0x00000003, 0x00000007, 0x0000000f, 0x0000001f, 0x0000003f,
    0x0000007f, 0x000000ff, 0x000001ff, 0x000003ff, 0x000007ff, 0x00000fff, 0x00001fff,
    0x00003fff, 0x00007fff, 0x0000ffff, 0x0001ffff, 0x0003ffff, 0x0007ffff, 0x000fffff,
    0x001fffff, 0x003fffff, 0x007fffff, 0x00ffffff, 0x01ffffff, 0x03ffffff, 0x07ffffff,
    0x0fffffff, 0x1fffffff, 0x3fffffff, 0x7fffffff, 0xffffffff};

// The reference: libogg 1.1.3's oggpack_look / oggpack_adv and libvorbis
// 1.2.0's decoders as their C is written, with the guest's 32-bit long (and
// the shifts of bytes made unsigned, which C leaves undefined past bit 31).
// They read the book from the host-side Book and the packet and floats from
// the fake guest memory.
struct RefBuffer {
    int32_t endbyte;
    int32_t endbit;
    const uint8_t* ptr;
    int32_t storage;
};

int32_t RefLook(const RefBuffer* b, int bits) {
    uint32_t ret;
    const uint32_t m = kMask[bits];
    bits += b->endbit;
    if (b->endbyte + 4 >= b->storage) {
        // not the main path
        if (b->endbyte * 8 + bits > b->storage * 8) return -1;
    }
    ret = b->ptr[0] >> b->endbit;
    if (bits > 8) {
        ret |= uint32_t(b->ptr[1]) << (8 - b->endbit);
        if (bits > 16) {
            ret |= uint32_t(b->ptr[2]) << (16 - b->endbit);
            if (bits > 24) {
                ret |= uint32_t(b->ptr[3]) << (24 - b->endbit);
                if (bits > 32 && b->endbit) ret |= uint32_t(b->ptr[4]) << (32 - b->endbit);
            }
        }
    }
    return int32_t(m & ret);
}

void RefAdv(RefBuffer* b, int bits) {
    bits += b->endbit;
    b->ptr += bits / 8;
    b->endbyte += bits / 8;
    b->endbit = bits & 7;
}

int32_t RefDecodePacked(const tb::Book& book, RefBuffer* b) {
    int read = book.maxlength;
    int32_t lo, hi;
    int32_t lok = RefLook(b, book.firsttablen);
    if (lok >= 0) {
        const int32_t entry = int32_t(book.firsttable[size_t(lok)]);
        if (uint32_t(entry) & 0x80000000u) {
            lo = (entry >> 15) & 0x7fff;
            hi = book.used - (entry & 0x7fff);
        } else {
            RefAdv(b, book.dec_codelengths[size_t(entry - 1)]);
            return entry - 1;
        }
    } else {
        lo = 0;
        hi = book.used;
    }
    lok = RefLook(b, read);
    while (lok < 0 && read > 1) lok = RefLook(b, --read);
    if (lok < 0) return -1;
    {
        const uint32_t testword = tb::BitReverse(uint32_t(lok));
        while (hi - lo > 1) {
            const int32_t p = (hi - lo) >> 1;
            const int32_t test = book.codelist[size_t(lo + p)] > testword;
            lo += p & (test - 1);
            hi -= p & (-test);
        }
        if (book.dec_codelengths[size_t(lo)] <= read) {
            RefAdv(b, book.dec_codelengths[size_t(lo)]);
            return lo;
        }
    }
    RefAdv(b, read);
    return -1;
}

int32_t RefBookDecode(const tb::Book& book, RefBuffer* b) {
    const int32_t packed = RefDecodePacked(book, b);
    if (packed >= 0) return book.dec_index[size_t(packed)];
    return packed;
}

float GetF(const uint8_t* mem, uint32_t address) {
    return std::bit_cast<float>(tb::Get32(mem, address));
}

// a[i] += v, at a guest address; reach: one past the furthest float added to
void AddF(uint8_t* mem, uint32_t a, int32_t i, float v, int64_t* reach) {
    const uint32_t address = a + 4 * uint32_t(i);
    tb::Put32(mem, address, std::bit_cast<uint32_t>(GetF(mem, address) + v));
    *reach = std::max<int64_t>(*reach, int64_t(i) + 1);
}

int32_t RefDecodevAdd(const tb::Book& book, uint8_t* mem, uint32_t a, RefBuffer* b, int n,
                      int64_t* reach) {
    int i, j, entry;
    const float* t;
    if (book.dim > 8) {
        for (i = 0; i < n;) {
            entry = RefDecodePacked(book, b);
            if (entry == -1) return -1;
            t = book.values.data() + size_t(entry) * size_t(book.dim);
            for (j = 0; j < book.dim;) AddF(mem, a, i++, t[j++], reach);
        }
    } else {
        for (i = 0; i < n;) {
            entry = RefDecodePacked(book, b);
            if (entry == -1) return -1;
            t = book.values.data() + size_t(entry) * size_t(book.dim);
            j = 0;
            switch (book.dim) {
            case 8: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 7: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 6: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 5: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 4: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 3: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 2: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 1: AddF(mem, a, i++, t[j++], reach); [[fallthrough]];
            case 0: break;
            }
        }
    }
    return 0;
}

int32_t RefDecodevsAdd(const tb::Book& book, uint8_t* mem, uint32_t a, RefBuffer* b, int n,
                       int64_t* reach) {
    const int step = n / book.dim;
    std::vector<int32_t> entry(size_t(std::max(step, 0)));
    std::vector<const float*> t(size_t(std::max(step, 0)));
    int i, j, o;
    for (i = 0; i < step; i++) {
        entry[size_t(i)] = RefDecodePacked(book, b);
        if (entry[size_t(i)] == -1) return -1;
        t[size_t(i)] = book.values.data() + size_t(entry[size_t(i)]) * size_t(book.dim);
    }
    for (i = 0, o = 0; i < book.dim; i++, o += step) {
        for (j = 0; j < step; j++) AddF(mem, a, o + j, t[size_t(j)][i], reach);
    }
    return 0;
}

// reach: per channel, counted from its float offset / ch
int32_t RefDecodevvAdd(const tb::Book& book, uint8_t* mem, uint32_t a, int32_t offset, int ch,
                       RefBuffer* b, int n, int64_t* reach) {
    int32_t i, j, entry;
    int chptr = 0;
    const int32_t first = offset / ch;
    for (i = offset / ch; i < (offset + n) / ch;) {
        entry = RefDecodePacked(book, b);
        if (entry == -1) return -1;
        const float* t = book.values.data() + size_t(entry) * size_t(book.dim);
        for (j = 0; j < book.dim; j++) {
            const uint32_t channel = tb::Get32(mem, a + 4 * uint32_t(chptr));
            AddF(mem, channel + 4 * uint32_t(first), i - first, t[j], &reach[chptr]);
            chptr++;
            if (chptr == ch) {
                chptr = 0;
                i++;
            }
        }
    }
    return 0;
}

VorbisCodebook LoadBook(const uint8_t* mem) {
    auto w = [&](uint32_t offset) { return tb::Get32(mem, tb::kBook + offset); };
    return {int32_t(w(0)), int32_t(w(4)), int32_t(w(8)),  w(16), w(20),
            w(24),         w(28),         w(32),          int32_t(w(36)), int32_t(w(40))};
}

// the native decoder on the fake guest memory, the reader loaded from and
// stored back to its oggpack_buffer as the hooks do
struct NativeOps : tb::Ops {
    int64_t Decode(tb::Kind kind, uint8_t* mem, const tb::Step& s) override {
        const VorbisGuest guest{mem, 0};
        const VorbisCodebook book = LoadBook(mem);
        auto w = [&](uint32_t offset) { return tb::Get32(mem, tb::kOpb + offset); };
        VorbisBitReader b{w(0), w(4), w(8), w(12), w(16)};
        REQUIRE(VorbisCodebookDecodable(book));
        REQUIRE(VorbisBitReaderUsable(b));
        REQUIRE(VorbisCodebookHasValues(book));
        int64_t r = 0;
        uint8_t* a = mem + tb::kFloats + 4 * uint32_t(s.offset);
        switch (kind) {
        case tb::kDecode: r = VorbisDecodePackedEntryNumber(guest, book, b); break;
        case tb::kBookDecode: r = VorbisBookDecode(guest, book, b); break;
        case tb::kDecodevAdd: r = VorbisBookDecodevAdd(guest, book, a, b, s.n); break;
        case tb::kDecodevsAdd: r = VorbisBookDecodevsAdd(guest, book, a, b, s.n); break;
        case tb::kDecodevvAdd: {
            uint8_t* channels[tb::kChannels];
            for (int c = 0; c < s.ch; c++) {
                channels[c] = mem + tb::Get32(mem, tb::kChannelList + 4 * uint32_t(c)) +
                              4 * uint32_t(s.offset / s.ch);
            }
            r = VorbisBookDecodevvAdd(guest, book, channels, s.offset, s.ch, b, s.n);
            break;
        }
        default: break;
        }
        tb::Put32(mem, tb::kOpb + 0, b.endbyte);
        tb::Put32(mem, tb::kOpb + 4, b.endbit);
        tb::Put32(mem, tb::kOpb + 12, b.ptr);
        return r;
    }
};

// the reference on the same kind of memory; reach: how far it added, per
// channel (decodev/decodevs: channel 0)
struct RefOps : tb::Ops {
    const tb::Book* book = nullptr;
    int64_t reach[tb::kChannels] = {};

    int64_t Decode(tb::Kind kind, uint8_t* mem, const tb::Step& s) override {
        std::fill(std::begin(reach), std::end(reach), 0);
        const uint32_t endbyte = tb::Get32(mem, tb::kOpb + 0);
        RefBuffer b{int32_t(endbyte), int32_t(tb::Get32(mem, tb::kOpb + 4)),
                    mem + tb::Get32(mem, tb::kOpb + 12), int32_t(tb::Get32(mem, tb::kOpb + 16))};
        const uint32_t a = tb::kFloats + 4 * uint32_t(s.offset);
        int64_t r = 0;
        switch (kind) {
        case tb::kDecode: r = RefDecodePacked(*book, &b); break;
        case tb::kBookDecode: r = RefBookDecode(*book, &b); break;
        case tb::kDecodevAdd: r = RefDecodevAdd(*book, mem, a, &b, s.n, reach); break;
        case tb::kDecodevsAdd: r = RefDecodevsAdd(*book, mem, a, &b, s.n, reach); break;
        case tb::kDecodevvAdd:
            r = RefDecodevvAdd(*book, mem, tb::kChannelList, s.offset, s.ch, &b, s.n, reach);
            break;
        default: break;
        }
        tb::Put32(mem, tb::kOpb + 0, uint32_t(b.endbyte));
        tb::Put32(mem, tb::kOpb + 4, uint32_t(b.endbit));
        tb::Put32(mem, tb::kOpb + 12, uint32_t(b.ptr - mem));
        return r;
    }
};

// the floats a step can add to, as the hooks' check copies them: per channel,
// from where the decoder starts
struct Region {
    uint32_t address;
    int64_t count;
};
std::vector<Region> Regions(const tb::Step& s, int dim, const uint8_t* mem) {
    const uint32_t a = tb::kFloats + 4 * uint32_t(s.offset);
    switch (s.kind) {
    case tb::kDecodevAdd: return {{a, VorbisDecodevAddSpan(dim, s.n)}};
    case tb::kDecodevsAdd: return {{a, VorbisDecodevsAddSpan(dim, s.n)}};
    case tb::kDecodevvAdd: {
        std::vector<Region> regions;
        for (int c = 0; c < s.ch; c++) {
            regions.push_back({tb::Get32(mem, tb::kChannelList + 4 * uint32_t(c)) +
                                   4 * uint32_t(s.offset / s.ch),
                               VorbisDecodevvAddSpan(dim, s.offset, s.ch, s.n, c)});
        }
        return regions;
    }
    default: return {};
    }
}

}

TEST_CASE("native_vorbis's codebook decoder does what the game's recompiled one does, bit for bit") {
    // FNV-1a (tb::RunScenario) of every result, the reader after every call
    // and the floats at the end, from the recompiled functions run on the
    // scenario each seed makes. To make them again: compile the generated
    // DEFINE_REX_FUNC bodies of oggpack_look, oggpack_adv,
    // decode_packed_entry_number, vorbis_book_decode and
    // vorbis_book_decodevs_add, _decodev_add and _decodevv_add against a
    // stand-in SDK (a PPCContext of plain registers with a ctr, a no-op fpscr,
    // REX_LOAD_U8/U32/U64 and REX_STORE_U32/U64 byte-swapping at base +
    // address, ppc_trap aborting, RtlCheckStack12 doing nothing, the
    // __savegprlr / __restgprlr 22, 25, 26 and 27 millicode as a host-side
    // stack of r14-r31) with 4 GB reserved as guest memory, libogg's 33 masks
    // stored big-endian at 0x82CADC98 where the game keeps them; build each
    // seed's scenario into it with tb::MakeScenario and run tb::RunScenario
    // with an Ops that calls the recompiled function for each step (r1 =
    // 0x01F00000, r3 = the book, then the arguments as NativeOps passes them:
    // r4 the floats or the reader, and so on) and returns r3.
    const struct {
        uint32_t seed;
        uint64_t hash;
        const char* what;
    } kRecompiled[] = {
        {1, 0x2FB955585E00C373ull, "9 entries, 8 used, dim 7"},
        {3, 0x9AE95E35BD8E4981ull, "39 entries, 23 used, dim 16"},
        {7, 0x53D9509144576A87ull, "codewords to 24 bits, a 48-byte packet"},
        {12, 0x01952F58C8A801EAull, "577 entries, a 6-bit first table"},
        {15, 0xB29FC8814C1641C4ull, "codewords to 32 bits, sparse, a 31-byte packet"},
        {17, 0x04DCFC5EAAA201E7ull, "codewords to 32 bits, sparse, dim 8"},
        {19, 0x30B9C39DD415957Dull, "dim 9, a 38-byte packet"},
        {21, 0x15ADFB9D1DF8340Full, "2148 entries, an 8-bit first table"},
        {22, 0x20986B88B2FC4900ull, "codewords to 32 bits, sparse, a 7-bit first table"},
    };
    std::vector<uint8_t> mem(tb::kMemory);
    for (const auto& g : kRecompiled) {
        CAPTURE(g.seed);
        CAPTURE(g.what);
        tb::Scenario sc;
        REQUIRE(tb::MakeScenario(g.seed, mem.data(), sc));
        NativeOps native;
        CHECK(tb::RunScenario(sc, mem.data(), native) == g.hash);
    }
}

TEST_CASE("native_vorbis's codebook decoder is libvorbis 1.2.0's on libogg 1.1.3's bit reader") {
    // random books (2 to 3000 entries, dims 1 to 16, sparse and
    // underpopulated ones, codewords up to 32 bits), random packets read from
    // random places, often their last bytes, every call checked against the
    // reference: what it returns, where it leaves the reader, the floats
    std::vector<uint8_t> native_mem(tb::kMemory);
    std::vector<uint8_t> ref_mem(tb::kMemory);
    uint64_t steps = 0;
    uint64_t failed = 0;
    uint64_t different = 0;
    for (uint32_t seed = 1000; seed < 1150; seed++) {
        tb::Scenario sc;
        if (!tb::MakeScenario(seed, native_mem.data(), sc)) continue;
        std::memcpy(ref_mem.data(), native_mem.data(), tb::kMemory);
        NativeOps native;
        RefOps ref;
        ref.book = &sc.book;
        for (const tb::Step& s : sc.steps) {
            if (s.reset) {
                tb::PutReader(native_mem.data(), sc.storage, s.endbyte, s.endbit);
                tb::PutReader(ref_mem.data(), sc.storage, s.endbyte, s.endbit);
            }
            const int64_t r = native.Decode(s.kind, native_mem.data(), s);
            const int64_t expected = ref.Decode(s.kind, ref_mem.data(), s);
            steps++;
            failed += r < 0;
            bool same = r == expected &&
                        std::memcmp(native_mem.data() + tb::kOpb, ref_mem.data() + tb::kOpb, 20) == 0;
            // the floats each can add to; all of them when it decodes all
            // its entries, never more
            const std::vector<Region> regions = Regions(s, sc.book.dim, native_mem.data());
            for (size_t c = 0; c < regions.size(); c++) {
                const Region& g = regions[c];
                same = same && ref.reach[c] <= g.count && (expected != 0 || ref.reach[c] == g.count);
                same = same && std::memcmp(native_mem.data() + g.address, ref_mem.data() + g.address,
                                           size_t(g.count) * 4) == 0;
            }
            if (!same) {
                if (different == 0) {
                    CAPTURE(seed);
                    CAPTURE(int(s.kind));
                    CAPTURE(s.n);
                    CAPTURE(s.offset);
                    CAPTURE(s.ch);
                    CAPTURE(sc.book.dim);
                    CHECK(r == expected);
                    CHECK(tb::Get32(native_mem.data(), tb::kOpb) == tb::Get32(ref_mem.data(), tb::kOpb));
                    CHECK(tb::Get32(native_mem.data(), tb::kOpb + 4) ==
                          tb::Get32(ref_mem.data(), tb::kOpb + 4));
                    FAIL("the native decoder and the reference differ");
                }
                different++;
            }
        }
        // and nothing else changed
        CHECK(std::memcmp(native_mem.data(), ref_mem.data(), tb::kMemory) == 0);
    }
    CHECK(different == 0);
    // enough of both kinds of call
    CHECK(steps > 10000);
    CHECK(failed > steps / 10);
    CHECK(failed < steps * 9 / 10);
}

TEST_CASE("native_vorbis's codebook decoder reads to the last bit of a packet as the game does") {
    // every start in a packet's last bytes, every bit of them: one codeword
    // at a time to the end (oggpack_look's -1 within the last four bytes, the
    // shorter looks after it, a codeword longer than what's left taking what's
    // left), and a decodev_add from there
    std::vector<uint8_t> native_mem(tb::kMemory);
    std::vector<uint8_t> ref_mem(tb::kMemory);
    std::vector<uint8_t> floats(4096);
    for (uint32_t seed : {2u, 7u, 15u, 17u, 22u, 23u}) {
        tb::Scenario sc;
        REQUIRE(tb::MakeScenario(seed, native_mem.data(), sc));
        std::memcpy(ref_mem.data(), native_mem.data(), tb::kMemory);
        std::memcpy(floats.data(), native_mem.data() + tb::kFloats, floats.size());
        for (uint32_t storage : {1u, 2u, 3u, 4u, 5u, 6u, 9u}) {
            for (uint32_t endbyte = storage > 6 ? storage - 6 : 0; endbyte <= storage; endbyte++) {
                for (uint32_t endbit = 0; endbit < 8; endbit++) {
                    if (endbyte == storage && endbit > 0) continue;
                    CAPTURE(seed);
                    CAPTURE(storage);
                    CAPTURE(endbyte);
                    CAPTURE(endbit);
                    for (tb::Kind kind : {tb::kDecode, tb::kDecodevAdd}) {
                        for (uint8_t* mem : {native_mem.data(), ref_mem.data()}) {
                            std::memcpy(mem + tb::kFloats, floats.data(), floats.size());
                            tb::PutReader(mem, storage, endbyte, endbit);
                        }
                        NativeOps native;
                        RefOps ref;
                        ref.book = &sc.book;
                        tb::Step s{};
                        s.kind = kind;
                        s.n = 3 * sc.book.dim + 1;
                        for (int call = 0; call < 80; call++) {
                            const int64_t r = native.Decode(kind, native_mem.data(), s);
                            const int64_t expected = ref.Decode(kind, ref_mem.data(), s);
                            REQUIRE(r == expected);
                            REQUIRE(std::memcmp(native_mem.data() + tb::kOpb,
                                                ref_mem.data() + tb::kOpb, 20) == 0);
                            if (r < 0 && call > 2) break;
                        }
                        REQUIRE(std::memcmp(native_mem.data() + tb::kFloats,
                                            ref_mem.data() + tb::kFloats, floats.size()) == 0);
                    }
                }
            }
        }
    }
}

TEST_CASE("a 32-bit codeword whose last bit is 1 doesn't decode, as on the guest's 32-bit long") {
    // lengths 1, 2, ..., 31, 32, 32: the two 32-bit codewords are 31 ones and
    // a 0 or a 1. oggpack_look's 32 bits of the second come back with the top
    // bit set, which decode_packed_entry_number takes as -1; it looks again at
    // 31 bits, finds a codeword longer than that, takes the 31 bits and
    // returns -1. The first decodes.
    std::vector<int> lengths;
    for (int i = 1; i <= 32; i++) lengths.push_back(i);
    lengths.push_back(32);
    std::vector<float> values(lengths.size(), 1.0f);
    tb::Book book;
    REQUIRE(tb::InitDecode(lengths, 1, values, book));
    std::vector<uint8_t> mem(tb::kMemory);
    tb::Place(mem.data(), tb::kBook, book);
    const VorbisGuest guest{mem.data(), 0};
    const VorbisCodebook c = LoadBook(mem.data());
    for (uint8_t last : {uint8_t(0x7F), uint8_t(0xFF)}) {
        CAPTURE(int(last));
        for (uint32_t i = 0; i < 8; i++) mem[tb::kPacket + i] = 0xFF;
        mem[tb::kPacket + 3] = last;
        VorbisBitReader b{0, 0, tb::kPacket, tb::kPacket, 8};
        const int32_t entry = VorbisDecodePackedEntryNumber(guest, c, b);
        if (last == 0x7F) {
            CHECK(entry >= 0);
            CHECK(book.dec_index[size_t(entry)] == 31);
            CHECK(b.endbyte == 4);
            CHECK(b.endbit == 0);
        } else {
            CHECK(entry == -1);
            CHECK(b.endbyte == 3);
            CHECK(b.endbit == 7);
            CHECK(b.ptr == tb::kPacket + 3);
        }
    }
}

TEST_CASE("native_vorbis takes only books and readers the game's decoder could have made") {
    for (int i = 0; i <= 32; i++) CHECK(VorbisLookMask(i) == kMask[i]);

    const VorbisCodebook good{4, 10, 8, 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 5, 12};
    CHECK(VorbisCodebookDecodable(good));
    CHECK(VorbisCodebookHasValues(good));
    auto with = [&](auto change) {
        VorbisCodebook b = good;
        change(b);
        return b;
    };
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.codelist = 0; })));
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.dec_codelengths = 0; })));
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.dec_firsttable = 0; })));
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.dec_firsttablen = 33; })));
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.dec_firsttablen = -1; })));
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.dec_maxlength = 33; })));
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.used_entries = 0; })));
    CHECK_FALSE(VorbisCodebookDecodable(with([](VorbisCodebook& b) { b.used_entries = 11; })));
    CHECK_FALSE(VorbisCodebookHasValues(with([](VorbisCodebook& b) { b.valuelist = 0; })));
    CHECK_FALSE(VorbisCodebookHasValues(with([](VorbisCodebook& b) { b.dim = 0; })));
    CHECK(VorbisBitReaderUsable({0, 7, 0x100, 0x100, 10}));
    CHECK_FALSE(VorbisBitReaderUsable({0, 8, 0x100, 0x100, 10}));
    CHECK_FALSE(VorbisBitReaderUsable({0, 0, 0, 0, 10}));

    // the floats the hooks' check copies: whole entries past n when dim
    // doesn't divide it; decodevs_add's n / dim of them; decodevv_add's
    // dealt round the channels from offset / ch
    CHECK(VorbisDecodevAddSpan(4, 8) == 8);
    CHECK(VorbisDecodevAddSpan(4, 10) == 12);
    CHECK(VorbisDecodevAddSpan(4, 0) == 0);
    CHECK(VorbisDecodevAddSpan(4, -3) == 0);
    CHECK(VorbisDecodevsAddSpan(3, 10) == 9);
    CHECK(VorbisDecodevsAddSpan(3, 2) == 0);
    CHECK(VorbisDecodevvAddSpan(2, 0, 2, 6, 0) == 3);
    CHECK(VorbisDecodevvAddSpan(2, 0, 2, 6, 1) == 3);
    CHECK(VorbisDecodevvAddSpan(4, 0, 2, 6, 0) == 4);  // two entries: one past the end each
    // floats 2 and 3 of two channels: two entries of three values, so floats 2
    // to 4 of both
    CHECK(VorbisDecodevvAddSpan(3, 4, 2, 4, 0) == 3);
    CHECK(VorbisDecodevvAddSpan(3, 4, 2, 4, 1) == 3);
    // float 2 of two channels: one entry, to floats 2 and 3 of channel 0
    CHECK(VorbisDecodevvAddSpan(3, 4, 2, 3, 0) == 2);
    CHECK(VorbisDecodevvAddSpan(3, 4, 2, 3, 1) == 1);
    CHECK(VorbisDecodevvAddSpan(4, 5, 2, 0, 0) == 0);
}

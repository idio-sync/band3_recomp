// Adapted from libvorbis's lib/codebook.c (Copyright (c) 2002-2020 Xiph.org
// Foundation, BSD-3-Clause: src/ThirdParty/libvorbis/LICENSE.txt) and libogg's
// src/bitwise.c (Copyright (c) 2002 Xiph.org Foundation, BSD-3-Clause:
// src/ThirdParty/libogg/LICENSE.txt).

#include "src/Audio/vorbis_codebook.h"

#include <bit>
#include <cstring>
#include <vector>

// The adds here are single float adds, which nothing fuses; contraction stays
// off all the same, as in vorbis_mdct.cpp, so no compiler is free to change
// what the guest rounds.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#else
#error "vorbis_codebook.cpp needs floating-point contraction off: add this compiler's pragma"
#endif

namespace band3::audio {

namespace {

static_assert(std::endian::native == std::endian::little);

// guest memory, big-endian, at 32-bit guest addresses
uint32_t Word(const VorbisGuest& guest, uint32_t address) {
    uint32_t raw;
    std::memcpy(&raw, guest.At(address), sizeof raw);
    return std::byteswap(raw);
}

uint8_t Byte(const VorbisGuest& guest, uint32_t address) { return *guest.At(address); }

float Float(const VorbisGuest& guest, uint32_t address) {
    return std::bit_cast<float>(Word(guest, address));
}

// a[i] at a host pointer to big-endian floats
float LoadFloat(const uint8_t* p) {
    uint32_t raw;
    std::memcpy(&raw, p, sizeof raw);
    return std::bit_cast<float>(std::byteswap(raw));
}

void StoreFloat(uint8_t* p, float value) {
    const uint32_t raw = std::byteswap(std::bit_cast<uint32_t>(value));
    std::memcpy(p, &raw, sizeof raw);
}

// oggpack_look (0x82C308F0): the next `bits` bits (0 to 32) without taking
// them, or -1 when they would run past the packet. Only within the last four
// bytes is the end checked; all of it in the guest's 32-bit signed compares.
// The bytes read are the ones it reads: ptr[0] always, the next only while
// more bits are wanted, ptr[4] only for a look over 32 bits from a bit other
// than 0. A look of 32 bits with the top one set comes back negative too,
// which the callers take as -1, as they do in the guest.
int32_t Look(const VorbisGuest& guest, const VorbisBitReader& b, int32_t bits) {
    const uint32_t m = VorbisLookMask(bits);
    const uint32_t total = b.endbit + uint32_t(bits);
    if (int32_t(b.endbyte + 4u) >= int32_t(b.storage)) {
        if (int32_t((b.endbyte << 3) + total) > int32_t(b.storage << 3)) return -1;
    }
    uint32_t ret = uint32_t(Byte(guest, b.ptr)) >> b.endbit;
    if (int32_t(total) > 8) {
        ret |= uint32_t(Byte(guest, b.ptr + 1)) << (8 - b.endbit);
        if (int32_t(total) > 16) {
            ret |= uint32_t(Byte(guest, b.ptr + 2)) << (16 - b.endbit);
            if (int32_t(total) > 24) {
                ret |= uint32_t(Byte(guest, b.ptr + 3)) << (24 - b.endbit);
                if (int32_t(total) > 32 && b.endbit != 0) {
                    ret |= uint32_t(Byte(guest, b.ptr + 4)) << (32 - b.endbit);
                }
            }
        }
    }
    return int32_t(ret & m);
}

// oggpack_adv (0x82C309B8): bits / 8 rounded toward zero (srawi, addze) to
// ptr and endbyte, bits & 7 to endbit
void Adv(VorbisBitReader& b, int32_t bits) {
    const int32_t total = int32_t(uint32_t(bits) + b.endbit);
    const uint32_t bytes = uint32_t(total / 8);
    b.endbit = uint32_t(total) & 7u;
    b.ptr += bytes;
    b.endbyte += bytes;
}

uint32_t BitReverse(uint32_t x) {
    x = ((x >> 16) & 0x0000FFFFu) | ((x << 16) & 0xFFFF0000u);
    x = ((x >> 8) & 0x00FF00FFu) | ((x << 8) & 0xFF00FF00u);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x << 4) & 0xF0F0F0F0u);
    x = ((x >> 2) & 0x33333333u) | ((x << 2) & 0xCCCCCCCCu);
    return ((x >> 1) & 0x55555555u) | ((x << 1) & 0xAAAAAAAAu);
}

// the guest address of entry's values: valuelist + entry * dim floats, in
// 32 bits (mullw, then the shift)
uint32_t Values(const VorbisCodebook& book, int32_t entry) {
    return book.valuelist + ((uint32_t(book.dim) * uint32_t(entry)) << 2);
}

}

bool VorbisCodebookDecodable(const VorbisCodebook& book) {
    return book.codelist != 0 && book.dec_codelengths != 0 && book.dec_firsttable != 0 &&
           book.dec_firsttablen >= 0 && book.dec_firsttablen <= 32 && book.dec_maxlength >= 0 &&
           book.dec_maxlength <= 32 && book.used_entries > 0 &&
           book.used_entries <= book.entries;
}

bool VorbisBitReaderUsable(const VorbisBitReader& b) { return b.endbit < 8 && b.ptr != 0; }

bool VorbisCodebookHasValues(const VorbisCodebook& book) {
    return book.valuelist != 0 && book.dim > 0;
}

// decode_packed_entry_number (0x82C37078), libvorbis 1.2.0's: the first table
// on the next dec_firsttablen bits, either a direct hit (entry + 1) or a hint
// of where to bisect; then the next dec_maxlength bits, one fewer at a time
// near the packet's end, and a bisection of the sorted codewords, unsigned
// compares. A codeword longer than the bits left still takes those bits.
int32_t VorbisDecodePackedEntryNumber(const VorbisGuest& guest, const VorbisCodebook& book,
                                      VorbisBitReader& b) {
    int32_t read = book.dec_maxlength;
    // lo and hi in 32 bits, as the guest compares and indexes with them
    uint32_t lo;
    uint32_t hi;
    int32_t lok = Look(guest, b, book.dec_firsttablen);
    if (lok >= 0) {
        const uint32_t entry = Word(guest, book.dec_firsttable + (uint32_t(lok) << 2));
        if (entry & 0x80000000u) {
            lo = (entry >> 15) & 0x7FFFu;
            hi = uint32_t(book.used_entries) - (entry & 0x7FFFu);
        } else {
            Adv(b, int8_t(Byte(guest, book.dec_codelengths + entry - 1)));
            return int32_t(entry - 1);
        }
    } else {
        lo = 0;
        hi = uint32_t(book.used_entries);
    }

    lok = Look(guest, b, read);
    while (lok < 0 && read > 1) lok = Look(guest, b, --read);
    if (lok < 0) return -1;

    const uint32_t testword = BitReverse(uint32_t(lok));
    while (int32_t(hi - lo) > 1) {
        const uint32_t p = uint32_t(int32_t(hi - lo) >> 1);
        const uint32_t test = Word(guest, book.codelist + ((lo + p) << 2)) > testword ? 1u : 0u;
        lo += p & (test - 1u);
        hi -= p & (0u - test);
    }
    const int32_t length = int8_t(Byte(guest, book.dec_codelengths + lo));
    if (length <= read) {
        Adv(b, length);
        return int32_t(lo);
    }
    Adv(b, read);
    return -1;
}

int64_t VorbisBookDecode(const VorbisGuest& guest, const VorbisCodebook& book, VorbisBitReader& b) {
    const int32_t packed = VorbisDecodePackedEntryNumber(guest, book, b);
    if (packed < 0) return packed;
    return int64_t(Word(guest, book.dec_index + (uint32_t(packed) << 2)));
}

// vorbis_book_decodev_add (0x82C373A8): the guest has two loops, one for
// dim > 8 and a switch on dim for the rest, which add the same values to the
// same floats in the same order (value + a in both)
int32_t VorbisBookDecodevAdd(const VorbisGuest& guest, const VorbisCodebook& book, uint8_t* a,
                             VorbisBitReader& b, int32_t n) {
    for (int32_t i = 0; i < n;) {
        const int32_t entry = VorbisDecodePackedEntryNumber(guest, book, b);
        if (entry == -1) return -1;
        const uint32_t t = Values(book, entry);
        for (int32_t j = 0; j < book.dim; j++, i++) {
            uint8_t* x = a + 4 * size_t(i);
            StoreFloat(x, Float(guest, t + 4 * uint32_t(j)) + LoadFloat(x));
        }
    }
    return 0;
}

// vorbis_book_decodevs_add (0x82C37280): the entries all read first, into the
// guest's stack; then dim passes, value i of entry j to a[i * step + j]
// (value + a)
int32_t VorbisBookDecodevsAdd(const VorbisGuest& guest, const VorbisCodebook& book, uint8_t* a,
                              VorbisBitReader& b, int32_t n) {
    const int32_t step = n / book.dim;
    // the guest's alloca: a few dozen entries in practice
    uint32_t fixed[256];
    std::vector<uint32_t> more;
    uint32_t* t = fixed;
    if (step > 256) {
        more.resize(size_t(step));
        t = more.data();
    }
    for (int32_t j = 0; j < step; j++) {
        const int32_t entry = VorbisDecodePackedEntryNumber(guest, book, b);
        if (entry == -1) return -1;
        t[j] = Values(book, entry);
    }
    for (int32_t i = 0; i < book.dim; i++) {
        uint8_t* row = a + 4 * (size_t(i) * size_t(step > 0 ? step : 0));
        for (int32_t j = 0; j < step; j++) {
            uint8_t* x = row + 4 * size_t(j);
            StoreFloat(x, Float(guest, t[j] + 4 * uint32_t(i)) + LoadFloat(x));
        }
    }
    return 0;
}

// vorbis_book_decodevv_add (0x82C37678): each entry's values to channel
// chptr's float i, chptr going round the channels and i stepping when it comes
// back to 0, an entry always finished even past the end (a + value, the
// other way round from the two above)
int32_t VorbisBookDecodevvAdd(const VorbisGuest& guest, const VorbisCodebook& book,
                              uint8_t* const* channels, int32_t offset, int32_t ch,
                              VorbisBitReader& b, int32_t n) {
    const int32_t first = offset / ch;
    const int32_t end = int32_t(uint32_t(offset) + uint32_t(n)) / ch;
    int32_t chptr = 0;
    for (int32_t i = first; i < end;) {
        const int32_t entry = VorbisDecodePackedEntryNumber(guest, book, b);
        if (entry == -1) return -1;
        const uint32_t t = Values(book, entry);
        for (int32_t j = 0; j < book.dim; j++) {
            uint8_t* x = channels[chptr] + 4 * size_t(int64_t(i) - first);
            StoreFloat(x, LoadFloat(x) + Float(guest, t + 4 * uint32_t(j)));
            if (++chptr == ch) {
                chptr = 0;
                i++;
            }
        }
    }
    return 0;
}

int64_t VorbisDecodevAddSpan(int32_t dim, int32_t n) {
    if (n <= 0) return 0;
    return (int64_t(n) + dim - 1) / dim * dim;
}

int64_t VorbisDecodevsAddSpan(int32_t dim, int32_t n) {
    const int32_t step = n / dim;
    return step > 0 ? int64_t(step) * dim : 0;
}

int64_t VorbisDecodevvAddSpan(int32_t dim, int32_t offset, int32_t ch, int32_t n, int32_t c) {
    const int64_t first = offset / ch;
    const int64_t end = int32_t(uint32_t(offset) + uint32_t(n)) / ch;
    if (end <= first) return 0;
    // the values dealt, counted from channel 0's float `first`: whole entries
    // while one starts before channel 0's float `end`
    const int64_t wanted = (end - first) * ch;
    const int64_t last = (wanted + dim - 1) / dim * dim - 1;
    return last >= c ? (last - c) / ch + 1 : 0;
}

}

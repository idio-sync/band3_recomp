#pragma once
#include <cstdint>

// native_vorbis (src/Hooks/native_vorbis.cpp): the codebook decoder of the
// game's Vorbis decoder, run natively. Every residue vector and floor value of
// a song is a Huffman codeword the game's libvorbis 1.2.0 reads one at a time
// with decode_packed_entry_number (0x82C37078) and libogg 1.1.3's
// oggpack_look / oggpack_adv, and adds to the residue through
// vorbis_book_decodev_add, _decodevs_add and _decodevv_add: about a quarter of
// the decode thread's time recompiled, in many small calls.
//
// This is those functions as the game's build compiled them: the same bytes
// read, the same 32-bit wraparound, the same signed and unsigned compares, the
// same bits consumed on every path, the failing ones near a packet's end
// included, and each float add the single-precision add the guest does, in the
// same order. Decoded entries and floats are integers and single adds, so it is
// exact by construction; the unit test and native_vorbis_check say so.
//
// Everything is read where the guest keeps it, big-endian (VorbisGuest): the
// codebook's tables, its values and the packet. The bit reader is
// a host copy the caller loads from the guest's oggpack_buffer and stores back
// (endbyte, endbit, ptr: what oggpack_adv writes), and the floats added to are
// big-endian at host pointers the caller gives, so the check can run all of it
// on copies without touching the guest's.
//
// Adapted from libvorbis's lib/codebook.c (Copyright (c) 2002-2020 Xiph.org
// Foundation, BSD-3-Clause: src/ThirdParty/libvorbis/LICENSE.txt) and libogg's
// src/bitwise.c (Copyright (c) 2002 Xiph.org Foundation, BSD-3-Clause:
// src/ThirdParty/libogg/LICENSE.txt).

namespace band3::audio {

// the guest's memory as the recompiled code addresses it: base + address, and
// from 0xE0000000 (physical memory) plus the SDK's REX_PHYS_HOST_OFFSET
struct VorbisGuest {
    const uint8_t* base;
    uint32_t high_offset;  // the SDK's REX_PHYS_HOST_OFFSET: 0x1000 on Windows, 0 on Linux

    const uint8_t* At(uint32_t address) const {
        return base + address + (address >= 0xE0000000u ? high_offset : 0u);
    }
};

// libvorbis 1.2.0's codebook as vorbis_book_init_decode fills it: the game's
// layout, its pointers as guest addresses
struct VorbisCodebook {
    int32_t dim;                // @0: values per entry
    int32_t entries;            // @4
    int32_t used_entries;       // @8: entries with a codeword
    uint32_t valuelist;         // @16: float[used_entries * dim]
    uint32_t codelist;          // @20: uint32[used_entries], bit-reversed codewords, sorted
    uint32_t dec_index;         // @24: int32[used_entries], the entry each one is
    uint32_t dec_codelengths;   // @28: signed char[used_entries]
    uint32_t dec_firsttable;    // @32: uint32[1 << dec_firsttablen]
    int32_t dec_firsttablen;    // @36
    int32_t dec_maxlength;      // @40
};

// libogg 1.1.3's oggpack_buffer (guest offsets 0, 4, 8, 12, 16)
struct VorbisBitReader {
    uint32_t endbyte;
    uint32_t endbit;
    uint32_t buffer;
    uint32_t ptr;  // the next byte: buffer + endbyte
    uint32_t storage;
};

// libogg's oggpack_look mask table (the game's at 0x82CADC98): mask[bits] is
// the low `bits` bits set, 0 to 32
inline constexpr uint32_t VorbisLookMask(int bits) {
    return bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
}

// Whether the native decoder takes the book and the reader: the tables
// decode_packed_entry_number reads present, first-table and codeword lengths
// the mask table covers (0 to 32 bits), 0 < used_entries <= entries, and a
// reader at a bit 0 to 7 of a byte (oggpack_adv never leaves it elsewhere).
bool VorbisCodebookDecodable(const VorbisCodebook& book);
bool VorbisBitReaderUsable(const VorbisBitReader& b);
// and for the vector decoders: values present, dim > 0
bool VorbisCodebookHasValues(const VorbisCodebook& book);

// decode_packed_entry_number(book, b): the sorted index of the next codeword,
// or -1 (the reader advanced as the game's leaves it, or not at all)
int32_t VorbisDecodePackedEntryNumber(const VorbisGuest& guest, const VorbisCodebook& book,
                                      VorbisBitReader& b);

// vorbis_book_decode(book, b): the entry number, or -1; as the guest's r3
// holds it (dec_index's word zero-extended, -1 sign-extended)
int64_t VorbisBookDecode(const VorbisGuest& guest, const VorbisCodebook& book, VorbisBitReader& b);

// vorbis_book_decodev_add(book, a, b, n): adds whole entries to a[0], a[1], ...
// while fewer than n are done, so up to VorbisDecodevAddSpan floats when dim
// doesn't divide n, as the guest does. 0, or -1 when a codeword isn't read.
int32_t VorbisBookDecodevAdd(const VorbisGuest& guest, const VorbisCodebook& book, uint8_t* a,
                             VorbisBitReader& b, int32_t n);

// vorbis_book_decodevs_add(book, a, b, n): n / dim entries read first (-1 at
// the first that isn't, nothing added), then value i of entry j added to
// a[i * step + j]
int32_t VorbisBookDecodevsAdd(const VorbisGuest& guest, const VorbisCodebook& book, uint8_t* a,
                              VorbisBitReader& b, int32_t n);

// vorbis_book_decodevv_add(book, a, offset, ch, b, n): entries' values dealt
// across ch channels from a[0][offset / ch] while i < (offset + n) / ch.
// channels[c] points at channel c's float offset / ch.
int32_t VorbisBookDecodevvAdd(const VorbisGuest& guest, const VorbisCodebook& book,
                              uint8_t* const* channels, int32_t offset, int32_t ch,
                              VorbisBitReader& b, int32_t n);

// How many floats each can add to, from a (decodevv: from channel c's float
// offset / ch); dim > 0 and ch > 0
int64_t VorbisDecodevAddSpan(int32_t dim, int32_t n);
int64_t VorbisDecodevsAddSpan(int32_t dim, int32_t n);
int64_t VorbisDecodevvAddSpan(int32_t dim, int32_t offset, int32_t ch, int32_t n, int32_t c);

}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay packet codec: the SFML-style wire encoding Slippi uses.
 *
 * Ported for native C use in melee-pc from project-slippi/Ishiiruka's use of
 * sf::Packet (SlippiNetplay.cpp writes with `packet << value` and reads with
 * `packet >> value`) and from Hero88go/melee-unlocked
 * port/runtime/hle/slippi_net.cpp:152-171, itself a port of that behaviour.
 * See docs/slippi-migration-plan.md.
 *
 * Encoding rules (must match Slippi exactly):
 *   - u8/u16/u32/i32: big-endian, 1/2/4 bytes.
 *   - bool: one byte, 0 or 1.
 *   - string: u32 byte length, then that many bytes (no terminator).
 *
 * sf::Packet tracks an "ok" flag: once a read runs past the end of the buffer
 * every later read returns zero and the flag stays false, because later fields
 * keep extracting from an empty packet in SFML. This codec reproduces that:
 * a reader that goes out of bounds latches `ok = false` and every subsequent
 * read yields zero without advancing, so a caller can read the whole payload
 * then check `ok` once. That mirrors the reference's `Packet::check`.
 *
 * The writer is bounded by a caller-supplied buffer and latches `ok = false`
 * on overflow rather than writing out of bounds; a caller that trusts the
 * buffer size can ignore the flag, but a caller building a packet from
 * network-controlled sizes must check it. */
#ifndef PC_SLIPPI_SLP_PACKET_H
#define PC_SLIPPI_SLP_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest packet the Slippi protocol can carry: a pad message with the
 * maximum accepted frame count (PC_SLIPPI_MAX_INPUT_FRAMES) plus its header.
 * Selections, chat and the build message are all far smaller. */
#define PC_SLIPPI_PACKET_MAX (14 + PC_SLIPPI_MAX_INPUT_FRAMES * 8)

typedef struct PcSlippiWriter {
    uint8_t* data; /* caller-owned buffer */
    size_t cap;    /* its capacity in bytes */
    size_t len;    /* bytes written so far */
    bool ok;       /* false once an append overflowed the buffer */
} PcSlippiWriter;

typedef struct PcSlippiReader {
    const uint8_t* data;
    size_t len;
    size_t pos; /* bytes consumed */
    bool ok;    /* false once a read ran past the end */
} PcSlippiReader;

/* ---- writer ----------------------------------------------------------- */
void pc_slippi_writer_init(PcSlippiWriter* w, void* data, size_t cap);
void pc_slippi_put_u8(PcSlippiWriter* w, uint8_t v);
void pc_slippi_put_bool(PcSlippiWriter* w, bool v);
void pc_slippi_put_u16(PcSlippiWriter* w, uint16_t v);
void pc_slippi_put_u32(PcSlippiWriter* w, uint32_t v);
void pc_slippi_put_i32(PcSlippiWriter* w, int32_t v);
void pc_slippi_put_bytes(PcSlippiWriter* w, const void* data, size_t n);
void pc_slippi_put_string(PcSlippiWriter* w, const char* s);

/* ---- reader ----------------------------------------------------------- */
void pc_slippi_reader_init(PcSlippiReader* r, const void* data, size_t len);
uint8_t pc_slippi_get_u8(PcSlippiReader* r);
bool pc_slippi_get_bool(PcSlippiReader* r);
uint16_t pc_slippi_get_u16(PcSlippiReader* r);
uint32_t pc_slippi_get_u32(PcSlippiReader* r);
int32_t pc_slippi_get_i32(PcSlippiReader* r);
/* Copies min(n, remaining) bytes into `out`; returns the number copied. If
 * fewer than n remain, `ok` latches false. */
size_t pc_slippi_get_bytes(PcSlippiReader* r, void* out, size_t n);
/* Reads a u32 length then that many bytes into a caller buffer, always
 * NUL-terminating. Copies at most cap-1 bytes and discards the rest of a
 * longer string so `pos` stays correct; a string longer than an internal
 * sanity limit latches `ok` false (Slippi never sends one). `out` may be
 * NULL to skip the contents but still advance. */
void pc_slippi_get_string(PcSlippiReader* r, char* out, size_t cap);

/* Bytes left to read. */
size_t pc_slippi_reader_remaining(const PcSlippiReader* r);

#ifdef __cplusplus
}
#endif

#endif

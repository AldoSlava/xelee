/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the Slippi netplay packet codec (src/pc/slippi/slp_packet.c).
 *
 * The codec is the foundation of wire compatibility: if it encodes a field
 * with the wrong width or byte order, every message is malformed to a Dolphin
 * peer and nothing downstream can be debugged. So this test pins the exact
 * bytes, not just round-trips:
 *   - every integer width and both bool values produce the bytes Slippi's
 *     sf::Packet writes (big-endian, bool as one byte);
 *   - strings are u32 length + bytes, no terminator, and a string exactly at
 *     the cap is truncated on read but the stream position still lands at the
 *     end (so the next field reads correctly);
 *   - a read past the end latches `ok` false and every later read yields zero
 *     without advancing -- the sf::Packet behaviour the reference copies;
 *   - a writer overflow latches `ok` false and writes nothing further;
 *   - a full NP_MSG_SLIPPI_PAD payload built by hand decodes to the expected
 *     frame, port, checksum and pad bytes, and a malformed "too many frames"
 *     packet is caught by the length arithmetic the receiver does.
 *
 * From the repo root:
 *   cc -DTARGET_PC=1 -DMELEE_PC=1 -DNDEBUG -std=gnu11 \
 *      -I src -I src/sdk_include \
 *      tools/test_slp_packet.c -o /tmp/test_slp_packet && /tmp/test_slp_packet
 * (The .c is #included above, so it must not also be linked on the command
 * line; no aurora/SDL dependency: the codec is plain C over a byte buffer.) */
#include "../src/pc/slippi/slp_packet.c"
#include "../src/pc/slippi/slp_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define assert(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void test_integer_encoding(void) {
    uint8_t buf[32];
    PcSlpWriter w;
    pc_slp_writer_init(&w, buf, sizeof buf);
    pc_slp_put_u8(&w, 0xAB);
    pc_slp_put_u16(&w, 0x1234);
    pc_slp_put_u32(&w, 0xDEADBEEF);
    pc_slp_put_i32(&w, -1);
    pc_slp_put_bool(&w, true);
    pc_slp_put_bool(&w, false);
    assert(w.ok);
    assert(w.len == 1 + 2 + 4 + 4 + 1 + 1);
    assert(buf[0] == 0xAB);
    assert(buf[1] == 0x12 && buf[2] == 0x34);
    assert(buf[3] == 0xDE && buf[4] == 0xAD && buf[5] == 0xBE && buf[6] == 0xEF);
    assert(buf[7] == 0xFF && buf[8] == 0xFF && buf[9] == 0xFF && buf[10] == 0xFF); /* -1 */
    assert(buf[11] == 0x01); /* bool true */
    assert(buf[12] == 0x00); /* bool false */

    PcSlpReader r;
    pc_slp_reader_init(&r, buf, w.len);
    assert(pc_slp_get_u8(&r) == 0xAB);
    assert(pc_slp_get_u16(&r) == 0x1234);
    assert(pc_slp_get_u32(&r) == 0xDEADBEEF);
    assert(pc_slp_get_i32(&r) == -1);
    assert(pc_slp_get_bool(&r) == true);
    assert(pc_slp_get_bool(&r) == false);
    assert(r.ok);
    assert(r.pos == r.len);
}

static void test_strings(void) {
    uint8_t buf[64];
    PcSlpWriter w;
    pc_slp_writer_init(&w, buf, sizeof buf);
    pc_slp_put_string(&w, "ABC#123");
    assert(w.ok);
    assert(w.len == 4 + 7);
    assert(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 7);
    assert(memcmp(buf + 4, "ABC#123", 7) == 0);

    char out[32];
    PcSlpReader r;
    pc_slp_reader_init(&r, buf, w.len);
    pc_slp_get_string(&r, out, sizeof out);
    assert(r.ok);
    assert(strcmp(out, "ABC#123") == 0);

    /* Empty string is length 0 and no bytes. */
    pc_slp_writer_init(&w, buf, sizeof buf);
    pc_slp_put_string(&w, "");
    assert(w.ok && w.len == 4);
    pc_slp_reader_init(&r, buf, w.len);
    pc_slp_get_string(&r, out, sizeof out);
    assert(r.ok && out[0] == '\0');

    /* A string longer than the destination truncates but the position still
     * lands after the whole string, so a following field reads correctly. */
    pc_slp_writer_init(&w, buf, sizeof buf);
    pc_slp_put_string(&w, "0123456789");
    pc_slp_put_u16(&w, 0xBEEF);
    pc_slp_reader_init(&r, buf, w.len);
    pc_slp_get_string(&r, out, 5); /* room for 4 chars + NUL */
    assert(r.ok);
    assert(strcmp(out, "0123") == 0);
    assert(pc_slp_get_u16(&r) == 0xBEEF);
    assert(r.ok && r.pos == r.len);

    /* out == NULL skips contents but advances. */
    pc_slp_reader_init(&r, buf, w.len);
    pc_slp_get_string(&r, NULL, 0);
    assert(r.ok);
    assert(pc_slp_get_u16(&r) == 0xBEEF);
}

static void test_reader_latches_ok(void) {
    uint8_t buf[4] = {1, 2, 3, 4};
    PcSlpReader r;
    pc_slp_reader_init(&r, buf, sizeof buf);
    assert(pc_slp_get_u16(&r) == 0x0102);
    assert(r.ok);
    /* Four bytes wanted, two remain: fails and latches. */
    assert(pc_slp_get_u32(&r) == 0);
    assert(!r.ok);
    /* Later reads yield zero and do not advance. */
    size_t before = r.pos;
    assert(pc_slp_get_u8(&r) == 0);
    assert(pc_slp_get_u16(&r) == 0);
    assert(r.pos == before);
}

static void test_writer_overflow(void) {
    uint8_t buf[4];
    PcSlpWriter w;
    pc_slp_writer_init(&w, buf, sizeof buf);
    pc_slp_put_u32(&w, 0x11223344);
    assert(w.ok && w.len == 4);
    pc_slp_put_u8(&w, 0x55); /* no room */
    assert(!w.ok);
    assert(w.len == 4); /* nothing written past the cap */
    assert(memcmp(buf, "\x11\x22\x33\x44", 3) == 0);
}

/* The full pad payload, built exactly as Slippi's SendSlippiPad does:
 * mid u8, frame i32, player_port u8, checksum_frame i32, checksum u32,
 * then count * 8 pad bytes newest-first. */
static void test_pad_payload(void) {
    uint8_t buf[PC_SLIPPI_PACKET_MAX];
    const int32_t frame = 1234;
    const uint8_t port = 1;
    const int32_t ck_frame = 1200;
    const uint32_t ck = 0xCAFEBABE;
    const uint8_t pads[3][PC_SLIPPI_PAD_DATA_SIZE] = {
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
        {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17},
        {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27},
    };
    const int count = 3;

    PcSlpWriter w;
    pc_slp_writer_init(&w, buf, sizeof buf);
    pc_slp_put_u8(&w, PC_SLIPPI_MSG_PAD);
    pc_slp_put_i32(&w, frame);
    pc_slp_put_u8(&w, port);
    pc_slp_put_i32(&w, ck_frame);
    pc_slp_put_u32(&w, ck);
    assert((int)w.len == PC_SLIPPI_PAD_DATA_OFFSET);
    for (int i = 0; i < count; i++) {
        pc_slp_put_bytes(&w, pads[i], PC_SLIPPI_PAD_DATA_SIZE);
    }
    assert(w.ok);
    assert(w.len == PC_SLIPPI_PAD_DATA_OFFSET + (size_t)count * PC_SLIPPI_PAD_DATA_SIZE);

    /* Decode as the receiver does. */
    PcSlpReader r;
    pc_slp_reader_init(&r, buf, w.len);
    assert(pc_slp_get_u8(&r) == PC_SLIPPI_MSG_PAD);
    int32_t got_frame = pc_slp_get_i32(&r);
    uint8_t got_port = pc_slp_get_u8(&r);
    int32_t got_ck_frame = pc_slp_get_i32(&r);
    uint32_t got_ck = pc_slp_get_u32(&r);
    assert(got_frame == frame && got_port == port && got_ck_frame == ck_frame && got_ck == ck);
    assert(r.pos == PC_SLIPPI_PAD_DATA_OFFSET);

    /* The receiver's length arithmetic: inputsToCopy is (claimed frame) minus
     * the head frame it already has; here head is 1232, so 2 new frames are
     * expected and the payload must hold 14 + 2*8 = 30 bytes. A packet that
     * claims more frames than it carries must be rejected. */
    int32_t head = 1232;
    long inputs_to_copy = (long)got_frame - head;
    assert(inputs_to_copy == 2);
    long need = PC_SLIPPI_PAD_DATA_OFFSET + inputs_to_copy * PC_SLIPPI_PAD_DATA_SIZE;
    assert(need == 30);
    assert((long)w.len > need); /* this packet actually carried 3 */
    /* If the packet had claimed frame 1400, need exceeds the payload: reject. */
    long bogus_need = PC_SLIPPI_PAD_DATA_OFFSET + ((long)1400 - head) * PC_SLIPPI_PAD_DATA_SIZE;
    assert(bogus_need > (long)w.len);
}

static void test_wire_header_order(void) {
    /* Pin the byte order of the four header fields directly, because a mixed
     * endianness is the defect most likely to survive a round-trip test. */
    uint8_t buf[14];
    PcSlpWriter w;
    pc_slp_writer_init(&w, buf, sizeof buf);
    pc_slp_put_u8(&w, 0x80);
    pc_slp_put_i32(&w, 0x01020304);
    pc_slp_put_u8(&w, 0x02);
    pc_slp_put_i32(&w, 0x05060708);
    pc_slp_put_u32(&w, 0x090A0B0C);
    assert(w.len == 14);
    const uint8_t expect[14] = {0x80, 0x01, 0x02, 0x03, 0x04, 0x02, 0x05, 0x06,
                                0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
    assert(memcmp(buf, expect, 14) == 0);
}

int main(void) {
    test_integer_encoding();
    test_strings();
    test_reader_latches_ok();
    test_writer_overflow();
    test_pad_payload();
    test_wire_header_order();
    printf("test_slp_packet: all checks passed\n");
    return 0;
}

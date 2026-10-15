/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for Slippi's desync checksum (src/pc/slippi/slp_checksum.c).
 *
 * This is the value that goes on the Slippi wire and is compared against a
 * Dolphin peer with a hard-abort on mismatch, so the test pins the arithmetic
 * by hand rather than only round-tripping:
 *   - the field xor order and the (low16 ^ high16) combine;
 *   - the single-precision float sum and its truncating (toward zero) cast;
 *   - absent entities contribute nothing;
 *   - stocks are xor'd per slot at byte width;
 *   - the hard-desync tolerance is +/-1 on the signed 16-bit low half.
 *
 * From the repo root:
 *   cc -DTARGET_PC=1 -DMELEE_PC=1 -DNDEBUG -std=gnu11 -I src -I src/sdk_include \
 *      tools/test_slp_checksum.c -o /tmp/test_slp_checksum && /tmp/test_slp_checksum
 * (The .c is #included above, so it is not linked separately.) */
#include "../src/pc/slippi/slp_checksum.c"

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

/* Fill one entity with chosen raw bits and matching float values. */
static void set_char(PcSlippiCharState* ch, uint32_t action, float x, float y, float pct,
                     uint32_t spawn) {
    ch->present = true;
    ch->action_state = action;
    memcpy(&ch->pos_x_bits, &x, 4);
    memcpy(&ch->pos_y_bits, &y, 4);
    memcpy(&ch->percent_bits, &pct, 4);
    ch->spawn_num = spawn;
    ch->pos_x = x;
    ch->pos_y = y;
    ch->percent = pct;
}

static void test_all_absent_is_zero(void) {
    PcSlippiSlotState s[PC_SLIPPI_SLOTS];
    memset(s, 0, sizeof s);
    assert(pc_slippi_checksum(s) == 0);
}

static void test_single_field_xor(void) {
    /* One slot, one entity, action state 0x10 only. checksum = 0x10.
     * float_sum = 0. combined = (0x10 ^ 0) = 0x10; hard = 0.
     * result = 0x0010_0000. */
    PcSlippiSlotState s[PC_SLIPPI_SLOTS];
    memset(s, 0, sizeof s);
    s[0].chars[0].present = true;
    s[0].chars[0].action_state = 0x10;
    uint32_t ck = pc_slippi_checksum(s);
    assert(pc_slippi_checksum_soft(ck) == 0x0010);
    assert(pc_slippi_checksum_hard(ck) == 0);
}

static void test_float_sum_truncation(void) {
    /* x=1.5, y=2.25, pct=0.5 -> float_sum = 4.25 -> int 4 (truncates). */
    PcSlippiSlotState s[PC_SLIPPI_SLOTS];
    memset(s, 0, sizeof s);
    set_char(&s[0].chars[0], 0, 1.5f, 2.25f, 0.5f, 1);
    uint32_t ck = pc_slippi_checksum(s);
    assert(pc_slippi_checksum_hard(ck) == 4);

    /* Negative components truncate toward zero: -4.25 -> -4 -> low16 0xFFFC. */
    PcSlippiSlotState t[PC_SLIPPI_SLOTS];
    memset(t, 0, sizeof t);
    set_char(&t[0].chars[0], 0, -1.5f, -2.25f, -0.5f, 1);
    uint32_t ck2 = pc_slippi_checksum(t);
    assert(pc_slippi_checksum_hard(ck2) == (uint16_t)(int32_t)(-4));
}

static void test_character_data_xor(void) {
    /* Two entities in one slot: xor of all five fields each, plus stocks. */
    PcSlippiSlotState s[PC_SLIPPI_SLOTS];
    memset(s, 0, sizeof s);
    s[0].stocks = 4;
    set_char(&s[0].chars[0], 0x11, 1.0f, 2.0f, 3.0f, 1);
    set_char(&s[0].chars[1], 0x22, 4.0f, 5.0f, 6.0f, 2);
    uint32_t expect_ck = 0;
    expect_ck ^= 0x11 ^ 0x22;
    expect_ck ^= 0x3F800000u ^ 0x40800000u; /* 1.0f ^ 4.0f */
    expect_ck ^= 0x40000000u ^ 0x40A00000u; /* 2.0f ^ 5.0f */
    expect_ck ^= 0x40400000u ^ 0x40C00000u; /* 3.0f ^ 6.0f */
    expect_ck ^= 1u ^ 2u;                   /* spawn nums */
    expect_ck ^= 4u;                        /* stocks */
    uint16_t expect_soft = (uint16_t)((expect_ck & 0xFFFFu) ^ (expect_ck >> 16));
    /* float_sum = 1+2+3 + 4+5+6 = 21 */
    uint32_t ck = pc_slippi_checksum(s);
    assert(pc_slippi_checksum_soft(ck) == expect_soft);
    assert(pc_slippi_checksum_hard(ck) == 21);
}

static void test_secondary_entity_matters(void) {
    /* Sheik vs Zelda / Nana present changes the checksum. Distinct
     * action_state and spawn_num so the xor cannot cancel. */
    PcSlippiSlotState a[PC_SLIPPI_SLOTS], b[PC_SLIPPI_SLOTS];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    set_char(&a[0].chars[0], 1, 0, 0, 0, 1);
    set_char(&b[0].chars[0], 1, 0, 0, 0, 1);
    set_char(&b[0].chars[1], 0x40, 0, 0, 0, 0x80); /* an extra entity */
    assert(pc_slippi_checksum(a) != pc_slippi_checksum(b));
}

static void test_hard_desync_tolerance(void) {
    /* Same soft half, hard halves within 1: not a desync. */
    uint32_t a = (0x1234u << 16) | 100u;
    assert(!pc_slippi_checksum_is_hard_desync(a, (0x1234u << 16) | 101u));
    assert(!pc_slippi_checksum_is_hard_desync(a, (0x1234u << 16) | 99u));
    assert(!pc_slippi_checksum_is_hard_desync(a, a));
    /* Difference of 2: a desync. */
    assert(pc_slippi_checksum_is_hard_desync(a, (0x1234u << 16) | 102u));
    assert(pc_slippi_checksum_is_hard_desync(a, (0x1234u << 16) | 98u));
    /* Signed wraparound: 0 vs 0xFFFF is -1, not a desync. */
    uint32_t z = (0x1234u << 16);
    uint32_t m1 = (0x1234u << 16) | 0xFFFFu;
    assert(!pc_slippi_checksum_is_hard_desync(z, m1));
    /* 0 vs 0xFFFE is -2: a desync. */
    assert(pc_slippi_checksum_is_hard_desync(z, (0x1234u << 16) | 0xFFFEu));
}

static void test_stocks_per_slot(void) {
    /* Stocks xor in at byte width once per slot, even with no entities. */
    PcSlippiSlotState s[PC_SLIPPI_SLOTS];
    memset(s, 0, sizeof s);
    s[0].stocks = 4;
    s[1].stocks = 3;
    s[2].stocks = 2;
    s[3].stocks = 1;
    uint32_t ck = pc_slippi_checksum(s);
    uint32_t expect = 4u ^ 3u ^ 2u ^ 1u;
    uint16_t expect_soft = (uint16_t)((expect & 0xFFFFu) ^ (expect >> 16));
    assert(pc_slippi_checksum_soft(ck) == expect_soft);
}

int main(void) {
    test_all_absent_is_zero();
    test_single_field_xor();
    test_float_sum_truncation();
    test_character_data_xor();
    test_secondary_entity_matters();
    test_hard_desync_tolerance();
    test_stocks_per_slot();
    printf("test_slp_checksum: all checks passed\n");
    return 0;
}

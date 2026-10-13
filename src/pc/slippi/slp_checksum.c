/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi's online desync checksum (slp_checksum.h). See that header for the
 * exact algorithm and the ASM it was ported from. */
#include "pc/slippi/slp_checksum.h"

#include <string.h>

uint32_t pc_slippi_checksum(const PcSlippiSlotState slots[PC_SLIPPI_SLOTS]) {
    uint32_t checksum = 0;
    /* The ASM accumulates in a single-precision register (fadds), so the sum
     * must be single-precision too: a double accumulator would round
     * differently and change the low 16 bits on a long frame. */
    float float_sum = 0.0f;

    for (int slot = 0; slot < PC_SLIPPI_SLOTS; slot++) {
        const PcSlippiSlotState* s = &slots[slot];
        for (int c = 0; c < PC_SLIPPI_CHARS_PER_SLOT; c++) {
            const PcSlippiCharState* ch = &s->chars[c];
            if (!ch->present) {
                continue; /* helper returns immediately when the gobj is null */
            }
            checksum ^= ch->action_state;
            checksum ^= ch->pos_x_bits;
            checksum ^= ch->pos_y_bits;
            checksum ^= ch->percent_bits;
            checksum ^= ch->spawn_num;
            float_sum += ch->pos_x;
            float_sum += ch->pos_y;
            float_sum += ch->percent;
        }
        /* Stocks, low byte, xor'd once per slot. */
        checksum ^= s->stocks;
    }

    /* Combine: (low16 ^ high16) in the high half, truncated float sum in the
     * low half. Mirrors the ASM's lhz/xor/rlwinm then fctiwz/stfd/lhz/or. */
    uint16_t combined = (uint16_t)((checksum & 0xFFFFu) ^ (checksum >> 16));
    /* fctiwz truncates toward zero, not floor. */
    int32_t fsum = (int32_t)float_sum;
    uint16_t hard = (uint16_t)((uint32_t)fsum & 0xFFFFu);
    return ((uint32_t)combined << 16) | hard;
}

uint16_t pc_slippi_checksum_hard(uint32_t checksum) {
    return (uint16_t)(checksum & 0xFFFFu);
}

uint16_t pc_slippi_checksum_soft(uint32_t checksum) {
    return (uint16_t)(checksum >> 16);
}

bool pc_slippi_checksum_is_hard_desync(uint32_t local, uint32_t remote) {
    /* ASM: extsh both hard halves, sub, then |diff| > 1 is a desync. */
    int32_t a = (int16_t)pc_slippi_checksum_hard(local);
    int32_t b = (int16_t)pc_slippi_checksum_hard(remote);
    int32_t diff = a - b;
    return diff < -1 || diff > 1;
}

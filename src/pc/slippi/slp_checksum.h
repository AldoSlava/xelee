/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi's online desync checksum, reimplemented natively.
 *
 * Ported from project-slippi/slippi-ssbm-asm
 * Online/Core/StartEngineLoop.asm "FN_COMPUTE_CHECKSUM" / "..._HELPER". This
 * is a DIFFERENT checksum from melee-pc's own frame_checksum
 * (src/pc/net_snapshot.c): ours is a stronger internal-state hash used for our
 * own LAN/desync detection, but it can never equal Dolphin's, so putting it on
 * the Slippi wire would kill every cross-play match with a false
 * "DESYNC DETECTED" (Dolphin hard-aborts on a mismatch, StartEngineLoop.asm
 * HARD_DESYNC_DETECTED). The Slippi wire therefore carries THIS value.
 *
 * The formula, exactly as the ASM computes it, over four player slots:
 *
 *   checksum = 0; float_sum = 0
 *   for each slot (stride 0xE90 from 0x80453080, four slots):
 *     for each of the slot's two entities (0xB0 primary, 0xB4 secondary):
 *       if entity == 0: skip
 *       checksum ^= action_state    (CharacterData+0x10)
 *       checksum ^= pos_x_bits      (CharacterData+0xB0, raw float bits)
 *       checksum ^= pos_y_bits      (CharacterData+0xB4, raw float bits)
 *       checksum ^= percent_bits    (CharacterData+0x1830, raw float bits)
 *       checksum ^= spawn_num       (CharacterData+0x8)
 *       float_sum += pos_x + pos_y + percent
 *     checksum ^= stocks            (player static +0x8E)
 *   combined = (checksum & 0xFFFF) ^ (checksum >> 16)
 *   result   = (combined << 16) | (int(float_sum) & 0xFFFF)
 *
 * The low 16 bits (the float sum) are the "hard" desync check and are compared
 * with a tolerance of +/- 1; the high 16 bits are the "soft" warning. The float
 * sum is truncated toward zero (fctiwz). Positions/percent contribute their raw
 * IEEE-754 bits, which are the same 32-bit value on our little-endian port as
 * on Dolphin's big-endian PowerPC (the float value is endian-independent), so
 * an identical simulation yields an identical checksum.
 *
 * This module is pure: it takes the values already gathered from the game, so
 * it is unit-testable without a running match. The game-side glue that fills
 * PcSlippiSlotState lives with the adapter (M3). */
#ifndef PC_SLIPPI_SLP_CHECKSUM_H
#define PC_SLIPPI_SLP_CHECKSUM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slippi wraps four player slots, each with a primary entity and an optional
 * secondary (Sheik/Zelda, Nana/Ice Climber). */
#define PC_SLIPPI_SLOTS 4
#define PC_SLIPPI_CHARS_PER_SLOT 2

typedef struct PcSlippiCharState {
    bool present;
    uint32_t action_state; /* motion_id */
    uint32_t pos_x_bits;   /* raw bits of cur_pos.x */
    uint32_t pos_y_bits;   /* raw bits of cur_pos.y */
    uint32_t percent_bits; /* raw bits of dmg.x1830_percent */
    uint32_t spawn_num;    /* x8_spawnNum */
    float pos_x;
    float pos_y;
    float percent;
} PcSlippiCharState;

typedef struct PcSlippiSlotState {
    uint8_t stocks; /* Player_GetStocks(slot) */
    PcSlippiCharState chars[PC_SLIPPI_CHARS_PER_SLOT];
} PcSlippiSlotState;

/* Compute Slippi's checksum over four slots. `slots` must have four entries;
 * an all-absent, zeroed set produces 0. */
uint32_t pc_slippi_checksum(const PcSlippiSlotState slots[PC_SLIPPI_SLOTS]);

/* The two halves, as the wire consumers use them.
 *   hard: low 16 bits (float sum), compared with a +/-1 tolerance.
 *   soft: high 16 bits (combined XOR), a warning only. */
uint16_t pc_slippi_checksum_hard(uint32_t checksum);
uint16_t pc_slippi_checksum_soft(uint32_t checksum);

/* Slippi's hard-desync test: |hard_local - hard_remote| as signed 16-bit
 * values is more than 1. Reproduces the ASM's extsh/sub/cmpwi sequence. */
bool pc_slippi_checksum_is_hard_desync(uint32_t local, uint32_t remote);

#ifdef __cplusplus
}
#endif

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay protocol constants, ported for native C use in melee-pc.
 *
 * Derived from project-slippi/Ishiiruka
 *   Source/Core/Core/NetPlayProto.h        (MessageID: the NP_MSG_* values)
 *   Source/Core/Core/Slippi/SlippiNetplay.h (ROLLBACK_MAX_FRAMES,
 *                                             SLIPPI_ONLINE_LOCKSTEP_INTERVAL,
 *                                             player-count limits)
 *   Source/Core/Core/Slippi/SlippiPad.h     (pad sizes)
 * and from Hero88go/melee-unlocked port/runtime/hle/slippi_net.{h,cpp}, which
 * is a wire-compatible native port of the same code. See
 * docs/slippi-migration-plan.md.
 *
 * This header is the single source of truth for the wire constants. Every
 * value here must match upstream Slippi exactly, or a Dolphin peer will read
 * our packets as malformed (or not recognise them at all). Do not "clean up"
 * a value without checking it against the sources named above. */
#ifndef PC_SLIPPI_SLP_PROTO_H
#define PC_SLIPPI_SLP_PROTO_H

#include <stdint.h>

/* ---- version gate ------------------------------------------------------
 * The Slippi netplay semantic version this build speaks. Pinned to a specific
 * release (decision 3, docs/slippi-migration-plan.md): keeping it current for
 * compatibility is our responsibility, not a runtime "track latest". Slippi's
 * matchmaking server advertises its version; a client whose version is older
 * than the server's newer one stays out of matchmaking rather than run two
 * different rule sets against an updated player. Mirrors
 * melee-unlocked port/runtime/hle/slippi_version.h.
 *
 * 3.6.4 is the current Slippi Dolphin release (Ishiiruka v3.6.4, 2026-06-15),
 * the same version melee-unlocked pins and has played port-vs-Dolphin against.
 * NOTE: this is Slippi's *netplay* version, NOT the .slp replay format version
 * (which is 3.18.0, src/pc/slp_format.h). The two are unrelated. */
#define PC_SLIPPI_SEMVER "3.6.4"

/* ---- rollback / timing ------------------------------------------------ */
#define PC_SLIPPI_ROLLBACK_MAX_FRAMES 7
#define PC_SLIPPI_LOCKSTEP_INTERVAL 30 /* frames between time-sync decisions */
#define PC_SLIPPI_PING_DISPLAY_INTERVAL 60

/* ---- player counts ---------------------------------------------------- */
#define PC_SLIPPI_REMOTE_PLAYER_MAX 3
#define PC_SLIPPI_PLAYER_COUNT_MAX (PC_SLIPPI_REMOTE_PLAYER_MAX + 1)

/* ---- pad sizes --------------------------------------------------------
 * A pad on the wire is the first 8 bytes of the 12-byte GCPadStatus. */
#define PC_SLIPPI_PAD_FULL_SIZE 0xC
#define PC_SLIPPI_PAD_DATA_SIZE 0x8

/* Offset of the first pad byte in an NP_MSG_SLIPPI_PAD payload, after
 * mid u8 + frame i32 + player_port u8 + checksum_frame i32 + checksum u32. */
#define PC_SLIPPI_PAD_DATA_OFFSET 14

/* ---- ENet channels ----------------------------------------------------
 * Three channels. Pad and pad-ack traffic is UNSEQUENCED so that loss and
 * reordering are handled by the Slippi protocol (every input is resent until
 * acked) and a slow reliable message cannot block them. Everything else
 * (selections, chat, synced state) is RELIABLE and ordered.
 *
 * Upstream allocates 3 channels (enet_host_create(..., 3, ...)). Reliable
 * traffic goes on channel 0. Note the subtlety copied verbatim from upstream
 * (SlippiNetplay.cpp: pads sent through the generic Send() path use channel 1,
 * but the inline pad-ack reply in OnData() is sent directly on channel 2).
 * Both are UNSEQUENCED; a receiver accepts either, so the split is harmless
 * and is reproduced here for byte-for-byte behaviour. */
#define PC_SLIPPI_ENET_CHANNELS 3
#define PC_SLIPPI_CH_RELIABLE 0
#define PC_SLIPPI_CH_PAD 1
#define PC_SLIPPI_CH_PAD_ACK 2

/* ---- message ids (NetPlayProto.h MessageID) --------------------------- */
enum PcSlippiMsg {
    PC_SLIPPI_MSG_PAD = 0x80,
    PC_SLIPPI_MSG_PAD_ACK = 0x81,
    PC_SLIPPI_MSG_MATCH_SELECTIONS = 0x82,
    PC_SLIPPI_MSG_CONN_SELECTED = 0x83,
    PC_SLIPPI_MSG_CHAT = 0x84,
    /* Ranked set-flow only; unused while Ranked is out of scope
     * (docs/slippi-migration-plan.md §7). Kept named so the ids stay
     * self-documenting and a stray packet is identified in logs. */
    PC_SLIPPI_MSG_COMPLETE_STEP = 0x85,
    PC_SLIPPI_MSG_SYNCED_STATE = 0x86,
    /* Not Slippi: melee-pc/melee-unlocked build identity, safe to send because
     * stock Slippi Dolphin logs an unknown id and ignores it. */
    PC_SLIPPI_MSG_BUILD = 0xE0,
};

/* ---- receive limits (upstream SlippiNetplay.cpp) ----------------------
 * A pad packet's payload is rejected when it claims more frames than this. */
#define PC_SLIPPI_MAX_INPUT_FRAMES 128

/* ---- chat ids ---------------------------------------------------------
 * Slippi accepts only these message ids; anything else is rejected. The
 * values are GC button-code derived (see melee-unlocked slippi_net.cpp
 * ReadChatMessage). CHAT_DISABLED is a sentinel, not a spoken message. */
#define PC_SLIPPI_CHAT_DISABLED 0x10

/* ---- matchmaking endpoints -------------------------------------------
 * From Ishiiruka Source/Core/Core/Slippi/SlippiMatchmaking.h. Production is
 * the default; dev exists for our own testing only and is not user-selectable
 * in a shipping build. */
#define PC_SLIPPI_MM_HOST_PROD "mm.slippi.gg"
#define PC_SLIPPI_MM_HOST_DEV "mm2.slippi.gg"
#define PC_SLIPPI_MM_PORT 43113

/* Ranked is out of scope: these mode ordinals are documented but never sent.
 * Slippi's OnlinePlayMode (SlippiMatchmaking.h). */
enum PcSlippiOnlinePlayMode {
    PC_SLIPPI_MODE_RANKED = 0,   /* excluded */
    PC_SLIPPI_MODE_UNRANKED = 1,
    PC_SLIPPI_MODE_DIRECT = 2,
    PC_SLIPPI_MODE_TEAMS = 3,
};

#endif

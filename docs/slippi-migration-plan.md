# Slippi netplay migration plan

Replaces the custom transport described in `docs/netcode-plan.md` with a native
implementation of Slippi's wire protocol, so this port can play against Slippi
Dolphin and use Slippi Launcher, recon replays and the surrounding ecosystem.
Ranked is explicitly out of scope (see §7).

Status: **M1 done, M2 next.** `docs/netcode-plan.md` remains the description of
what exists today.

Branch: **`slippi-net`**, cut from `quest-xr` at `2a1ec6b` (decision 4). All
migration work happens here; `main` stays untouched until the branch is ready.

Decisions resolved 2026-10-12: kill the *internet* fallback (DHT/ranked) in M6
but keep the legacy engine for LAN (1, revised), account-gated Unranked with
account-free Direct (2), pin `SLIPPI_SEMVER` to a release and maintain it
ourselves (3), work on a branch off `quest-xr` (4). See §8.

## 0. The decision in one paragraph

"Our current netplay" and "Slippi" are two complete, incompatible stacks. This
is a re-implementation, not a dependency swap: Slippi's netcode lives inside a
Dolphin emulator as `CEXISlippi` and a set of PowerPC Gecko/ASM codes, so it
cannot be linked into a native decompilation. The good news is that a sibling
native decompilation — **Melee Unlocked** — has already done exactly this and
published the blueprint. It implements Slippi's wire formats from scratch,
speaks the same ENet P2P protocol as Slippi Dolphin, logs in with the Slippi
Launcher's `user.json`, queues on the real `mm.slippi.gg` servers, and has a
recorded port-vs-Dolphin online match (`PORT_COMPLETION.md:135-146`). We copy
its *protocol*, not its *engine*: our snapshot engine, `.slp` recorder and
determinism work are already stronger than what it uses and should be kept.

The cost is real: we trade the serverless DHT/rating/resume design for a
central dependency on `mm.slippi.gg`, an ENet dependency, and a Slippi account
(`user.json`). The payoff is cross-play with Dolphin and the entire Slippi
tooling ecosystem. Ranked is excluded, which is exactly the boundary Melee
Unlocked drew (`slippi_net.cpp:436-438`).

## 1. What "use Slippi" can mean

| Reading | What it is | Viable? |
|---|---|---|
| **(A) Wire interop** — implement Slippi's protocols natively so Dolphin players can connect to us with their normal codes | Port `SlippiNetplay`/`SlippiMatchmaking`/`CEXISlippi` wire formats; keep our own sim/snapshot/loop | **Yes.** This is the only path, and Melee Unlocked is the proof. |
| (B) Link Slippi's Dolphin code | Reuse `SlippiNetplay.cpp`, `EXI_DeviceSlippi.cpp` directly | No. They are Dolphin classes (ENet host, `NetPlayUI`, config system, PowerPC MMU, Dolphin savestates). Not separable, and an emulator's savestate is not a decomp's memory image. |
| (C) Use Slippi Launcher as the client | Launch through the Launcher and let Dolphin do the rest | No. That is Dolphin; our game is not Dolphin. The Launcher itself is Electron + TypeScript and only manages Dolphin installs, replays and login. |

This plan is **(A)**. Throughout, "Slippi" means "Slippi Dolphin / Ishiiruka's
netplay protocol", not the Launcher's UI.

### What the two references actually contribute

- **`project-slippi/slippi-launcher`** (GPL-3.0): the ecosystem contract we must
  satisfy, not code to reuse. It owns the `user.json` account file (uid,
  playKey, connectCode, displayName) that Slippi clients read for login, the
  replay folder layout, and the login/refresh REST endpoints. We read its
  identity files exactly as Melee Unlocked does (`slippi_net.cpp:220-249`).
- **`Hero88go/melee-unlocked`** (GPL-2.0-or-later for the ported Dolphin/Slippi
  files, per `NOTICE`): the reference implementation. A native decomp with:
  - `port/runtime/hle/slippi_net.{h,cpp}` — ~1.6k lines, a from-scratch,
    wire-compatible port of `SlippiNetplayClient` on vendored ENet
    (`slippi_net.h:1-2`).
  - `port/runtime/hle/exi_slippi.cpp` + `slippi_online.cpp` — the Slippi
    EXI command surface (matchmaking, inputs, savestates, chat, reports).
  - `port/runtime/hle/native_savestate.{h,cpp}` — its own rollback snapshot
    engine (page-chunked, incremental).
  - `docs/mu-net-protocol.md` — a **separate, non-Slippi** P2P protocol used by
    its own lobby. Not relevant to interop; ignore it for this migration.

**License note.** Melee Unlocked's own code is GPL-2.0-or-later/GPL-3.0; the
files we care about are explicitly GPL-2.0-or-later (Dolphin/Slippi lineage)
and are compatible with our GPL-3.0-or-later port code (`licenses/LICENSE.md` §2).
We may port them with attribution. See §9.

## 2. What Slippi netplay actually is

Three layers, all of which must match for a Dolphin player to connect.

### 2.1 Game side — the Slippi codes (PowerPC Gecko/ASM)

On Dolphin, a set of `slippi-ssbm-asm` codes run inside the game and talk to an
**EXI device in slot B** via DMA. The device implements a command byte space
(`exi_slippi.cpp:49-59`, sizes at `:65-73`):

- `CMD_ONLINE_INPUTS 0xB0`, `CMD_CAPTURE_SAVESTATE 0xB1`,
  `CMD_LOAD_SAVESTATE 0xB2`, `CMD_GET_MATCH_STATE 0xB3`,
  `CMD_FIND_OPPONENT 0xB4`, `CMD_SET_MATCH_SELECTIONS 0xB5`,
  `CMD_GET_NEW_SEED 0xBC`, `CMD_REPORT_GAME 0xBD`, plus the recording/replay
  commands (`0x35`–`0x41`), file commands (`0xD1`–`0xD4`), delay (`0xD5`),
  music (`0xD6`–`0xD8`), rank (`0xE3`–`0xE5`).

The ASM decides **when** to capture/load a savestate, when to stall for
inputs, and how to sync the clock. The EXI device does the I/O. In a native
decomp there is no EXI device and no Gecko interpreter, so both halves must be
re-implemented as native C: the device logic (in `src/pc/slippi/`) and the
game-side behaviour (native hooks, replacing the ASM). This is the single
largest and riskiest part of the migration, and it is *behavioural* parity, not
just wire parity.

### 2.2 Transport — ENet P2P

Slippi uses **ENet** (MIT; vendored by Melee Unlocked at
`port/third_party/enet/`). Client setup: `enet_host_create(local, 10, 3, 0, 0)`
— 3 channels (`slippi_net.cpp:314`). Channel selection on send
(`slippi_net.cpp:499-513`): today's Slippi forks send pad/ack packets
`UNSEQUENCED` on channel **1** (ack on **2** in Melee Unlocked's variant),
everything else `RELIABLE` on channel **0**.

Message IDs (`slippi_net.h:39-46`):

```
NP_MSG_SLIPPI_PAD               0x80
NP_MSG_SLIPPI_PAD_ACK           0x81
NP_MSG_SLIPPI_MATCH_SELECTIONS  0x82
NP_MSG_SLIPPI_CONN_SELECTED     0x83
NP_MSG_SLIPPI_CHAT_MESSAGE      0x84
NP_MSG_SLIPPI_COMPLETE_STEP     0x85   (Ranked set-flow; unused when Ranked is off)
NP_MSG_SLIPPI_SYNCED_STATE      0x86   (Ranked tiebreak; unused)
```

Payload encoding is SFML-style **big-endian** (`Packet` class,
`slippi_net.cpp:152-171`): integers big-endian, `bool` one byte, strings as
`u32 length + bytes`. The pad payload is `frame i32, player_port u8,
checksum_frame i32, checksum u32`, then `8 × N` pad bytes newest-first
(`slippi_net.cpp:335-378`); a pad is the first 8 bytes of the 12-byte
`GCPadStatus` (`PAD_FULL_SIZE 0xC`, `PAD_DATA_SIZE 0x8`). Acks carry
`frame i32, player_idx u8`. Note our `WirePad` is already exactly this 8-byte
shape (`src/pc/net_internal.h:130-135`) — the pad encoding is already Slippi's.

### 2.3 Matchmaking + identity

- Identity: the Slippi Launcher's `user.json` (uid, playKey, connectCode,
  displayName). Read for login; refreshed from Slippi's REST API on a background
  thread (`slippi_net.cpp:181-249`). This replaces our ed25519 identity,
  `NAME#XXXX` code derivation and DHT topics.
- Matchmaking: ENet **plus JSON** messages to `mm.slippi.gg:43113` (prod) /
  `mm2.slippi.gg` (dev), per Ishiiruka `SlippiMatchmaking.h`. Flow: login →
  `FindMatch(mode, connectCode)` → server returns match id + player list +
  stage list + peer IP/port → simultaneous connect (hole punch) → peer-to-peer
  ENet session. Ranked additionally uses `COMPLETE_STEP`/`SYNCED_STATE` and
  calls back into Slippi's GraphQL/report API — all out of scope here.
- Version gate: the client speaks one `SLIPPI_SEMVER` (Melee Unlocked: `3.6.4`,
  `slippi_net.cpp:32`) and refuses matchmaking when the servers advertise a
  newer one (`slippi_version.h`).

## 3. Gap analysis: our tree vs the target

### 3.1 Keep — already at or above the reference

| Asset | Where | Why keep |
|---|---|---|
| Region snapshot engine | `src/pc/net_snapshot.c`, `melee_state.ld`, per-platform range tables | Whole-region `memcpy`, ~5.6 MB / 0.6 ms (measured), all platforms post-link verified (`netcode-plan.md:133-175`). Melee Unlocked's incremental page engine is a memory optimisation we do not need at our measured cost. |
| `.slp` recorder | `src/pc/slp.c`, `slp_format.c` | Already Slippi-format 3.18.0, already hooks the same game functions as Slippi's recording ASM (`slp.c` header), replay-verified. Only the online metadata fields are missing (§3.3). |
| Determinism groundwork | `-ffp-contract=off`, vendored musl trig, forced seed, prewarm, record/replay, sync test | Required for *any* rollback; Slippi's own determinism is exactly what makes cross-play possible. Already stronger than the reference's. |
| Per-frame loop seam | `gmscene.c:445-461` (`pc_net_sync` / `gm_RunSimTick` / `pc_net_after_tick`) | The place where a new transport plugs in without touching the game loop. |
| Pad injection | `HSD_PadRenewMasterStatus`, `net.c write_head` | Slippi's `GCPadStatus` and ours already match byte-for-byte. |

### 3.2 Retire (become dead code under the Slippi route)

- **DHT / serverless discovery**: `net_dht.c`, `net_dht_item.c`, the DHT half of
  `net_match.c`. Slippi matchmaking is central. (Keep the files until the new
  path is proven; delete in the final phase.)
- **On-device rating / signed records**: `net_rank*.c` (Ranked-only).
- **Custom reliable channel & wire v6–v9**: `net_reliable.c`, most of
  `net_wire.c`, the custom RULES/READY handshake (`net_handshake.c`). ENet
  provides the reliable channel; Slippi's own selections/seed flow replaces the
  handshake.
- **Custom `.slp` timing hooks**? No — keep; only the network-driven parts
  (`pc_net_sim_frame`/`pc_net_confirmed_frame`) get rewired.
- **Menu modes**: `mnonline.c` `RANKED` entry and `ONLINE_KIND_RANKED`. LAN can
  survive either as Slippi Direct over LAN or as our existing engine; see §6.

### 3.3 Add

1. **ENet dependency** (MIT). Vendored, like Melee Unlocked. Our CMake has no
   ENet today (`CMakeLists.txt:103` lists only platform socket libs).
2. **Slippi protocol module** — `src/pc/slippi/`:
   - `slp_packet.c/.h` — SFML-style big-endian packet codec (mirrors
     `slippi_net.cpp:152-171`).
   - `slp_proto.h` — the `NP_MSG_*` ids and payload sizes; the single source of
     truth for protocol constants, including `SLIPPI_SEMVER`.
   - `slp_net.c` — ENet client: connect, channels, send/receive, ack timers,
     remote pad queues, checksum exchange, time-offset sampling (ports
     `slippi_net.cpp:281-840`).
   - `slp_match.c` — matchmaking client: ENet + JSON to `mm.slippi.gg:43113`,
     login, queue, hole-punch, result (ports `SlippiMatchmaking`).
   - `slp_user.c` — `user.json` reader + REST refresh (ports `User`,
     `slippi_net.cpp:181-249`).
3. **Game-side Slippi behaviour** — native replacements for the ASM, added to
   the existing hook points:
   - online input delay scheduling and stalling (maps onto `net.c`'s window,
     but must match Slippi's cadence: `SYNC_INTERVAL 30`, `WINDOW 7`,
     offset formula `slippi_net.cpp:349-357`);
   - CSS/SSS selection broadcast/merge (`PlayerSelections`,
     `slippi_net.cpp:479-497`) so both clients pick together;
   - initial RNG seed handshake (`CMD_GET_NEW_SEED` / game start);
   - per-frame finalized-state checksum (we have `frame_checksum`; must emit it
     in Slippi's field layout and compare as Slippi does);
   - savestate capture/load cadence driven by the game side, calling our
     snapshot engine instead of `CMD_CAPTURE_SAVESTATE` DMAs;
   - chat (`0x84`), match-state polling (`CMD_GET_MATCH_STATE`).
4. **Online replay metadata** — fill the currently-zero `0x1A5` display names,
   `0x221` connect codes, `0x249` Slippi UIDs, `0x2BE` session id, `0x2F1` game
   number, `0x2F5` tiebreaker (`slp_format.c:151-155`) from the session, so our
   `.slp` files are indistinguishable from Dolphin's online replays.
5. **Launcher-facing identity** — read/write the Slippi Launcher user directory
   layout; surface connect code in the menu.

## 4. Target architecture and the seam

Everything a new transport must satisfy is already named in `src/pc/net.h`.
Keep that contract and swap the implementation behind it. The game code does
not change.

```
gmscene.c  ──pc_net_sync()──▶  ┌─────────────────────────────┐
           ──pc_net_after_tick()─▶│  transport adapter          │
           ──pc_net_stats()──▶  │  (src/pc/slippi/adapter.c)  │
           ──pc_net_resim()──▶  └───────┬─────────────┬───────┘
                                        │             │
                          ┌─────────────▼──┐   ┌──────▼──────────────┐
                          │ slp_net.c      │   │ net_snapshot.c      │  keep
                          │ (ENet, Slippi  │   │ (capture/restore)   │
                          │  msg ids)      │   └─────────────────────┘
                          └───────┬────────┘
                                  │
                    ┌─────────────▼───┐   ┌──────────────────┐
                    │ slp_match.c     │   │ pc_slp.c         │  keep
                    │ mm.slippi.gg    │   │ (.slp recorder)  │
                    └─────────────────┘   └──────────────────┘
```

The adapter owns rollback *policy* (when to capture, when to stall, when to
load), because Slippi's policy is defined by its ASM and is what the wire
behaviour must match. `net_snapshot.c` stays a dumb, correct store:
`snapshot_take(Snapshot*, frame)` / `snapshot_restore()` are transport-agnostic
already. The input injection point stays `HSD_PadRenewMasterStatus` /
`write_head`, since our pad bytes already equal Slippi's.

Why the adapter rather than editing `gmscene.c`: the loop's contract is
`(sync → tick → maybe re-run)`, which is protocol-neutral. Swapping behind
`pc_net_*` keeps the risk surface in `src/pc/slippi/` and lets the old engine
and the new one coexist behind a build/runtime switch during the migration.

**A/B switch.** Add `MELEE_NET_STACK=slippi|legacy` (default `legacy` until
parity, then `slippi`), so regression tests and the current LAN/Direct flows
keep working throughout.

## 5. Phased plan

Each phase has a gate; do not start the next until the gate passes.

### M1 — ENet + protocol codec (no game integration) — **DONE** (`9459517`)
Vendored ENet v1.3.13 (MIT) at `extern/enet`, built as `melee_enet`
(`SEPARATE_FROM_ALL`; not linked until M2). Added `src/pc/slippi/slp_proto.h`
(wire constants — the single source of truth, pinned `PC_SLIPPI_SEMVER`) and
`slp_packet.{c,h}` (SFML-style big-endian codec incl. the `ok`-latch
semantics). `tools/test_slp_packet.c` pins byte order, widths, string
truncation and pad-payload arithmetic; registered as ctest `slp_packet`.
**Gate met:** the codec encodes/decodes the same bytes as Slippi's `sf::Packet`
and latches/rejects exactly as the reference does. Verified on this host with
`cc -std=gnu11` (no cmake in PATH here; CMake wiring is in place for CI).

### M2 — Slippi P2P client over ENet
`slp_net.c`: connect two local instances via `Direct` (no matchmaking), exchange
`PAD`/`PAD_ACK`, selections, chat. Drive it from a headless harness, not the
game yet.
**Gate:** two instances exchange pads and selections for a scripted match; log
shows ping/offset sampling matching the reference formula.

### M3 — Game-side wiring + rollback parity
Adapter + native Slippi behaviour: input delay, stall/capture/load cadence,
seed handshake, checksum exchange, CSS sync. Reuse `net_snapshot.c`.
**Gate:** local instance-vs-instance match runs with rollback driven by the
Slippi message flow; two headless Dolphin builds cannot be scripted here, so
Dolphin parity is proven later (§M5). Internally, our own record/replay and
`MELEE_NET_RESIM_AUDIT` must show a pure tick and faithful re-simulation.

### M4 — Identity + matchmaking
`slp_user.c` (read Launcher `user.json`, REST refresh) and `slp_match.c`
(Unranked + Direct on `mm.slippi.gg:43113`). Version gate against the server's
advertised Slippi version.
**Gate:** a real Unranked ticket is accepted and a match id + peer endpoint are
returned; Direct connect succeeds between two of our instances.

### M5 — Cross-play verification (the whole point)
1. Our instance vs our instance over the real matchmaking servers, full game.
2. **Our instance vs Slippi Dolphin**, Unranked and Direct, full game — the
   acceptance test this entire migration exists for.
3. Both peers' `.slp` files for (2) compare equal via `tools/net_state_compare`
   plus a Slippi replay diff; online metadata fields populated.
**Gate:** a recorded port-vs-Dolphin match completes with agreeing checksums and
equal replays, the same standard Melee Unlocked met (`PORT_COMPLETION.md:135`).

### M6 — Retirement (internet fallback only; LAN kept)
Delete the *internet* fallback: `net_dht*.c`, `net_rank*.c`, the custom
internet handshake and the DHT/ranked parts of `net_match.c`; retire the Ranked
menu entry. **Keep the legacy LAN stack** (`net_lan.c` mDNS discovery + election
+ direct-IP, and the `net.c`/`net_wire.c`/`net_handshake.c`/`net_reliable.c`
engine it runs on) as the LAN transport (decision 1, revised 2026-10-12).
Selection becomes mode-driven, not a build switch: LAN → legacy engine,
Unranked/Direct → Slippi. `MELEE_NET_STACK` is retained as the engine selector
rather than removed. Update `docs/netcode-plan.md` (mark the DHT/ranked sections
superseded) and `README.md`.

### M7 — LAN: kept on the legacy engine, not folded into Direct
**Decided (2026-10-12, revised):** LAN play stays on the proven legacy engine
(mDNS discovery + election + the custom transport), which needs no Slippi
account, no `mm.slippi.gg`, and runs at delay 0. It is **not** folded into
Slippi Direct. This is the one deliberate exception to "Slippi only": the mDNS
path is well-tested (`README.md` LAN rows, Android phone-to-PC matches) and its
independence from central infrastructure is a feature. Slippi Direct (endpoint)
remains available too, so LAN players can opt into Dolphin cross-play if they
want. Maintenance cost is accepted: two transports, selected by mode.

## 6. Rollback/snapshot reuse — is the seam real?

Yes. Evidence:

- `snapshot_take(Snapshot*, int32_t frame)` and `snapshot_restore(const
  Snapshot*)` (`net_snapshot.c:602`, `:706`) take no transport types. The
  snapshot knows its frame and scene and validates both.
- Rollback *policy* today lives in `net.c` (`predict`, `snap_predicted`,
  `rollback_to`, the barrier, `:2469-2710`). That policy is what changes; the
  storage does not.
- The frame checksum (`frame_checksum`, `net_snapshot.c:266`) and state ring are
  transport-neutral; only the field layout we put on the wire changes.
- The pad queue contract (`write_head`, `unconsume`, re-applied from the live
  copy after restore, `net.c:1214-1301`) is exactly where Slippi's
  `CMD_ONLINE_INPUTS` result must land. Our existing code already survives the
  "raw samples queued since the snapshot" case rollback needs.

The one genuine risk is that Slippi's stall/capture/load **cadence** (defined by
its ASM) differs from ours in ways that affect the wire and thus interoperability
— that is behavioural work in `slp_net`/adapter, not snapshot work.

## 7. Ranked is out of scope

Agreed and baked in:

- No `Ranked` menu entry, no rating, no set flow (best-of-three, bans/picks).
- Skip `NP_MSG_SLIPPI_COMPLETE_STEP` / `NP_MSG_SLIPPI_SYNCED_STATE` (the code
  points above are literally commented "Ranked … no Ranked in Melee Unlocked",
  `slippi_net.cpp:436-438`).
- No Slippi GraphQL `reportOnlineGame` / rank fetch, no `net_rank*.c`.
- We still read `user.json`, so we have a connect code and can play Unranked and
  Direct; the server will simply never place us in a ranked set.
- Rationale confirmed: this port is a novel representation of Melee; until the
  community accepts it, it should not touch the ranked ladder — the same call
  Melee Unlocked made and shipped.

## 8. Risks and open questions

| # | Risk | Mitigation |
|---|---|---|
| 1 | **Behavioural parity with Slippi's ASM** (stall/capture/load timing, input delay semantics) is the hardest part and is not in the wire format. | Port Melee Unlocked's game-side logic verbatim first; verify against a real Dolphin in M5. |
| 2 | Central dependency: `mm.slippi.gg` can change, rate-limit or die; unknown-message and version-gate behaviour must degrade gracefully. | Follow `slippi_version.h` gating; keep Direct (endpoint) working without matchmaking. Keep the legacy stack on the branch until M6, then remove it (decision 1). |
| 3 | Slippi protocol version drift (we pin `SLIPPI_SEMVER`; Dolphin may move). | **Decided: pin to a specific Slippi release** (reference pins 3.6.4; pick the current stable release at M1). Single constant in `slp_proto.h`; refresh from the server, refuse on newer. Keeping it current for compatibility is explicitly our responsibility (decision 3). |
| 4 | ENet vendoring on all platforms (Windows/macOS/Android/iOS) — new C dependency in a tree that currently has none. | ENet is portable C with existing Android/macOS support; add to `CMakeLists.txt` and CI matrix. |
| 5 | Account requirement: Slippi matchmaking needs a Slippi Launcher login. | Read `user.json`; allow Direct with an arbitrary code when no account exists. |
| 6 | Licensing/attribution for ported GPL-2.0-or-later code. | §9. |
| 7 | We give up distinct strengths (serverless DHT, signed rating) for internet interop, while keeping LAN. | Intentional per this decision. Legacy internet modules stay behind the mode/`MELEE_NET_STACK` selector until M6; LAN keeps using them permanently. |
| 8 | Determinism across our build and Dolphin's is not the same as determinism within our builds. | M5 is the only test that counts. Our record/replay proves our internal purity first. |

### Decisions (resolved by the user, 2026-10-12)

1. **M6 end state: kill the internet fallback, keep LAN.** Once Slippi parity
   is proven in M5, the serverless *internet* stack (DHT discovery, ranked,
   signed rating) is deleted. **LAN play is retained on the legacy engine**
   (decision revised 2026-10-12): mDNS discovery and the custom transport are
   proven, account-free and independent of Slippi's servers. So the end state is
   two transports — legacy for LAN, Slippi for Unranked/Direct — selected by
   mode. `MELEE_NET_STACK` stays as that selector; the DHT/ranked modules do not.
2. **Identity: account-gated Unranked is acceptable.** Unranked matchmaking
   requires a Slippi Launcher account (`user.json`); Direct must remain usable
   without one (manual endpoint / code).
3. **Version policy: pin to a release.** Pin `SLIPPI_SEMVER` to a specific
   current Slippi release and keep it updated ourselves — compatibility is our
   responsibility, not "track latest" at runtime.
4. **Sequencing: work on a branch.** All migration work lands on the
   `slippi-net` branch, which was branched off `quest-xr` (the Quest XR build,
   `2a1ec6b`). `main` is untouched during the migration.

## 9. Licensing and attribution

- Our port code is GPL-3.0-or-later (`licenses/LICENSE.md` §2). The reference's
  netplay files are GPL-2.0-or-later (Dolphin/Slippi lineage, per its `NOTICE`),
  compatible with GPL-3.
- Add to `licenses/`: ENet (MIT), and a `NOTICE`-style attribution for code
  ported from `Hero88go/melee-unlocked` (GPL-2.0-or-later) and, transitively,
  from `project-slippi/Ishiiruka` and Dolphin.
- Every ported file keeps its original SPDX header and gains a line naming the
  source path it was derived from, as the reference does.
- Do **not** copy Melee Unlocked's `docs/mu-net-protocol.md` P2P protocol — it is
  its own invention, not Slippi, and is irrelevant to interop.

## 10. Sources (external)

- `project-slippi/Ishiiruka` `Source/Core/Core/Slippi/{SlippiNetplay,SlippiMatchmaking,SlippiUser}.h`,
  `Source/Core/Core/HW/EXI_DeviceSlippi.*`; `project-slippi/slippi-ssbm-asm`.
- `project-slippi/slippi-launcher` — `user.json` account format, replay layout,
  REST login/refresh.
- `Hero88go/melee-unlocked` — `port/runtime/hle/{slippi_net,slippi_online,exi_slippi,native_savestate}.{h,cpp}`,
  `port/third_party/{enet,slippilib}`, `PORT_COMPLETION.md:128-146`,
  `docs/mu-net-protocol.md`, `NOTICE`.
- Local context: `docs/netcode-plan.md` (§3-§6 transport, §7 DHT, §9 ranked,
  §14 sources), `src/pc/net.h`, `src/pc/net_internal.h`, `src/pc/net_snapshot.c`,
  `src/pc/slp_format.c`, `licenses/LICENSE.md`.

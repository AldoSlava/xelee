#!/usr/bin/env python3
"""Catalogue Slippi's game modifications from its Gecko code set.

Slippi Dolphin carries its entire game-side behaviour as Gecko codes in
Sys/GameSettings/GALE01r2.ini (NTSC-U 1.02): every override of Melee's own
code, each one labelled with the source .asm it was built from. That file is the
authoritative list of what Slippi changes, so this script reads it and writes
docs/slippi-gecko-catalog.md: one row per patch, grouped by what it does, with
the status of our port beside it.

    tools/gecko_catalog.py [GALE01r2.ini] [-o docs/slippi-gecko-catalog.md]

The ini is found under the Slippi Launcher's netplay folder when not given.
The generated tables are mechanical; the *status* column comes from NOTES below,
which records what was checked in this tree (and where). A row with no note is
reported as "not reviewed": absence of a note is not a claim that it is done.

`--disasm` appends a PowerPC disassembly of each C2 (insert-asm) patch; it needs
the `capstone` module (pip install capstone).
"""
import argparse
import os
import re
import struct
import sys

DEFAULT_INI = os.path.expanduser(
    "~/.config/Slippi Launcher/netplay/Sys/GameSettings/GALE01r2.ini")

# Code types seen in the file (Dolphin's Gecko code handler).
TYPES = {
    "04": "write32",     # 04aaaaaa vvvvvvvv: store a word at 0x80aaaaaa
    "C2": "insert-asm",  # C2aaaaaa nnnnnnnn: branch from the address into n code lines
    "C0": "execute-asm",
    "06": "write-string",
    "0": "write8/16",
    "02": "write16",
    "00": "write8",
}

# (substring of the patch label, category, status, note). First match wins.
# Status: DONE (implemented and where), PARTIAL, MISSING (searched, not found),
# N/A (Dolphin-side plumbing or replaced by our own design), UI (menu work, see
# the M4 UI port section of docs/slippi-migration-plan.md).
NOTES = [
    # ---- rules the codes pin ------------------------------------------------
    ("Unlock All Characters and Stages", "rules", "DONE",
     "net_handshake.c unlock_force(): both peers pin the full unlock set"),
    ("Stock Mode", "rules", "DONE", "net_handshake.c slippi_rules_force()"),
    ("4 Stocks", "rules", "DONE", "slippi_rules_force()"),
    ("8 Minutes", "rules", "DONE", "slippi_rules_force()"),
    ("No Items", "rules", "DONE", "slippi_rules_force(): item_freq 0xFF"),
    ("Singles Stages", "rules", "DONE", "slippi_rules_force(): stage mask E70000B0"),
    # ---- gameplay changes ---------------------------------------------------
    ("NeutralSpawn", "gameplay", "DONE",
     "gmvs.c neutralSpawn(): online players take the table's fixed neutral spots on the six "
     "standard stages, else the stage's own spawn point n for the n-th open slot. Singles "
     "only (2v2 stays retail); the asm's two scene-id exclusions (0x21C, 0x10F) are not "
     "reproduced because only online VS reaches it. Verified: both peers log the same spots."),
    ("UCF Dashback", "gameplay", "DONE",
     "ftCo_Turn.c ftCo_UcfDashback(): compared line by line with Slippi's asm and the vanilla "
     "ftCo_Turn_IASA instructions at the hook (sub-fighter flag, frame 2.0, stick x facing >= "
     "dash_smash_stick_threshold, timer <= 1, >75-unit raw flick, has_turned/just_turned, Nana "
     "rewrite). Reads the pad buffer; the timer test is the asm's literal <= 1."),
    ("UCF Shield Drop Extended", "gameplay", "DONE",
     "ftCo_Pass.c ftCo_8009A080 with ftUcf_ShieldDropHeld() (the counter above 1 ORs into the "
     "stick-down test); verified against the vanilla instructions from the disc's main.dol"),
    ("UCF Shield Drop", "gameplay", "DONE",
     "ftCo_Escape.c ftCo_UcfBlocksSpotDodge(): compared line by line with Slippi's asm and the "
     "vanilla callers (both end in li r3,1 so the asm's LR+8 return means not handled): C-stick "
     "y, roll-window timer, lstick y > -0.8, floor index, platform flag 0x100, rim test."),
    ("UCF SDI", "gameplay", "DONE",
     "ftucf.c ftUcf_SdiAllowed(), ORed into ftCo_Damage_OnEveryHitlag's timer test; polarity "
     "checked against vanilla (hook 0x8008E54C is the y-timer compare, LT = SDI)"),
    ("UCF Shield SDI", "gameplay", "DONE",
     "ftucf.c ftUcf_ShieldSdiAllowed(), ORed into ftCo_80093240's timer test (X axis only)"),
    ("UCF Tumble", "gameplay", "DONE",
     "ftucf.c ftUcf_TumbleAllowed(): replaces the x214 window test in ftCo_DamageFall_IASA "
     "(timer 0 always, timer 1 only on a >75-unit flick from inside the threshold)"),
    ("UCF Pad Buffer", "gameplay", "DONE",
     "ftucf.c ftUcf_Update(): per-port raw stick history, the 1.0 cardinals snap and the "
     "flick counter, called from Fighter_procInput before the trigger timers. Offline too "
     "(ucf pref, default on). The SDI, shield-SDI and tumble rules below read this history."),
    ("UCF DBOOC SquatRv Fix", "gameplay", "DONE",
     "ftCo_SquatRv.c: stand-up threshold 0.59 on the first rim frame"),
    ("UCF Text", "ui", "N/A", "the on-screen UCF indicator; cosmetic"),
    ("FreezeGlitchFix", "gameplay", "DONE", "ftnanaspecials.c, gated on pc_net_deterministic()"),
    ("PreventWobbling", "gameplay", "DONE", "ftwobble.c (3-hit limit), gated on pc_net_deterministic()"),
    ("FreezeDeadUpFallPhysics", "gameplay", "DONE",
     "ftdrawcommon.c UpdateModelPos and ftCo_DeadUpFall_Anim, gated on pc_net_deterministic()"),
    ("WhispyBlowDirFix", "gameplay", "DONE",
     "ftlib.c ftLib_GetOpponentsDir(): fighters in motion states 0-11 (death/respawn) no longer "
     "vote on the direction; asm checked against the vanilla loop"),
    ("NanaDeterminism", "gameplay", "DONE",
     "the asm zeroes two uninitialised registers in Nana's cpu stick routine; the decomp's "
     "ftCo_800AC5A0 already starts stick_x and stick_y at 0 (and the build zero-inits locals)"),
    ("BrawlOffscreenDamage", "gameplay", "DONE",
     "fighter.c slippiOffscreen(): the offscreen-damage test uses the stage camera limits instead "
     "of the magnifier HUD; Home-Run Contest, x221F_b1 and motion 4/6 excluded as in the asm"),
    ("PSCameraIndependentMonitor", "gameplay", "DONE", "grpstadium.c, gated on pc_net_deterministic()"),
    ("DesyncProofBGTransformations", "gameplay", "DONE",
     "grlast.c grLast_8021AAB0(): the Final Destination background transform runs between a "
     "save and restore of the RNG seed"),
    ("Stadium/StadiumFileLoad", "gameplay", "DONE",
     "net.c pc_net_pure_load(): GrPs1-4.dat are fetched before the first predicting frame"),
    ("Stadium/GrPsxIsValid", "gameplay", "DONE", "same preload mechanism"),
    ("Stadium/CustomZeroBuffer", "gameplay", "DONE", "same preload mechanism"),
    ("Stadium/IngameCheckIfFrozen", "gameplay", "DONE",
     "Slippi reads a static byte (0 = transforms). Ours: pc_net_rules() frozen=0 for a Slippi session"),
    ("Stadium/ChangeJumbotronText", "ui", "N/A", "cosmetic: the Stadium scoreboard text"),
    ("FrozenStadiumToggle", "ui", "MISSING",
     "Direct-only: a Frozen Pokemon Stadium toggle on the stage select (alt_stage_mode). "
     "Needs the selections flow."),
    ("Disable FoD During Doubles", "gameplay", "N/A", "we only play 1v1"),
    ("PreventDeadStranding", "gameplay", "N/A", "teams only"),
    ("Teams/", "ui", "N/A", "teams CSS; we only play 1v1"),
    ("LGLExceededGameEnd", "engine", "N/A",
     "ends the game and says the opponent exceeded the lag limit (45 frames); a netplay timeout "
     "policy, ours is the stall/disconnect handling in net.c"),
    ("PauseCounter", "gameplay", "PARTIAL",
     "Slippi allows pausing in Direct only (bookkeeping in its online struct); ours disables "
     "pausing in every online mode (gmonlinemode.c), which matches Unranked"),
    ("InitPause", "gameplay", "PARTIAL", "see PauseCounter: Direct pause is not offered here"),
    ("PreventPauseStranding", "gameplay", "PARTIAL", "see PauseCounter"),
    ("Salty Runback", "ui", "N/A",
     "results/exit flow: A+B held at scene exit returns code 2 (runback); no simulation effect"),
    ("Remember Who LRA Started", "engine", "N/A",
     "records who quit with L+R+A+Start for no-contest reporting to Slippi"),
    ("ReportNoContest", "engine", "N/A", "no-contest reporting to Slippi's servers"),
    ("Lagless FoD", "gameplay", "N/A",
     "Recommended performance code: sets bit 2 of a byte in the stage data at FoD init. Not "
     "disassembled further; nothing in it reads or writes fighter or stage physics state we saw"),
    ("Normal Lag Reduction", "engine", "N/A",
     "two branches in the Dolphin SDK's pad read (0x80376xxx) made unconditional so the read "
     "stops waiting for the next SI poll; our SDK layer is native. Its goal (read as late as "
     "possible) is met: PADRead runs right after the pacing wait and vsync, and the 1000 Hz "
     "thread keeps the sample 0.45 ms old on average (p99 1.0 ms, MELEE_INPUT_HUD=1)."),
    ("UseInGameDelay", "engine", "N/A",
     "Slippi applies its input delay in every in-game scene; ours applies net.delay to fights"),
    ("GetCommonMinorID", "n/a", "N/A", "helper for UseInGameDelay"),
    ("Polling Drift Fix", "n/a", "N/A", "Dolphin controller polling; not applicable to a native build"),
    ("DynamicsFix", "engine", "N/A",
     "tweak for Slippi's fast-forward resimulation; ours resimulates through the snapshot engine"),
    ("PreventPadAlarm", "n/a", "N/A", "Dolphin/pad plumbing"),
    ("ForceEngineOnRollback", "engine", "N/A", "Slippi's rollback loop; replaced by our snapshot engine"),
    ("ForceInputRefetchOnAdvance", "engine", "N/A", "rollback input plumbing"),
    ("LoopEngineForRollback", "engine", "N/A", "rollback loop"),
    ("SkipNewInputFetchOnRollback", "engine", "N/A", "rollback loop"),
    ("StartEngineLoop", "engine", "N/A", "rollback loop"),
    ("TriggerSendInput", "engine", "N/A", "input send path; ours is slp_net/slp_session"),
    ("InitOnlinePlay", "engine", "N/A", "per-match init; ours is net.c connect + slippi_rules_force"),
    ("HandleRumble", "engine", "N/A", "rumble during rollback"),
    ("EXIFileLoad", "engine", "N/A", "Dolphin EXI file transfer"),
    ("Hacks/", "engine", "N/A", "Dolphin crash/assert workarounds"),
    ("Music/", "engine", "N/A", "Dolphin audio-thread workarounds; ours has its own audio path"),
    ("Sound/", "engine", "N/A", "Dolphin audio-thread workarounds"),
    ("PreventFileAlarms", "engine", "N/A", "Dolphin DVD workarounds"),
    ("Online/Static", "engine", "N/A", "helper functions shared by the other patches"),
    ("Online/Debugging", "engine", "N/A", "Dolphin debugging"),
    ("CompatibilityHooks", "engine", "N/A", "m-ex compatibility"),
    ("m-ex codeset", "engine", "N/A", "m-ex compatibility"),
    ("Slippi Online Scene", "ui", "UI", "the Online scene; ours is gmonlinemode.c"),
    # ---- menus --------------------------------------------------------------
    ("Menus/CSS/TextEntryScreen", "ui", "PARTIAL",
     "connect-code entry (8 chars, English forced for Direct): our Direct screen still takes the DHT code"),
    ("Menus/CSS/", "ui", "UI", "online CSS; see the M4 UI port section (single panel done)"),
    ("Menus/SSS/", "ui", "UI", "online stage select; Unranked uses the ticket list, Direct keeps picks"),
    ("Menus/InGame", "ui", "UI", "in-match name display; connect-code tags done"),
    ("Menus/ResultsScreen", "ui", "MISSING", "results panels for remote players"),
    ("Menus/VSScreen", "ui", "MISSING", "the VS splash: hides the stage, announcer, colours"),
    ("Menus/TitleMenu", "ui", "UI", "online entry on the title menu; ours is mnonline.c"),
    ("Menus/Hacks", "ui", "N/A", "Dolphin rumble workaround"),
    # ---- recording ----------------------------------------------------------
    ("Recording/", "recording", "DONE", "src/pc/slp.c writes .slp natively; Dolphin's EXI path is not used"),
    ("Common/AllocSceneBuffer", "recording", "N/A", "recording support"),
    ("Common/GetIsFollower", "recording", "N/A", "recording support"),
    ("Common/IncrementFrameIndex", "recording", "N/A", "recording support"),
    ("Common/Initialize", "recording", "N/A", "recording support"),
    ("Common/CSS KO Stars", "recording", "N/A", "recording support"),
    # ---- general-purpose / quality of life ----------------------------------
    ("CustomizeMessageLRAS", "engine", "N/A", "the LRAS no-contest message text"),
    ("Skip Memcard", "qol", "N/A", "no memory card dialog in a native build"),
    ("Disable Special Messages", "qol", "UNVERIFIED", "suppresses unlock/trophy popups"),
    ("Disable Trophy Messages", "qol", "UNVERIFIED", "suppresses unlock/trophy popups"),
    ("C-Stick in Single Player", "qol", "N/A", "single-player only"),
    ("Debug Menu", "qol", "N/A", "Slippi enables the debug menu; not wanted in online play"),
    ("Debug Menu Returns", "qol", "N/A", "debug menu"),
    ("Faster CSS Load", "qol", "UNVERIFIED", "UnclePunch's faster CSS load"),
    ("L+R+A Returns to CSS", "qol", "UNVERIFIED", "LRA during stage load"),
    ("Unrestricted Camera while Paused", "qol", "N/A", "pause camera; see the free-camera note in launcher.cpp"),
    ("C-Stick Panning while Paused", "qol", "N/A", "pause camera"),
    ("DPad Down at CSS", "qol", "UNVERIFIED", "random stage shortcut"),
    ("Random Stage Music", "qol", "UNVERIFIED", "music only"),
    ("Random CSS Music", "qol", "UNVERIFIED", "music only"),
    ("CSS Player Hands", "qol", "UNVERIFIED", "CSS defaults"),
    ("Hide Nametag When Invisible", "qol", "UNVERIFIED", "tag display"),
    ("Closing CSS Door", "qol", "UNVERIFIED", "CSS nametag handling"),
    ("Unplugging Closes CSS Door", "qol", "UNVERIFIED", "CSS door handling"),
    ("Winners Names are Gold", "qol", "UNVERIFIED", "CSS cosmetic"),
    ("CostumeBoundCheck", "qol", "UNVERIFIED", "costume index bounds check (crash fix)"),
    # ---- optional codes -----------------------------------------------------
    ("FreezeFDSlippi", "optional", "N/A", "optional: freezes the Final Destination background"),
    ("Widescreen", "optional", "N/A", "optional: our renderer has its own widescreen"),
    ("Disable Screen Shake", "optional", "N/A", "optional, cosmetic"),
    ("Force2PCenterHud", "optional", "N/A", "optional, cosmetic"),
    ("FlashRedFailedLCancel", "optional", "N/A", "optional, cosmetic"),
    ("IndicateLocalPlayer", "optional", "N/A", "optional, cosmetic"),
    ("ShowAllyIndicator", "optional", "N/A", "optional, cosmetic"),
]

CATEGORY_TITLES = [
    ("rules", "Rules the codes pin"),
    ("gameplay", "Gameplay and determinism changes"),
    ("engine", "Rollback engine and Dolphin plumbing"),
    ("recording", "Replay recording"),
    ("ui", "Menus and on-screen UI"),
    ("qol", "Quality of life"),
    ("optional", "Optional codes (off by default)"),
    ("unclassified", "Not classified"),
]


def parse(path):
    """Yield (group, label, type, address, words) per patch in the [Gecko] section."""
    with open(path) as f:
        lines = f.read().split("\n")
    enabled = []
    section = None
    group = None
    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("["):
            section = ln.strip("[]")
        elif section == "Gecko_Enabled" and ln.startswith("$"):
            enabled.append(ln.split("[")[0].strip())
        elif section == "Gecko":
            if ln.startswith("$"):
                group = ln.split("[")[0].strip()
            elif ln.strip() and group and not ln.startswith("*") and not ln.startswith("#"):
                head = ln.split("#", 1)
                tokens = head[0].split()
                label = head[1].strip() if len(head) > 1 else ""
                if len(tokens) >= 2:
                    kind = tokens[0][:2].upper()
                    addr = int(tokens[0], 16) & 0x1FFFFFF | 0x80000000
                    words = []
                    if kind == "C2":
                        n = int(tokens[1], 16)
                        for k in range(1, n + 1):
                            words += [int(x, 16) for x in lines[i + k].split()[:2]]
                        i += n
                    if not label and out and out[-1][0] == group:
                        # A continuation of the previous labelled patch (extra
                        # hook points, or more words of the same code).
                        g, lab, k, a, w, extra = out[-1]
                        out[-1] = (g, lab, k, a, w + words, extra + [addr])
                    else:
                        out.append((group, label or "(unlabelled)", kind, addr, words, []))
        i += 1
    return enabled, out


def classify(label, group):
    for key, cat, status, note in NOTES:
        if key in label or key in group:
            return cat, status, note
    return "unclassified", "not reviewed", ""


def disasm(words):
    try:
        from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
    except ImportError:
        return None
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    data = b"".join(struct.pack(">I", w) for w in words)
    return [f"{i.address:04x}: {i.mnemonic} {i.op_str}".rstrip() for i in md.disasm(data, 0)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ini", nargs="?", default=DEFAULT_INI)
    ap.add_argument("-o", "--out", default=os.path.join(os.path.dirname(__file__), "..", "docs",
                                                         "slippi-gecko-catalog.md"))
    ap.add_argument("--disasm", action="store_true")
    args = ap.parse_args()
    if not os.path.exists(args.ini):
        sys.exit(f"gecko_catalog: {args.ini} not found (install the Slippi Launcher first)")
    enabled, patches = parse(args.ini)

    rows = {}
    for group, label, kind, addr, words, extra in patches:
        cat, status, note = classify(label, group)
        rows.setdefault(cat, []).append((group, label, kind, addr, len(words) // 2, status, note, words, extra))

    md = []
    md.append("# Slippi game modifications catalogue\n")
    md.append("Generated by `tools/gecko_catalog.py` from Slippi Dolphin's `GALE01r2.ini`; do not "
              "edit the tables by hand. Statuses come from the `NOTES` list in the script and "
              "record what was checked in this tree.\n")
    md.append(f"Source: `{os.path.basename(args.ini)}` ({len(patches)} patches in "
              f"{len({p[0] for p in patches})} code groups).\n")
    md.append("Enabled by default in Slippi netplay: " + ", ".join(f"`{e}`" for e in enabled) + ".\n")
    tally = {}
    for cat_rows in rows.values():
        for r in cat_rows:
            tally[r[5]] = tally.get(r[5], 0) + 1
    md.append("Status totals: " + ", ".join(f"{k} {v}" for k, v in sorted(tally.items())) + ".\n")
    md.append("Status key: **DONE** implemented here, **MISSING** searched for and not found, "
              "**UNVERIFIED** exists in some form but was not compared with the asm, **UI** menu "
              "behaviour (see the M4 UI port section of `slippi-migration-plan.md`), **PARTIAL**, "
              "**N/A** Dolphin plumbing or replaced by our own design.\n")
    for cat, title in CATEGORY_TITLES:
        if cat not in rows:
            continue
        md.append(f"\n## {title}\n")
        md.append("| Patch | Group | Hook | Words | Status | Note |")
        md.append("|---|---|---|---|---|---|")
        for group, label, kind, addr, n, status, note, words, extra in rows[cat]:
            g = group.replace("$", "")
            more = f" (+{len(extra)})" if extra else ""
            md.append(f"| `{label}` | {g} | `{addr:08X}`{more} {TYPES.get(kind, kind)} | {n or ''} | "
                      f"**{status}** | {note} |")
    if args.disasm:
        md.append("\n## Disassembly\n")
        for cat, _ in CATEGORY_TITLES:
            for group, label, kind, addr, n, status, note, words, extra in rows.get(cat, []):
                if kind != "C2":
                    continue
                d = disasm(words)
                if d is None:
                    sys.exit("gecko_catalog: --disasm needs the capstone module")
                md.append(f"\n### {label} (`{addr:08X}`)\n")
                md.append("```\n" + "\n".join(d) + "\n```")
    with open(args.out, "w") as f:
        f.write("\n".join(md) + "\n")
    print(f"gecko_catalog: wrote {args.out} ({len(patches)} patches)")
    unclassified = len(rows.get("unclassified", []))
    if unclassified:
        print(f"gecko_catalog: {unclassified} patches have no note; add them to NOTES")


if __name__ == "__main__":
    main()

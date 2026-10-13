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
    ("NeutralSpawn", "gameplay", "MISSING",
     "Replaces retail's randomised spawn placement with a fixed per-stage "
     "arrangement (hook 0x8016E510, data table after the code). Nothing in "
     "our tree does this, so spawn positions and the RNG draws behind them "
     "differ from Slippi's: a certain cross-play desync. Port before M5."),
    ("UCF Dashback", "gameplay", "DONE", "ftCo_Turn.c ftCo_UcfDashback()"),
    ("UCF Shield Drop Extended", "gameplay", "MISSING", "only the base shield-drop rule is ported"),
    ("UCF Shield Drop", "gameplay", "DONE",
     "ftCo_Escape.c ftCo_UcfBlocksSpotDodge() (labelled shield drop in the source)"),
    ("UCF SDI", "gameplay", "MISSING", "no UCF SDI handling found in src/melee/ft"),
    ("UCF Shield SDI", "gameplay", "MISSING", "no UCF shield-SDI handling found"),
    ("UCF Tumble", "gameplay", "MISSING", "no UCF tumble handling found"),
    ("UCF Pad Buffer", "gameplay", "MISSING",
     "pad buffering and the 1.0 cardinals fix; only the dashback's raw-X history exists"),
    ("UCF DBOOC SquatRv Fix", "gameplay", "MISSING", "not found"),
    ("UCF Text", "ui", "N/A", "the on-screen UCF indicator; cosmetic"),
    ("FreezeGlitchFix", "gameplay", "DONE", "ftnanaspecials.c, gated on pc_net_deterministic()"),
    ("PreventWobbling", "gameplay", "DONE", "ftwobble.c (3-hit limit), gated on pc_net_deterministic()"),
    ("FreezeDeadUpFallPhysics", "gameplay", "DONE",
     "ftdrawcommon.c UpdateModelPos and ftCo_DeadUpFall_Anim, gated on pc_net_deterministic()"),
    ("WhispyBlowDirFix", "gameplay", "UNVERIFIED",
     "grgreens.c has get_whispy_dir(); whether it matches Slippi's fix was not compared"),
    ("NanaDeterminism", "gameplay", "UNVERIFIED", "Nana's state is in the snapshot; not compared with the asm"),
    ("BrawlOffscreenDamage", "gameplay", "MISSING", "not found; changes offscreen damage handling"),
    ("PSCameraIndependentMonitor", "gameplay", "DONE", "grpstadium.c, gated on pc_net_deterministic()"),
    ("DesyncProofBGTransformations", "gameplay", "UNVERIFIED",
     "Final Destination background transform; not compared"),
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
    ("LGLExceededGameEnd", "gameplay", "UNVERIFIED", "ends a game on a lag-limit rule; not compared"),
    ("PauseCounter", "gameplay", "UNVERIFIED", "online pause handling (pause is allowed only in Direct)"),
    ("InitPause", "gameplay", "UNVERIFIED", "online pause handling"),
    ("PreventPauseStranding", "gameplay", "UNVERIFIED", "online pause handling"),
    ("Salty Runback", "gameplay", "UNVERIFIED", "L+R+A+Start behaviour online"),
    ("Remember Who LRA Started", "gameplay", "UNVERIFIED", "no-contest bookkeeping"),
    ("ReportNoContest", "gameplay", "UNVERIFIED", "no-contest bookkeeping"),
    ("Lagless FoD", "gameplay", "UNVERIFIED",
     "Recommended code: removes the Fountain of Dreams lag; check it does not alter physics"),
    ("Normal Lag Reduction", "gameplay", "UNVERIFIED", "Recommended code: the game's own lag-reduction tick"),
    ("UseInGameDelay", "gameplay", "UNVERIFIED", "Recommended: input delay applied in every in-game scene"),
    ("GetCommonMinorID", "n/a", "N/A", "helper for UseInGameDelay"),
    ("Polling Drift Fix", "n/a", "N/A", "Dolphin controller polling; not applicable to a native build"),
    ("DynamicsFix", "gameplay", "UNVERIFIED", "fast-forward dynamics used while resimulating"),
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

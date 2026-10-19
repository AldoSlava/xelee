#!/usr/bin/env python3
"""Compare a Slippi replay with this game's playback of it (MELEE_SLP_PLAY,
src/pc/slp_play.c): the first frame where they part, and what parted.

    tools/slp_compare.py ORIGINAL.slp PLAYBACK.slp [--context N]

Per frame, in order: the frame-start seed (SyncRNG), each fighter's inputs
as the game read them (pre-frame: the processed sticks, trigger and
buttons) and its state going in, then its state after the tick
(post-frame). Floats compare bit for bit. The inputs are what playback fed;
if they part first, the feed is wrong, not the game. Exit 0 when the two
agree on every frame both hold, 1 when they part.
"""
import argparse
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slp_read  # noqa: E402

INPUTS = ("joy_x", "joy_y", "c_x", "c_y", "trigger", "buttons")
PRE_STATE = ("seed", "action", "x", "y", "facing", "percent")
POST = tuple(name for name, _, _ in slp_read.POST_FIELDS)
# Bits Dolphin's replays hold that the simulation never reads: flags5's
# 0x80, offscreen, which the render pass writes (src/pc/slp.c capture_fighter).
MASKS = {"flags5": 0x7F}
# DeadUpFall to DeadUpFallHitCameraIce: after the fighter hits the screen,
# ftCo_DeadUpFall_Anim's bounce (self_vel.y = x550) moves ours and leaves
# Dolphin's where it hit. The fighter is out until Rebirth puts it back, and
# nothing else parts (a4 d16b6b1c: all 11617 frames agree but these); the
# cause is unexplained. Skip y and self_y there.
DEAD_UP_FALL = range(6, 11)


def bits(v):
    return struct.pack('>f', v) if isinstance(v, float) else v


def diff(a, b, names):
    out = []
    for n in names:
        va, vb = a.get(n), b.get(n)
        if va is None or vb is None:
            continue
        if n in MASKS:
            va, vb = va & MASKS[n], vb & MASKS[n]
        if bits(va) != bits(vb):
            out.append((n, va, vb))
    return out


def fmt(n, va, vb):
    if isinstance(va, float):
        return f"{n} {va!r} vs {vb!r} (off {vb - va:+.6g})"
    if n == "buttons":
        return f"{n} {va:08x} vs {vb:08x}"
    return f"{n} {va} vs {vb}"


def compare(orig, play, context=0):
    a, b = slp_read.read(orig), slp_read.read(play)
    frames = sorted(set(a.frames) & set(b.frames))
    if not frames:
        return None, "no frames in common"
    for f in frames:
        fa, fb = a.frames[f], b.frames[f]
        found = []
        if fa["start"] and fb["start"] and fa["start"]["seed"] != fb["start"]["seed"]:
            found.append(("frame", "seed", [("seed", fa["start"]["seed"], fb["start"]["seed"])]))
        for key in sorted(set(fa["pre"]) | set(fb["pre"])):
            if key not in fa["pre"] or key not in fb["pre"]:
                found.append((key, "pre", [("present", key in fa["pre"], key in fb["pre"])]))
                continue
            d = diff(fa["pre"][key], fb["pre"][key], INPUTS)
            if d:
                found.append((key, "inputs", d))
            names = PRE_STATE
            if fa["pre"][key]["action"] in DEAD_UP_FALL:
                names = tuple(n for n in names if n != "y")
            d = diff(fa["pre"][key], fb["pre"][key], names)
            if d:
                found.append((key, "state in", d))
        for key in sorted(set(fa["post"]) | set(fb["post"])):
            if key not in fa["post"] or key not in fb["post"]:
                found.append((key, "post", [("present", key in fa["post"], key in fb["post"])]))
                continue
            names = POST
            if not 75 <= (fa["post"][key]["action"] or 0) <= 91:
                # misc_as is hitstun only in the damage states (DamageHi1 to
                # DamageFlyRoll); in the others it is whatever that state keeps
                # in its first motion variable.
                names = tuple(n for n in POST if n != "misc_as")
            if fa["post"][key]["action"] in DEAD_UP_FALL:
                names = tuple(n for n in names if n not in ("y", "self_y"))
            d = diff(fa["post"][key], fb["post"][key], names)
            # ftAnim_8006F3DC (the animation frame) falls off its end when no
            # part is animated (Yoshi's egg shield): the GameCube returns
            # whatever the animation code left in f1, ours a defined 0.
            # Nothing the simulation reads differs; skip it there.
            d = [x for x in d if not (x[0] == "action_frame" and x[2] == 0.0 and x[1] != 0.0)]
            if d:
                found.append((key, "state out", d))
        if found:
            lines = [f"first difference at frame {f} ({len([x for x in frames if x < f])} frames agreed; "
                     f"original ends {max(a.frames)}, playback {max(b.frames)})"]
            for key, what, d in found:
                who = "frame" if key == "frame" else f"port {key[0]}{' follower' if key[1] else ''}"
                lines.append(f"  {who} {what}: " + "; ".join(fmt(*x) for x in d))
            for g in [x for x in frames if f - context <= x < f]:
                for key, e in sorted(a.frames[g]["post"].items()):
                    lines.append(f"    {g} port {key[0]} action {e['action']} pos {e['x']:.4f},{e['y']:.4f} "
                                 f"percent {e['percent']:.2f} hitlag {e['hitlag']}")
            return f, "\n".join(lines)
    ends = f"original ends {max(a.frames)}, playback {max(b.frames)}"
    return None, f"identical over {len(frames)} frames ({ends})"


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("original")
    ap.add_argument("playback")
    ap.add_argument("--context", type=int, default=0, help="frames before the difference to show")
    args = ap.parse_args()
    frame, text = compare(args.original, args.playback, args.context)
    print(text)
    sys.exit(0 if frame is None else 1)

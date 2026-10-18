#!/usr/bin/env python3
"""Read a Slippi replay (.slp): its game start, its per-frame events and its
metadata, as the project-slippi SPEC.md lays them out (offsets from the
command byte, big-endian). The format src/pc/slp_format.c writes.

    tools/slp_read.py FILE.slp ...      one summary line per file
    tools/slp_read.py --frames N FILE   also the first N frames' pre/post events

As a module: read(path) -> Replay (game start, frames by number, metadata).
Fields a file's version predates come back as None.
"""
import struct
import sys

CMD_PAYLOADS, CMD_START, CMD_PRE, CMD_POST, CMD_END, CMD_FRAME_START = 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A
CMD_ITEM, CMD_BOOKEND, CMD_SPLIT, CMD_GECKO = 0x3B, 0x3C, 0x10, 0x3D
FIRST_FRAME = -123

# Stage (StKind) and character (external CKind) names, for the summaries.
STAGES = {2: "FoD", 3: "Stadium", 8: "Yoshi's Story", 28: "Dream Land", 31: "Battlefield", 32: "FD"}
CHARS = ["Falcon", "DK", "Fox", "G&W", "Kirby", "Bowser", "Link", "Luigi", "Mario", "Marth", "Mewtwo", "Ness",
         "Peach", "Pikachu", "ICs", "Puff", "Samus", "Yoshi", "Zelda", "Sheik", "Falco", "Y.Link", "Doc", "Roy",
         "Pichu", "Ganon"]


class Replay:
    def __init__(self):
        self.version = (0, 0, 0)
        self.start = {}       # game start fields
        self.frames = {}      # frame -> {"start": {...}, "pre": {(port, follower): {...}}, "post": {...}, "items": [...]}
        self.end = None
        self.gecko = b""      # the Gecko List event's code bytes (v3.3+)
        self.metadata = {}

    def at_least(self, *v):
        return self.version >= v


def _ubjson(buf, i):
    """One UBJSON value at buf[i]; returns (value, next index). Enough for Slippi metadata."""
    t = chr(buf[i]); i += 1
    if t == '{':
        out = {}
        while chr(buf[i]) != '}':
            n, i = _ubjson_len(buf, i)
            key = buf[i:i + n].decode('utf-8', 'replace'); i += n
            out[key], i = _ubjson(buf, i)
        return out, i + 1
    if t == '[':
        out = []
        while chr(buf[i]) != ']':
            v, i = _ubjson(buf, i)
            out.append(v)
        return out, i + 1
    if t == 'S':
        n, i = _ubjson_len(buf, i)
        return buf[i:i + n].decode('utf-8', 'replace'), i + n
    fmt = {'i': '>b', 'U': '>B', 'I': '>h', 'l': '>i', 'L': '>q', 'd': '>f', 'D': '>d'}.get(t)
    if fmt:
        size = struct.calcsize(fmt)
        return struct.unpack_from(fmt, buf, i)[0], i + size
    if t in 'TF':
        return t == 'T', i
    if t == 'Z':
        return None, i
    raise ValueError(f"UBJSON type {t!r} at {i - 1}")


def _ubjson_len(buf, i):
    v, i = _ubjson(buf, i)
    return v, i


def _f(b, off, fmt):
    """A field at `off` (from the command byte) if the event is long enough, else None."""
    size = struct.calcsize(fmt)
    return struct.unpack_from('>' + fmt, b, off)[0] if off + size <= len(b) else None


def _start(b, rep):
    rep.version = (b[1], b[2], b[3])
    info = b[5:5 + 0x138]
    s = {
        "info": bytes(info),
        "stage": struct.unpack_from('>H', info, 0xE)[0],
        "seed": _f(b, 0x13D, 'I'),
        "players": [],
        "pal": _f(b, 0x1A1, '?'),
        "frozen_ps": _f(b, 0x1A2, '?'),
        "minor_scene": _f(b, 0x1A3, 'B'),
        "major_scene": _f(b, 0x1A4, 'B'),
        "match_id": b[0x2BE:0x2BE + 51].split(b"\0")[0].decode('ascii', 'replace') if len(b) > 0x2BE + 51 else None,
    }
    for p in range(4):
        o = 0x60 + 0x24 * p
        s["players"].append({
            "char": info[o], "type": info[o + 1], "stocks": info[o + 2], "color": info[o + 3],
            "dashback": _f(b, 0x141 + 4 * p, 'I'), "shield_drop": _f(b, 0x151 + 4 * p, 'I'),
        })
    rep.start = s


def _pre(b):
    return {
        "frame": _f(b, 1, 'i'), "port": b[5], "follower": b[6] != 0, "seed": _f(b, 7, 'I'),
        "action": _f(b, 0xB, 'H'), "x": _f(b, 0xD, 'f'), "y": _f(b, 0x11, 'f'), "facing": _f(b, 0x15, 'f'),
        "joy_x": _f(b, 0x19, 'f'), "joy_y": _f(b, 0x1D, 'f'), "c_x": _f(b, 0x21, 'f'), "c_y": _f(b, 0x25, 'f'),
        "trigger": _f(b, 0x29, 'f'), "buttons": _f(b, 0x2D, 'I'), "phys_buttons": _f(b, 0x31, 'H'),
        "phys_l": _f(b, 0x33, 'f'), "phys_r": _f(b, 0x37, 'f'), "raw_x": _f(b, 0x3B, 'b'),
        "percent": _f(b, 0x3C, 'f'), "raw_y": _f(b, 0x40, 'b'), "raw_cx": _f(b, 0x41, 'b'), "raw_cy": _f(b, 0x42, 'b'),
    }


POST_FIELDS = [("character", 0x7, 'B'), ("action", 0x8, 'H'), ("x", 0xA, 'f'), ("y", 0xE, 'f'),
               ("facing", 0x12, 'f'), ("percent", 0x16, 'f'), ("shield", 0x1A, 'f'), ("last_attack", 0x1E, 'B'),
               ("combo", 0x1F, 'B'), ("last_hit_by", 0x20, 'B'), ("stocks", 0x21, 'B'), ("action_frame", 0x22, 'f'),
               ("flags1", 0x26, 'B'), ("flags2", 0x27, 'B'), ("flags3", 0x28, 'B'), ("flags4", 0x29, 'B'),
               ("flags5", 0x2A, 'B'), ("misc_as", 0x2B, 'I'), ("airborne", 0x2F, '?'), ("ground_id", 0x30, 'H'),
               ("jumps", 0x32, 'B'), ("l_cancel", 0x33, 'B'), ("hurtbox", 0x34, 'B'), ("self_air_x", 0x35, 'f'),
               ("self_y", 0x39, 'f'), ("attack_x", 0x3D, 'f'), ("attack_y", 0x41, 'f'),
               ("self_ground_x", 0x45, 'f'), ("hitlag", 0x49, 'f'), ("animation", 0x4D, 'I'),
               ("instance_hit_by", 0x51, 'H'), ("instance_id", 0x53, 'H')]


def _post(b):
    d = {"frame": _f(b, 1, 'i'), "port": b[5], "follower": b[6] != 0}
    for name, off, fmt in POST_FIELDS:
        d[name] = _f(b, off, fmt)
    return d


def read(path, max_frames=None):
    data = open(path, 'rb').read()
    if data[:11] != b'{U\x03raw[$U#l':
        raise ValueError(f"{path}: not a .slp (no raw element)")
    raw_len = struct.unpack_from('>I', data, 11)[0]
    raw = data[15:15 + raw_len]
    rep = Replay()
    sizes = {}
    i = 0
    split = bytearray()
    while i < len(raw):
        cmd = raw[i]
        if cmd == CMD_PAYLOADS:
            n = raw[i + 1]
            for k in range(i + 2, i + 1 + n, 3):
                sizes[raw[k]] = struct.unpack_from('>H', raw, k + 1)[0]
            i += 1 + n
            continue
        if cmd not in sizes:
            raise ValueError(f"{path}: event 0x{cmd:02X} at {i} has no payload size")
        b = raw[i:i + 1 + sizes[cmd]]
        i += 1 + sizes[cmd]
        if cmd == CMD_START:
            _start(b, rep)
        elif cmd in (CMD_PRE, CMD_POST, CMD_FRAME_START, CMD_ITEM):
            frame = _f(b, 1, 'i')
            if max_frames is not None and frame >= FIRST_FRAME + max_frames:
                continue
            fr = rep.frames.setdefault(frame, {"start": None, "pre": {}, "post": {}, "items": []})
            if cmd == CMD_PRE:
                e = _pre(b)
                fr["pre"][(e["port"], e["follower"])] = e
            elif cmd == CMD_POST:
                e = _post(b)
                fr["post"][(e["port"], e["follower"])] = e
            elif cmd == CMD_FRAME_START:
                fr["start"] = {"seed": _f(b, 5, 'I'), "scene_frame": _f(b, 9, 'I')}
            else:
                fr["items"].append(bytes(b))
        elif cmd == CMD_END:
            rep.end = {"method": b[1], "lras": _f(b, 2, 'b')}
        elif cmd == CMD_SPLIT:
            # Message Splitter: 512 data bytes, their used size, the inner command, the last flag.
            used = struct.unpack_from('>H', b, 0x201)[0]
            split += b[1:1 + used]
            if b[0x204] and b[0x203] == CMD_GECKO:
                rep.gecko = bytes(split)
                split = bytearray()
    rest = data[15 + raw_len:]
    if rest.startswith(b'U\x08metadata'):
        try:
            rep.metadata, _ = _ubjson(rest, 10)
        except (ValueError, IndexError, struct.error):
            rep.metadata = {}
    return rep


def summary(path, rep):
    ps = [p for p in rep.start["players"] if p["type"] != 3]
    who = " vs ".join(CHARS[p["char"]] if p["char"] < len(CHARS) else str(p["char"]) for p in ps)
    frames = [f for f in rep.frames if f >= FIRST_FRAME]
    last = max(frames) if frames else None
    pre0 = next(iter(rep.frames.get(FIRST_FRAME, {}).get("pre", {}).values()), {})
    have_raw = "raw stick+c" if pre0.get("raw_cy") is not None else "raw stick" if pre0.get("raw_y") is not None else \
        "raw x only" if pre0.get("raw_x") is not None else "no raw"
    played = rep.metadata.get("playedOn", "?")
    scene = f"{rep.start['major_scene']}/{rep.start['minor_scene']}"
    return (f"{path.split('/')[-1][:40]:40s} v{'.'.join(map(str, rep.version))} {played:8s} scene {scene:6s} "
            f"{STAGES.get(rep.start['stage'], rep.start['stage'])!s:14s} {who:20s} frames {last} {have_raw} "
            f"gecko {len(rep.gecko)} B, ucf {[p['dashback'] for p in ps]}/{[p['shield_drop'] for p in ps]}")


if __name__ == "__main__":
    args = sys.argv[1:]
    nframes = None
    if args[:1] == ["--frames"]:
        nframes = int(args[1])
        args = args[2:]
    for path in args:
        try:
            rep = read(path)
        except (ValueError, struct.error, IndexError) as e:
            print(f"{path}: {e}")
            continue
        print(summary(path, rep))
        if nframes:
            for f in sorted(rep.frames)[:nframes]:
                print(f, rep.frames[f]["start"])
                for k, e in sorted(rep.frames[f]["pre"].items()):
                    print("  pre", k, e)
                for k, e in sorted(rep.frames[f]["post"].items()):
                    print("  post", k, {n: e[n] for n in ("action", "x", "y", "percent", "stocks")})

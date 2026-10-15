"""Bounds of a stage's model parts in rest pose, read from the disc.

stagebounds.py ISO GrXx.dat [part]: every part (map_gobj) of the stage, or one
part joint by joint (joints numbered depth first, as xr_scene.c counts them).
Animation is not applied."""
import math, struct, sys

def u32(b, o): return struct.unpack_from('>I', b, o)[0]

def iso_file(path, name):
    with open(path, 'rb') as f:
        f.seek(0x424); fst_off, fst_size = struct.unpack('>II', f.read(8))
        f.seek(fst_off); fst = f.read(fst_size)
    n = u32(fst, 8); strs = n * 12
    for i in range(1, n):
        w0, off, size = struct.unpack_from('>III', fst, i * 12)
        if w0 >> 24: continue
        s = fst[strs + (w0 & 0xFFFFFF):]; s = s[:s.index(b'\0')].decode()
        if s.lower() == name.lower():
            with open(path, 'rb') as f:
                f.seek(off); return f.read(size)
    raise SystemExit(f'{name} not on the disc')

dat = iso_file(sys.argv[1], sys.argv[2])
size, dsize, nreloc, nroot = struct.unpack_from('>IIII', dat, 0)
D = dat[0x20:]
tab = 0x20 + dsize + nreloc * 4
strtab = tab + (nroot + u32(dat, 0x10)) * 8
roots = {}
for i in range(nroot):
    off, so = struct.unpack_from('>II', dat, tab + i * 8)
    s = dat[strtab + so:]; roots[s[:s.index(b'\0')].decode()] = off

def p(o): return u32(D, o)
def f3(o): return struct.unpack_from('>3f', D, o)

def mat(rot, scl, tr):
    # HSD: M = T * Rz * Ry * Rx * S
    cx, sx = math.cos(rot[0]), math.sin(rot[0]); cy, sy = math.cos(rot[1]), math.sin(rot[1])
    cz, sz = math.cos(rot[2]), math.sin(rot[2])
    r = [[cy*cz, sx*sy*cz - cx*sz, cx*sy*cz + sx*sz],
         [cy*sz, sx*sy*sz + cx*cz, cx*sy*sz - sx*cz],
         [-sy, sx*cy, cx*cy]]
    return [[r[i][0]*scl[0], r[i][1]*scl[1], r[i][2]*scl[2], tr[i]] for i in range(3)]

def mul(a, b):
    return [[sum(a[i][k]*b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]

def xf(m, v): return tuple(m[i][0]*v[0] + m[i][1]*v[1] + m[i][2]*v[2] + m[i][3] for i in range(3))

CSIZE = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4}
CLR = {0: 2, 1: 3, 2: 4, 3: 2, 4: 3, 5: 4}

def pobj_points(po):
    descs = []; d = p(po + 8)
    while True:
        attr, atype, cnt, ctype = struct.unpack_from('>IIII', D, d)
        if attr == 0xFF: break
        descs.append((attr, atype, cnt, ctype, D[d + 0x10], struct.unpack_from('>H', D, d + 0x12)[0], p(d + 0x14)))
        d += 0x18
    dl, dlen = p(po + 0x10), struct.unpack_from('>H', D, po + 0xE)[0] * 32
    def read_pos(i, cnt, ctype, frac, stride, data):
        n = 3 if cnt == 1 else 2
        fmt = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'f'}[ctype]
        v = struct.unpack_from('>' + fmt * n, D, data + i * stride)
        if ctype != 4: v = tuple(x / (1 << frac) for x in v)
        return v + (0.0,) * (3 - n)
    out = []; o = dl; end = dl + dlen
    while o < end:
        op = D[o]; o += 1
        if op & 0xF8 not in (0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8): break
        n = struct.unpack_from('>H', D, o)[0]; o += 2
        for _ in range(n):
            for attr, atype, cnt, ctype, frac, stride, data in descs:
                if atype in (2, 3):
                    i = D[o] if atype == 2 else struct.unpack_from('>H', D, o)[0]
                    o += atype - 1
                    if attr == 9: out.append(read_pos(i, cnt, ctype, frac, stride, data))
                elif atype == 1:
                    if attr <= 8: o += 1
                    elif attr in (11, 12): o += CLR[ctype]
                    else:
                        n2 = {9: 3 if cnt else 2, 10: 3, 25: 9}.get(attr, 2 if cnt else 1)
                        if attr == 9: out.append(read_pos(0, cnt, ctype, frac, 0, o))
                        o += n2 * CSIZE[ctype]
    return out

def walk(jo, parent, idx, acc):
    while jo:
        flags = p(jo + 4)
        m = mul(parent, mat(f3(jo + 0x14), f3(jo + 0x20), f3(jo + 0x2C)))
        j = idx[0]; idx[0] += 1
        if not flags & ((1 << 5) | (1 << 14)):
            do = p(jo + 0x10)
            while do:
                po = p(do + 0xC)
                while po:
                    world = (struct.unpack_from('>H', D, po + 0xC)[0] & 0x3000) == 0x2000
                    for v in pobj_points(po):
                        acc.setdefault(j, []).append(xf(m, v) if not world else v)
                    po = p(po + 4)
                do = p(do + 4)
        if not flags & (1 << 12):
            walk(p(jo + 8), m, idx, acc)
        jo = p(jo + 0xC)

def box(pts):
    return [min(v[i] for v in pts) for i in range(3)], [max(v[i] for v in pts) for i in range(3)]

head = roots['map_head']
descs, count = p(head + 8), p(head + 0xC)
I = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
only = int(sys.argv[3]) if len(sys.argv) > 3 else None
for part in range(count):
    if only is not None and part != only: continue
    acc = {}; walk(p(descs + part * 0x34), I, [0], acc)
    pts = [v for l in acc.values() for v in l]
    if not pts: print(f'part {part}: no meshes'); continue
    lo, hi = box(pts)
    print(f'part {part}: x {lo[0]:8.1f} {hi[0]:8.1f}  y {lo[1]:8.1f} {hi[1]:8.1f}  z {lo[2]:8.1f} {hi[2]:8.1f}  ({len(pts)} verts)')
    if only is not None:
        for j, l in sorted(acc.items()):
            lo, hi = box(l)
            print(f'  joint {j:3}: x {lo[0]:8.1f} {hi[0]:8.1f}  y {lo[1]:8.1f} {hi[1]:8.1f}  z {lo[2]:8.1f} {hi[2]:8.1f}  ({len(l)})')

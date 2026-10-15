#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the launcher's Melee-style art (resources/launcher.rml) into
resources/launcher/, and cuts its menu font:

  launcher/sprites.png   menu plates (idle and selected), the cursor disc,
                         the value arrows; drawn at 2x, see launcher.rcss
  launcher/backdrop.png  the navy grid and light swirls behind the menus
                         (PNG: aurora's RmlUi loads no other format)
  menu.ttf, menu-bold.ttf
                         Noto Sans SemiCondensed Regular and Bold, cut to
                         Latin (OFL; FONT-LICENSE.txt)

The look follows Melee's own Main Menu and Options screens: dark plates with
a gold rim, a chevron at the left end and a D tab at the right, the selected
plate filled yellow with the glowing cursor disc on its tab. Everything is
drawn here; nothing comes from the disc.

Needs Pillow and fontTools. The Noto files come from the system
(Fedora's google-noto-sans-fonts) unless --noto points elsewhere.
Run from anywhere: python3 tools/launcher_art/make_launcher_art.py
"""
import argparse
import math
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RES = os.path.join(ROOT, "resources")
OUT = os.path.join(RES, "launcher")

SS = 4  # supersampling: shapes drawn at 4x the sprite size, then reduced

# Melee's menu palette, sampled from the Main Menu and Options screens.
GOLD_HI = (246, 214, 112)
GOLD = (214, 170, 62)
GOLD_LO = (120, 88, 26)
PLATE_TOP = (34, 28, 12)
PLATE_BOTTOM = (6, 6, 8)
YELLOW_HI = (255, 236, 96)
YELLOW = (255, 203, 0)
YELLOW_LO = (226, 160, 0)
DISC = (253, 238, 100)
DISC_RING = (215, 211, 85)

# Sprite sheet layout. Sizes are sprite pixels at 2x (the sheet's
# `resolution: 2x`), so a 112 px plate is 56 dp tall. launcher.rcss repeats
# these rectangles; keep the two in step.
PLATE_W, PLATE_H = 480, 112
SMALL_H = 88
DISC_SIZE = 136
ARROW_W, ARROW_H = 28, 40


def vgradient(w, h, stops):
    """A vertical gradient through (position, colour) stops."""
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for i in range(len(stops) - 1):
            p0, c0 = stops[i]
            p1, c1 = stops[i + 1]
            if p0 <= t <= p1:
                f = (t - p0) / max(1e-6, p1 - p0)
                c = tuple(int(c0[k] + (c1[k] - c0[k]) * f) for k in range(3)) + (255,)
                break
        for x in range(w):
            px[x, y] = c
    return img


def plate_outline(w, h, inset):
    """The plate's silhouette: a chevron at the left, square at the right,
    shrunk by `inset` (for the fill inside the rim)."""
    cut = h * 0.42
    tab = h * 0.62  # the right end stops short of the D tab's far edge
    i = inset
    return [
        (cut + i * 0.6, i),
        (w - tab - i, i),
        (w - tab - i, h - i),
        (cut + i * 0.6, h - i),
        (2 + i * 1.4, h / 2),
    ]


def tab_box(w, h, inset):
    """The D tab: a half disc on the plate's right end."""
    r = h * 0.30
    cx = w - h * 0.62
    return (cx - r + inset, h / 2 - r + inset, cx + r - inset, h / 2 + r - inset)


def draw_plate(h, selected):
    w = PLATE_W
    W, H = w * SS, h * SS
    rim = 6 * SS
    # Rim: gold, lit from above.
    rim_fill = vgradient(W, H, [(0, GOLD_HI), (0.45, GOLD), (1, GOLD_LO)])
    mask = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(mask)
    d.polygon(plate_outline(W, H, 0), fill=255)
    d.pieslice(tab_box(W, H, 0), -90, 90, fill=255)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    img.paste(rim_fill, (0, 0), mask)
    # Body: dark with a warm top edge, or the selected yellow.
    if selected:
        body = vgradient(W, H, [(0, YELLOW_HI), (0.35, YELLOW), (1, YELLOW_LO)])
    else:
        body = vgradient(W, H, [(0, PLATE_TOP), (0.5, (14, 12, 8)), (1, PLATE_BOTTOM)])
    inner = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(inner)
    d.polygon(plate_outline(W, H, rim), fill=255)
    img.paste(body, (0, 0), inner)
    # The tab: a dark half disc inside the rim (the cursor disc covers it on
    # the selected plate, which keeps it yellow).
    tab = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(tab)
    d.pieslice(tab_box(W, H, rim), -90, 90, fill=255)
    img.paste((255, 214, 40, 255) if selected else (10, 9, 6, 255), (0, 0), tab)
    # A thin dark line between the body and the rim, as Melee draws it.
    line = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(line)
    d.line(plate_outline(W, H, rim) + [plate_outline(W, H, rim)[0]], fill=255, width=SS * 2)
    img.paste((40, 26, 4, 200) if selected else (0, 0, 0, 230), (0, 0), line)
    return img.resize((w, h), Image.LANCZOS)


def draw_disc():
    s = DISC_SIZE * SS
    c = s / 2
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # Soft halo.
    halo = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(halo)
    d.ellipse((c - s * 0.46, c - s * 0.46, c + s * 0.46, c + s * 0.46), fill=(255, 236, 110, 150))
    halo = halo.filter(ImageFilter.GaussianBlur(s * 0.05))
    img.alpha_composite(halo)
    d = ImageDraw.Draw(img)
    # Ring, then the disc, then an inner ring and highlight.
    r = s * 0.36
    d.ellipse((c - r, c - r, c + r, c + r), fill=DISC_RING + (255,))
    r = s * 0.31
    d.ellipse((c - r, c - r, c + r, c + r), fill=DISC + (255,))
    r = s * 0.17
    d.ellipse((c - r, c - r, c + r, c + r), outline=(200, 180, 40, 255), width=int(SS * 3))
    shine = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    r = s * 0.07
    ImageDraw.Draw(shine).ellipse((c - r - s * 0.09, c - r - s * 0.11, c + r - s * 0.09, c + r - s * 0.11),
                                  fill=(255, 255, 235, 190))
    img.alpha_composite(shine.filter(ImageFilter.GaussianBlur(SS * 2)))
    return img.resize((DISC_SIZE, DISC_SIZE), Image.LANCZOS)


def draw_arrow(direction, colour):
    w, h = ARROW_W * SS, ARROW_H * SS
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = 3 * SS
    if direction < 0:
        pts = [(w - m, m), (w - m, h - m), (m, h / 2)]
    else:
        pts = [(m, m), (m, h - m), (w - m, h / 2)]
    d.polygon(pts, fill=colour + (255,))
    return img.resize((ARROW_W, ARROW_H), Image.LANCZOS)


def sprites():
    """One sheet. The rectangles here are the ones launcher.rcss names."""
    sheet = Image.new("RGBA", (1024, 512), (0, 0, 0, 0))
    y = 0
    sheet.alpha_composite(draw_plate(PLATE_H, False), (0, y))
    sheet.alpha_composite(draw_plate(PLATE_H, True), (512, y))
    y += PLATE_H  # 112
    sheet.alpha_composite(draw_plate(SMALL_H, False), (0, y))
    sheet.alpha_composite(draw_plate(SMALL_H, True), (512, y))
    y += SMALL_H  # 200
    sheet.alpha_composite(draw_disc(), (0, y))
    x = DISC_SIZE + 8
    for i, (direction, colour) in enumerate(
            [(-1, GOLD), (1, GOLD), (-1, (24, 18, 4)), (1, (24, 18, 4))]):
        sheet.alpha_composite(draw_arrow(direction, colour), (x + i * (ARROW_W + 8), y))
    return sheet


def backdrop(w=1600, h=1000):
    """Melee's menu backdrop: deep navy, a grid bent like the inside of a
    sphere, and thin rings of light sweeping through it."""
    rnd = random.Random(1102)
    img = vgradient(w, h, [(0, (7, 7, 26)), (0.45, (13, 14, 48)), (1, (5, 5, 20))]).convert("RGB")

    # Coloured haze, as the menus have behind the rings.
    haze = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(haze)
    for (cx, cy, r, col) in [(0.82, 0.18, 0.30, (90, 18, 48)), (0.12, 0.62, 0.22, (14, 70, 40)),
                             (0.55, 0.85, 0.35, (30, 26, 110)), (0.30, 0.20, 0.25, (40, 30, 120))]:
        d.ellipse(((cx - r) * w, (cy - r) * w + (h - w) / 2, (cx + r) * w, (cy + r) * w + (h - w) / 2), fill=col)
    haze = haze.filter(ImageFilter.GaussianBlur(w * 0.08))
    img = ImageChops.add(img, haze.point(lambda v: int(v * 0.5)))

    # The grid, drawn bowed outward from the centre.
    grid = Image.new("L", (w * 2, h * 2), 0)
    d = ImageDraw.Draw(grid)
    k = 0.18

    def bow(x, y):
        nx, ny = x / w - 0.5, y / h - 0.5
        f = 1 + k * (nx * nx + ny * ny) * 4
        return ((nx * f + 0.5) * w * 2, (ny * f + 0.5) * h * 2)

    step = w / 22
    for gx in range(-4, 27):
        pts = [bow(gx * step, y) for y in range(-200, h + 200, 20)]
        d.line(pts, fill=150, width=3)
    for gy in range(-4, int(h / step) + 5):
        pts = [bow(x, gy * step) for x in range(-200, w + 200, 20)]
        d.line(pts, fill=150, width=3)
    grid = grid.resize((w, h), Image.LANCZOS)
    img.paste((56, 68, 170), (0, 0), grid.point(lambda v: int(v * 0.75)))

    # Rings of light.
    rings = Image.new("RGB", (w, h), (0, 0, 0))
    for i in range(9):
        layer = Image.new("RGB", (w, h), (0, 0, 0))
        d = ImageDraw.Draw(layer)
        cx = w * rnd.uniform(0.25, 0.8)
        cy = h * rnd.uniform(0.3, 0.8)
        rx = w * rnd.uniform(0.18, 0.55)
        ry = rx * rnd.uniform(0.25, 0.6)
        col = rnd.choice([(110, 120, 255), (150, 110, 255), (90, 160, 255), (200, 200, 255)])
        start = rnd.uniform(0, 360)
        d.arc((cx - rx, cy - ry, cx + rx, cy + ry), start, start + rnd.uniform(90, 220), fill=col, width=3)
        layer = layer.rotate(rnd.uniform(-35, 35), center=(cx, cy), resample=Image.BICUBIC)
        glow = layer.filter(ImageFilter.GaussianBlur(10))
        rings = ImageChops.add(rings, ImageChops.add(layer, glow))
    img = ImageChops.add(img, rings.point(lambda v: int(v * 0.55)))

    # Darken the edges so the frame reads against them.
    vignette = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(vignette)
    d.ellipse((-w * 0.15, -h * 0.2, w * 1.15, h * 1.2), fill=255)
    vignette = vignette.filter(ImageFilter.GaussianBlur(w * 0.08))
    img = Image.composite(img, Image.new("RGB", (w, h), (4, 4, 14)), vignette)
    return img


# Latin, Latin-1, Latin Extended-A, general punctuation, currency, arrows.
LATIN = ("U+0020-007E,U+00A0-00FF,U+0100-017F,U+2010-2027,U+2030-205E,"
         "U+20A0-20C0,U+2122,U+2190-2195,U+2212")


def cut_font(src, dst):
    from fontTools import subset
    options = subset.Options()
    options.layout_features = ["kern", "liga", "calt", "ccmp", "locl", "mark", "mkmk"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    font = subset.load_font(src, options)
    sub = subset.Subsetter(options)
    sub.populate(unicodes=subset.parse_unicodes(LATIN))
    sub.subset(font)
    subset.save_font(font, dst, options)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--noto", default="/usr/share/fonts/google-noto",
                        help="directory holding NotoSans-SemiCondensed{,Bold}.ttf")
    parser.add_argument("--no-fonts", action="store_true", help="redraw the art only")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    sprites().save(os.path.join(OUT, "sprites.png"), optimize=True)
    backdrop().save(os.path.join(OUT, "backdrop.png"), optimize=True)
    if not args.no_fonts:
        cut_font(os.path.join(args.noto, "NotoSans-SemiCondensed.ttf"), os.path.join(RES, "menu.ttf"))
        cut_font(os.path.join(args.noto, "NotoSans-SemiCondensedBold.ttf"), os.path.join(RES, "menu-bold.ttf"))
    for name in ["launcher/sprites.png", "launcher/backdrop.png", "menu.ttf", "menu-bold.ttf"]:
        path = os.path.join(RES, name)
        if os.path.exists(path):
            print(f"{name}: {os.path.getsize(path) // 1024} KiB")


if __name__ == "__main__":
    main()

/* PC only: Slippi Direct's Frozen Pokemon Stadium toggle, drawn on the stage
 * select as Slippi's patched MnSlMap draws it (FrozenStadiumToggle/*.asm):
 *
 *  - The stage name reads "Frozen" instead of "Kanto" above "Pokemon Stadium"
 *    while the toggle is on. Slippi adds a frame to the name's texture
 *    animation; we copy the vanilla frame at runtime and paste our own caption
 *    over its first rows (mnstagesel_frozen.h, tools/gen_sss_frozen.py).
 *  - While Stadium is hovered, the "STAGE SELECT" banner slides out to the
 *    right and a "[Z] ALT STAGE" banner slides in (and back when the cursor
 *    leaves). Slippi's joints use one translate-X track, 60 at frame 0 to 0 at
 *    frame 20, eased at both ends, and step their frames toward 20 or 0 each
 *    frame. We build both joints at runtime from the vanilla banner's own
 *    descriptors; the ALT image is set from the vanilla letters (A, L, T, S,
 *    G, E all appear in "STAGE SELECT") after a Z pill of Slippi's colour.
 *
 * Nothing here ships Nintendo's or Slippi's pixels: the vanilla ones are read
 * from the player's disc when the scene loads. */

#include "mnstagesel_slippi.h"

#include <math.h>
#include <string.h>

#include <dolphin/gx/GXEnum.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>

#include "pc/pc.h"

#include "mnstagesel_frozen.h"

/* ---- GX texture tiles ------------------------------------------------------ */

static u8 i4Get(const u8* data, int w, int x, int y)
{
    int tiles_x = (w + 7) / 8;
    const u8* tile = data + ((y / 8) * tiles_x + x / 8) * 32;
    u8 b = tile[(y % 8) * 4 + (x % 8) / 2];
    u8 v = x % 2 == 0 ? b >> 4 : b & 15;
    return (u8) (v * 17);
}

static void i4Put(u8* data, int w, int x, int y, u8 v)
{
    int tiles_x = (w + 7) / 8;
    u8* tile = data + ((y / 8) * tiles_x + x / 8) * 32;
    u8* b = &tile[(y % 8) * 4 + (x % 8) / 2];
    u8 q = (u8) ((v * 15 + 127) / 255);
    *b = x % 2 == 0 ? (u8) ((*b & 0x0F) | q << 4) : (u8) ((*b & 0xF0) | q);
}

static void rgba8Put(u8* data, int w, int x, int y, u8 r, u8 g, u8 b, u8 a)
{
    int tiles_x = (w + 3) / 4;
    u8* tile = data + ((y / 4) * tiles_x + x / 4) * 64;
    int i = (y % 4) * 4 + x % 4;
    tile[2 * i] = a;
    tile[2 * i + 1] = r;
    tile[32 + 2 * i] = g;
    tile[32 + 2 * i + 1] = b;
}

static HSD_ImageDesc* imageNew(int w, int h, GXTexFmt fmt, u8** out)
{
    HSD_ImageDesc* desc = HSD_ImageDescAlloc();
    size_t size = fmt == GX_TF_RGBA8 ? (size_t) w * h * 4 : (size_t) w * h / 2;
    u8* buf = HSD_MemAlloc((ssize_t) size);
    memset(buf, 0, size);
    memset(desc, 0, sizeof *desc);
    DP_SET(desc->image_ptr, buf);
    desc->width = (u16) w;
    desc->height = (u16) h;
    desc->format = fmt;
    *out = buf;
    return desc;
}

/* ---- the frozen stage name ------------------------------------------------- */

#define NAME_CAPTION_ROWS 16 /* the caption lives in rows 0-15; the title below */
#define NAME_CAPTION_Y 2     /* "Kanto"'s top row */

static const HSD_ImageDesc* frozen_src;
static HSD_ImageDesc* frozen_img;

static HSD_ImageDesc* frozenName(const HSD_ImageDesc* src)
{
    int w = src->width, h = src->height, x, y;
    const u8* in = DP(u8, src->image_ptr);
    u8* out;
    if (frozen_img != NULL && frozen_src == src) {
        return frozen_img;
    }
    if (src->format != GX_TF_I4 || w < SSS_FROZEN_W || h < NAME_CAPTION_ROWS) {
        return NULL;
    }
    frozen_img = imageNew(w, h, GX_TF_I4, &out);
    frozen_src = src;
    for (y = NAME_CAPTION_ROWS; y < h; y++) {
        for (x = 0; x < w; x++) {
            i4Put(out, w, x, y, i4Get(in, w, x, y));
        }
    }
    for (y = 0; y < SSS_FROZEN_H; y++) {
        for (x = 0; x < SSS_FROZEN_W; x++) {
            i4Put(out, w, (w - SSS_FROZEN_W) / 2 + x, NAME_CAPTION_Y + y,
                  sss_frozen_caption[y * SSS_FROZEN_W + x]);
        }
    }
    return frozen_img;
}

static void nameSwap(HSD_JObj* jobj)
{
    HSD_JObj* child;
    if (!(jobj->flags & (JOBJ_INSTANCE | JOBJ_PTCL | JOBJ_SPLINE))) {
        HSD_DObj* dobj;
        for (dobj = jobj->u.dobj; dobj != NULL; dobj = dobj->next) {
            HSD_TObj* tobj;
            for (tobj = dobj->mobj != NULL ? dobj->mobj->tobj : NULL; tobj != NULL;
                 tobj = tobj->next)
            {
                HSD_ImageDesc* img = tobj->imagedesc != NULL ? frozenName(tobj->imagedesc) : NULL;
                if (img != NULL) {
                    tobj->imagedesc = img;
                }
            }
        }
    }
    for (child = jobj->child; child != NULL; child = child->next) {
        nameSwap(child);
    }
}

void mnStageSel_SlippiFrozenName(HSD_JObj* name)
{
    if (name != NULL) {
        nameSwap(name);
    }
}

/* ---- the ALT STAGE banner -------------------------------------------------- */

#define BANNER_W 232
#define BANNER_H 16
#define BANNER_LETTERS 11 /* S T A G E S E L E C T */
#define BANNER_FRAMES 20
#define BANNER_SLIDE 60.0f
#define PILL_X0 2
#define PILL_X1 47
#define PILL_Y0 2
#define PILL_Y1 13
#define ALT_TEXT_X 56

typedef struct Glyph {
    int x0, x1;
    u8 lo[BANNER_H], hi[BANNER_H]; /* per row, inclusive; lo > hi when empty */
} Glyph;

static HSD_JObj* banner_text;
static HSD_JObj* banner_alt;
static int banner_text_frame, banner_alt_frame, banner_age;

static u8 alphaAt(const u8* a, int x, int y)
{
    return x >= 0 && x < BANNER_W && y >= 0 && y < BANNER_H ? a[y * BANNER_W + x] : 0;
}

/* Label the letters of the vanilla banner (8-connected blobs), left to right. */
static int glyphsFind(const u8* a, u8* label, Glyph* g, int max)
{
    static short stack[BANNER_W * BANNER_H * 2];
    int n = 0, x, y, k;
    memset(label, 0, BANNER_W * BANNER_H);
    for (x = 0; x < BANNER_W; x++) {
        for (y = 0; y < BANNER_H; y++) {
            int sp = 0;
            if (a[y * BANNER_W + x] == 0 || label[y * BANNER_W + x] != 0) {
                continue;
            }
            if (n == max) {
                return -1;
            }
            g[n].x0 = BANNER_W;
            g[n].x1 = -1;
            for (k = 0; k < BANNER_H; k++) {
                g[n].lo[k] = 255;
                g[n].hi[k] = 0;
            }
            label[y * BANNER_W + x] = (u8) (n + 1);
            stack[sp++] = (short) x;
            stack[sp++] = (short) y;
            while (sp > 0) {
                int cy = stack[--sp], cx = stack[--sp], dx, dy;
                if (cx < g[n].x0) g[n].x0 = cx;
                if (cx > g[n].x1) g[n].x1 = cx;
                if (cx < g[n].lo[cy]) g[n].lo[cy] = (u8) cx;
                if (cx > g[n].hi[cy]) g[n].hi[cy] = (u8) cx;
                for (dx = -1; dx <= 1; dx++) {
                    for (dy = -1; dy <= 1; dy++) {
                        int nx = cx + dx, ny = cy + dy;
                        if (alphaAt(a, nx, ny) != 0 && label[ny * BANNER_W + nx] == 0) {
                            label[ny * BANNER_W + nx] = (u8) (n + 1);
                            stack[sp++] = (short) nx;
                            stack[sp++] = (short) ny;
                        }
                    }
                }
            }
            n++;
        }
    }
    return n;
}

/* The closest approach, over the rows both use, of glyph b placed `shift`
 * columns right of where it is to glyph a placed at `a_shift`. */
static int rowGap(const Glyph* a, int a_shift, const Glyph* b, int shift)
{
    int y, gap = 1000;
    for (y = 0; y < BANNER_H; y++) {
        if (a->lo[y] <= a->hi[y] && b->lo[y] <= b->hi[y]) {
            int d = (b->lo[y] + shift) - (a->hi[y] + a_shift);
            if (d < gap) gap = d;
        }
    }
    return gap;
}

static HSD_ImageDesc* altImage(const HSD_ImageDesc* src)
{
    static u8 a[BANNER_W * BANNER_H], label[BANNER_W * BANNER_H];
    static const char order[] = "STAGESELECT";
    static const char text[] = "ALT STAGE";
    Glyph g[BANNER_LETTERS];
    int idx[128];
    int x, y, i, n, gap_letter, gap_word;
    const u8* in;
    u8* out;
    HSD_ImageDesc* img;
    int prev = -1, prev_shift = 0;
    bool word_break = false;

    if (src->format != GX_TF_I4 || src->width != BANNER_W || src->height != BANNER_H) {
        return NULL;
    }
    in = DP(u8, src->image_ptr);
    for (y = 0; y < BANNER_H; y++) {
        for (x = 0; x < BANNER_W; x++) {
            a[y * BANNER_W + x] = i4Get(in, BANNER_W, x, y);
        }
    }
    n = glyphsFind(a, label, g, BANNER_LETTERS);
    if (n != BANNER_LETTERS) {
        pc_log_line("sss: STAGE SELECT banner has %d letters, not %d; no ALT STAGE banner", n,
                    BANNER_LETTERS);
        return NULL;
    }
    for (i = 0; i < 128; i++) idx[i] = -1;
    for (i = BANNER_LETTERS - 1; i >= 0; i--) idx[(int) order[i]] = i;
    gap_letter = rowGap(&g[0], 0, &g[1], 0); /* S-T, as set in the original */
    gap_word = rowGap(&g[4], 0, &g[5], 0);   /* E-S, across the word break */

    img = imageNew(BANNER_W, BANNER_H, GX_TF_RGBA8, &out);
    /* The Z pill, Slippi's #7316FF, rounded ends, a Z cut out of it. */
    for (y = PILL_Y0; y <= PILL_Y1; y++) {
        for (x = PILL_X0; x <= PILL_X1; x++) {
            float r = (PILL_Y1 - PILL_Y0 + 1) * 0.5f;
            float cy = (PILL_Y0 + PILL_Y1) * 0.5f;
            float cx = x < PILL_X0 + r - 0.5f ? PILL_X0 + r - 0.5f :
                       x > PILL_X1 - r + 0.5f ? PILL_X1 - r + 0.5f : (float) x;
            float cov = r - sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) + 0.5f;
            int zx = x - ((PILL_X0 + PILL_X1 + 1) / 2 - 4), zy = y - 4;
            bool hole = false;
            if (zx >= 0 && zx < 9 && zy >= 0 && zy < 8) {
                float t = (zy - 1.5f) / 5.0f;
                hole = zy < 2 || zy >= 6 || fabsf(zx - 8.0f * (1.0f - t)) <= 1.5f;
            }
            if (cov > 0.0f && !hole) {
                rgba8Put(out, BANNER_W, x, y, 0x73, 0x16, 0xFF,
                         (u8) (255.0f * (cov > 1.0f ? 1.0f : cov)));
            }
        }
    }
    /* The letters, each set so its closest row gap to the last is the
     * original's (the italic boxes overlap; the rows never touch). */
    for (i = 0; text[i] != '\0'; i++) {
        int k = idx[(int) (unsigned char) text[i]], shift;
        if (text[i] == ' ') {
            word_break = true;
            continue;
        }
        if (prev < 0) {
            shift = ALT_TEXT_X - g[k].x0;
        } else {
            int want = word_break ? gap_word : gap_letter;
            shift = prev_shift + g[prev].x1 - g[k].x0;
            while (rowGap(&g[prev], prev_shift, &g[k], shift) > want) shift--;
            while (rowGap(&g[prev], prev_shift, &g[k], shift) < want) shift++;
        }
        for (y = 0; y < BANNER_H; y++) {
            for (x = g[k].x0; x <= g[k].x1; x++) {
                if (label[y * BANNER_W + x] == k + 1 && x + shift >= 0 && x + shift < BANNER_W) {
                    rgba8Put(out, BANNER_W, x + shift, y, 255, 255, 255, a[y * BANNER_W + x]);
                }
            }
        }
        prev = k;
        prev_shift = shift;
        word_break = false;
    }
    return img;
}

static void* descCopy(const void* src, size_t size)
{
    void* p = HSD_MemAlloc((ssize_t) size);
    memcpy(p, src, size);
    return p;
}

/* A joint holding one copy of the banner's text DObj; `alt` gives it our
 * image and Slippi's material for it (lit, ambient C0C0C0, no PE). */
static HSD_JObj* bannerJoint(const HSD_Joint* root, const HSD_DObjDesc* text, HSD_ImageDesc* alt)
{
    HSD_Joint* j = descCopy(root, sizeof *j);
    HSD_DObjDesc* d = descCopy(text, sizeof *d);
    DP_SET(j->child, NULL);
    DP_SET(j->next, NULL);
    DP_SET(j->mtx, NULL);
    DP_SET(j->robjdesc, NULL);
    /* B19 is the translucent pass (XLU), as on the vanilla banner's root. */
    j->flags = JOBJ_UNK_B19 | JOBJ_CLASSICAL_SCALE | (alt != NULL ? JOBJ_LIGHTING : 0);
    j->rotation.x = j->rotation.y = j->rotation.z = 0.0f;
    j->scale.x = j->scale.y = j->scale.z = 1.0f;
    j->position.x = BANNER_SLIDE;
    j->position.y = j->position.z = 0.0f;
    DP_SET(d->next, NULL);
    if (alt != NULL) {
        HSD_MObjDesc* m = descCopy(DP(HSD_MObjDesc, d->mobjdesc), sizeof(HSD_MObjDesc));
        HSD_MaterialDesc* mat = descCopy(DP(HSD_MaterialDesc, m->mat), sizeof(HSD_MaterialDesc));
        HSD_TObjDesc* t = descCopy(DP(HSD_TObjDesc, m->texdesc), sizeof(HSD_TObjDesc));
        GXColor amb = { 0xC0, 0xC0, 0xC0, 0xFF };
        mat->ambient = amb;
        m->rendermode = 0x60000015;
        DP_SET(m->pedesc, NULL);
        DP_SET(m->mat, mat);
        DP_SET(t->next, NULL);
        DP_SET(t->imagedesc, alt);
        DP_SET(t->tlutdesc, NULL);
        /* The text's own TEV takes its colour from a constant (white-gray);
         * the ALT image brings its own colours (Slippi's has no TEV). */
        DP_SET(t->tev, NULL);
        DP_SET(m->texdesc, t);
        DP_SET(d->mobjdesc, m);
    }
    DP_SET(j->u.dobjdesc, d);
    return HSD_JObjLoadJoint(j);
}

void mnStageSel_SlippiBannerCreate(HSD_JObj* border, const HSD_Joint* border_desc)
{
    const HSD_DObjDesc* d0;
    const HSD_DObjDesc* d1;
    const HSD_MObjDesc* m;
    const HSD_TObjDesc* t;
    HSD_ImageDesc* alt;
    banner_text = banner_alt = NULL;
    banner_text_frame = BANNER_FRAMES;
    banner_alt_frame = 0;
    banner_age = 0;
    d0 = DP(HSD_DObjDesc, border_desc->u.dobjdesc);
    d1 = d0 != NULL ? DP(HSD_DObjDesc, d0->next) : NULL;
    m = d1 != NULL ? DP(HSD_MObjDesc, d1->mobjdesc) : NULL;
    t = m != NULL ? DP(HSD_TObjDesc, m->texdesc) : NULL;
    if (t == NULL || border->u.dobj == NULL || border->u.dobj->next == NULL) {
        return;
    }
    alt = altImage(DP(HSD_ImageDesc, t->imagedesc));
    if (alt == NULL) {
        return;
    }
    banner_text = bannerJoint(border_desc, d1, NULL);
    banner_alt = bannerJoint(border_desc, d1, alt);
    HSD_JObjAddChild(border, banner_text);
    HSD_JObjAddChild(border, banner_alt);
    HSD_DObjSetFlags(border->u.dobj->next, DOBJ_HIDDEN); /* the copy stands in for it */
}

static float bannerX(int frame)
{
    float t = (float) frame / BANNER_FRAMES;
    return BANNER_SLIDE * (1.0f - t * t * (3.0f - 2.0f * t));
}

static void bannerAlpha(HSD_JObj* jobj, float alpha)
{
    if (jobj->u.dobj != NULL && jobj->u.dobj->mobj != NULL && jobj->u.dobj->mobj->mat != NULL) {
        jobj->u.dobj->mobj->mat->alpha = alpha;
    }
}

void mnStageSel_SlippiBannerFrame(bool stadium_hovered)
{
    float fade;
    if (banner_text == NULL) {
        return;
    }
    if (stadium_hovered) {
        if (banner_text_frame > 0) banner_text_frame--;
        if (banner_alt_frame < BANNER_FRAMES) banner_alt_frame++;
    } else {
        if (banner_text_frame < BANNER_FRAMES) banner_text_frame++;
        if (banner_alt_frame > 0) banner_alt_frame--;
    }
    HSD_JObjSetTranslateX(banner_text, bannerX(banner_text_frame));
    HSD_JObjSetTranslateX(banner_alt, bannerX(banner_alt_frame));
    /* The vanilla banner fades in over the scene's first 10 frames. */
    fade = banner_age < 10 ? (float) banner_age / 10.0f : 1.0f;
    banner_age++;
    bannerAlpha(banner_text, fade);
    bannerAlpha(banner_alt, fade);
}

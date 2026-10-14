/* PC only: the status text Slippi draws on its online character select
 * ("Unranked Mode", "Character selected", "Locked in", "Searching for
 * opponent", ...). gmonlinemode.c owns the state and hands over the steps a
 * frame; this file only draws them, as SIS text on its own ortho canvas above
 * the CSS, laid out as Slippi's: the user beside the panel, the mode title and
 * a stack of marked steps to its right, a dim hint under them. */

#include <melee/gm/gmonlinemode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <dolphin/gx.h>
#include <dolphin/gx/GXGeometry.h>
#include <dolphin/gx/GXStruct.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/hsd_3915.h>
#include "pc/button_icons.h"
#include <sysdolphin/baselib/sislib.h>

#ifdef TARGET_PC

#define CSS_LINE_LEN 64

typedef struct CssLine {
    int entry;
    char str[CSS_LINE_LEN];
    int color; /* index into the palette, -1 unset */
} CssLine;

enum { c_white, c_dim, c_green, c_blue, c_red, c_count };

static GXColor palette[c_count] = {
    { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xC8, 0xC8, 0xC8, 0xFF }, { 0x40, 0xE0, 0x60, 0xFF },
    { 0x50, 0xA0, 0xFF, 0xFF }, { 0xFF, 0x60, 0x60, 0xFF },
};

enum { l_title, l_user_label, l_user_name, l_hint, l_hint2, l_mark, l_text = l_mark + CSS_STEPS_MAX,
       l_count = l_text + CSS_STEPS_MAX };

static HSD_Text* css_text;
static unsigned css_frame;
/* A pending step's marker is a plus drawn from two quads that turns to its 45
 * degree rotation and back, each way in about 0.3 s. The glyph engine cannot
 * rotate text, so the marker is drawn in the overlay's own render pass. */
#define MARK_PERIOD_FRAMES 36
#define MARK_HALF_LEN 4.2f
#define MARK_HALF_WIDTH 0.95f
static bool css_pending[CSS_STEPS_MAX];
static bool css_hint_z; /* the hint is "Press <Z> to ...": the icon stands for the Z */
#define HINT_Y_FROM_STEPS (BLOCK_Y + 30.0f + STEP_DY * CSS_STEPS_MAX + 12.0f)
#define HINT_ICON_X 43.0f /* from the hint's left edge to the icon's centre, in screen units */
#define HINT_ICON_W 30.0f
#define HINT_REST_X 64.0f
static float css_mark_y[CSS_STEPS_MAX];
static CssLine css_lines[l_count];

/* The overlay lives in the CSS's own text canvas and camera (gx link 1), so
 * the hand and token (later links) draw over it. Layout is kept in 640x480
 * screen units, as Slippi's; these map them onto the CSS world, calibrated
 * against marks at known world positions (x = 0 is the screen centre, about
 * 9.2 screen units a world unit across, 8.5 down). */
#define BLOCK_X 384.0f
#define BLOCK_Y 251.0f
#define STEP_DY 22.5f
#define ORG_X 190.0f
#define ORG_Y 236.0f
static float world_x(float u) { return (u - 320.0f) / 9.2f; }
static float world_y(float u) { return (u - 234.0f) / 8.5f; } /* the text's own y: down is positive */

static void drawRotatedRect(float cx, float cy, float hx, float hy, float a, GXColor* c)
{
    static const float sx[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    static const float sy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    float cs = cosf(a), sn = sinf(a);
    int i;
    GXBegin(0x80, 0, 4);
    for (i = 0; i < 4; i++) {
        float px = sx[i] * hx, py = sy[i] * hy;
        GXPosition2f32(cx + px * cs - py * sn, cy + px * sn + py * cs);
        GXColor4u8(c->r, c->g, c->b, c->a);
    }
    GXEnd();
}

static void drawMarks(UNUSED HSD_GObj* gobj, int pass)
{
    float t, tri, ease, angle;
    int i;
    (void) pass; /* opaque either way, so a second pass draws the same pixels */
    t = (float) (css_frame % MARK_PERIOD_FRAMES) / (float) MARK_PERIOD_FRAMES;
    tri = t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
    ease = tri * tri * (3.0f - 2.0f * tri);
    angle = ease * 0.78539816f; /* 45 degrees */
    hsd_80391A04(1.0f, 1.0f, 1);
    if (css_hint_z) {
        pc_button_icon_draw(PC_BUTTON_ICON_Z, world_x(BLOCK_X + HINT_ICON_X),
                            -world_y(HINT_Y_FROM_STEPS + 24.5f), 0.0f, HINT_ICON_W / 9.2f,
                            HINT_ICON_W / 9.2f);
        hsd_80391A04(1.0f, 1.0f, 1); /* the icon changed the vertex formats back */
    }
    for (i = 0; i < CSS_STEPS_MAX; i++) {
        if (css_pending[i]) {
            float cx = world_x(BLOCK_X + 4.0f);
            float cy = -world_y(css_mark_y[i]); /* the world's y is up */
            float hl = MARK_HALF_LEN / 9.2f, hw = MARK_HALF_WIDTH / 9.2f;
            drawRotatedRect(cx, cy, hl, hw, angle, &palette[c_blue]);
            drawRotatedRect(cx, cy, hw, hl, angle, &palette[c_blue]);
        }
    }
}

static void addLine(CssLine* l, float x, float y, float scale, int color)
{
    l->str[0] = '\0';
    l->color = color;
    l->entry = HSD_SisLib_803A6B98(css_text, x - ORG_X, (y - ORG_Y) * (9.2f / 8.5f), "%s", l->str);
    HSD_SisLib_803A7548(css_text, l->entry, scale, scale);
    HSD_SisLib_803A74F0(css_text, l->entry, &palette[color]);
}

/* Only a line whose text or tint changed touches the SIS buffer. */
static void setLine(CssLine* l, const char* s, int color)
{
    if (strcmp(l->str, s) != 0) {
        snprintf(l->str, sizeof(l->str), "%s", s);
        HSD_SisLib_803A70A0(css_text, l->entry, "%s", l->str);
    }
    if (l->color != color) {
        l->color = color;
        HSD_SisLib_803A74F0(css_text, l->entry, &palette[color]);
    }
}

void mnOnlineCss_Create(int ctx)
{
    int i;
    if (css_text != NULL) {
        return;
    }
    /* In the CSS's own canvas and camera, on its text link: the hand is a later
     * link, so it draws over everything here. */
    GObj_SetupGXLink(GObj_Create(4, 5, 0x80), (GObj_RenderFunc) drawMarks, 1, 0x80);
    css_text = HSD_SisLib_803A6754(0, ctx);
    css_text->pos_x = world_x(ORG_X);
    css_text->pos_y = world_y(ORG_Y);
    css_text->pos_z = 0.0f;
    css_text->box_size_x = 500.0f;
    css_text->box_size_y = 300.0f;
    css_text->font_size.x = 1.0f / 9.2f;
    css_text->font_size.y = 1.0f / 9.2f;
    css_text->x4E = 1;
    css_text->default_kerning = 1;
    css_text->x4C = 1;
    memset(css_lines, 0, sizeof(css_lines));
    addLine(&css_lines[l_title], BLOCK_X, BLOCK_Y, 0.42f, c_white);
    addLine(&css_lines[l_user_label], 217.0f, BLOCK_Y, 0.3f, c_dim);
    addLine(&css_lines[l_user_name], 217.0f, BLOCK_Y + 18.0f, 0.38f, c_white);
    for (i = 0; i < CSS_STEPS_MAX; i++) {
        float y = BLOCK_Y + 30.0f + STEP_DY * i;
        addLine(&css_lines[l_mark + i], BLOCK_X, y, 0.34f, c_blue);
        addLine(&css_lines[l_text + i], BLOCK_X + 16.0f, y, 0.34f, c_white);
    }
    addLine(&css_lines[l_hint], BLOCK_X, HINT_Y_FROM_STEPS, 0.3f, c_dim);
    addLine(&css_lines[l_hint2], BLOCK_X + HINT_REST_X, HINT_Y_FROM_STEPS, 0.3f, c_dim);
}

void mnOnlineCss_Update(const char* title, const char* user, const OnlineCssStep* steps, int n,
                        const char* hint)
{
    int i;
    if (css_text == NULL) {
        return;
    }
    css_frame++;
    setLine(&css_lines[l_title], title, c_white);
    setLine(&css_lines[l_user_label], user[0] ? "User" : "", c_dim);
    setLine(&css_lines[l_user_name], user, c_white);
    for (i = 0; i < CSS_STEPS_MAX; i++) {
        char mark[2] = { 0, 0 };
        int color = c_blue;
        const char* text = "";
        if (i < n) {
            mark[0] = steps[i].mark;
            if (mark[0] == '+' || mark[0] == 'x') {
                mark[0] = 0; /* drawn, not typed */
            }
            color = steps[i].mark == '-' ? c_green : steps[i].mark == '!' ? c_red : c_blue;
            text = steps[i].text;
        }
        css_pending[i] = i < n && (steps[i].mark == '+' || steps[i].mark == 'x');
        css_mark_y[i] = BLOCK_Y + 30.0f + STEP_DY * i + 24.5f;
        setLine(&css_lines[l_mark + i], mark, color);
        setLine(&css_lines[l_text + i], text, i < n && steps[i].mark == '!' ? c_red : c_white);
    }
    /* The hint stays plain text ("Press Z to cancel"); the Z icon path below
     * is kept for when it is wanted. */
    css_hint_z = false;
    setLine(&css_lines[l_hint], hint, c_dim);
    setLine(&css_lines[l_hint2], "", c_dim);
}

void mnOnlineCss_Destroy(void)
{
    if (css_text != NULL) {
        HSD_SisLib_803A5CC4(css_text);
        css_text = NULL;
    }
}

#else

void mnOnlineCss_Create(UNUSED int ctx) {}
void mnOnlineCss_Update(UNUSED const char* title, UNUSED const char* user,
                        UNUSED const OnlineCssStep* steps, UNUSED int n, UNUSED const char* hint)
{
}
void mnOnlineCss_Destroy(void) {}

#endif

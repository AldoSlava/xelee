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

#include <dolphin/gx/GXStruct.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
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

enum { l_title, l_user_label, l_user_name, l_hint, l_mark, l_text = l_mark + CSS_STEPS_MAX,
       l_count = l_text + CSS_STEPS_MAX };

static HSD_Text* css_text;
static unsigned css_frame;
/* A pending step's marker turns between a plus and its 45 degree rotation, a
 * change about every 0.3 s. */
#define MARK_FLIP_FRAMES 18
static CssLine css_lines[l_count];

#define BLOCK_X 380.0f
#define BLOCK_Y 251.0f
#define STEP_DY 22.5f

static void addLine(CssLine* l, float x, float y, float scale, int color)
{
    l->str[0] = '\0';
    l->color = color;
    l->entry = HSD_SisLib_803A6B98(css_text, x, y, "%s", l->str);
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

void mnOnlineCss_Create(void)
{
    int canvas;
    int i;
    if (css_text != NULL) {
        return;
    }
    /* Ortho 640x480 canvas above the CSS cameras, as the lobby's. */
    canvas = HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
    css_text = HSD_SisLib_803A6754(0, canvas);
    css_text->default_kerning = 1;
    memset(css_lines, 0, sizeof(css_lines));
    addLine(&css_lines[l_title], BLOCK_X, BLOCK_Y, 0.42f, c_white);
    addLine(&css_lines[l_user_label], 202.0f, BLOCK_Y, 0.3f, c_dim);
    addLine(&css_lines[l_user_name], 202.0f, BLOCK_Y + 18.0f, 0.38f, c_white);
    for (i = 0; i < CSS_STEPS_MAX; i++) {
        float y = BLOCK_Y + 30.0f + STEP_DY * i;
        addLine(&css_lines[l_mark + i], BLOCK_X, y, 0.34f, c_blue);
        addLine(&css_lines[l_text + i], BLOCK_X + 16.0f, y, 0.34f, c_white);
    }
    addLine(&css_lines[l_hint], BLOCK_X, BLOCK_Y + 30.0f + STEP_DY * CSS_STEPS_MAX + 12.0f, 0.3f,
            c_dim);
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
                mark[0] = (css_frame / MARK_FLIP_FRAMES) & 1 ? 'x' : '+';
            }
            color = steps[i].mark == '-' ? c_green : steps[i].mark == '!' ? c_red : c_blue;
            text = steps[i].text;
        }
        setLine(&css_lines[l_mark + i], mark, color);
        setLine(&css_lines[l_text + i], text, i < n && steps[i].mark == '!' ? c_red : c_white);
    }
    setLine(&css_lines[l_hint], hint, c_dim);
}

void mnOnlineCss_Destroy(void)
{
    if (css_text != NULL) {
        HSD_SisLib_803A5CC4(css_text);
        css_text = NULL;
    }
}

#else

void mnOnlineCss_Create(void) {}
void mnOnlineCss_Update(UNUSED const char* title, UNUSED const char* user,
                        UNUSED const OnlineCssStep* steps, UNUSED int n, UNUSED const char* hint)
{
}
void mnOnlineCss_Destroy(void) {}

#endif

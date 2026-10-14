/* PC only: the status text Slippi draws on its online character select
 * ("Unranked Mode", "Select your character", "Searching for opponent", ...).
 * gmonlinemode.c owns the state and hands over four strings a frame; this file
 * only draws them, as SIS text on its own ortho canvas above the CSS. */

#include <melee/gm/gmonlinemode.h>

#include <stdio.h>
#include <string.h>

#include <dolphin/gx/GXStruct.h>
#include <sysdolphin/baselib/displayfunc.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/hsd_3915.h>
#include <sysdolphin/baselib/sislib.h>

#ifdef TARGET_PC

#define CSS_LINE_LEN 64

enum { css_title, css_line1, css_line2, css_hint, css_line_count };

typedef struct CssLine {
    int entry;
    char str[CSS_LINE_LEN];
    bool error;
} CssLine;

static HSD_Text* css_text;
static CssLine css_lines[css_line_count];

static GXColor col_white = { 0xFF, 0xFF, 0xFF, 0xFF };
static GXColor col_dim = { 0xC8, 0xC8, 0xC8, 0xFF };
static GXColor col_error = { 0xFF, 0x60, 0x60, 0xFF };

/* Slippi replaces the rules banner with the mode title; the plate hides ours
 * (the rules text of the stock CSS) under it. */
static void drawPlate(UNUSED HSD_GObj* gobj, int pass)
{
    static GXColor plate = { 0x0A, 0x14, 0x32, 0xFF };
    if (pass != 2) {
        return;
    }
    hsd_80391A04(1.0f, 1.0f, 1);
    DrawRectangle(206.0f, -72.0f, 300.0f, 26.0f, &plate);
}

static void addLine(CssLine* l, float x, float y, float scale, GXColor* color)
{
    l->str[0] = '\0';
    l->error = false;
    l->entry = HSD_SisLib_803A6B98(css_text, x, y, "%s", l->str);
    HSD_SisLib_803A7548(css_text, l->entry, scale, scale);
    HSD_SisLib_803A74F0(css_text, l->entry, color);
}

/* Only a line whose text or tint changed touches the SIS buffer. */
static void setLine(CssLine* l, const char* s, bool error, GXColor* normal)
{
    if (strcmp(l->str, s) != 0) {
        snprintf(l->str, sizeof(l->str), "%s", s);
        HSD_SisLib_803A70A0(css_text, l->entry, "%s", l->str);
    }
    if (l->error != error) {
        l->error = error;
        HSD_SisLib_803A74F0(css_text, l->entry, error ? &col_error : normal);
    }
}

void mnOnlineCss_Create(void)
{
    int canvas;
    if (css_text != NULL) {
        return;
    }
    /* Ortho 640x480 canvas above the CSS cameras, as the lobby's. */
    canvas = HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
    {
        HSD_GObj* plate = GObj_Create(9, 0xD, 0);
        GObj_SetupGXLink(plate, (GObj_RenderFunc) drawPlate, 0xE, 0);
    }
    css_text = HSD_SisLib_803A6754(0, canvas);
    css_text->default_kerning = 1;
    memset(css_lines, 0, sizeof(css_lines));
    addLine(&css_lines[css_title], 232.0f, 44.0f, 0.7f, &col_white);
    addLine(&css_lines[css_line1], 250.0f, 296.0f, 0.6f, &col_white);
    addLine(&css_lines[css_line2], 250.0f, 326.0f, 0.5f, &col_white);
    addLine(&css_lines[css_hint], 250.0f, 352.0f, 0.45f, &col_dim);
}

void mnOnlineCss_Update(const char* title, const char* line1, const char* line2,
                        const char* hint, bool error)
{
    if (css_text == NULL) {
        return;
    }
    setLine(&css_lines[css_title], title, false, &col_white);
    setLine(&css_lines[css_line1], line1, error, &col_white);
    setLine(&css_lines[css_line2], line2, false, &col_white);
    setLine(&css_lines[css_hint], hint, false, &col_dim);
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
void mnOnlineCss_Update(UNUSED const char* title, UNUSED const char* line1, UNUSED const char* line2,
                        UNUSED const char* hint, UNUSED bool error) {}
void mnOnlineCss_Destroy(void) {}

#endif

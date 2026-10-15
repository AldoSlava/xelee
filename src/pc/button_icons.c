/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/button_icons.h"

#include <dolphin/gx.h>
#include <dolphin/gx/GXGeometry.h>
#include <sysdolphin/baselib/state.h>

#define ICON_SIZE 64

void pc_button_icon_draw(int which, float x, float y, float z, float w, float h)
{
    static GXTexObj tex[PC_BUTTON_ICON_COUNT];
    static unsigned char inited[PC_BUTTON_ICON_COUNT];
    const float x0 = x - w * 0.5f, x1 = x + w * 0.5f;
    const float y0 = y + h * 0.5f, y1 = y - h * 0.5f;

    if (which < 0 || which >= PC_BUTTON_ICON_COUNT) {
        return;
    }
    if (!inited[which]) {
        GXInitTexObj(&tex[which], (void*) pc_button_icon_data[which], ICON_SIZE, ICON_SIZE,
                     GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&tex[which], GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE,
                        GX_ANISO_1);
        inited[which] = 1;
    }
    GXClearVtxDesc();
    GXSetCullMode(GX_CULL_NONE);
    GXSetNumChans(0);
    GXSetNumTexGens(1);
    GXSetNumTevStages(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_DISABLE,
                      GX_PTIDENTITY);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXLoadTexObj(&tex[which], GX_TEXMAP0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetColorUpdate(GX_ENABLE);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x0, y0, z);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(x1, y0, z);
    GXTexCoord2f32(1.0f, 0.0f);
    GXPosition3f32(x1, y1, z);
    GXTexCoord2f32(1.0f, 1.0f);
    GXPosition3f32(x0, y1, z);
    GXTexCoord2f32(0.0f, 1.0f);
    GXEnd();
}

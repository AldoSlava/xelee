#include "ftucf.h"

#ifdef TARGET_PC

#include <melee/ft/fighter.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/ftCo_0A01.h>
#include <melee/ft/types.h>
#include <sysdolphin/baselib/controller.h>

#include <math.h>
#include <pc/pc.h>

/* Slippi's UCF 0.84 "Pad Buffer + 1.0 Cardinals" (GALE01r2.ini, General Codes,
 * External/UCF 0.84/UCF/UCF Pad Buffer + 1.0 Cardinals.asm, hook 0x8006B460 in
 * Fighter_procInput, right before the trigger timers; docs/slippi-gecko-
 * catalog.md). Two jobs:
 *
 *  - 1.0 cardinals: when the raw main stick (or C-stick) reads at or past the
 *    rim on one axis and within 6 units of centre on the other, the processed
 *    stick becomes exactly (+-1.0, 0) or (0, +-1.0). Melee's own clamp and
 *    scale leave a slightly off-axis value there, which is what UCF removes.
 *  - the pad buffer: the last four raw (x, y) sticks per port, plus a counter
 *    that starts when the stick is flicked down to the rim (a y change of more
 *    than 44 units over two frames while the stick has only just crossed its
 *    threshold) and runs while it stays there. The other UCF rules (shield
 *    drop, SDI, shield SDI, tumble) read both.
 *
 * Slippi keeps these in its code block, zeroed at boot and never reset; ours
 * are zeroed with the program and carried across matches the same way. */

FtUcfPad ftUcf_pad[4];

/* The two cardinal snaps (the asm's helper at +0x40, called with the raw x, y
 * and the destination pair). */
static void snapCardinal(s8 x, s8 y, Vec2* out)
{
    if ((u8) (x + 0x4F) > 0x9E) {
        /* x is at or past +-80 */
        if ((u8) (y + 6) > 12) {
            return;
        }
        out->x = x < 0 ? -1.0F : 1.0F;
        out->y = 0.0F;
    } else {
        if ((u8) (y + 0x4F) <= 0x9E) {
            return; /* neither axis is at the rim */
        }
        if ((u8) (x + 6) > 12) {
            return;
        }
        out->x = 0.0F;
        out->y = y < 0 ? -1.0F : 1.0F;
    }
}

void ftUcf_Update(Fighter* fp)
{
    FtUcfPad* b;
    const PcRawSticks* raw;
    u8 flag;
    int port;

    if (ftCo_IsCpuControlled(fp)) {
        return;
    }
    port = fp->pad_port;
    if (port >= 4) {
        return;
    }
    b = &ftUcf_pad[port];
    raw = &pc_pad_game_raw[port];

    b->idx = (b->idx + 1) & 3;
    b->hist[b->idx][0] = raw->x;
    b->hist[b->idx][1] = raw->y;

    /* Zelda's transformation keeps its own stick values. */
    if (!(fp->kind == Ft_Kind_Zelda && fp->motion_id == 0x15D)) {
        snapCardinal(raw->x, raw->y, &fp->input.lstick[0]);
        snapCardinal(raw->cx, raw->cy, &fp->input.cstick[0]);
    }

    flag = 0;
    if (fp->input.lstick[0].y <= -0.609375F) {
        /* |stick| * 80 truncated (fmsubs: one rounding), a unit of slack on
         * each axis; the stick has to be at the rim for the counter to run. */
        int xi = (int) fmaf(fabsf(fp->input.lstick[0].x), 80.0F,
                            -9.99999747378752e-05F);
        int yi = (int) fmaf(fabsf(fp->input.lstick[0].y), 80.0F,
                            -9.99999747378752e-05F);
        if ((xi + 2) * (xi + 2) + (yi + 2) * (yi + 2) > 0x1900) {
            flag = b->flag;
            if (flag != 0) {
                flag++;
            } else if (fp->active_timer.lstick.y <= 1) {
                int dy = b->hist[b->idx][1] - b->hist[(b->idx - 2) & 3][1];
                if (dy * dy > 0x790) {
                    flag++;
                }
            }
        }
    }
    b->flag = flag;
}

int ftUcf_FlickSq(int port)
{
    const FtUcfPad* b = &ftUcf_pad[port];
    int dx = b->hist[b->idx][0] - b->hist[(b->idx - 2) & 3][0];
    int dy = b->hist[b->idx][1] - b->hist[(b->idx - 2) & 3][1];
    return dx * dx + dy * dy;
}

int ftUcf_FlickYSq(int port)
{
    const FtUcfPad* b = &ftUcf_pad[port];
    int dy = b->hist[b->idx][1] - b->hist[(b->idx - 2) & 3][1];
    return dy * dy;
}

#endif

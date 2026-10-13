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

int ftUcf_FlickXSq(int port)
{
    const FtUcfPad* b = &ftUcf_pad[port];
    int dx = b->hist[b->idx][0] - b->hist[(b->idx - 2) & 3][0];
    return dx * dx;
}

/* UCF 0.84 "SDI" (UCF SDI.asm, hook 0x8008E54C in ftCo_Damage_OnEveryHitlag,
 * replacing the compare of the stick timer with the SDI window). Vanilla lets a
 * stick SDI only while a timer that counts from the stick crossing the smash
 * threshold is inside the window, which a quick flick on some controllers
 * misses. UCF adds: the stick crossed a threshold within the last frame on
 * either axis, last frame's stick was inside the dead zone, and the raw stick
 * moved more than 62 units over the last two frames. The result is the OR
 * with the vanilla test, so this is only asked once that has failed. */
bool ftUcf_SdiAllowed(Fighter* fp)
{
    float lx = fp->input.lstick[1].x;
    float ly = fp->input.lstick[1].y;
    float thr = p_ftCommonData->sdi_min_stick_mag;
    int port = fp->pad_port;
    if (port >= 4) {
        return false;
    }
    if (fp->active_sticky.lstick.x > 1 && fp->active_sticky.lstick.y > 1) {
        return false;
    }
    /* fmuls y*y, then fmadds x*x + that: one rounding for the sum */
    if (thr * thr <= fmaf(lx, lx, ly * ly)) {
        return false;
    }
    return ftUcf_FlickSq(port) > 0xF04;
}

/* UCF 0.84 "Shield SDI" (hook 0x80093294 in ftCo_80093240): the same idea on
 * the X axis alone. Last frame's X is compared signed, as in the asm. */
bool ftUcf_ShieldSdiAllowed(Fighter* fp)
{
    int port = fp->pad_port;
    if (port >= 4) {
        return false;
    }
    if (fp->active_sticky.lstick.x > 1) {
        return false;
    }
    if (fp->input.lstick[1].x >= p_ftCommonData->sdi_min_stick_mag) {
        return false;
    }
    return ftUcf_FlickXSq(port) > 0xF04;
}

/* UCF 0.84 "Tumble" (UCF Tumble.asm, hook 0x800908F4 in ftCo_DamageFall_IASA,
 * replacing `cmpw timer, window` with `cmpwi timer, 1`). Vanilla lets a stick
 * past the x210 threshold escape tumble while its timer is under the x214
 * window. UCF tightens that: on the first frame (timer 0) it always does; on
 * the second (timer 1) only if last frame's stick was still inside the
 * threshold and the raw stick moved more than 75 units over two frames (a
 * real flick, not a drift); later frames never do. */
bool ftUcf_TumbleAllowed(Fighter* fp)
{
    int t = fp->active_timer.lstick.x;
    int port = fp->pad_port;
    if (t != 1) {
        return t < 1;
    }
    if (port >= 4) {
        return false;
    }
    if (fabsf(fp->input.lstick[1].x) >= p_ftCommonData->x210) {
        return false;
    }
    return ftUcf_FlickXSq(port) > 0x15F9;
}

/* UCF 0.84 "Shield Drop Extended" (hook 0x8009A0B8, the stick-down compare
 * inlined into ftCo_8009A080): vanilla OR the counter being above 1. */
bool ftUcf_ShieldDropHeld(Fighter* fp)
{
    int port = fp->pad_port;
    return port < 4 && ftUcf_pad[port].flag > 1;
}

int ftUcf_FlickYSq(int port)
{
    const FtUcfPad* b = &ftUcf_pad[port];
    int dy = b->hist[b->idx][1] - b->hist[(b->idx - 2) & 3][1];
    return dy * dy;
}

#endif

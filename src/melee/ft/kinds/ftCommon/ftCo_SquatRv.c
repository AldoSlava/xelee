#include "ftCo_SquatRv.h"

#include <Runtime/platform.h>

#include <melee/ft/forward.h>

#include <stdbool.h>
#include <math.h>
#include <pc/pc.h>

#include "forward.h"
#include "ftCo_AppealS.h"
#include "ftCo_Attack1.h"
#include "ftCo_Attack100.h"
#include "ftCo_AttackHi3.h"
#include "ftCo_AttackHi4.h"
#include "ftCo_AttackLw3.h"
#include "ftCo_AttackLw4.h"
#include "ftCo_AttackS3.h"
#include "ftCo_AttackS4.h"
#include "ftCo_Guard.h"
#include "ftCo_Jump.h"
#include "ftCo_Walk.h"
#include <melee/ft/fighter.h>
#include <melee/ft/ft_081B.h>
#include <melee/ft/ft_084E.h>
#include <melee/ft/ft_0892.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>

/* 0D6620 */ static void ftCo_SquatRv_Enter(Fighter_GObj* gobj);

bool ftCo_SquatRv_CheckInput(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    float threshold = p_ftCommonData->x94;
#ifdef TARGET_PC
    /* UCF 0.84 "DBOOC SquatRv Fix" (hook 0x800D65EC): on the first frame of a
     * stick at the rim the stand-up threshold drops to 0.59, so a dash back
     * out of crouch is not lost to a stick that lands just short of x94. */
    if (pc_is_ucf_enabled() && fp->active_timer.lstick.x < 1) {
        int xi = (int) fmaf(fabsf(fp->input.lstick[0].x), 80.0F, -9.99999747378752e-05F);
        int yi = (int) fmaf(fabsf(fp->input.lstick[0].y), 80.0F, -9.99999747378752e-05F);
        if ((xi + 2) * (xi + 2) + (yi + 2) * (yi + 2) > 0x1900) {
            threshold = 0.59F;
        }
    }
#endif
    if (fp->input.lstick[0].y > -threshold) {
        ftCo_SquatRv_Enter(gobj);
        return true;
    }

    return false;
}

void ftCo_SquatRv_Enter(Fighter_GObj* gobj)
{
    Fighter_ChangeMotionState(gobj, ftCo_MS_SquatRv, Ft_MF_None, 0.0F, 1.0F,
                              0.0F, NULL);
}

void ftCo_SquatRv_Anim(Fighter_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ft_8008A2BC(gobj);
    }
}

void ftCo_SquatRv_IASA(Fighter_GObj* gobj)
{
    RETURN_IF(ftCo_800D68C0(gobj));
    RETURN_IF(ftCo_Attack100_CheckInput(gobj));
    RETURN_IF(ftCo_AttackS4_CheckInput(gobj));
    RETURN_IF(ftCo_AttackHi4_CheckInput(gobj));
    RETURN_IF(ftCo_AttackLw4_CheckInput(gobj));
    RETURN_IF(ftCo_AttackS3_CheckInput(gobj));
    RETURN_IF(ftCo_AttackHi3_CheckInput(gobj));
    RETURN_IF(ftCo_AttackLw3_CheckInput(gobj));
    RETURN_IF(ftCo_Attack1_CheckInput(gobj));
    RETURN_IF(ftCo_80091A4C(gobj));
    RETURN_IF(ftCo_800DE9D8(gobj));
    RETURN_IF(ftCo_Jump_CheckInput(gobj));
    RETURN_IF(ftCo_Walk_CheckInput(gobj));
}

void ftCo_SquatRv_Phys(Fighter_GObj* gobj)
{
    ft_80084F3C(gobj);
}

void ftCo_SquatRv_Coll(Fighter_GObj* gobj)
{
    ft_80083F88(gobj);
}

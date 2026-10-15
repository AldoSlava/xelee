#ifndef MELEE_MN_MNSTAGESEL_SLIPPI_H
#define MELEE_MN_MNSTAGESEL_SLIPPI_H

/* PC only: the stage select's look for Slippi Direct's Frozen Stadium toggle
 * (mnstagesel_slippi.c). */

#include <stdbool.h>

#include <sysdolphin/baselib/forward.h>
#include <sysdolphin/baselib/jobj.h>

/* Swap the Pokemon Stadium name's caption to "Frozen" in a freshly made name. */
void mnStageSel_SlippiFrozenName(HSD_JObj* name);
/* Add the sliding "STAGE SELECT" / "[Z] ALT STAGE" banner pair to the border
 * model (its jobj and its disc descriptor); then call the frame step. */
void mnStageSel_SlippiBannerCreate(HSD_JObj* border, const HSD_Joint* border_desc);
void mnStageSel_SlippiBannerFrame(bool stadium_hovered);

#endif

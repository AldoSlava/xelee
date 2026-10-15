#ifndef MELEE_FT_FTUCF_H
#define MELEE_FT_FTUCF_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>

#ifdef TARGET_PC
/// The pad buffer UCF 0.84 keeps per port (see ftucf.c). The history is
/// simulation state: it lives in the decomp's statics, so rollback restores it.
typedef struct FtUcfPad {
    s8 hist[4][2]; ///< the last four raw (x, y) sticks, a ring
    u8 idx;        ///< index of the newest entry
    u8 flag;       ///< frames since a downward flick at the rim, 0 when none
    u8 pad[2];
} FtUcfPad;
extern FtUcfPad ftUcf_pad[4];

/// UCF "Pad Buffer + 1.0 Cardinals": record the port's raw stick and snap
/// rim cardinals to exactly 1.0. Called once per human fighter per frame.
void ftUcf_Update(Fighter* fp);

/// Squared distance between the newest raw stick and the one two frames
/// before it: the flick measure the SDI, shield SDI and tumble rules use.
int ftUcf_FlickSq(int port);
int ftUcf_FlickYSq(int port);
int ftUcf_FlickXSq(int port);

/// UCF "SDI" and "Shield SDI": an extra way into smash DI, taken when the
/// stick was flicked past the threshold (see ftucf.c). Both are only asked
/// after the vanilla timer test has already said no.
bool ftUcf_SdiAllowed(Fighter* fp);
bool ftUcf_ShieldSdiAllowed(Fighter* fp);

/// UCF "Tumble": the test for wiggling out of tumble (DamageFall) with a
/// sideways stick. It replaces the vanilla window test rather than adding to it.
bool ftUcf_TumbleAllowed(Fighter* fp);

/// UCF "Shield Drop Extended": the downward flick counter has run for more
/// than one frame, which lets a shield drop through even when the stick has
/// already drifted back above the vanilla threshold.
bool ftUcf_ShieldDropHeld(Fighter* fp);
#endif

#endif

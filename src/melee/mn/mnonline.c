#include "mnonline.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "types.h"
#include <melee/gm/gmonlinemode.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <sysdolphin/baselib/gobj.h>

#include "pc/net_match.h"
#include "pc/slippi/slp_user.h"

/* Main Menu > Online Play. Each entry selects its GM_ONLINE lobby mode. The
 * labels are drawn text (the menu's own labels are textures with no room for
 * new words), spelled as Slippi spells them. */

static const char* const online_labels[] = {
    "Slippi",
    "LAN",
};

/* <= 44 chars: that is what fits the bottom bar at mnmain.c's font size */
static const char* const online_descriptions[] = {
    "Play over the internet with Slippi.",
    "Play another player on your local network.",
};

/* Online Play > Slippi: signed out there is only Login; signed in it is
 * Unranked, Direct and Logout. */
static const char* const slippi_in_labels[] = { "Unranked", "Direct", "Logout" };
static const char* const slippi_in_descriptions[] = {
    "Play unranked matches.",
    "Play a specific person.",
    "Sign out of your Slippi account.",
};

static const char* notice;

static int slippi_built_count;

int mnOnline_SlippiCount(void)
{
    return pc_net_match_slippi_ready() ? (int) ARRAY_SIZE(slippi_in_labels) : 1;
}

void mnOnline_SlippiBuilt(void)
{
    slippi_built_count = mnOnline_SlippiCount();
}

const char* mnOnline_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_ONLINE) {
        return "Online Play";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_labels))
    {
        return online_labels[selection];
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI) {
        if (!pc_net_match_slippi_ready()) {
            return selection == 0 ? "Login" : NULL;
        }
        if (selection >= 0 && selection < (int) ARRAY_SIZE(slippi_in_labels)) {
            return slippi_in_labels[selection];
        }
    }
    return NULL;
}

const char* mnOnline_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_ONLINE) {
        return "Play against other players over the network.";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_descriptions))
    {
        return online_descriptions[selection];
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI) {
        if (!pc_net_match_slippi_ready()) {
            return selection == 0 ? "Sign in to your Slippi account." : NULL;
        }
        if (selection >= 0 && selection < (int) ARRAY_SIZE(slippi_in_descriptions)) {
            return slippi_in_descriptions[selection];
        }
    }
    return NULL;
}

const char* mnOnline_TakeNotice(void)
{
    const char* s = notice;
    notice = NULL;
    return s;
}

static void enterOnline(OnlineKind kind)
{
    MenuExitData* data = gm_GetCurrentSceneExitData();
    sfxForward();
    gmOnline_SetKind(kind);
    data->pending_mode = GM_ONLINE;
    gm_801A4B60();
}

/* Up/Down wrap over `count` entries. */
static void moveHover(u32 buttons, int count)
{
    if (buttons & MenuInput_Up) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + count - 1) % count;
    } else if (buttons & MenuInput_Down) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + 1) % count;
    }
}

/// @brief Online menu think, after mn_8022D594: Slippi or LAN
void mnOnline_Think(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        switch (mn_804A04F0.hovered_selection) {
        case SEL_ONLINE_SLIPPI:
            sfxForward();
            mn_80229894(MENU_KIND_ONLINE_SLIPPI, SEL_SLIPPI_UNRANKED, 1);
            break;
        case SEL_ONLINE_LAN:
            enterOnline(ONLINE_KIND_LAN);
            break;
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_MAIN, SEL_MAIN_ONLINE, 3);
    } else {
        moveHover(buttons, (int) ARRAY_SIZE(online_labels));
    }
}

/// @brief Online Play > Slippi think: Login, or the Slippi modes and Logout
void mnOnline_SlippiThink(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);
    bool signed_in = pc_net_match_slippi_ready();

    mn_804A04F0.buttons = buttons;
    if (mnOnline_SlippiCount() != slippi_built_count) {
        /* The account changed under the menu (a sign-out, or a sign-in that
         * finished): build it again with the rows that now apply. */
        mn_80229894(MENU_KIND_ONLINE_SLIPPI, 0, 1);
        return;
    }
    if (buttons & MenuInput_Confirm) {
        if (!signed_in) {
            enterOnline(ONLINE_KIND_LOGIN);
            return;
        }
        switch (mn_804A04F0.hovered_selection) {
        case SEL_SLIPPI_UNRANKED:
            enterOnline(ONLINE_KIND_UNRANKED);
            break;
        case SEL_SLIPPI_DIRECT:
            enterOnline(ONLINE_KIND_DIRECT);
            break;
        case SEL_SLIPPI_LOGOUT:
            sfxForward();
            pc_slippi_account_forget();
            pc_net_match_slippi_refresh();
            break; /* the account check above rebuilds the menu next frame */
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_ONLINE, SEL_ONLINE_SLIPPI, 3);
    } else {
        moveHover(buttons, mnOnline_SlippiCount());
    }
}

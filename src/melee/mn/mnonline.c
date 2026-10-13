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

/* VS Mode > Online. Each entry selects its GM_ONLINE lobby mode. */

static const char* const online_labels[] = {
    "SLIPPI",
    "LAN",
};

static const char* const slippi_labels[] = {
    "UNRANKED", "DIRECT CONNECT",
};

/* <= 44 chars: that is what fits the bottom bar at mnmain.c's font size */
static const char* const online_descriptions[] = {
    "Play over the internet with Slippi.",
    "Play another player on your local network.",
};

static const char* const slippi_descriptions[] = {
    "Find an opponent over the internet.",
    "Connect to a friend using a connect code.",
};

static const char* notice;

const char* mnOnline_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "ONLINE";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_labels))
    {
        return online_labels[selection];
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI && selection >= 0 &&
        selection < (int) ARRAY_SIZE(slippi_labels))
    {
        return slippi_labels[selection];
    }
    return NULL;
}

const char* mnOnline_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "Play against other players over the network.";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_descriptions))
    {
        return online_descriptions[selection];
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI && selection >= 0 &&
        selection < (int) ARRAY_SIZE(slippi_descriptions))
    {
        return slippi_descriptions[selection];
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
        mn_80229894(MENU_KIND_VS, SEL_VS_ONLINE, 3);
    } else {
        moveHover(buttons, (int) ARRAY_SIZE(online_labels));
    }
}

/// @brief Online > Slippi think: the Slippi modes
void mnOnline_SlippiThink(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        switch (mn_804A04F0.hovered_selection) {
        case SEL_SLIPPI_UNRANKED:
            enterOnline(ONLINE_KIND_UNRANKED);
            break;
        case SEL_SLIPPI_DIRECT:
            enterOnline(ONLINE_KIND_DIRECT);
            break;
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_ONLINE, SEL_ONLINE_SLIPPI, 3);
    } else {
        moveHover(buttons, (int) ARRAY_SIZE(slippi_labels));
    }
}

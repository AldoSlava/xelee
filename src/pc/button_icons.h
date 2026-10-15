/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GameCube button icons for the in-game hints (SmashWiki, CC BY-SA 4.0; see
 * assets/button_icons/README.md). */
#ifndef PC_BUTTON_ICONS_H
#define PC_BUTTON_ICONS_H

#ifdef __cplusplus
extern "C" {
#endif

enum PcButtonIcon { PC_BUTTON_ICON_L, PC_BUTTON_ICON_R, PC_BUTTON_ICON_Y, PC_BUTTON_ICON_Z,
                    PC_BUTTON_ICON_COUNT };

extern const unsigned char* const pc_button_icon_data[PC_BUTTON_ICON_COUNT];

/* Draws icon `which` as a w x h quad centred on (x, y, z) in the current
 * camera's space (the caller has set the view matrix, as the menus' own
 * overlay draws do). */
void pc_button_icon_draw(int which, float x, float y, float z, float w, float h);

#ifdef __cplusplus
}
#endif

#endif

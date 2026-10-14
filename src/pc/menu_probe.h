/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MELEE_MENU_PROBE=1: logs each camera's projection and, per GObj render
 * callback, how deep the joints it draws sit in that camera's view space.
 * Measuring tool for the menu window-with-parallax plan; no effect otherwise. */
#ifndef PC_MENU_PROBE_H
#define PC_MENU_PROBE_H

#ifdef TARGET_PC
struct HSD_CObj;
struct HSD_JObj;
struct HSD_GObj;
void pc_menu_probe_camera(struct HSD_CObj* cobj);
/* HSD_GObj_JObjCallback is drawing a GObj of link `arg` (begin), or is done. */
void pc_menu_probe_gobj(struct HSD_GObj* gobj, long arg, int begin);
void pc_menu_probe_jobj(struct HSD_JObj* jobj);
#else
#define pc_menu_probe_camera(c) ((void) 0)
#define pc_menu_probe_gobj(r, a, b) ((void) 0)
#define pc_menu_probe_jobj(j) ((void) 0)
#endif

#endif

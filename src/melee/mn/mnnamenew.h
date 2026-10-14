#ifndef MELEE_MN_NAME_NEW_H
#define MELEE_MN_NAME_NEW_H

#include <sysdolphin/baselib/forward.h>

#include <placeholder.h>

#include <melee/mn/types.h>

/* 23B0F8 */ void mnNameNew_8023B0F8(HSD_GObj* arg0, u8 arg1);
/* 23B224 */ void mnNameNew_8023B224(u8);
/* 23B314 */ void mnNameNew_8023B314(NameNewEntry*, s32);
/* 23B3FC */ HSD_Text* mnNameNew_KeySetup(NameNewEntry*, u8);
/* 23BAA8 */ s32 mnNameNew_8023BAA8(NameNewEntry* arg0, s32 arg1, u8 arg2);
/* 23BD4C */ s32 PickAutoName(HSD_GObj* arg0);
/* 23BEE0 */ bool NameContainsOnlySpaces(void);
/* 23BFE4 */ s32 WriteCharactersForNameAtIndex(u8 arg0, s32 arg1);
/* 23C148 */ char* AddCharacterToName(char* arg0, u8 arg1, u8 arg2, u8 arg3);
/* 23C290 */ void mnNameNew_GlyphVariantInput(HSD_GObj*);
/* 23C54C */ void mnNameNew_MainInput(HSD_GObj*);
/* 23CE4C */ void mnNameNew_8023CE4C(void);
/* 23CFC8 */ void fn_8023CFC8(HSD_GObj* arg0);
/* 23D0F8 */ void fn_8023D0F8(void*);
/* 23D130 */ HSD_Text* mnNameNew_8023D130(GlyphVariantEntry* arg0, u16 arg1,
                                          u8 arg2, s32 arg3);
/* 23D3E8 */ HSD_GObj* mnNameNew_GlyphVariantSetup(NameNewEntry* arg0,
                                                   u16 arg1, s32 arg2);
/* 23DA08 */ s32 mnNameNew_8023DA08(NameNewEntry*);
/* 23DAEC */ void fn_8023DAEC(HSD_GObj*);
/* 23DBE8 */ void fn_8023DBE8(HSD_GObj*);
/* 23E0D8 */ void mnNameNew_8023E0D8(NameNewEntry* arg0);
/* 23E264 */ s32 InitNameEntryUIState(NameNewEntry*, s32);
/* 23E32C */ void mnNameNew_8023E32C(s32);
/* 23E630 */ void mnNameNew_EnterFromMnName(int count);
/* 23E6D0 */ void mnNameNew_EnterFromMnCharSel(HSD_Archive*, s32);
/* 23EA08 */ void mnNameNew_8023EA08(UNK_T);
#ifdef TARGET_PC
/* Slippi's keyboard: the Name Entry screen with ASCII text in it, opened from
 * a scene that has MnExtAll loaded (as the CSS does). `done` is called when the
 * entry ends: `confirmed` false for Back; returning false from a confirm keeps
 * the screen up (the text was refused). The text is `max_len` characters at
 * most and masked with '*' when `mask`. */
typedef bool (*MnNameNewDoneFn)(bool confirmed, const char* text, void* user);
/* Y fills the entry with what this returns (a code from the clipboard or the
 * last opponent), or does nothing when it returns NULL. */
typedef const char* (*MnNameNewFillFn)(void* user);
/* `css`: the screen is opened from the CSS, so on leaving it the CSS is built
 * again (as it is when a nametag entry ends) before `done` runs. */
/* `shiftable`: the keys are lowercase and L/R (Shift) capitalises them, as for
 * an email or password; otherwise they are capitals only, as for a code. */
void mnNameNew_PcOpen(HSD_Archive* ext_all, const char* initial, int max_len, bool mask,
                      bool shiftable, const char* description, MnNameNewDoneFn done,
                      MnNameNewFillFn fill, bool css, void* user);
bool mnNameNew_PcActive(void);
#endif
/* 3EE720 */ extern char* mnNameNew_803EE720[];
/* 3EE724 */ extern char* mnNameNew_803EE724[];

#endif

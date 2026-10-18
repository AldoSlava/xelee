/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube's fused single-precision ops, for game code that has to
 * compute what the disc's code computes (a Slippi peer runs that in
 * Dolphin). mwcc fused most a*b + c in the original into one fmadds,
 * fmsubs, fnmadds or fnmsubs, rounded once where our build, which never
 * contracts (-ffp-contract=off, CMakeLists.txt), rounds twice: a ULP apart,
 * which a fight then grows (tools/slp_compare.py finds where). A site the
 * DOL shows fused is written with these. Argument order is the
 * instruction's: a*c, then b. They are aurora's (dolphin/ppc_math.h), which
 * computes them as Dolphin does, in double and then rounded to single, so
 * the SDK's vector code and the game's agree. */
#ifndef PC_GEKKO_FP_H
#define PC_GEKKO_FP_H

#include <dolphin/ppc_math.h>

#define gk_fmadds ppc_fmadds   /* a*c + b */
#define gk_fmsubs ppc_fmsubs   /* a*c - b */
#define gk_fnmadds ppc_fnmadds /* -(a*c + b) */
#define gk_fnmsubs ppc_fnmsubs /* -(a*c - b), that is b - a*c */

#endif

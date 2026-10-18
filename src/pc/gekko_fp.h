/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube's fused ops, for game code that has to compute what the
 * disc's code computes (a Slippi peer runs that in Dolphin). mwcc fused
 * most a*b + c in the original into one fmadds, fmsubs, fnmadds or fnmsubs
 * (fmadd and the rest in double), rounded once where our build, which
 * never contracts (-ffp-contract=off, CMakeLists.txt), rounds twice: a ULP
 * apart, which a fight then grows (tools/slp_compare.py finds where). A
 * site the DOL shows fused is written with these (tools/fuse_sites.py
 * finds and rewrites them). Argument order is the instruction's: a*c, then
 * b.
 *
 * On PC they are aurora's (dolphin/ppc_math.h), computed as Dolphin does:
 * a single-precision one in double and then rounded to single, a double
 * one as one fma. Under mwcc they are the plain expressions it fuses by
 * itself, so the source still builds as the disc's code. */
#ifndef PC_GEKKO_FP_H
#define PC_GEKKO_FP_H

#ifdef __MWERKS__
#define gk_fmadds(a, c, b) ((a) * (c) + (b))
#define gk_fmsubs(a, c, b) ((a) * (c) - (b))
#define gk_fnmadds(a, c, b) (-((a) * (c) + (b)))
#define gk_fnmsubs(a, c, b) ((b) - (a) * (c))
#define gk_fmadd gk_fmadds
#define gk_fmsub gk_fmsubs
#define gk_fnmadd gk_fnmadds
#define gk_fnmsub gk_fnmsubs
#else
#include <dolphin/ppc_math.h>

#define gk_fmadds ppc_fmadds   /* a*c + b */
#define gk_fmsubs ppc_fmsubs   /* a*c - b */
#define gk_fnmadds ppc_fnmadds /* -(a*c + b) */
#define gk_fnmsubs ppc_fnmsubs /* -(a*c - b), computed as b - a*c */
#define gk_fmadd ppc_fmadd     /* the same in double */
#define gk_fmsub ppc_fmsub
#define gk_fnmadd ppc_fnmadd
#define gk_fnmsub ppc_fnmsub
#endif

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube's own trig, bit for bit, on every platform (see pc_trig.h).
 *
 * sinf, cosf and tanf are Metrowerks MSL's (the decomp's src/MSL/trigf.c,
 * math_1.c), atanf is the game's own (src/melee/lb/lbtrigf.c, which only
 * builds it for the GameCube): Melee calls these, and a Slippi peer runs
 * them in Dolphin. A correctly rounded libm is not the same function:
 * cosf(float(-3pi/4)) is bf3504f4 here, bf3504f3 in musl, and a wavedash on
 * the down-back diagonal came out one ULP apart from Dolphin's.
 *
 * The code follows the machine code in the disc's DOL (sinf 0x803263D4,
 * cosf 0x80326240, tanf 0x803261BC, atanf 0x80022E68), not just the C:
 * mwcc fused every a*b + c there into one fmadds (or fnmsubs, fnmadds),
 * which the madds() family below reproduces, and every table and constant
 * is the DOL's bits. Each Gekko single-precision op is computed as Dolphin
 * computes it: in double, then rounded to single. Its inputs are singles,
 * so a product is exact in double; a fused op's sum is rounded to double,
 * then to single (Dolphin's fmadds: an fma in double, then ForceSingle).
 * The plain single ops (fmuls, fadds, fsubs, fdivs) give the same bits as
 * float arithmetic. Needs FLT_EVAL_METHOD 0 and no contraction
 * (-ffp-contract=off), as the rest of the simulation does. */
#include "pc_libm.h"
#include "pc_trig.h"
#include "pc/gekko_fp.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#undef sinf
#undef cosf
#undef tanf
#undef atanf

#define madds gk_fmadds
#define nmsubs gk_fnmsubs
#define nmadds gk_fnmadds

static inline uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}
static inline float from_bits(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* A table of float bit patterns, read as floats. */
typedef union {
    uint32_t u;
    float f;
} F32;

/* MSL trigf.c's __four_over_pi_m1 (as __sinit_trigf_c sets it from
 * tmp_float, 0x803B8F40): 4/pi - 1 in four parts. */
static const F32 k_four_over_pi_m1[4] = {{0x3e800000}, {0x3cbe6080}, {0x34372200}, {0x2da44152}};
/* math_data.c, 0x803B9358 and 0x803B9378. */
static const F32 k_quadrant[8] = {{0x00000000}, {0x3f800000}, {0x3f800000}, {0x00000000},
                                  {0x00000000}, {0xbf800000}, {0xbf800000}, {0x00000000}};
static const F32 k_poly[10] = {{0x366ccfaa}, {0x34a5e129}, {0xb9aae275}, {0xb8196543}, {0x3c81e0ed},
                               {0x3b2335dd}, {0xbe9de9e6}, {0xbda55de7}, {0x3f800000}, {0x3f490fdb}};
#define TWO_OVER_PI from_bits(0x3f22f983) /* r2-6216 */
#define HALF from_bits(0x3f000000)        /* r2-6212 */
#define EPSILON from_bits(0x39b504f3)     /* r2-6208, __epsilon */

#define Q(i) k_quadrant[i].f
#define P(i) k_poly[i].f

/* Both functions' argument reduction: y in quarter turns, n the quadrant. */
static inline float reduce(float x, int* n) {
    const float z = TWO_OVER_PI * x;
    /* fadds/fsubs, then fctiwz: toward zero */
    *n = (bits(x) & 0x80000000u) ? (int)(z - HALF) : (int)(z + HALF);
    float y = x - (float)(*n * 2);
    y = madds(k_four_over_pi_m1[0].f, x, y);
    y = madds(k_four_over_pi_m1[1].f, x, y);
    y = madds(k_four_over_pi_m1[2].f, x, y);
    y = madds(k_four_over_pi_m1[3].f, x, y);
    *n &= 3;
    return y;
}

float pc_sinf(float x) {
    int n;
    const float y = reduce(x, &n);
    if (fabsf(y) < EPSILON) {
        n <<= 1;
        return madds(P(9), Q(n + 1) * y, Q(n));
    }
    const float ysq = y * y;
    float z;
    if (n & 1) {
        n <<= 1;
        z = madds(P(0), ysq, P(2));
        z = madds(ysq, z, P(4));
        z = madds(ysq, z, P(6));
        z = madds(ysq, z, P(8));
        return z * Q(n);
    }
    n <<= 1;
    z = madds(P(1), ysq, P(3));
    z = madds(ysq, z, P(5));
    z = madds(ysq, z, P(7));
    z = madds(ysq, z, P(9));
    z = y * z;
    return z * Q(n + 1);
}

float pc_cosf(float x) {
    int n;
    const float y = reduce(x, &n);
    if (fabsf(y) < EPSILON) {
        n <<= 1;
        return nmsubs(y, Q(n), Q(n + 1)); /* q[n+1] - y*q[n] */
    }
    const float ysq = y * y;
    float z;
    if (n & 1) {
        n <<= 1;
        z = madds(P(1), ysq, P(3));
        z = madds(ysq, z, P(5));
        z = madds(ysq, z, P(7));
        z = nmadds(ysq, z, P(9));
        z = y * z;
        return z * Q(n);
    }
    n <<= 1;
    z = madds(P(0), ysq, P(2));
    z = madds(ysq, z, P(4));
    z = madds(ysq, z, P(6));
    z = madds(ysq, z, P(8));
    return z * Q(n + 1);
}

float pc_tanf(float x) {
    const float c = pc_cosf(x); /* tanf calls cos__Ff first */
    const float s = pc_sinf(x);
    return s / c;
}

/* lbtrigf.c's atanf_lookup, 0x803B7300. */
static const F32 k_atan[46] = {
    {0x3f800000}, {0xbeaaaaaa}, {0x3e4ccc81}, {0xbe123e7d}, {0x3de21f95}, {0xbdad417c}, {0x3d41186d},
    {0x40da826b}, {0x404f5958}, {0x40000000}, {0x3fb925ab}, {0x3f95f61a}, {0x3f851081}, {0x36ef692f},
    {0x355c1df9}, {0x00000000}, {0x35291d45}, {0x00000000}, {0x00000000}, {0x00000000}, {0x3ec90eaa},
    {0x3f16cbe4}, {0x3f490fda}, {0x3f7b53c5}, {0x3f96cbe2}, {0x3fafedd9}, {0x00000000}, {0x37185d99},
    {0x32c59189}, {0x33874a9e}, {0x353cfa83}, {0x348637bd}, {0x35541063}, {0x401a8277}, {0x3fbf90c7},
    {0x3f800000}, {0x3f2b0dc1}, {0x3ed413cd}, {0x3e4bafaf}, {0x3516dc59}, {0x00000000}, {0x00000000},
    {0x00000000}, {0x00000000}, {0x00000000}, {0x00000000},
};
#define L(i) k_atan[i].f
#define SILVER_RATIO from_bits(0x401a827a)      /* r2-31760 */
#define SILVER_CONJUGATE from_bits(0x3ed413cd)  /* r2-31756 */
#define ONE from_bits(0x3f800000)               /* r2-31772 */
#define HALF_PI from_bits(0x3fc90fdb)           /* r2-31776 */

float pc_atanf(float x) {
    const uint32_t sign = bits(x) & 0x80000000u;
    x = from_bits(bits(x) & 0x7fffffffu);
    const int32_t bx = (int32_t)bits(x);
    int idx = -1; /* the table row; -1 reads the zero entries below each column */
    bool past_ratio = false;
    float r;
    if (x >= SILVER_RATIO) {
        past_ratio = true;
        r = ONE / x;
    } else if (SILVER_CONJUGATE < x) {
        idx = 0;
        switch ((uint32_t)bx & 0x7f800000u) {
        case 0x3f000000u: /* [0.5, 1) */
            if (!(bx < 0x3f08d5b9)) {
                idx = 1;
            }
            if (!(bx < 0x3f521801)) {
                idx += 1;
            }
            break;
        case 0x3f800000u: /* [1, 2) */
            idx = 2;
            if (!(bx < 0x3f9bf7ec)) {
                idx = 3;
            }
            if (!(bx < 0x3fef789e)) {
                idx += 1;
            }
            break;
        case 0x40000000u: /* [2, the silver ratio) */
            idx = 4;
            break;
        }
        const float o39 = L(idx + 39), o33 = L(idx + 33);
        r = ONE / (o33 + (x + o39));
        r = nmsubs(r, L(idx + 7), o33) + nmsubs(r, L(idx + 13), o39);
    } else {
        r = x;
    }
    const float r2 = r * r;
    const float r3 = r * r2;
    float p = madds(r2, L(6), L(5));
    p = madds(r2, p, L(4));
    p = madds(r2, p, L(3));
    p = madds(r2, p, L(2));
    p = madds(r2, p, L(1));
    r = madds(r3, p, r);
    r += L(idx + 27);
    r += L(idx + 20);
    if (past_ratio) {
        r -= HALF_PI;
        return sign ? r : -r;
    }
    return from_bits(bits(r) | sign);
}

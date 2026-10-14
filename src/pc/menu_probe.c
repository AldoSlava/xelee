/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/menu_probe.h"

#include "pc/pc.h"

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define MAX_CAMS 8
#define MAX_LINKS 96
#define STABLE_FRAMES 90

typedef struct {
    int n;
    float vz[2], wz[2], vx[2], vy[2]; /* min, max: view z, world z, view x / y */
    double vz_sum;
} Bucket;

typedef struct {
    HSD_CObj* cobj;
    int type;
    float p[4], near, far, eye[3], interest[3], vp[4];
    Bucket link[MAX_LINKS];
    HSD_GObj* link_gobj[MAX_LINKS];
    long link_arg[MAX_LINKS];
    int link_info[MAX_LINKS][4]; /* classifier, p_link, gx_link, render_priority */
    float link_root[MAX_LINKS][3];
    int links, seen;
} Cam;

static Cam s_cam[MAX_CAMS];
static int s_cams, s_cur = -1, s_link = -1;
static uint32_t s_sig;
static int s_stable, s_frames;
static int64_t s_last_ns;
static int s_on = -1;

static bool on(void)
{
    if (s_on < 0) {
        const char* e = getenv("MELEE_MENU_PROBE");
        s_on = e != NULL && e[0] != '0';
    }
    return s_on;
}

static void fold(float* mm, float v, int first)
{
    if (first || v < mm[0]) {
        mm[0] = v;
    }
    if (first || v > mm[1]) {
        mm[1] = v;
    }
}

static void dump(void)
{
    pc_log_line("probe: ---- %d cameras ----", s_cams);
    for (int c = 0; c < s_cams; c++) {
        const Cam* m = &s_cam[c];
        if (m->type == 1) { /* perspective */
            pc_log_line("probe: cam %d persp fov %.2f aspect %.3f near %.1f far %.1f eye (%.1f %.1f %.1f) "
                        "interest (%.1f %.1f %.1f) vp %.0f,%.0f-%.0f,%.0f",
                        c, m->p[0], m->p[1], m->near, m->far, m->eye[0], m->eye[1], m->eye[2], m->interest[0],
                        m->interest[1], m->interest[2], m->vp[0], m->vp[1], m->vp[2], m->vp[3]);
        } else {
            pc_log_line("probe: cam %d type %d box t%.1f b%.1f l%.1f r%.1f near %.1f far %.1f eye (%.1f %.1f %.1f) "
                        "interest (%.1f %.1f %.1f) vp %.0f,%.0f-%.0f,%.0f",
                        c, m->type, m->p[0], m->p[1], m->p[2], m->p[3], m->near, m->far, m->eye[0], m->eye[1],
                        m->eye[2], m->interest[0], m->interest[1], m->interest[2], m->vp[0], m->vp[1], m->vp[2],
                        m->vp[3]);
        }
        for (int l = 0; l < m->links; l++) {
            const Bucket* b = &m->link[l];
            if (b->n == 0) {
                continue;
            }
            pc_log_line("probe:   gobj cls %d plink %d gx %d prio %d pass %ld root world (%.1f %.1f %.1f)",
                        m->link_info[l][0], m->link_info[l][1], m->link_info[l][2], m->link_info[l][3],
                        m->link_arg[l], m->link_root[l][0], m->link_root[l][1], m->link_root[l][2]);
            pc_log_line("probe:       n %4d  view z %8.1f .. %8.1f (mean %8.1f)  world z %8.1f .. %8.1f  "
                        "view x %7.1f .. %7.1f  y %7.1f .. %7.1f",
                        b->n, b->vz[0], b->vz[1], b->vz_sum / b->n, b->wz[0], b->wz[1], b->vx[0],
                        b->vx[1], b->vy[0], b->vy[1]);
        }
    }
}

/* A frame ends when the cameras fall quiet for a few ms. A scene is the same
 * cameras for STABLE_FRAMES frames; a new one forgets the old cameras. */
static void frame_check(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = (int64_t) ts.tv_sec * 1000000000ll + ts.tv_nsec;
    const bool gap = now - s_last_ns > 4000000;
    s_last_ns = now;
    if (!gap) {
        return;
    }
    uint32_t sig = 2166136261u;
    int kept = 0;
    for (int k = 0; k < s_cams; k++) {
        if (s_cam[k].seen == s_frames) {
            sig = (sig ^ (uint32_t) (uintptr_t) s_cam[k].cobj) * 16777619u;
            kept++;
        }
    }
    if (sig != s_sig) {
        int w = 0;
        for (int k = 0; k < s_cams; k++) {
            if (s_cam[k].seen == s_frames) {
                s_cam[w] = s_cam[k];
                for (int l = 0; l < MAX_LINKS; l++) {
                    s_cam[w].link[l].n = 0;
                }
                s_cam[w].links = 0;
                w++;
            }
        }
        s_cams = w;
        s_sig = sig;
        s_stable = 0;
    } else if (++s_stable % STABLE_FRAMES == 0) {
        dump();
        for (int k = 0; k < s_cams; k++) {
            for (int l = 0; l < MAX_LINKS; l++) {
                s_cam[k].link[l].n = 0;
            }
            s_cam[k].links = 0;
        }
    }
    s_frames++;
}

void pc_menu_probe_camera(HSD_CObj* cobj)
{
    if (!on() || cobj == NULL) {
        return;
    }
    frame_check();
    int c;
    for (c = 0; c < s_cams && s_cam[c].cobj != cobj; c++) {
    }
    if (c == s_cams) {
        if (s_cams == MAX_CAMS) {
            s_cur = -1;
            return;
        }
        s_cam[s_cams++] = (Cam) {.cobj = cobj};
    }
    Cam* m = &s_cam[c];
    m->seen = s_frames;
    s_cur = c;
    s_link = -1;
    m->type = cobj->projection_type;
    m->near = cobj->near;
    m->far = cobj->far;
    if (m->type == 1) {
        m->p[0] = cobj->projection_param.perspective.fov;
        m->p[1] = cobj->projection_param.perspective.aspect;
    } else {
        m->p[0] = cobj->projection_param.ortho.top;
        m->p[1] = cobj->projection_param.ortho.bottom;
        m->p[2] = cobj->projection_param.ortho.left;
        m->p[3] = cobj->projection_param.ortho.right;
    }
    Vec3 e = {0}, i = {0};
    HSD_CObjGetEyePosition(cobj, &e);
    HSD_CObjGetInterest(cobj, &i);
    m->eye[0] = e.x, m->eye[1] = e.y, m->eye[2] = e.z;
    m->interest[0] = i.x, m->interest[1] = i.y, m->interest[2] = i.z;
    m->vp[0] = cobj->viewport.xmin, m->vp[1] = cobj->viewport.ymin;
    m->vp[2] = cobj->viewport.xmax, m->vp[3] = cobj->viewport.ymax;
}

void pc_menu_probe_gobj(HSD_GObj* gobj, long arg, int begin)
{
    if (!on() || s_cur < 0) {
        return;
    }
    if (!begin) {
        s_link = -1;
        return;
    }
    Cam* m = &s_cam[s_cur];
    int l;
    for (l = 0; l < m->links && m->link_gobj[l] != gobj; l++) {
    }
    if (l == m->links) {
        if (l == MAX_LINKS) {
            return;
        }
        m->links++;
        m->link_gobj[l] = gobj;
        m->link_arg[l] = arg;
        m->link_info[l][0] = gobj->classifier;
        m->link_info[l][1] = gobj->p_link;
        m->link_info[l][2] = gobj->gx_link;
        m->link_info[l][3] = gobj->render_priority;
        m->link[l].n = 0;
    }
    s_link = l;
}

void pc_menu_probe_jobj(HSD_JObj* jobj)
{
    if (!on() || s_cur < 0 || s_link < 0) {
        return;
    }
    Cam* m = &s_cam[s_cur];
    Bucket* b = &m->link[s_link];
    HSD_JObjSetupMatrix(jobj); /* the draw does this next; mtx is world space after it */
    const float* v = (const float*) HSD_CObjGetViewingMtxPtrDirect(m->cobj);
    const float wx = jobj->mtx[0][3], wy = jobj->mtx[1][3], wz = jobj->mtx[2][3];
    const float vx = v[0] * wx + v[1] * wy + v[2] * wz + v[3];
    const float vy = v[4] * wx + v[5] * wy + v[6] * wz + v[7];
    const float vz = v[8] * wx + v[9] * wy + v[10] * wz + v[11];
    const int first = b->n == 0;
    if (first) {
        m->link_root[s_link][0] = wx;
        m->link_root[s_link][1] = wy;
        m->link_root[s_link][2] = wz;
    }
    fold(b->vz, vz, first);
    fold(b->wz, wz, first);
    fold(b->vx, vx, first);
    fold(b->vy, vy, first);
    b->vz_sum += vz;
    b->n++;
}

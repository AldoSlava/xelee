/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Native-only pieces of src/pc that the browser platform replaces.
 *
 * The desktop launcher (launcher.cpp), the libusb GameCube adapter
 * (gcadapter.c) and the archive file cache (file_cache.cpp) have no browser
 * counterpart: settings come from the hosting page through the environment,
 * pads arrive through SDL, and disc reads are cached by
 * disc-cache.mjs. Everything else in src/pc is compiled unchanged.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/file_cache.h"
#include "pc/pc.h"
#include "pc/slp.h"
#include "pc/slippi/slp_match.h"
#include "pc/slippi/slp_user.h"

/* Desktop launcher preferences; defaults match launcher_data.hpp. MELEE_UCF is
 * the one the launcher also reads from the environment (launcher.cpp). */
bool pc_is_unlock_all_enabled(void) {
    return false;
}
bool pc_is_frozen_stadium_enabled(void) {
    return false;
}
bool pc_is_free_camera_enabled(void) {
    return false;
}
bool pc_is_ucf_enabled(void) {
    const char* env = getenv("MELEE_UCF");
    return env != NULL && env[0] != '0';
}
int pc_get_hud_mode(void) {
    return 0;
}
bool pc_is_custom_textures_enabled(void) {
    return false;
}
/* Netplay lobby identity and Direct Connect settings (net_lan.c, net_match.c); the lobby itself is
 * unreachable here. */
const char* pc_get_net_name(void) {
    return "";
}
const char* pc_get_net_target(void) {
    return "";
}
void pc_set_net_target(const char* code) {
    (void)code;
}
int pc_get_net_port(void) {
    return 0;
}
uint64_t pc_install_id(void) {
    return 0;
}
const char* pc_app_rev(void) {
    return "browser";
}

/* libusb GameCube adapter. */
void pc_gcadapter_init(void) {}
void pc_gcadapter_poll(void) {}
void pc_gcadapter_apply(void) {}
bool pc_gcadapter_status(int port, struct PADStatus* out) {
    (void)port;
    (void)out;
    return false;
}
bool pc_gcadapter_raw(int port, uint8_t raw[6], bool* wireless) {
    (void)port;
    (void)raw;
    (void)wireless;
    return false;
}
uint64_t pc_gcadapter_report_count(void) {
    return 0;
}
void pc_gcadapter_motor(int slot, unsigned command) {
    (void)slot;
    (void)command;
}

/* Archive cache and background prewarm: the page owns disc caching. */
bool pc_file_cache_get(const char* filename, void* dst, size_t* size) {
    (void)filename;
    (void)dst;
    (void)size;
    return false;
}
bool pc_file_cache_get_size(const char* filename, size_t* size) {
    (void)filename;
    (void)size;
    return false;
}
void pc_file_cache_put(const char* filename, const void* data, size_t size) {
    (void)filename;
    (void)data;
    (void)size;
}
void pc_file_cache_start_prewarm(void) {}
bool pc_file_cache_require(const char* filename) {
    (void)filename;
    return false;
}

/* Replay recording writes to a native file; the browser has no recorder. */
void pc_slp_match_start(const struct StartMeleeData* data) {
    (void)data;
}
void pc_slp_match_end(void) {}
void pc_slp_tick_begin(void) {}
void pc_slp_tick_end(uint64_t proc_mask) {
    (void)proc_mask;
}
void pc_slp_pre_frame(struct HSD_GObj* gobj) {
    (void)gobj;
}
void pc_slp_sync_rng(uint32_t seed) {
    (void)seed;
}
/* Nor replay playback, which reads one. */
bool pc_slp_play_active(void) {
    return false;
}
uint32_t pc_slp_play_rng_offset(void) {
    return 0;
}
bool pc_slp_play_frozen_stadium(bool* frozen) {
    (void)frozen;
    return false;
}
void pc_slp_play_setup(struct StartMeleeData* start) {
    (void)start;
}
void pc_slp_play_match_start(void) {}
void pc_slp_play_feed(struct PADStatus* head) {
    (void)head;
}
void pc_slp_play_match_end(void) {}
void pc_slp_play_tick_end(bool fight_frame) {
    (void)fight_frame;
}
/* Nor its L-cancel stats. */
void pc_slp_l_cancel(struct HSD_GObj* gobj, bool in_window) {
    (void)gobj;
    (void)in_window;
}

/* Gameplay toggles from the launcher; both on by default there
 * (launcher_data.hpp), with the same environment overrides. */
bool pc_is_slippi_fixes_enabled(void) {
    const char* env = getenv("MELEE_SLIPPI_FIXES");
    return env == NULL || env[0] != '0';
}
bool pc_is_cstick_1p_enabled(void) {
    const char* env = getenv("MELEE_CSTICK_1P");
    return env == NULL || env[0] != '0';
}

/* Expanded stage sounds (stage_ambience.c, stage_mix.c) are compiled out. */
void pc_stage_mix_begin(int rate) {
    (void)rate;
}
void pc_ambience_frame(void) {}
void pc_ambience_tick(uint64_t proc_mask) {
    (void)proc_mask;
}
bool pc_ambience_kongo_jungle(void) {
    return false;
}
void pc_ambience_set_duck(float gain) {
    (void)gain;
}

/* Slippi accounts and matchmaking (slp_user.c, slp_match.c) need HTTPS and
 * UDP: signed out, no login, and no search ever starts. */
int pc_match_local_port(void) {
    return 0;
}
const char* pc_slippi_user_dir(void) {
    return "";
}
bool pc_slippi_account_load(PcSlippiAccount* out, bool rediscover) {
    (void)rediscover;
    memset(out, 0, sizeof *out);
    return false;
}
void pc_slippi_account_forget(void) {}
bool pc_slippi_account_login_available(void) {
    return false;
}
bool pc_slippi_account_login(const char* email, const char* password, PcSlippiAccount* out,
                             char* err, size_t err_cap) {
    (void)email;
    (void)password;
    (void)out;
    snprintf(err, err_cap, "Online login is not available in the browser");
    return false;
}
bool pc_slippi_connect_code_valid(const char* code) {
    (void)code;
    return false;
}
bool pc_slippi_connect_code_normalize(const char* in, char* out, size_t cap) {
    (void)in;
    if (cap > 0) {
        out[0] = '\0';
    }
    return false;
}
PcSlippiMatch* pc_slippi_match_start(const PcSlippiAccount* account, int mode,
                                     const char* connect_code, uint16_t local_port) {
    (void)account;
    (void)mode;
    (void)connect_code;
    (void)local_port;
    return NULL;
}
void pc_slippi_match_stop(PcSlippiMatch* m) {
    (void)m;
}
PcSlippiMatchState pc_slippi_match_state(const PcSlippiMatch* m) {
    (void)m;
    return PC_SLIPPI_MATCH_ERROR;
}
const char* pc_slippi_match_error(const PcSlippiMatch* m) {
    (void)m;
    return "Online play is not available in the browser";
}
bool pc_slippi_match_result(const PcSlippiMatch* m, PcSlippiTicket* out) {
    (void)m;
    (void)out;
    return false;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi account identity (slp_user.h). The JSON parsing and serialization are
 * pure; discovery, load, save and the REST refresh are the I/O edges. */
#include "pc/slippi/slp_user.h"

#include "pc/slippi/slp_json.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---------------------------------------------------------- */

static void copy_field(char* dst, size_t cap, const char* src) {
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* True when the account has the fields a Slippi session needs. connectCode is
 * what the servers key on for Direct; a login without one cannot play. */
static bool account_usable(const PcSlippiAccount* a) {
    return a->uid[0] != '\0' || a->connect_code[0] != '\0' || a->play_key[0] != '\0';
}

void pc_slippi_account_default_chat(PcSlippiAccount* a) {
    static const char* const defaults[PC_SLIPPI_CHAT_MESSAGES] = {
        "ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
        "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later",
        "bad connection",
    };
    for (int i = 0; i < PC_SLIPPI_CHAT_MESSAGES; i++) {
        copy_field(a->chat_messages[i], sizeof a->chat_messages[i], defaults[i]);
    }
}

static void account_clear(PcSlippiAccount* a) {
    memset(a, 0, sizeof *a);
    pc_slippi_account_default_chat(a);
}

/* Fill the chat slots from a JSON array of strings, or the defaults when it is
 * missing or not exactly 16 entries (as Slippi's sanitize does). */
static void read_chat(const PcSlippiJson* obj, PcSlippiAccount* out) {
    const PcSlippiJson* cm = pc_slippi_json_get(obj, "chatMessages");
    if (!pc_slippi_json_is_array(cm) || cm->child_count != PC_SLIPPI_CHAT_MESSAGES) {
        pc_slippi_account_default_chat(out);
        return;
    }
    for (int i = 0; i < PC_SLIPPI_CHAT_MESSAGES; i++) {
        const PcSlippiJson* e = pc_slippi_json_at(cm, i);
        if (!pc_slippi_json_string(e, out->chat_messages[i], sizeof out->chat_messages[i])) {
            pc_slippi_account_default_chat(out);
            return;
        }
    }
}

/* A short decoded-string helper: decode `key` from `obj` into a fixed buffer,
 * falling back to empty. */
static void get_str(const PcSlippiJson* obj, const char* key, char* out, size_t cap) {
    if (!pc_slippi_json_string(pc_slippi_json_get(obj, key), out, cap)) {
        if (cap > 0) {
            out[0] = '\0';
        }
    }
}

bool pc_slippi_account_from_json(const char* text, size_t len, PcSlippiAccount* out) {
    account_clear(out);
    if (text == NULL) {
        return false;
    }
    PcSlippiJson* root = NULL;
    char err[64];
    if (!pc_slippi_json_parse(text, len, &root, err, sizeof err)) {
        return false;
    }
    if (root->type != PC_SLIPPI_JSON_OBJECT) {
        pc_slippi_json_free(root);
        return false;
    }
    get_str(root, "uid", out->uid, sizeof out->uid);
    get_str(root, "playKey", out->play_key, sizeof out->play_key);
    get_str(root, "displayName", out->display_name, sizeof out->display_name);
    get_str(root, "connectCode", out->connect_code, sizeof out->connect_code);
    get_str(root, "latestVersion", out->latest_version, sizeof out->latest_version);
    read_chat(root, out);
    pc_slippi_json_free(root);
    out->logged_in = account_usable(out);
    return out->logged_in;
}

/* Escape a string into a JSON literal, returning false when it does not fit.
 * Account fields are short and ASCII in practice; control bytes are escaped so
 * a display name can never break the file. */
static bool json_escape(const char* s, char* out, size_t cap, size_t* w) {
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; p++) {
        char esc[8];
        const char* add = NULL;
        switch (*p) {
        case '"': add = "\\\""; break;
        case '\\': add = "\\\\"; break;
        case '\n': add = "\\n"; break;
        case '\r': add = "\\r"; break;
        case '\t': add = "\\t"; break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                add = esc;
            }
            break;
        }
        if (add == NULL) {
            if (*w + 1 >= cap) {
                return false;
            }
            out[(*w)++] = (char)*p;
        } else {
            size_t n = strlen(add);
            if (*w + n >= cap) {
                return false;
            }
            memcpy(out + *w, add, n);
            *w += n;
        }
    }
    return true;
}

size_t pc_slippi_account_to_json(const PcSlippiAccount* a, char* out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t w = 0;
    bool ok = true;
/* One `"key":"value",` member, escaping the value. */
#define MEMBER(key, field)                                                                    \
    do {                                                                                     \
        int n = snprintf(out + w, cap - w, "\"" key "\":");                                  \
        if (n < 0 || (size_t)n >= cap - w) {                                                 \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        w += (size_t)n;                                                                      \
        if (w + 1 >= cap) {                                                                  \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        out[w++] = '"';                                                                      \
        if (!json_escape((field), out, cap - 1, &w)) {                                       \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        if (w + 2 >= cap) {                                                                  \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        out[w++] = '"';                                                                      \
        out[w++] = ',';                                                                      \
    } while (0)
    if (w + 1 >= cap) {
        ok = false;
    } else {
        out[w++] = '{';
    }
    if (ok) MEMBER("uid", a->uid);
    if (ok) MEMBER("playKey", a->play_key);
    if (ok) MEMBER("displayName", a->display_name);
    if (ok) MEMBER("connectCode", a->connect_code);
    if (ok) MEMBER("latestVersion", a->latest_version);
    if (ok) {
        /* chatMessages: an array of 16 strings. Slippi's shape. */
        int n = snprintf(out + w, cap - w, "\"chatMessages\":[");
        if (n < 0 || (size_t)n >= cap - w) {
            ok = false;
        } else {
            w += (size_t)n;
        }
    }
    for (int i = 0; ok && i < PC_SLIPPI_CHAT_MESSAGES; i++) {
        if (w + 1 >= cap) {
            ok = false;
            break;
        }
        out[w++] = '"';
        if (!json_escape(a->chat_messages[i], out, cap - 1, &w)) {
            ok = false;
            break;
        }
        if (w + 2 >= cap) {
            ok = false;
            break;
        }
        out[w++] = '"';
        out[w++] = (i + 1 < PC_SLIPPI_CHAT_MESSAGES) ? ',' : ']';
    }
    if (ok && w + 1 < cap) {
        out[w++] = '}';
        out[w] = '\0';
    } else {
        ok = false;
    }
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return w;
#undef MEMBER
}

/* ---- directory / store ------------------------------------------------ */

const char* pc_slippi_user_dir(void) {
    static char dir[1024];
    static bool done;
    if (!done) {
        done = true;
        const char* override = getenv("MELEE_SLIPPI_DIR");
        if (override != NULL && override[0] != '\0') {
            snprintf(dir, sizeof dir, "%s", override);
        } else {
            char* pref = SDL_GetPrefPath(NULL, "melee-pc");
            if (pref != NULL) {
                snprintf(dir, sizeof dir, "%sSlippi", pref);
                SDL_free(pref);
            } else {
                snprintf(dir, sizeof dir, "Slippi");
            }
        }
    }
    return dir;
}

static bool read_file(const char* path, char* out, size_t cap, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    *len = n;
    return true;
}

bool pc_slippi_account_load(PcSlippiAccount* out, bool rediscover) {
    account_clear(out);
    char path[1200];
    snprintf(path, sizeof path, "%s/slippi-account.json", pc_slippi_user_dir());
    char buf[16384];
    size_t len = 0;
    if (read_file(path, buf, sizeof buf, &len) && pc_slippi_account_from_json(buf, len, out)) {
        return true;
    }
    if (rediscover) {
        return pc_slippi_account_discover(out);
    }
    return false;
}

bool pc_slippi_account_save(const PcSlippiAccount* a) {
    const char* dir = pc_slippi_user_dir();
    SDL_CreateDirectory(dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/slippi-account.json", dir);
    char buf[32768];
    size_t n = pc_slippi_account_to_json(a, buf, sizeof buf);
    if (n == 0) {
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(buf, 1, n, f) == n;
    fclose(f);
    return ok;
}

void pc_slippi_account_forget(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/slippi-account.json", pc_slippi_user_dir());
    remove(path);
}

/* ---- Launcher discovery (desktop convenience) ------------------------- */

/* Try one candidate user.json path. Returns true and fills `out` on success. */
static bool try_user_json(const char* path, PcSlippiAccount* out) {
    char buf[16384];
    size_t len = 0;
    if (!read_file(path, buf, sizeof buf, &len)) {
        return false;
    }
    return pc_slippi_account_from_json(buf, len, out);
}

/* The documented Launcher layout under a root: <root>/<build>/User/Slippi/
 * user.json. `build` names the Dolphin folder the Launcher installs. */
static bool try_root(const char* root, PcSlippiAccount* out) {
    if (root == NULL || root[0] == '\0') {
        return false;
    }
    static const char* const builds[] = {"netplay", "netplay-beta", "playback"};
    char path[1400];
    for (size_t i = 0; i < sizeof builds / sizeof builds[0]; i++) {
        snprintf(path, sizeof path, "%s/%s/User/Slippi/user.json", root, builds[i]);
        if (try_user_json(path, out)) {
            return true;
        }
    }
    return false;
}

bool pc_slippi_account_discover(PcSlippiAccount* out) {
    account_clear(out);
    /* A user.json dropped directly in our own directory wins: it is explicit
     * and needs no Launcher. */
    char own[1200];
    snprintf(own, sizeof own, "%s/user.json", pc_slippi_user_dir());
    if (try_user_json(own, out)) {
        return true;
    }
    /* Standard Launcher roots. APPDATA/LOCALAPPDATA on Windows; on Linux and
     * macOS the Launcher uses a Slippi Launcher folder under the platform's
     * app-data dir. Everything is best-effort and silently absent when there
     * is no Launcher (Quest, a fresh desktop). */
    const char* roots[8];
    int n = 0;
    const char* appdata = getenv("APPDATA");
    const char* localappdata = getenv("LOCALAPPDATA");
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    char win1[1200], win2[1200], lin[1200], mac[1200];
    if (appdata != NULL && appdata[0] != '\0') {
        snprintf(win1, sizeof win1, "%s/Slippi Launcher", appdata);
        roots[n++] = win1;
    }
    if (localappdata != NULL && localappdata[0] != '\0') {
        snprintf(win2, sizeof win2, "%s/Slippi Launcher", localappdata);
        roots[n++] = win2;
    }
    if (home != NULL && home[0] != '\0') {
        snprintf(lin, sizeof lin, "%s/.config/Slippi Launcher", home);
        roots[n++] = lin;
        snprintf(mac, sizeof mac, "%s/Library/Application Support/Slippi Launcher", home);
        roots[n++] = mac;
    }
    if (xdg != NULL && xdg[0] != '\0') {
        char x[1200];
        snprintf(x, sizeof x, "%s/Slippi Launcher", xdg);
        roots[n++] = x;
    }
    for (int i = 0; i < n; i++) {
        if (try_root(roots[i], out)) {
            return true;
        }
    }
    return false;
}

/* ---- REST refresh ----------------------------------------------------- */

typedef struct RefreshArgs {
    PcSlippiAccount snapshot;
} RefreshArgs;

static int refresh_thread(void* user) {
    RefreshArgs* args = (RefreshArgs*)user;
    const char* uid = args->snapshot.uid;
    if (uid[0] == '\0') {
        free(args);
        return 0;
    }
    char url[256];
    snprintf(url, sizeof url, "https://users.slippi.gg/user/%s", uid);
    char* body = (char*)malloc(16384);
    if (body == NULL) {
        free(args);
        return 0;
    }
    int status = 0;
    if (pc_slippi_http_get(url, NULL, body, 16384, &status) && status == 200) {
        /* Patch the display name / connect code / latest version / chat from
         * the server's copy; the stored file's are what we keep on failure. A
         * name changed on the website is the point of this refresh. */
        PcSlippiJson* root = NULL;
        char err[64];
        if (pc_slippi_json_parse(body, strlen(body), &root, err, sizeof err)) {
            PcSlippiAccount* fresh = (PcSlippiAccount*)malloc(sizeof *fresh);
            if (fresh != NULL) {
                *fresh = args->snapshot;
                char name[PC_SLIPPI_DISPLAY_NAME_MAX];
                char code[PC_SLIPPI_CONNECT_CODE_MAX];
                char latest[32];
                if (pc_slippi_json_string(pc_slippi_json_get(root, "displayName"), name,
                                          sizeof name)
                    && name[0] != '\0') {
                    copy_field(fresh->display_name, sizeof fresh->display_name, name);
                }
                if (pc_slippi_json_string(pc_slippi_json_get(root, "connectCode"), code,
                                          sizeof code)
                    && code[0] != '\0') {
                    copy_field(fresh->connect_code, sizeof fresh->connect_code, code);
                }
                if (pc_slippi_json_string(pc_slippi_json_get(root, "latestVersion"), latest,
                                          sizeof latest)
                    && latest[0] != '\0') {
                    copy_field(fresh->latest_version, sizeof fresh->latest_version, latest);
                }
                const PcSlippiJson* cm = pc_slippi_json_get(root, "chatMessages");
                if (pc_slippi_json_is_array(cm) && cm->child_count == PC_SLIPPI_CHAT_MESSAGES) {
                    for (int i = 0; i < PC_SLIPPI_CHAT_MESSAGES; i++) {
                        pc_slippi_json_string(pc_slippi_json_at(cm, i), fresh->chat_messages[i],
                                              sizeof fresh->chat_messages[i]);
                    }
                }
                pc_slippi_account_save(fresh);
                free(fresh);
            }
            pc_slippi_json_free(root);
        }
    }
    free(body);
    free(args);
    return 0;
}

void pc_slippi_account_refresh_async(const PcSlippiAccount* a) {
    if (a == NULL || a->uid[0] == '\0') {
        return;
    }
    /* Local test peering must never touch Slippi's servers. */
    const char* local = getenv("MELEE_SLIPPI_LOCAL_PEER");
    if (local != NULL && local[0] == '1') {
        return;
    }
    RefreshArgs* args = (RefreshArgs*)malloc(sizeof *args);
    if (args == NULL) {
        return;
    }
    args->snapshot = *a;
    SDL_Thread* t = SDL_CreateThread(refresh_thread, "slippi refresh", args);
    if (t == NULL) {
        free(args);
        return;
    }
    SDL_DetachThread(t);
}

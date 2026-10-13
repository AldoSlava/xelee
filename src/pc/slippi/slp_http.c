/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small HTTP GET/POST for the Slippi account refresh and login flow
 * (slp_user.h). Built on libcurl where the platform has it (desktop, and the
 * same dependency the updater already uses); on a platform without it the
 * calls report failure, which the account refresh treats as "keep the stored
 * file". This is not on the per-frame path. */
#include "pc/slippi/slp_user.h"

#include <stdlib.h>
#include <string.h>

#if defined(MELEE_USE_CURL) && defined(_WIN32)
#include <windows.h>
#endif

#if defined(MELEE_USE_CURL)
#include <curl/curl.h>

typedef struct Sink {
    char* out;
    size_t cap;
    size_t len;
    bool overflow;
} Sink;

static size_t write_cb(char* data, size_t size, size_t nmemb, void* user) {
    Sink* s = (Sink*)user;
    size_t n = size * nmemb;
    if (s->len + n >= s->cap) {
        /* Copy what fits (leaving room for the NUL) and flag the overflow; the
         * caller sees a truncated body rather than a crash. */
        size_t room = s->cap > s->len + 1 ? s->cap - s->len - 1 : 0;
        if (room > 0) {
            memcpy(s->out + s->len, data, room);
            s->len += room;
        }
        s->out[s->len] = '\0';
        s->overflow = true;
        return n; /* claim it was consumed so curl does not abort */
    }
    memcpy(s->out + s->len, data, n);
    s->len += n;
    s->out[s->len] = '\0';
    return n;
}

static bool ensure_global(void) {
    static bool done;
    static bool ok;
    if (!done) {
        done = true;
        ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }
    return ok;
}

static bool http_request(const char* url, const char* body, const char* bearer, bool post, char* out,
                         size_t cap, int* status) {
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    if (url == NULL || out == NULL || cap == 0 || !ensure_global()) {
        return false;
    }
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        return false;
    }
    Sink sink = {out, cap, 0, false};
    struct curl_slist* headers = NULL;
    /* A Slippi ID token is a JWT, commonly over 1 KB, so this must not be a
     * small fixed buffer that silently drops the header and fails the login. */
    char* auth = NULL;
    if (bearer != NULL && bearer[0] != '\0') {
        static const char kPrefix[] = "Authorization: Bearer ";
        size_t need = strlen(bearer) + sizeof kPrefix;
        auth = (char*)malloc(need);
        if (auth == NULL) {
            curl_easy_cleanup(curl);
            return false;
        }
        snprintf(auth, need, "%s%s", kPrefix, bearer);
        headers = curl_slist_append(headers, auth);
    }
    if (post) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "melee-pc Slippi");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (headers != NULL) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (post && body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status != NULL) {
        *status = (int)code;
    }
    if (headers != NULL) {
        curl_slist_free_all(headers);
    }
    free(auth);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status) {
    return http_request(url, NULL, bearer, false, out, cap, status);
}

bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status) {
    return http_request(url, body, bearer, true, out, cap, status);
}

#else /* no curl: the refresh is unavailable, everything else still works */

bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status) {
    (void)url;
    (void)bearer;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    return false;
}

bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status) {
    (void)url;
    (void)body;
    (void)bearer;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    return false;
}

#endif

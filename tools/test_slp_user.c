/* SPDX-License-Identifier: GPL-2.0-or-later */
/* slp_user (M4): Slippi account parsing and serialization.
 *
 * The account fields come from untrusted JSON (a discovered user.json, or our
 * own store written by a login flow), and a wrong read means the wrong connect
 * code or playKey on the wire. So this pins the parse of a real user.json
 * shape, the defaults for missing/incorrect fields, the 16-message chat rule,
 * a round-trip through our writer, and that hostile input is refused rather
 * than accepted with garbage fields.
 *
 * The writer is pure and is #included, so the HTTP edge (slp_http.c) is stubbed
 * here; nothing in this test touches the network. */
#include "../src/pc/slippi/slp_json.c"
#include "../src/pc/slippi/slp_user.c"

#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

/* Stubs: the refresh's HTTP edge is not exercised here. */
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

static void test_parse_real(void) {
    const char* text =
        "{\"uid\":\"abc123\",\"playKey\":\"KEY-xyz\",\"displayName\":\"Player One\","
        "\"connectCode\":\"PLYR#123\",\"latestVersion\":\"3.6.4\","
        "\"chatMessages\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\","
        "\"m\",\"n\",\"o\",\"p\"]}";
    PcSlippiAccount a;
    assert(pc_slippi_account_from_json(text, strlen(text), &a));
    assert(a.logged_in);
    assert(strcmp(a.uid, "abc123") == 0);
    assert(strcmp(a.play_key, "KEY-xyz") == 0);
    assert(strcmp(a.display_name, "Player One") == 0);
    assert(strcmp(a.connect_code, "PLYR#123") == 0);
    assert(strcmp(a.latest_version, "3.6.4") == 0);
    assert(strcmp(a.chat_messages[0], "a") == 0);
    assert(strcmp(a.chat_messages[15], "p") == 0);
}

static void test_defaults(void) {
    /* No chatMessages -> Slippi defaults; still usable if uid present. */
    PcSlippiAccount a;
    assert(pc_slippi_account_from_json("{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"C#1\"}",
                                       strlen("{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"C#1\"}"),
                                       &a));
    assert(strcmp(a.chat_messages[0], "ggs") == 0);
    assert(strcmp(a.chat_messages[15], "bad connection") == 0);
    /* Wrong-size chat array also falls back. */
    PcSlippiAccount b;
    assert(pc_slippi_account_from_json("{\"uid\":\"u\",\"chatMessages\":[\"only\"]}",
                                       strlen("{\"uid\":\"u\",\"chatMessages\":[\"only\"]}"), &b));
    assert(strcmp(b.chat_messages[0], "ggs") == 0);
}

static void test_empty_is_logged_out(void) {
    PcSlippiAccount a;
    assert(!pc_slippi_account_from_json("{}", 2, &a));
    assert(!a.logged_in);
    assert(a.uid[0] == '\0');
    /* Not an object at all. */
    assert(!pc_slippi_account_from_json("[]", 2, &a));
    assert(!pc_slippi_account_from_json("not json", 8, &a));
    assert(!pc_slippi_account_from_json("", 0, &a));
}

static void test_round_trip(void) {
    PcSlippiAccount a;
    memset(&a, 0, sizeof a);
    strcpy(a.uid, "u1");
    strcpy(a.play_key, "k1");
    strcpy(a.display_name, "Name \"quoted\"");
    strcpy(a.connect_code, "ME#1");
    strcpy(a.latest_version, "3.6.4");
    pc_slippi_account_default_chat(&a);
    a.logged_in = true;
    char buf[8192];
    size_t n = pc_slippi_account_to_json(&a, buf, sizeof buf);
    assert(n > 0);
    PcSlippiAccount b;
    assert(pc_slippi_account_from_json(buf, n, &b));
    assert(strcmp(b.uid, "u1") == 0);
    assert(strcmp(b.play_key, "k1") == 0);
    assert(strcmp(b.display_name, "Name \"quoted\"") == 0);
    assert(strcmp(b.connect_code, "ME#1") == 0);
    assert(strcmp(b.chat_messages[2], "brb") == 0);
    /* A buffer too small must fail, not emit a partial file. */
    char tiny[16];
    assert(pc_slippi_account_to_json(&a, tiny, sizeof tiny) == 0);
    assert(tiny[0] == '\0');
}

static void test_hostile(void) {
    PcSlippiAccount a;
    /* A very long display name is truncated, not overflowed. */
    char big[4096];
    int w = snprintf(big, sizeof big, "{\"uid\":\"u\",\"displayName\":\"");
    for (int i = 0; i < 1000; i++) {
        big[w++] = 'x';
    }
    w += snprintf(big + w, sizeof big - w, "\"}");
    assert(pc_slippi_account_from_json(big, (size_t)w, &a));
    /* An over-long field is dropped whole (the JSON reader never returns a
     * truncated string as success), so it is empty, not a 31-byte prefix. */
    assert(a.display_name[0] == '\0');
    assert(strcmp(a.uid, "u") == 0);
    /* A bad JSON type for a string field leaves it empty, not garbage. */
    assert(!pc_slippi_account_from_json("{\"uid\":123}", strlen("{\"uid\":123}"), &a));
    assert(a.uid[0] == '\0');
}

static void test_login_unconfigured(void) {
    /* No FIREBASE_KEY / GRAPHQL_ENDPOINT in the test env: login must refuse
     * cleanly, not attempt a request, and say why. */
    unsetenv("MELEE_SLIPPI_FIREBASE_KEY");
    unsetenv("MELEE_SLIPPI_GRAPHQL_ENDPOINT");
    assert(!pc_slippi_account_login_available());
    PcSlippiAccount a;
    char err[128];
    assert(!pc_slippi_account_login("a@b.c", "pw", &a, err, sizeof err));
    assert(err[0] != '\0');
    /* Empty credentials are refused too, once configured. */
    setenv("MELEE_SLIPPI_FIREBASE_KEY", "k", 1);
    setenv("MELEE_SLIPPI_GRAPHQL_ENDPOINT", "https://example.test/gql", 1);
    assert(pc_slippi_account_login_available());
    assert(!pc_slippi_account_login("", "", &a, err, sizeof err));
    assert(strstr(err, "email") != NULL);
    /* Configured, but the HTTP edge is stubbed to fail: a clean failure with a
     * message, never a crash. */
    assert(!pc_slippi_account_login("a@b.c", "pw", &a, err, sizeof err));
    assert(err[0] != '\0');
    unsetenv("MELEE_SLIPPI_FIREBASE_KEY");
    unsetenv("MELEE_SLIPPI_GRAPHQL_ENDPOINT");
}

/* The Linux netplay AppImage keeps user.json under ~/.config/SlippiOnline,
 * outside the Launcher's folder; discovery has to look there. */
static void test_discover_appimage_dir(void) {
    char root[] = "/tmp/slp_user_test_XXXXXX";
    assert(mkdtemp(root) != NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/.config", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/SlippiOnline", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/SlippiOnline/Slippi", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/SlippiOnline/Slippi/user.json", root);
    FILE* f = fopen(path, "w");
    assert(f != NULL);
    fputs("{\"uid\":\"u1\",\"playKey\":\"k\",\"connectCode\":\"ABC#123\","
          "\"displayName\":\"abc\"}", f);
    fclose(f);
    setenv("HOME", root, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("MELEE_SLIPPI_DIR");
    PcSlippiAccount a;
    assert(pc_slippi_account_discover(&a));
    assert(strcmp(a.connect_code, "ABC#123") == 0);
    remove(path);
}

static void test_connect_codes(void) {
    char out[32];
    assert(pc_slippi_connect_code_valid("MERC#931"));
    assert(pc_slippi_connect_code_valid("A#1"));
    assert(pc_slippi_connect_code_valid("ABCDEFG#999"));
    assert(pc_slippi_connect_code_valid("A1B2#07"));
    assert(!pc_slippi_connect_code_valid("ABCDEFGH#1")); /* tag too long */
    assert(!pc_slippi_connect_code_valid("#123"));       /* no tag */
    assert(!pc_slippi_connect_code_valid("ABC#"));       /* no digits */
    assert(!pc_slippi_connect_code_valid("ABC#1234"));   /* too many digits */
    assert(!pc_slippi_connect_code_valid("ABC#12A"));    /* letters after the hash */
    assert(!pc_slippi_connect_code_valid("abc#123"));    /* lower case */
    assert(!pc_slippi_connect_code_valid("AB-C#123"));
    assert(!pc_slippi_connect_code_valid("ABC123"));
    assert(!pc_slippi_connect_code_valid("A#1#2"));
    assert(!pc_slippi_connect_code_valid(""));
    assert(!pc_slippi_connect_code_valid(NULL));
    assert(pc_slippi_connect_code_normalize("  merc#931\n", out, sizeof out));
    assert(strcmp(out, "MERC#931") == 0);
    assert(pc_slippi_connect_code_normalize("merc#931 add me!", out, sizeof out));
    assert(strcmp(out, "MERC#931") == 0);
    assert(!pc_slippi_connect_code_normalize("hello world", out, sizeof out));
    assert(!pc_slippi_connect_code_normalize(NULL, out, sizeof out));
    assert(out[0] == '\0');
}

int main(void) {
    test_connect_codes();
    test_discover_appimage_dir();
    test_parse_real();
    test_defaults();
    test_empty_is_logged_out();
    test_round_trip();
    test_hostile();
    test_login_unconfigured();
    printf("slp_user: all tests passed\n");
    return 0;
}

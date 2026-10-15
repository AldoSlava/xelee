/* SPDX-License-Identifier: GPL-2.0-or-later */
/* slp_json: the minimal JSON reader used for Slippi matchmaking and account
 * files. No network, no game: parse a set of real and hostile inputs and check
 * the tree, the typed accessors, string decoding, and that malformed input
 * fails without leaking or crashing. */
#include "../src/pc/slippi/slp_json.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static PcSlippiJson* parse_ok(const char* text) {
    PcSlippiJson* root = NULL;
    char err[128];
    if (!pc_slippi_json_parse(text, strlen(text), &root, err, sizeof err)) {
        fprintf(stderr, "parse failed on %s: %s\n", text, err);
        assert(0);
    }
    return root;
}

static void parse_fails(const char* text) {
    PcSlippiJson* root = NULL;
    char err[128];
    bool ok = pc_slippi_json_parse(text, strlen(text), &root, err, sizeof err);
    if (ok) {
        fprintf(stderr, "expected failure on: %s\n", text);
        pc_slippi_json_free(root);
        assert(0);
    }
    assert(root == NULL);
}

static void test_basics(void) {
    PcSlippiJson* root = parse_ok("{\"a\":1,\"b\":\"two\",\"c\":true,\"d\":null,\"e\":[1,2,3]}");
    assert(root->type == PC_SLIPPI_JSON_OBJECT);
    assert(pc_slippi_json_int(pc_slippi_json_get(root, "a"), -1) == 1);
    assert(pc_slippi_json_int(pc_slippi_json_get(root, "missing"), -1) == -1);
    assert(pc_slippi_json_bool(pc_slippi_json_get(root, "c"), false) == true);
    assert(pc_slippi_json_is_null(pc_slippi_json_get(root, "d")));
    assert(!pc_slippi_json_is_null(pc_slippi_json_get(root, "a")));
    const PcSlippiJson* e = pc_slippi_json_get(root, "e");
    assert(pc_slippi_json_is_array(e));
    assert(e->child_count == 3);
    assert(pc_slippi_json_int(pc_slippi_json_at(e, 0), -1) == 1);
    assert(pc_slippi_json_int(pc_slippi_json_at(e, 2), -1) == 3);
    assert(pc_slippi_json_at(e, 3) == NULL);
    /* A string accessor returns the default when the type is wrong. */
    assert(strcmp(pc_slippi_json_str(pc_slippi_json_get(root, "a"), "x"), "x") == 0);
    pc_slippi_json_free(root);
}

static void test_string_decode(void) {
    PcSlippiJson* root = parse_ok("{\"s\":\"a\\\"b\\\\c\\/d\\ne\\u0041\\u00e9\"}");
    char out[64];
    const PcSlippiJson* s = pc_slippi_json_get(root, "s");
    bool ok = pc_slippi_json_string(s, out, sizeof out);
    assert(ok);
    /* a " b \ c / d newline e A é (U+00E9 as UTF-8: C3 A9) */
    const char* want = "a\"b\\c/d\neA\xc3\xa9";
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "decode mismatch: got '%s'\n", out);
        assert(0);
    }
    /* A too-small buffer is a failure, not a truncated string. */
    char small[4];
    assert(!pc_slippi_json_string(s, small, sizeof small));
    assert(small[0] == '\0');
    /* A non-string node fails and writes "". */
    assert(!pc_slippi_json_string(pc_slippi_json_get(root, "missing"), out, sizeof out));
    pc_slippi_json_free(root);
}

static void test_surrogate_pair(void) {
    /* U+1F600 GRINNING FACE = \uD83D\uDE00 -> F0 9F 98 80 */
    PcSlippiJson* root = parse_ok("{\"s\":\"\\uD83D\\uDE00\"}");
    char out[16];
    assert(pc_slippi_json_string(pc_slippi_json_get(root, "s"), out, sizeof out));
    assert(memcmp(out, "\xf0\x9f\x98\x80", 4) == 0);
    pc_slippi_json_free(root);
}

static void test_numbers(void) {
    PcSlippiJson* root = parse_ok("{\"i\":42,\"neg\":-7,\"f\":1.5,\"e\":1e3,\"e2\":-2.5E-2}");
    assert(pc_slippi_json_int(pc_slippi_json_get(root, "i"), -1) == 42);
    assert(pc_slippi_json_int(pc_slippi_json_get(root, "neg"), 0) == -7);
    assert(pc_slippi_json_num(pc_slippi_json_get(root, "f"), 0) == 1.5);
    assert(pc_slippi_json_num(pc_slippi_json_get(root, "e"), 0) == 1000.0);
    assert(pc_slippi_json_num(pc_slippi_json_get(root, "e2"), 0) == -0.025);
    pc_slippi_json_free(root);
}

static void test_real_ticket_shape(void) {
    /* The shape of a Slippi get-ticket-resp (subset). */
    const char* text =
        "{\"type\":\"get-ticket-resp\",\"matchId\":\"mode.direct-abc123\","
        "\"isHost\":true,\"players\":["
        "{\"uid\":\"u1\",\"displayName\":\"Me\",\"connectCode\":\"ME#1\",\"port\":1,"
        "\"isLocalPlayer\":true,\"ipAddress\":\"1.2.3.4:41000\",\"ipAddressLan\":\"10.0.0.2:41000\"},"
        "{\"uid\":\"u2\",\"displayName\":\"Them\",\"connectCode\":\"TH#2\",\"port\":2,"
        "\"isLocalPlayer\":false,\"isBot\":false,\"ipAddress\":\"5.6.7.8:41001\","
        "\"ipAddressLan\":\"10.0.0.3:41001\",\"rank\":{\"rating\":1500.0}}"
        "],\"stages\":[3,8,28],\"items\":0}";
    PcSlippiJson* root = parse_ok(text);
    char buf[64];
    assert(pc_slippi_json_string(pc_slippi_json_get(root, "type"), buf, sizeof buf));
    assert(strcmp(buf, "get-ticket-resp") == 0);
    assert(pc_slippi_json_bool(pc_slippi_json_get(root, "isHost"), false) == true);
    const PcSlippiJson* players = pc_slippi_json_get(root, "players");
    assert(players->child_count == 2);
    const PcSlippiJson* p0 = pc_slippi_json_at(players, 0);
    assert(pc_slippi_json_bool(pc_slippi_json_get(p0, "isLocalPlayer"), false));
    assert(pc_slippi_json_int(pc_slippi_json_get(p0, "port"), 0) == 1);
    assert(pc_slippi_json_string(pc_slippi_json_get(p0, "displayName"), buf, sizeof buf));
    assert(strcmp(buf, "Me") == 0);
    const PcSlippiJson* p1 = pc_slippi_json_at(players, 1);
    assert(pc_slippi_json_int(pc_slippi_json_get(p1, "port"), 0) == 2);
    assert(pc_slippi_json_bool(pc_slippi_json_get(p1, "isLocalPlayer"), true) == false);
    const PcSlippiJson* rank = pc_slippi_json_get(p1, "rank");
    assert(pc_slippi_json_num(pc_slippi_json_get(rank, "rating"), 0) == 1500.0);
    const PcSlippiJson* stages = pc_slippi_json_get(root, "stages");
    assert(stages->child_count == 3);
    assert(pc_slippi_json_int(pc_slippi_json_at(stages, 2), -1) == 28);
    pc_slippi_json_free(root);
}

static void test_hostile(void) {
    parse_fails("");
    parse_fails("   ");
    parse_fails("{");
    parse_fails("[1,2");
    parse_fails("{\"a\":}");
    parse_fails("{\"a\" 1}");
    parse_fails("{\"a\":1,}");
    parse_fails("[1,]");
    parse_fails("tru");
    parse_fails("nul");
    parse_fails("\"unterminated");
    parse_fails("{\"a\":1}garbage");
    parse_fails("\x01");
    /* Deep nesting is bounded, not a stack overflow. */
    {
        char deep[4096];
        size_t i = 0;
        for (; i < 200; i++) {
            deep[i] = '[';
        }
        for (size_t j = 0; j < 200; j++) {
            deep[i + j] = ']';
        }
        deep[i + 200] = '\0';
        parse_fails(deep);
    }
}

int main(void) {
    test_basics();
    test_string_decode();
    test_surrogate_pair();
    test_numbers();
    test_real_ticket_shape();
    test_hostile();
    printf("slp_json: all tests passed\n");
    return 0;
}

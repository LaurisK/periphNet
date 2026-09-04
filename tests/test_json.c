/*
 * test_json.c — Shared/Json (docs/task_json_module.md).
 *
 * The writer's whole reason for existing is that it must not overrun, so the
 * tests that matter run it against a buffer with guard bytes either side and
 * assert the guards are untouched.  Truncation is expected and fine; a write
 * past the end is the defect.
 *
 * The reader's addition over the three tokenizers it replaces is escapes, so
 * that is where the reject matrix is densest — including the round trip that
 * closes only if both halves agree.
 */

#include "json.h"
#include "test_util.h"

#include <stdlib.h>

/* Guard-band harness: the payload sits inside a poisoned block, so any write
 * outside [cap) shows up as a changed guard byte rather than as luck. */
#define GUARD           16u
#define GUARD_BYTE      0xA5u

typedef struct {
    uint8_t *block;
    char    *buf;
    size_t   cap;
} sGuarded;

static void guarded_new(sGuarded *g, size_t cap)
{
    g->block = (uint8_t *)malloc(cap + (2u * GUARD));
    memset(g->block, GUARD_BYTE, cap + (2u * GUARD));
    g->buf = (char *)(g->block + GUARD);
    g->cap = cap;
}

static int guarded_intact(const sGuarded *g)
{
    uint32_t i;

    for (i = 0u; i < GUARD; i++) {
        if (g->block[i] != GUARD_BYTE) {
            return 0;
        }
        if (g->block[GUARD + g->cap + i] != GUARD_BYTE) {
            return 0;
        }
    }
    return 1;
}

static void guarded_free(sGuarded *g)
{
    free(g->block);
    g->block = NULL;
}

/* ==========================================================================
 * Writer
 * ========================================================================== */

static void test_cat_appends_and_reports_the_position(void)
{
    sGuarded g;
    size_t   pos = 0u;

    guarded_new(&g, 64u);
    pos = Json_Cat(g.buf, g.cap, pos, "{\"a\":%d", 12);
    pos = Json_Cat(g.buf, g.cap, pos, ",\"b\":\"%s\"}", "hi");

    TEST_ASSERT(0 == strcmp(g.buf, "{\"a\":12,\"b\":\"hi\"}"));
    TEST_ASSERT(pos == strlen(g.buf));
    TEST_ASSERT(guarded_intact(&g));
    guarded_free(&g);
}

/* THE defect this module exists for.  The old idiom accumulated snprintf()'s
 * return -- the length it WOULD have written -- so `cap - pos` underflowed to
 * ~4 GB and the next append wrote past the end.  Here the appends keep coming
 * long after the buffer is full. */
static void test_cat_saturates_instead_of_underflowing(void)
{
    sGuarded g;
    size_t   pos = 0u;
    int      i;

    guarded_new(&g, 32u);
    for (i = 0; i < 200; i++) {
        pos = Json_Cat(g.buf, g.cap, pos, "%s", "0123456789");
        TEST_ASSERT(pos <= g.cap);
    }
    TEST_ASSERT(pos == g.cap);              /* the "did not fit" signal      */
    TEST_ASSERT(strlen(g.buf) < g.cap);
    TEST_ASSERT(guarded_intact(&g));
    guarded_free(&g);
}

/* The roll-back idiom the HTTP handlers use: an element that did not fit is
 * removed entirely, so what remains is still parseable. */
static void test_saturation_supports_whole_elements_only(void)
{
    sGuarded g;
    size_t   pos = 0u;
    int      i;

    guarded_new(&g, 40u);
    pos = Json_Cat(g.buf, g.cap - 2u, pos, "[");
    for (i = 0; i < 20; i++) {
        const size_t mark = pos;

        pos = Json_Cat(g.buf, g.cap - 2u, pos, "%s{\"i\":%d}",
                       (i == 0) ? "" : ",", i);
        if (pos >= (g.cap - 2u)) {
            pos = mark;
            g.buf[pos] = '\0';
            break;
        }
    }
    pos = Json_Cat(g.buf, g.cap, pos, "]");

    TEST_ASSERT('[' == g.buf[0]);
    TEST_ASSERT(']' == g.buf[strlen(g.buf) - 1u]);
    TEST_ASSERT(NULL == strstr(g.buf, ",}"));
    TEST_ASSERT(guarded_intact(&g));
    guarded_free(&g);
}

static void test_escape_covers_what_breaks_a_document(void)
{
    char out[64];

    TEST_ASSERT(0 == strcmp((Json_Escape(out, sizeof(out), "a\"b"), out),
                            "a\\\"b"));
    TEST_ASSERT(0 == strcmp((Json_Escape(out, sizeof(out), "a\\b"), out),
                            "a\\\\b"));
    TEST_ASSERT(0 == strcmp((Json_Escape(out, sizeof(out), "a\nb"), out),
                            "a\\nb"));
    TEST_ASSERT(0 == strcmp((Json_Escape(out, sizeof(out), "a\tb"), out),
                            "a\\tb"));
    TEST_ASSERT(0 == strcmp((Json_Escape(out, sizeof(out), "a\x01" "b"), out),
                            "a\\u0001b"));
    /* '/' needs no escape, and UTF-8 passes through: re-encoding it would
     * mean decoding it first, which nothing here needs. */
    TEST_ASSERT(0 == strcmp((Json_Escape(out, sizeof(out), "a/\xc3\xa4"), out),
                            "a/\xc3\xa4"));
    TEST_ASSERT(0 == strcmp((Json_Escape(out, sizeof(out), NULL), out), ""));
}

/* A truncation that cut "\\u0001" in half would emit a lone backslash and
 * make the whole document unparseable -- worse than dropping the field. */
static void test_escaping_never_splits_an_escape(void)
{
    sGuarded g;
    size_t   n;
    size_t   cap;

    for (cap = 2u; cap < 12u; cap++) {
        guarded_new(&g, cap);
        n = Json_CatEscaped(g.buf, g.cap, 0u, "\x01\x02\x03");
        TEST_ASSERT(n <= g.cap);
        TEST_ASSERT(guarded_intact(&g));
        /* Whatever survived is a whole number of "\u00XX" groups. */
        TEST_ASSERT(0u == (strlen(g.buf) % 6u));
        guarded_free(&g);
    }
}

static void test_escape_into_a_short_buffer_stays_terminated(void)
{
    char out[5];

    TEST_ASSERT(3u == Json_Escape(out, sizeof(out), "abc"));
    TEST_ASSERT(0 == strcmp(out, "abc"));
    /* Two escaped quotes want 4 bytes; only one fits with its NUL. */
    TEST_ASSERT(2u == Json_Escape(out, 4u, "\"\""));
    TEST_ASSERT(0 == strcmp(out, "\\\""));
    TEST_ASSERT(0u == Json_Escape(out, 1u, "abc"));
    TEST_ASSERT('\0' == out[0]);
}

/* ==========================================================================
 * Reader
 * ========================================================================== */

static sJsonMemSource s_mem;

static void reader_over(sJsonReader *r, const char *doc)
{
    Json_MemSourceInit(&s_mem, doc, (uint32_t)strlen(doc));
    Json_ReaderInit(r, Json_MemRead, &s_mem);
}

static void test_the_token_stream(void)
{
    sJsonReader r;
    char        text[32];

    reader_over(&r, "{ \"k\" : [ -12, true, false, null ] }");

    TEST_ASSERT(jsonTok_lBrace   == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_string   == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(0 == strcmp(text, "k"));
    TEST_ASSERT(jsonTok_colon    == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_lBracket == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_number   == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(0 == strcmp(text, "-12"));
    TEST_ASSERT(jsonTok_comma    == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_true     == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_comma    == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_false    == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_comma    == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_null     == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_rBracket == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_rBrace   == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(jsonTok_eof      == Json_Next(&r, text, sizeof(text)));
}

/* The window is deliberately smaller than most documents, so a token that
 * straddles a refill is the normal case, not the exotic one. */
static void test_a_token_spanning_the_refill_window(void)
{
    sJsonReader r;
    char        doc[JSON_WINDOW_LEN * 3u];
    char        text[JSON_WINDOW_LEN * 3u];
    uint32_t    i;

    doc[0] = '"';
    for (i = 1u; i < (sizeof(doc) - 2u); i++) {
        doc[i] = 'x';
    }
    doc[sizeof(doc) - 2u] = '"';
    doc[sizeof(doc) - 1u] = '\0';

    reader_over(&r, doc);
    TEST_ASSERT(jsonTok_string == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(strlen(text) == (sizeof(doc) - 3u));
}

static void test_escapes_decode(void)
{
    sJsonReader r;
    char        text[64];

    reader_over(&r, "\"a\\\"b\\\\c\\nd\\te\\u0041f\\u00e4\"");
    TEST_ASSERT(jsonTok_string == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(0 == strcmp(text, "a\"b\\c\nd\te" "A" "f" "\xc3\xa4"));
}

/* The point of adding escapes to the reader: a name that had to be escaped on
 * the way out must come back as itself on the way in.  Neither end alone
 * closes this. */
static void test_the_round_trip_closes(void)
{
    static const char *k_raw[] = {
        "a\"b", "back\\slash", "line\nfeed", "tab\there", "ctl\x01here",
        "plain", ""
    };
    uint32_t i;

    for (i = 0u; i < (sizeof(k_raw) / sizeof(k_raw[0])); i++) {
        sJsonReader r;
        char        doc[128];
        char        back[128];
        size_t      pos = 0u;

        pos = Json_Cat(doc, sizeof(doc), pos, "\"");
        pos = Json_CatEscaped(doc, sizeof(doc), pos, k_raw[i]);
        pos = Json_Cat(doc, sizeof(doc), pos, "\"");
        TEST_ASSERT(pos < sizeof(doc));

        reader_over(&r, doc);
        TEST_ASSERT(jsonTok_string == Json_Next(&r, back, sizeof(back)));
        TEST_ASSERT(0 == strcmp(back, k_raw[i]));
    }
}

static void test_the_reject_matrix(void)
{
    static const struct {
        const char *doc;
        const char *why;
    } k_bad[] = {
        { "\"unterminated",     "no closing quote"                  },
        { "\"bad\\qescape\"",   "not an escape character"           },
        { "\"short\\u00\"",     "truncated \\u"                     },
        { "\"raw\nnewline\"",   "control character must be escaped" },
        { "1e3",                "exponent notation"                 },
        { "tru",                "truncated literal"                 },
        { "@",                  "not a JSON character"              },
    };
    uint32_t i;

    for (i = 0u; i < (sizeof(k_bad) / sizeof(k_bad[0])); i++) {
        sJsonReader r;
        char        text[32];

        reader_over(&r, k_bad[i].doc);
        if (jsonTok_err != Json_Next(&r, text, sizeof(text))) {
            printf("FAIL accepted %s (%s)\n", k_bad[i].doc, k_bad[i].why);
            test_failures++;
        } else {
            TEST_ASSERT(NULL != r.reason);
        }
    }
}

static void test_a_string_longer_than_its_field_is_refused(void)
{
    sJsonReader r;
    char        text[8];

    reader_over(&r, "\"0123456789\"");
    TEST_ASSERT(jsonTok_err == Json_Next(&r, text, sizeof(text)));
}

static void test_numbers_refuse_rather_than_wrap(void)
{
    int32_t  i32;
    uint32_t u32;

    TEST_ASSERT(0 == Json_ToI32("2147483647", &i32) && (2147483647 == i32));
    TEST_ASSERT(0 == Json_ToI32("-42", &i32) && (-42 == i32));
    TEST_ASSERT(-1 == Json_ToI32("2147483648", &i32));
    TEST_ASSERT(-1 == Json_ToI32("99999999999999", &i32));
    TEST_ASSERT(-1 == Json_ToI32("1.5", &i32));
    TEST_ASSERT(-1 == Json_ToI32("", &i32));
    TEST_ASSERT(-1 == Json_ToI32("-", &i32));

    TEST_ASSERT(0 == Json_ToU32("4294967295", &u32) && (4294967295u == u32));
    TEST_ASSERT(-1 == Json_ToU32("4294967296", &u32));
    TEST_ASSERT(-1 == Json_ToU32("-1", &u32));
}

static void test_the_convenience_layer(void)
{
    sJsonReader r;
    char        s[16];
    int32_t     v;
    int         b;

    reader_over(&r, "{\"n\":\"x\",\"v\":7,\"b\":true}");
    TEST_ASSERT(0 == Json_Expect(&r, jsonTok_lBrace));
    TEST_ASSERT(1 == Json_ReadString(&r, s, sizeof(s)) && 0 == strcmp(s, "n"));
    TEST_ASSERT(0 == Json_Expect(&r, jsonTok_colon));
    TEST_ASSERT(1 == Json_ReadString(&r, s, sizeof(s)) && 0 == strcmp(s, "x"));
    TEST_ASSERT(0 == Json_Expect(&r, jsonTok_comma));
    TEST_ASSERT(1 == Json_ReadString(&r, s, sizeof(s)));
    TEST_ASSERT(0 == Json_Expect(&r, jsonTok_colon));
    TEST_ASSERT(1 == Json_ReadI32(&r, &v) && (7 == v));
    TEST_ASSERT(0 == Json_Expect(&r, jsonTok_comma));
    TEST_ASSERT(1 == Json_ReadString(&r, s, sizeof(s)));
    TEST_ASSERT(0 == Json_Expect(&r, jsonTok_colon));
    TEST_ASSERT(1 == Json_ReadBool(&r, &b) && (1 == b));
    TEST_ASSERT(0 == Json_Expect(&r, jsonTok_rBrace));
}

/* A caller that looks ahead at a structural character before deciding what to
 * parse -- the shape App/Pack/pack_cfg.c uses throughout. */
static void test_the_byte_layer_looks_ahead_without_consuming(void)
{
    sJsonReader r;

    reader_over(&r, "   \n\t {x");
    TEST_ASSERT('{' == Json_SkipWs(&r));
    TEST_ASSERT('{' == Json_Peek(&r));
    TEST_ASSERT('{' == Json_Get(&r));
    TEST_ASSERT('x' == Json_Peek(&r));
    TEST_ASSERT('x' == Json_Get(&r));
    TEST_ASSERT(-1 == Json_Get(&r));
    TEST_ASSERT(8u == r.offset_bytes);   /* the whole document      */
}

static int failing_source(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    (void)ctx; (void)buf; (void)maxLen;
    return -1;
}

/* A source error must not read as end of input: one means the upload broke,
 * the other means the document ended, and they take different branches. */
static void test_a_source_error_is_not_eof(void)
{
    sJsonReader r;
    char        text[8];

    Json_ReaderInit(&r, failing_source, NULL);
    TEST_ASSERT(jsonTok_err == Json_Next(&r, text, sizeof(text)));
    TEST_ASSERT(NULL != r.reason);
}

int main(void)
{
    RUN_TEST(test_cat_appends_and_reports_the_position);
    RUN_TEST(test_cat_saturates_instead_of_underflowing);
    RUN_TEST(test_saturation_supports_whole_elements_only);
    RUN_TEST(test_escape_covers_what_breaks_a_document);
    RUN_TEST(test_escaping_never_splits_an_escape);
    RUN_TEST(test_escape_into_a_short_buffer_stays_terminated);

    RUN_TEST(test_the_token_stream);
    RUN_TEST(test_a_token_spanning_the_refill_window);
    RUN_TEST(test_escapes_decode);
    RUN_TEST(test_the_round_trip_closes);
    RUN_TEST(test_the_reject_matrix);
    RUN_TEST(test_a_string_longer_than_its_field_is_refused);
    RUN_TEST(test_numbers_refuse_rather_than_wrap);
    RUN_TEST(test_the_convenience_layer);
    RUN_TEST(test_the_byte_layer_looks_ahead_without_consuming);
    RUN_TEST(test_a_source_error_is_not_eof);

    printf("%s: %d failure(s)\n", __FILE__, test_failures);
    return test_failures ? 1 : 0;
}

/**
 * Unit tests for App/Pack/pack_cfg.c — the uploaded pack configuration
 * (docs/design_battery_pack.md §12; test plan in
 * docs/task_battery_pack_tests.md).
 *
 * THE PARSE IS THE VALIDATION, exactly as it is for the Modbus compiler, so
 * this file is also the accept/reject matrix for the whole §12 schema.
 * Pack_ConfigVerify and Pack_ConfigApply differ only in whether they commit
 * what this parser produced, which is why a host test of the parser is a test
 * of both.
 *
 * EVERY TEST IN THIS FILE IS EXPECTED TO FAIL until pack_cfg.c is
 * implemented.
 */

#include "test_util.h"
#include <stdio.h>

#include "App/Pack/pack.h"
#include "App/Pack/pack_cfg.h"

#include <string.h>

/* ============================================================================
 * Byte source with a configurable chunk size — the HTTP body cursor decides
 * the chunking in production, so the parser must not care what it is.
 * ============================================================================ */

typedef struct {
    const char *data;
    uint32_t    len;
    uint32_t    pos;
    uint32_t    chunk;      /* max bytes per read; 0 = unlimited */
} sMemSource;

static int mem_source(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sMemSource *m = (sMemSource *)ctx;
    uint32_t    n = m->len - m->pos;

    if (n > maxLen) {
        n = maxLen;
    }
    if (m->chunk != 0u && n > m->chunk) {
        n = m->chunk;
    }
    memcpy(buf, &m->data[m->pos], n);
    m->pos += n;
    return (int)n;
}

static int parse_str(const char *json, uint32_t chunk, sPackCfg *cfg,
                     sPackCfgResult *res)
{
    sMemSource src = { json, (uint32_t)strlen(json), 0u, chunk };

    memset(cfg, 0, sizeof(*cfg));
    memset(res, 0, sizeof(*res));
    return PackCfg_Parse(mem_source, &src, cfg, res);
}

/* A sink that appends into a fixed buffer, for the round trip. */
typedef struct {
    char     buf[4096];
    uint32_t len;
} sMemSink;

static int mem_sink(void *ctx, const char *data, uint32_t len)
{
    sMemSink *s = (sMemSink *)ctx;

    if ((s->len + len) >= sizeof(s->buf)) {
        return -1;
    }
    memcpy(&s->buf[s->len], data, len);
    s->len += len;
    s->buf[s->len] = '\0';
    return (int)len;
}

/* ============================================================================
 * The §12 worked example, verbatim: three packs, two types, one carrying a
 * narrowing `commands` allowlist and two relying on per-type defaults.
 * ============================================================================ */

static const char WORKED_EXAMPLE[] =
    "{\n"
    "  \"version\": 1,\n"
    "  \"packs\": [\n"
    "    { \"name\": \"sodas_a\", \"type\": \"jkbms\", \"bind\": \"rs485:2\",\n"
    "      \"nameplate_ah\": 660, \"cells\": 16, \"chemistry\": \"lfp\",\n"
    "      \"staleAfter_ms\": 15000,\n"
    "      \"commands\": {\n"
    "        \"chargeEnable\": true,\n"
    "        \"dischargeEnable\": false,\n"
    "        \"chargeLimit\": { \"min_a\": 0, \"max_a\": 200 }\n"
    "      } },\n"
    "\n"
    "    { \"name\": \"sodas_b\", \"type\": \"jkbms\", \"bind\": \"rs485:15\",\n"
    "      \"nameplate_ah\": 300, \"cells\": 16, \"chemistry\": \"lfp\" },\n"
    "\n"
    "    { \"name\": \"rack_c\", \"type\": \"pylontech\", \"bind\": \"can:0\",\n"
    "      \"nameplate_ah\": 261, \"cells\": 16, \"chemistry\": \"lfp\" }\n"
    "  ]\n"
    "}\n";

/* ============================================================================
 * Area 8 — the accept case
 * ============================================================================ */

static void test_accepts_the_worked_example(void)
{
    sPackCfg       cfg;
    sPackCfgResult res;

    TEST_ASSERT(parse_str(WORKED_EXAMPLE, 0u, &cfg, &res) == packErr_ok);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(cfg.version == 1u);
    TEST_ASSERT(cfg.count == 3u);

    TEST_ASSERT(strcmp(cfg.pack[0].name, "sodas_a") == 0);
    TEST_ASSERT(cfg.pack[0].typeId == (uint8_t)packType_jkBms);
    TEST_ASSERT(strcmp(cfg.pack[0].bind, "rs485:2") == 0);
    TEST_ASSERT(cfg.pack[0].chemistry == (uint8_t)packChem_lfp);
    TEST_ASSERT(cfg.pack[0].cellCount == 16u);
    TEST_ASSERT(cfg.pack[0].staleAfter_ms == 15000u);

    /* §2 point 3: "A pack API that reports only percentages is unusable" —
     * 660 Ah beside 300 Ah on one bus, so nameplate must survive as mAh. */
    TEST_ASSERT(cfg.pack[0].nameplate_mAh == 660000u);
    TEST_ASSERT(cfg.pack[1].nameplate_mAh == 300000u);
    TEST_ASSERT(cfg.pack[2].nameplate_mAh == 261000u);

    TEST_ASSERT(strcmp(cfg.pack[2].name, "rack_c") == 0);
    TEST_ASSERT(cfg.pack[2].typeId == (uint8_t)packType_pylontech);
    TEST_ASSERT(strcmp(cfg.pack[2].bind, "can:0") == 0);
}

/* "commands is a NARROWING ALLOWLIST, and narrowing only.  Absent = every
 * command the type confirms." */
static void test_commands_narrow_and_absent_means_everything(void)
{
    sPackCfg       cfg;
    sPackCfgResult res;

    TEST_ASSERT(parse_str(WORKED_EXAMPLE, 0u, &cfg, &res) == packErr_ok);

    /* sodas_a: chargeEnable permitted, dischargeEnable explicitly FORBIDDEN,
     * chargeLimit permitted with a narrowed domain.  This is where the answer
     * to "who may disconnect this battery" lives. */
    TEST_ASSERT((cfg.pack[0].cmdAllow &
                 PACK_CMD_BIT(packCmd_chargeEnable)) != 0u);
    TEST_ASSERT((cfg.pack[0].cmdAllow &
                 PACK_CMD_BIT(packCmd_dischargeEnable)) == 0u);
    TEST_ASSERT((cfg.pack[0].cmdAllow &
                 PACK_CMD_BIT(packCmd_chargeLimit)) != 0u);

    /* min_a/max_a are amps in the document and mA in the bound, because no
     * float crosses this API. */
    {
        const sPackCmdBound *b = NULL;
        uint8_t              i;

        for (i = 0u; i < cfg.pack[0].boundCount; i++) {
            if (cfg.pack[0].bounds[i].cmd == packCmd_chargeLimit) {
                b = &cfg.pack[0].bounds[i];
            }
        }
        TEST_ASSERT(b != NULL);
        if (b != NULL) {
            TEST_ASSERT(b->min_scaled == 0);
            TEST_ASSERT(b->max_scaled == 200000);
        }
    }

    /* sodas_b has no `commands` block at all: every command the type confirms
     * is permitted, which is cmdAllow with every bit set. */
    TEST_ASSERT(cfg.pack[1].cmdAllow == 0xFFFFFFFFu);
}

/* Defaults per type (§12): jkbms 15000/60000, pylontech 5000/60000. */
/* An EMPTY commands block is not the same as an ABSENT one: absent allows
 * every command the type confirms, present-but-empty allows none.  That
 * distinction is the one §12's table exists to make, and it is the difference
 * between "I did not think about commands" and "I forbid all of them". */
static void test_empty_commands_block_forbids_everything(void)
{
    static const char JSON[] =
        "{\"version\":1,\"packs\":[{"
        "\"name\":\"p\",\"type\":\"jkbms\",\"bind\":\"rs485:2\","
        "\"nameplate_ah\":100,\"cells\":16,\"chemistry\":\"lfp\","
        "\"commands\":{}}]}";
    sPackCfg       cfg;
    sPackCfgResult res;

    TEST_ASSERT(parse_str(JSON, 0u, &cfg, &res) == packErr_ok);
    TEST_ASSERT(cfg.count == 1u);
    TEST_ASSERT(cfg.pack[0].cmdAllow == 0u);
    TEST_ASSERT(cfg.pack[0].boundCount == 0u);
}

/* A command listed but not enabled is forbidden, and so is one never
 * mentioned while the block is present.  Both must land on the same answer. */
static void test_unlisted_command_in_a_present_block_is_forbidden(void)
{
    static const char JSON[] =
        "{\"version\":1,\"packs\":[{"
        "\"name\":\"p\",\"type\":\"jkbms\",\"bind\":\"rs485:2\","
        "\"nameplate_ah\":100,\"cells\":16,\"chemistry\":\"lfp\","
        "\"commands\":{\"chargeEnable\":true}}]}";
    sPackCfg       cfg;
    sPackCfgResult res;

    TEST_ASSERT(parse_str(JSON, 0u, &cfg, &res) == packErr_ok);
    TEST_ASSERT((cfg.pack[0].cmdAllow &
                 PACK_CMD_BIT(packCmd_chargeEnable)) != 0u);
    /* never mentioned -> forbidden, same as an explicit false */
    TEST_ASSERT((cfg.pack[0].cmdAllow &
                 PACK_CMD_BIT(packCmd_dischargeEnable)) == 0u);
    TEST_ASSERT((cfg.pack[0].cmdAllow &
                 PACK_CMD_BIT(packCmd_balanceEnable)) == 0u);
}

static void test_per_type_defaults_are_applied(void)
{
    sPackCfg       cfg;
    sPackCfgResult res;

    TEST_ASSERT(parse_str(WORKED_EXAMPLE, 0u, &cfg, &res) == packErr_ok);

    TEST_ASSERT(cfg.pack[1].staleAfter_ms == PACK_CFG_JK_STALE_MS);
    TEST_ASSERT(cfg.pack[2].staleAfter_ms == PACK_CFG_PYLON_STALE_MS);
}

/* The HTTP body cursor decides the chunking, not the parser. */
static void test_chunked_feed_matches(void)
{
    sPackCfg       whole;
    sPackCfg       chunked;
    sPackCfgResult res;
    uint32_t       chunk;

    TEST_ASSERT(parse_str(WORKED_EXAMPLE, 0u, &whole, &res) == packErr_ok);

    for (chunk = 1u; chunk <= 17u; chunk += 4u) {
        TEST_ASSERT(parse_str(WORKED_EXAMPLE, chunk, &chunked, &res) ==
                    packErr_ok);
        TEST_ASSERT_MEM_EQ(&whole, &chunked, sizeof(whole));
    }
}

/* Data-faithful, not byte-identical: what comes out must parse back to an
 * identical sPackCfg. */
static void test_export_round_trip(void)
{
    sPackCfg       a;
    sPackCfg       b;
    sPackCfgResult res;
    sMemSink       sink;

    TEST_ASSERT(parse_str(WORKED_EXAMPLE, 0u, &a, &res) == packErr_ok);

    memset(&sink, 0, sizeof(sink));
    TEST_ASSERT(PackCfg_Serialize(&a, mem_sink, &sink) == packErr_ok);
    TEST_ASSERT(sink.len > 0u);

    TEST_ASSERT(parse_str(sink.buf, 0u, &b, &res) == packErr_ok);
    TEST_ASSERT_MEM_EQ(&a, &b, sizeof(a));
}

/* ============================================================================
 * Area 9 — the reject matrix (§12)
 * ============================================================================ */

static void expect_reject(const char *json, const char *whatIsWrong,
                          const char *expectField)
{
    sPackCfg       cfg;
    sPackCfgResult res;

    if (parse_str(json, 0u, &cfg, &res) == packErr_ok) {
        printf("FAIL %s:%d: accepted but should reject (%s)\n",
               __FILE__, __LINE__, whatIsWrong);
        test_failures++;
        return;
    }
    /* A reject must NAME the offender — that is what makes a stale config
     * fail by name instead of half-applying. */
    TEST_ASSERT(res.ok == 0);
    if (NULL != expectField) {
        if (strcmp(res.field, expectField) != 0) {
            printf("FAIL %s:%d: %s -> field \"%s\", expected \"%s\"\n",
                   __FILE__, __LINE__, whatIsWrong, res.field, expectField);
            test_failures++;
        }
    }
}

/* "Unknown keys are rejected, as the Modbus compiler does — the useful
 * failure mode, and what makes a stale config fail by name." */
static void test_rejects_unknown_keys(void)
{
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        " \"nameplate_ah\": 100, \"topicPrefix\": \"periphnet\" } ] }",
        "unknown pack-level key", "topicPrefix");

    expect_reject(
        "{ \"version\": 1, \"mqtt\": true, \"packs\": [] }",
        "unknown document-level key", "mqtt");

    /* A devOrd is a POSITION IN ANOTHER MODULE'S ARRAY and is expressly not
     * how a pack binds (§12), so the key must not quietly exist. */
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"devOrd\": 0, \"nameplate_ah\": 100 } ] }",
        "devOrd is not a binding", "devOrd");
}

/* "name is THE IDENTITY.  Unique, [A-Za-z0-9_-], <= 15 chars.  Persisted
 * per-pack state keys on it, so reordering the array cannot reattach one
 * pack's history to another." */
static void test_rejects_bad_names(void)
{
    expect_reject(
        "{ \"version\": 1, \"packs\": ["
        " { \"name\": \"dup\", \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        "   \"nameplate_ah\": 100 },"
        " { \"name\": \"dup\", \"type\": \"jkbms\", \"bind\": \"rs485:3\","
        "   \"nameplate_ah\": 100 } ] }",
        "duplicate name", "name");

    /* 16 characters: one past PACK_CFG_NAME_MAX_CHARS, and exactly the case a
     * truncating parser would silently accept as a 15-char duplicate. */
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"0123456789abcdef\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        " \"nameplate_ah\": 100 } ] }",
        "name too long", "name");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        " \"nameplate_ah\": 100 } ] }",
        "empty name", "name");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"has space\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        " \"nameplate_ah\": 100 } ] }",
        "name outside [A-Za-z0-9_-]", "name");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"type\": \"jkbms\","
        " \"bind\": \"rs485:2\", \"nameplate_ah\": 100 } ] }",
        "missing name", "name");
}

/* A `bind` token is OPAQUE to the core — the TYPE parses it — so the only
 * checks the parser can honestly make are width and printability.
 * "rs485:99" is well-formed here and fails later at bind time with
 * packWhy_noBinding, which is the honest place for it. */
static void test_rejects_bad_bind_tokens(void)
{
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"\", \"nameplate_ah\": 100 } ] }",
        "empty bind", "bind");

    /* 24 characters: one past PACK_BIND_LEN - 1. */
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:0123456789012345678\","
        " \"nameplate_ah\": 100 } ] }",
        "bind token too long", "bind");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"nameplate_ah\": 100 } ] }",
        "missing bind", "bind");
}

/* THE ONE THAT MATTERS MOST: "A bound WIDER than the type's is a 422 at parse
 * time, not a silent clamp."  A clamp here would let an operator believe they
 * had capped a charge current they had not. */
static void test_rejects_a_bound_wider_than_the_types(void)
{
    const sPackTypeLimits *lim = PackCfg_TypeLimits((uint8_t)packType_jkBms);

    /* The parser must have a static ceiling to check against; without one
     * every `commands` block reads as unbounded. */
    TEST_ASSERT(lim != NULL);

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\", \"nameplate_ah\": 100,"
        " \"commands\": { \"chargeLimit\":"
        " { \"min_a\": 0, \"max_a\": 100000 } } } ] }",
        "chargeLimit max wider than the type's", "chargeLimit");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\", \"nameplate_ah\": 100,"
        " \"commands\": { \"chargeLimit\":"
        " { \"min_a\": -50, \"max_a\": 100 } } } ] }",
        "chargeLimit min below the type's", "chargeLimit");

    /* A command no type accepts at all. */
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"pylontech\", \"bind\": \"can:0\","
        " \"nameplate_ah\": 100,"
        " \"commands\": { \"chargeEnable\": true } } ] }",
        "command the pylontech type cannot offer", "chargeEnable");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\", \"nameplate_ah\": 100,"
        " \"commands\": { \"selfDestruct\": true } } ] }",
        "unknown command name", "selfDestruct");

    /* An inverted bound is not narrowing, it is nonsense. */
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\", \"nameplate_ah\": 100,"
        " \"commands\": { \"chargeLimit\":"
        " { \"min_a\": 100, \"max_a\": 10 } } } ] }",
        "min above max", "chargeLimit");
}

static void test_rejects_more_than_pack_max(void)
{
    static char json[2048];
    uint32_t    i;
    int         n;

    n = snprintf(json, sizeof(json), "{ \"version\": 1, \"packs\": [");
    for (i = 0u; i < (PACK_MAX + 1u); i++) {
        n += snprintf(&json[n], sizeof(json) - (size_t)n,
                      "%s{ \"name\": \"p%u\", \"type\": \"jkbms\","
                      " \"bind\": \"rs485:%u\", \"nameplate_ah\": 100 }",
                      (i == 0u) ? "" : ",", (unsigned)i, (unsigned)(i + 1u));
    }
    (void)snprintf(&json[n], sizeof(json) - (size_t)n, "] }");

    expect_reject(json, "PACK_MAX + 1 packs", "packs");
}

static void test_rejects_bad_scalars(void)
{
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"nosuchtype\", \"bind\": \"rs485:2\","
        " \"nameplate_ah\": 100 } ] }",
        "unknown type", "type");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        " \"nameplate_ah\": 100, \"chemistry\": \"unobtainium\" } ] }",
        "unknown chemistry", "chemistry");

    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        " \"nameplate_ah\": 100, \"cells\": 64 } ] }",
        "cells above PACK_CELLS_MAX", "cells");

    /* A version this image does not understand must fail loudly rather than
     * be read with the wrong field meanings. */
    expect_reject(
        "{ \"version\": 99, \"packs\": [] }",
        "unsupported document version", "version");

    expect_reject("{ \"packs\": [ ", "truncated document", "json");
    expect_reject("not json at all", "not JSON", "json");
}

/* Zero packs is a VALID, FIRST-CLASS state — "unprovisioned" — and not an
 * error the parser invents. */
static void test_empty_pack_list_is_valid(void)
{
    sPackCfg       cfg;
    sPackCfgResult res;

    TEST_ASSERT(parse_str("{ \"version\": 1, \"packs\": [] }", 0u, &cfg, &res)
                == packErr_ok);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(cfg.count == 0u);
}

/* ============================================================================
 * Area 10 — the name tables both directions
 * ============================================================================ */

static void test_name_tables_round_trip(void)
{
    uint8_t    id = 0xFFu;
    uint8_t    chem = 0xFFu;
    ePackCmdId cmd = packCmd_last;

    TEST_ASSERT(PackCfg_TypeIdFromName("jkbms", &id) == packErr_ok);
    TEST_ASSERT(id == (uint8_t)packType_jkBms);
    TEST_ASSERT(PackCfg_TypeIdFromName("pylontech", &id) == packErr_ok);
    TEST_ASSERT(id == (uint8_t)packType_pylontech);
    TEST_ASSERT(PackCfg_TypeIdFromName("sim", &id) == packErr_notFound);
    TEST_ASSERT(PackCfg_TypeName((uint8_t)packType_jkBms) != NULL);
    if (PackCfg_TypeName((uint8_t)packType_jkBms) != NULL) {
        TEST_ASSERT(strcmp(PackCfg_TypeName((uint8_t)packType_jkBms),
                           "jkbms") == 0);
    }
    TEST_ASSERT(PackCfg_TypeName((uint8_t)packType_last) == NULL);

    TEST_ASSERT(PackCfg_ChemFromName("lfp", &chem) == packErr_ok);
    TEST_ASSERT(chem == (uint8_t)packChem_lfp);
    TEST_ASSERT(PackCfg_ChemFromName("lto", &chem) == packErr_ok);
    TEST_ASSERT(chem == (uint8_t)packChem_lto);
    TEST_ASSERT(PackCfg_ChemName((uint8_t)packChem_lfp) != NULL);

    TEST_ASSERT(PackCfg_CmdIdFromName("chargeEnable", &cmd) == packErr_ok);
    TEST_ASSERT(cmd == packCmd_chargeEnable);
    TEST_ASSERT(PackCfg_CmdIdFromName("dischargeLimit", &cmd) == packErr_ok);
    TEST_ASSERT(cmd == packCmd_dischargeLimit);
    TEST_ASSERT(PackCfg_CmdIdFromName("nope", &cmd) == packErr_notFound);
    TEST_ASSERT(PackCfg_CmdName(packCmd_chargeLimit) != NULL);
    TEST_ASSERT(PackCfg_CmdName(packCmd_last) == NULL);
}

/* ==========================================================================
 * Regression tests for the review findings (2026-08-26).
 *
 * Each of these was ACCEPTED by the parser before the fix, and the first two
 * corrupted memory rather than merely misbehaving.
 * ========================================================================== */

/** THE ORIGINAL CRASHING INPUT, kept as a regression test.
 *
 *  200 repeats of one `commands` key walked boundCount past
 *  sPackCmdBound[6]; ASan showed a global-buffer-overflow escaping sPackCfg
 *  entirely, and on the device the target is a static in .bss reachable from
 *  an unauthenticated POST /api/pack/config.
 *
 *  What now rejects it is the DUPLICATE-KEY check -- with only five distinct
 *  commands and each accepted once, boundCount can no longer reach the array
 *  end by any input, so the bound check in parse_commands is unreachable
 *  defence rather than the active guard.  It stays because pack_jkbms.c's
 *  equivalent loop has always carried one, and because "unreachable" is a
 *  property of today's enum, not of the loop. */
static void test_rejects_the_original_overflow_input(void)
{
    static char json[16384];
    uint32_t    i;
    int         n;

    n = snprintf(json, sizeof(json),
                 "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
                 " \"type\": \"jkbms\", \"bind\": \"rs485:2\","
                 " \"nameplate_ah\": 100, \"commands\": { ");
    for (i = 0u; i < 200u; i++) {
        n += snprintf(&json[n], sizeof(json) - (size_t)n,
                      "%s\"chargeLimit\": { \"min_a\": 0, \"max_a\": 200 }",
                      (i == 0u) ? "" : ", ");
    }
    (void)snprintf(&json[n], sizeof(json) - (size_t)n, " } } ] }");

    expect_reject(json, "repeated command key", "chargeLimit");
}

/** Even TWO of the same key is a malformed document: two domains for one
 *  command give the operator no way to know which is enforced. */
static void test_rejects_a_command_key_seen_twice(void)
{
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\", \"nameplate_ah\": 100,"
        " \"commands\": { \"chargeLimit\": { \"min_a\": 0, \"max_a\": 200 },"
        " \"chargeLimit\": { \"min_a\": 0, \"max_a\": 100 } } } ] }",
        "command key twice", "chargeLimit");
}

/** rd_number rejected an overflowing literal, but amps -> milliamps then
 *  multiplied by 1000 and overflowed again -- and the ceiling check ran on
 *  the WRAPPED value, so this was accepted as a 0.704 A .. 299.7 A domain. */
static void test_rejects_an_amps_value_that_would_overflow(void)
{
    expect_reject(
        "{ \"version\": 1, \"packs\": [ { \"name\": \"a\","
        " \"type\": \"jkbms\", \"bind\": \"rs485:2\", \"nameplate_ah\": 100,"
        " \"commands\": { \"chargeLimit\":"
        " { \"min_a\": 4294968, \"max_a\": 4295267 } } } ] }",
        "amps overflow", "min_a");
}

/** Two entries naming one physical battery both match every sample it sends,
 *  so the board reports two healthy packs where one exists and a summing
 *  consumer doubles the site's capacity.  maxInstances cannot catch this --
 *  the collision is per-token, not per-type. */
static void test_rejects_a_duplicate_bind_token(void)
{
    expect_reject(
        "{ \"version\": 1, \"packs\": ["
        " { \"name\": \"a\", \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        "   \"nameplate_ah\": 100 },"
        " { \"name\": \"b\", \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        "   \"nameplate_ah\": 100 } ] }",
        "duplicate bind", "bind");
}

/** ...while two DIFFERENT tokens of the same type stay legal: board #2 runs
 *  exactly this today (slave 2 and slave 15 on one bus). */
static void test_accepts_two_packs_on_distinct_addresses(void)
{
    sPackCfg       cfg;
    sPackCfgResult res;

    TEST_ASSERT(0 == parse_str(
        "{ \"version\": 1, \"packs\": ["
        " { \"name\": \"sodas_a\", \"type\": \"jkbms\", \"bind\": \"rs485:2\","
        "   \"nameplate_ah\": 660 },"
        " { \"name\": \"sodas_b\", \"type\": \"jkbms\", \"bind\": \"rs485:15\","
        "   \"nameplate_ah\": 300 } ] }", 0u, &cfg, &res));
    TEST_ASSERT(res.ok != 0u);
    TEST_ASSERT(2u == cfg.count);
    TEST_ASSERT(660000u == cfg.pack[0].nameplate_mAh);
    TEST_ASSERT(300000u == cfg.pack[1].nameplate_mAh);
}

int main(void)
{
    printf("=== pack_cfg tests ===\n");

    RUN_TEST(test_accepts_the_worked_example);
    RUN_TEST(test_commands_narrow_and_absent_means_everything);
    RUN_TEST(test_empty_commands_block_forbids_everything);
    RUN_TEST(test_unlisted_command_in_a_present_block_is_forbidden);
    RUN_TEST(test_per_type_defaults_are_applied);
    RUN_TEST(test_chunked_feed_matches);
    RUN_TEST(test_export_round_trip);

    RUN_TEST(test_rejects_unknown_keys);
    RUN_TEST(test_rejects_bad_names);
    RUN_TEST(test_rejects_bad_bind_tokens);
    RUN_TEST(test_rejects_a_bound_wider_than_the_types);
    RUN_TEST(test_rejects_more_than_pack_max);
    RUN_TEST(test_rejects_bad_scalars);
    RUN_TEST(test_empty_pack_list_is_valid);

    RUN_TEST(test_name_tables_round_trip);

    /* review regressions */
    RUN_TEST(test_rejects_the_original_overflow_input);
    RUN_TEST(test_rejects_a_command_key_seen_twice);
    RUN_TEST(test_rejects_an_amps_value_that_would_overflow);
    RUN_TEST(test_rejects_a_duplicate_bind_token);
    RUN_TEST(test_accepts_two_packs_on_distinct_addresses);

    printf("%s (%d failures)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}

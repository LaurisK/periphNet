/**
 * Unit tests for App/Cluster/cluster_cfg.c — the streaming configuration
 * parser, the tunable bounds, the export round trip and all nine
 * Cluster_*Name() accessors
 * (docs/design_battery_cluster.md §2, §5J, constraint 12).
 *
 * TWO OF THE BOUNDS HERE ARE LOAD-BEARING ARITHMETIC, not validation hygiene,
 * and they are tested as such:
 *   cfg_rejects_bindFrac_below_loadTarget   — the gate's ceiling (defect L2)
 *   cfg_rejects_a_riseRate_above_the_bound  — the slew's uint32 headroom
 *
 * -Werror=switch is scoped onto cluster_cfg.c for this target too, so a new
 * enumerator without a name fails HERE — which is where a developer looks
 * first — rather than after a cross-compile.
 */

#include "test_util.h"

#include "App/Cluster/cluster.h"
#include "App/Cluster/cluster_cfg.h"

#include <string.h>

/* ============================================================================
 * Byte source / sink over plain memory
 * ============================================================================ */

typedef struct {
    const char *s;
    uint32_t    pos;
    uint32_t    len;
} sMemSrc;

static int mem_src(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sMemSrc *m = (sMemSrc *)ctx;
    uint32_t n = m->len - m->pos;

    if (n > maxLen) {
        n = maxLen;
    }
    if (n > 0u) {
        (void)memcpy(buf, &m->s[m->pos], n);
        m->pos += n;
    }
    return (int)n;
}

typedef struct {
    char     buf[1024];
    uint32_t len;
} sMemSink;

static int mem_sink(void *ctx, const char *data, uint32_t len)
{
    sMemSink *k = (sMemSink *)ctx;

    if ((k->len + len) >= sizeof(k->buf)) {
        return -1;
    }
    (void)memcpy(&k->buf[k->len], data, len);
    k->len += len;
    k->buf[k->len] = '\0';
    return (int)len;
}

static int parse(const char *doc, sClusterCfg *cfg, sClusterCfgResult *res)
{
    sMemSrc m = { doc, 0u, (uint32_t)strlen(doc) };

    return ClusterCfg_Parse(mem_src, &m, cfg, res);
}

/* The honest day-one sodas document: two JK packs on one bus, the profile the
 * Solis at that site is actually set to (43009 = 1 = PYLON_LV). */
static const char SODAS[] =
    "{\"version\":1,\"profile\":\"pylon_lv\","
    "\"members\":[\"sodas_a\",\"sodas_b\"]}";

/* ============================================================================
 * Parsing
 * ============================================================================ */

static void test_cfg_accepts_the_sodas_document(void)
{
    sClusterCfg       cfg;
    sClusterCfgResult res;

    TEST_ASSERT(parse(SODAS, &cfg, &res) == cluErr_ok);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(res.members == 2);
    TEST_ASSERT(cfg.count == 2u);
    TEST_ASSERT(strcmp(cfg.member[0], "sodas_a") == 0);
    TEST_ASSERT(strcmp(cfg.member[1], "sodas_b") == 0);
    /* The profile is an OPAQUE TOKEN: stored verbatim, never compared here. */
    TEST_ASSERT(strcmp(cfg.profile, "pylon_lv") == 0);
}

static void test_cfg_defaults_are_applied_when_keys_are_absent(void)
{
    sClusterCfg cfg;

    TEST_ASSERT(parse(SODAS, &cfg, NULL) == cluErr_ok);
    TEST_ASSERT(cfg.tune.riseRate_mA_per_s  == CLUSTER_DFLT_RISE_MA_PER_S);
    TEST_ASSERT(cfg.tune.loadTarget_pm      == CLUSTER_DFLT_LOAD_TARGET_PM);
    TEST_ASSERT(cfg.tune.bindFrac_pm        == CLUSTER_DFLT_BIND_FRAC_PM);
    /* The JK's own measured derations, carried so the board is no more
     * aggressive than the battery it replaces. */
    TEST_ASSERT(cfg.tune.chargeDerate_pm    == 800u);
    TEST_ASSERT(cfg.tune.dischargeDerate_pm == 850u);
    TEST_ASSERT(cfg.tune.elecMaxAge_ms      == 5000u);
    TEST_ASSERT(cfg.tune.holdMaxAge_ms      == 3600000u);
    TEST_ASSERT(cfg.tune.voltDiverge_mV     == 500u);
    TEST_ASSERT(cfg.version == CLUSTER_CFG_VERSION);
}

static void test_cfg_reads_a_whole_tune_block(void)
{
    sClusterCfg cfg;
    static const char doc[] =
        "{\"members\":[\"a\"],\"tune\":{"
        "\"riseRate_mA_per_s\":2500,\"limitMax_mA\":400000,"
        "\"holdMaxAge_ms\":600000,\"voltDiverge_mV\":250,"
        "\"elecMaxAge_ms\":3000,\"loadTarget_pm\":850,"
        "\"bindFrac_pm\":880,\"limitDeadband_pm\":30,"
        "\"convergeTol_pm\":40,\"chargeDerate_pm\":750,"
        "\"dischargeDerate_pm\":900,\"socDiverge_pm\":150,"
        "\"shareDiverge_pm\":650}}";

    TEST_ASSERT(parse(doc, &cfg, NULL) == cluErr_ok);
    TEST_ASSERT(cfg.tune.riseRate_mA_per_s == 2500u);
    TEST_ASSERT(cfg.tune.limitMax_mA == 400000u);
    TEST_ASSERT(cfg.tune.holdMaxAge_ms == 600000u);
    TEST_ASSERT(cfg.tune.loadTarget_pm == 850u);
    TEST_ASSERT(cfg.tune.bindFrac_pm == 880u);
    TEST_ASSERT(cfg.tune.dischargeDerate_pm == 900u);
    TEST_ASSERT(cfg.tune.shareDiverge_pm == 650u);
}

static void test_cfg_an_empty_cluster_is_a_valid_statement(void)
{
    sClusterCfg cfg;

    TEST_ASSERT(parse("{\"members\":[]}", &cfg, NULL) == cluErr_ok);
    TEST_ASSERT(cfg.count == 0u);
}

/* ============================================================================
 * Rejection — every one of these names the offending key
 * ============================================================================ */

static void test_cfg_rejects_an_unknown_key_by_name(void)
{
    sClusterCfg       cfg;
    sClusterCfgResult res;

    TEST_ASSERT(parse("{\"membrs\":[\"a\"]}", &cfg, &res) == cluErr_badArg);
    TEST_ASSERT(res.ok == 0);
    TEST_ASSERT(strcmp(res.field, "membrs") == 0);

    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":{\"riseRate\":5}}",
                      &cfg, &res) == cluErr_badArg);
    TEST_ASSERT(strcmp(res.field, "riseRate") == 0);
}

static void test_cfg_rejects_a_duplicate_member_name(void)
{
    sClusterCfg       cfg;
    sClusterCfgResult res;

    /* Two slots naming one pack would count its amp-hours twice and let one
     * battery bind the limit against itself. */
    TEST_ASSERT(parse("{\"members\":[\"a\",\"b\",\"a\"]}", &cfg, &res)
                == cluErr_badArg);
    TEST_ASSERT(res.memberIdx == 2);
    TEST_ASSERT(strstr(res.reason, "duplicate") != NULL);
}

static void test_cfg_rejects_more_than_pack_max_members(void)
{
    sClusterCfg       cfg;
    sClusterCfgResult res;
    static const char doc[] =
        "{\"members\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}";

    TEST_ASSERT(parse(doc, &cfg, &res) == cluErr_badArg);
    TEST_ASSERT(res.memberIdx == (int)CLUSTER_PACK_MAX);
}

static void test_cfg_rejects_an_illegal_member_name(void)
{
    sClusterCfg cfg;

    TEST_ASSERT(parse("{\"members\":[\"a b\"]}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"\"]}", &cfg, NULL) == cluErr_badArg);
    /* Longer than PACK_NAME_LEN - 1: it could never name a real pack. */
    TEST_ASSERT(parse("{\"members\":[\"aaaaaaaaaaaaaaaaaaaa\"]}", &cfg, NULL)
                == cluErr_badArg);
}

/** DEFECT L2, AS A PARSE RULE.  The gate's ceiling is
 *  (loadTarget_pm / bindFrac_pm) x min(L_i/f_i).  An operator lowering
 *  bindFrac_pm to chase binding samples — the natural response to
 *  `bindingSampleChg: 0` in the status — would move that ceiling to 1.8x
 *  every pack's own limit with no alarm and every other test still green. */
static void test_cfg_rejects_bindFrac_below_loadTarget(void)
{
    sClusterCfg       cfg;
    sClusterCfgResult res;

    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"loadTarget_pm\":900,\"bindFrac_pm\":500}}",
                      &cfg, &res) == cluErr_badArg);
    TEST_ASSERT(strcmp(res.field, "bindFrac_pm") == 0);

    /* And in the other key order, which is why the rule is checked after the
     * whole document rather than inside parse_tune. */
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"bindFrac_pm\":500,\"loadTarget_pm\":900}}",
                      &cfg, &res) == cluErr_badArg);
    TEST_ASSERT(strcmp(res.field, "bindFrac_pm") == 0);

    /* Equal is legal, and is the shipping default: the ceiling is then
     * exactly min(L_i/f_i). */
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"bindFrac_pm\":900,\"loadTarget_pm\":900}}",
                      &cfg, &res) == cluErr_ok);
}

static void test_cfg_rejects_a_riseRate_above_the_overflow_bound(void)
{
    sClusterCfg       cfg;
    sClusterCfgResult res;

    /* rate x dt must stay inside uint32 with dt clamped at 2000 ms. */
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"riseRate_mA_per_s\":100001}}", &cfg, &res)
                == cluErr_badArg);
    TEST_ASSERT(strcmp(res.field, "riseRate_mA_per_s") == 0);

    /* ZERO IS REJECTED rather than meaning "frozen": the step floor would
     * still let it climb at 40 mA/s, which is a surprise, not a policy. */
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"riseRate_mA_per_s\":0}}", &cfg, NULL)
                == cluErr_badArg);
}

static void test_cfg_rejects_limitMax_zero_or_above_the_macro(void)
{
    sClusterCfg cfg;

    /* published x loadTarget_pm must stay inside uint32. */
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":{\"limitMax_mA\":0}}",
                      &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"limitMax_mA\":1000001}}", &cfg, NULL)
                == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"limitMax_mA\":1000000}}", &cfg, NULL) == cluErr_ok);
}

static void test_cfg_rejects_out_of_range_per_mille_tunables(void)
{
    sClusterCfg cfg;

    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"loadTarget_pm\":1000}}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"loadTarget_pm\":0}}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"chargeDerate_pm\":1001}}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"chargeDerate_pm\":0}}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"limitDeadband_pm\":500}}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"elecMaxAge_ms\":0}}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"holdMaxAge_ms\":0}}", &cfg, NULL) == cluErr_badArg);
    /* A negative anything is refused before it can be cast to unsigned. */
    TEST_ASSERT(parse("{\"members\":[\"a\"],\"tune\":"
                      "{\"socDiverge_pm\":-1}}", &cfg, NULL) == cluErr_badArg);
}

static void test_cfg_refuses_a_newer_document_version(void)
{
    sClusterCfg       cfg;
    sClusterCfgResult res;

    /* Reading a newer document with this image's field meanings would misread
     * a tunable that bounds a current limit. */
    TEST_ASSERT(parse("{\"version\":99,\"members\":[\"a\"]}", &cfg, &res)
                == cluErr_badArg);
    TEST_ASSERT(strcmp(res.field, "version") == 0);
}

static void test_cfg_rejects_truncated_and_malformed_documents(void)
{
    sClusterCfg cfg;

    TEST_ASSERT(parse("", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":[\"a\"", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("[\"a\"]", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"members\":\"a\"}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(parse("{\"tune\":5}", &cfg, NULL) == cluErr_badArg);
    TEST_ASSERT(ClusterCfg_Parse(NULL, NULL, &cfg, NULL) == cluErr_badArg);
}

/* ============================================================================
 * Export
 * ============================================================================ */

static void test_cfg_export_round_trips(void)
{
    sClusterCfg a;
    sClusterCfg b;
    sMemSink    sink;
    static const char doc[] =
        "{\"version\":1,\"profile\":\"pylon_lv\","
        "\"members\":[\"sodas_a\",\"sodas_b\"],"
        "\"tune\":{\"riseRate_mA_per_s\":2500,\"chargeDerate_pm\":750}}";

    TEST_ASSERT(parse(doc, &a, NULL) == cluErr_ok);

    (void)memset(&sink, 0, sizeof(sink));
    TEST_ASSERT(ClusterCfg_Serialize(&a, mem_sink, &sink) == cluErr_ok);

    /* DATA-FAITHFUL, not byte-identical: what comes back out must parse to an
     * identical sClusterCfg. */
    TEST_ASSERT(parse(sink.buf, &b, NULL) == cluErr_ok);
    TEST_ASSERT_MEM_EQ(&a, &b, sizeof(a));
}

static void test_cfg_profile_token_is_opaque_and_round_trips(void)
{
    sClusterCfg a;
    sClusterCfg b;
    sMemSink    sink;

    /* Not a known dialect name — this module never compares it, and a token it
     * refused would be this module deciding what a CAN dialect is. */
    TEST_ASSERT(parse("{\"profile\":\"whatever-1\",\"members\":[\"a\"]}",
                      &a, NULL) == cluErr_ok);
    (void)memset(&sink, 0, sizeof(sink));
    TEST_ASSERT(ClusterCfg_Serialize(&a, mem_sink, &sink) == cluErr_ok);
    TEST_ASSERT(parse(sink.buf, &b, NULL) == cluErr_ok);
    TEST_ASSERT(strcmp(b.profile, "whatever-1") == 0);
}

/* ============================================================================
 * The nine name accessors
 * ============================================================================ */

/** No name may be NULL, empty, "?" for a legal value, or contain a character
 *  that would break the JSON it is emitted into unescaped. */
static void check_name(const char *s)
{
    TEST_ASSERT(s != NULL);
    if (s == NULL) {
        return;
    }
    TEST_ASSERT(s[0] != '\0');
    TEST_ASSERT(strcmp(s, "?") != 0);
    TEST_ASSERT(strchr(s, '"') == NULL);
    TEST_ASSERT(strchr(s, '\\') == NULL);
}

static void test_names_cover_every_enumerator_and_are_json_safe(void)
{
    unsigned i;

    for (i = 0u; i < (unsigned)cluCond_last; i++) {
        check_name(Cluster_CondName((uint8_t)i));
    }
    for (i = 0u; i < (unsigned)cluMember_last; i++) {
        check_name(Cluster_MemberStateName((uint8_t)i));
    }
    for (i = 0u; i < (unsigned)cluWhy_last; i++) {
        check_name(Cluster_MemberWhyName((uint8_t)i));
    }
    for (i = 0u; i < (unsigned)cluLoop_last; i++) {
        check_name(Cluster_LoopStateName((uint8_t)i));
    }
    for (i = 0u; i < (unsigned)cluRestart_last; i++) {
        check_name(Cluster_RestartName((uint8_t)i));
    }
    for (i = 0u; i < (unsigned)cluLimitWhy_last; i++) {
        check_name(Cluster_LimitWhyName((uint8_t)i));
    }

    /* The bit-mask enums have no _last sentinel and never will — an index
     * would be a second numbering to keep in step with the first — so they
     * are walked bit by bit against the highest value each defines. */
    for (i = 0u; (1u << i) <= (unsigned)cluAlarm_packCfgChanged; i++) {
        check_name(Cluster_AlarmName(1u << i));
    }
    for (i = 0u; (1u << i) <= (unsigned)cluMemFlag_limitSaturated; i++) {
        check_name(Cluster_MemberFlagName(1u << i));
    }
    for (i = 0u; (1u << i) <= (unsigned)cluField_switches; i++) {
        check_name(Cluster_FieldName(1u << i));
    }
}

/** RANGE-CHECK AND RETURN "?", NEVER NULL: both adapters pass the result
 *  straight into a %s, and the older PackCfg_TypeName NULL convention is not
 *  to be copied. */
static void test_names_are_bounded_and_never_null(void)
{
    TEST_ASSERT(strcmp(Cluster_CondName(200u), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_MemberStateName(200u), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_MemberWhyName(200u), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_LoopStateName(200u), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_RestartName(200u), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_LimitWhyName(200u), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_AlarmName(1u << 30), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_MemberFlagName(1u << 30), "?") == 0);
    TEST_ASSERT(strcmp(Cluster_FieldName(1u << 30), "?") == 0);
    /* A mask with two bits set names neither. */
    TEST_ASSERT(strcmp(Cluster_AlarmName(3u), "?") == 0);
}

/* ============================================================================ */

int main(void)
{
    printf("=== cluster_cfg ===\n");

    RUN_TEST(test_cfg_accepts_the_sodas_document);
    RUN_TEST(test_cfg_defaults_are_applied_when_keys_are_absent);
    RUN_TEST(test_cfg_reads_a_whole_tune_block);
    RUN_TEST(test_cfg_an_empty_cluster_is_a_valid_statement);

    RUN_TEST(test_cfg_rejects_an_unknown_key_by_name);
    RUN_TEST(test_cfg_rejects_a_duplicate_member_name);
    RUN_TEST(test_cfg_rejects_more_than_pack_max_members);
    RUN_TEST(test_cfg_rejects_an_illegal_member_name);
    RUN_TEST(test_cfg_rejects_bindFrac_below_loadTarget);
    RUN_TEST(test_cfg_rejects_a_riseRate_above_the_overflow_bound);
    RUN_TEST(test_cfg_rejects_limitMax_zero_or_above_the_macro);
    RUN_TEST(test_cfg_rejects_out_of_range_per_mille_tunables);
    RUN_TEST(test_cfg_refuses_a_newer_document_version);
    RUN_TEST(test_cfg_rejects_truncated_and_malformed_documents);

    RUN_TEST(test_cfg_export_round_trips);
    RUN_TEST(test_cfg_profile_token_is_opaque_and_round_trips);

    RUN_TEST(test_names_cover_every_enumerator_and_are_json_safe);
    RUN_TEST(test_names_are_bounded_and_never_null);

    printf("%s: %d failure(s)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}

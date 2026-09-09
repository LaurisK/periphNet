/*
 * cluster_cfg.c
 *
 * The cluster configuration — streaming parse, re-serialise, tunable bounds —
 * and all nine Cluster_*Name() accessors
 * (docs/design_battery_cluster.md §2, §6, constraint 12).
 *
 * LIBC ONLY, and deliberately so: this file is compiled by tests/ as well as
 * by the firmware, and it must never learn what a flash area is.  Persistence
 * lives in cluster.c.
 *
 * The parser is a hand-rolled pull parser for one fixed schema over
 * Shared/Json's one tokenizer, fed from an abstract byte source — the HTTP
 * body cursor on the device, a memory buffer in tests.  RAM cost is one small
 * lookahead buffer regardless of document size, and UNKNOWN KEYS ARE REJECTED
 * so a stale config fails by name instead of losing a setting silently.
 * pack_cfg.c is the precedent, down to the rd_* wrappers.
 *
 * NO PARSE RULE IS LOAD-BEARING ARITHMETIC ANY MORE, and that is worth saying
 * because two used to be.  `bindFrac_pm >= loadTarget_pm` went with the gate it
 * guarded; `riseRate_mA_per_s <= CLUSTER_RISE_MAX_MA_PER_S` bounded the rate
 * limiter's `rate * dt` and went with the rate limiter (§14.8).  Every field is
 * now bounded on its own account, and the limit's safety property
 * (`target <= sum(L_i)`) belongs to the arithmetic rather than to any setting.
 *
 * THE RETIRED KEYS ARE STILL REFUSED BY NAME, not ignored — unknown keys are
 * rejected, so a stored document naming one fails loudly instead of silently
 * losing the setting it thought it was making.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Cluster/cluster_cfg.h"

#include "json.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define CFG_TOK_MAX     32u     /* longest key/word we ever need to hold     */
/* Longest fragment the serialiser emits, with every operator-supplied string
 * at its worst-case escape expansion of six bytes per byte in.  A stack local
 * inside emit(), not a static — .bss is the tight region. */
#define CFG_LINE_MAX    320u

#define CLU_NAME_ESC_LEN     (((CLUSTER_NAME_LEN - 1u) * 6u) + 1u)
#define CLU_PROFILE_ESC_LEN  (((CLUSTER_PROFILE_LEN - 1u) * 6u) + 1u)

/* Private types ------------------------------------------------------------*/

typedef sJsonReader sCfgReader;

/* Private function prototypes ----------------------------------------------*/

static void SetFailure(sClusterCfgResult *res, int memberIdx,
                       const char *field, const char *reason);

/* Private functions --------------------------------------------------------*/

static void SetFailure(sClusterCfgResult *res, int memberIdx,
                       const char *field, const char *reason)
{
    if (res == NULL) {
        return;
    }
    res->ok        = 0;
    res->memberIdx = memberIdx;
    (void)snprintf(res->field,  sizeof(res->field),  "%s", field);
    (void)snprintf(res->reason, sizeof(res->reason), "%s", reason);
}

/* --- the reader -------------------------------------------------------- */

static int rd_peek(sCfgReader *r, char *out)
{
    const int ch = Json_Peek(r);

    if (ch < 0) {
        return 0;
    }
    *out = (char)ch;
    return 1;
}

static int rd_bump(sCfgReader *r, char *out)
{
    const int ch = Json_Get(r);

    if (ch < 0) {
        return 0;
    }
    *out = (char)ch;
    return 1;
}

static void rd_take(sCfgReader *r)
{
    (void)Json_Get(r);
}

static int rd_skip_ws(sCfgReader *r)
{
    return (Json_SkipWs(r) >= 0) ? 1 : 0;
}

static int rd_expect(sCfgReader *r, char want)
{
    char c;

    if (!rd_skip_ws(r) || !rd_bump(r, &c)) {
        return 0;
    }
    return (c == want) ? 1 : 0;
}

static int rd_string(sCfgReader *r, char *out, uint32_t cap)
{
    return Json_ReadString(r, out, cap);
}

/* REJECTS rather than wraps: signed overflow is undefined behaviour, and a
 * wrapped value is worse than a rejected one because it passes every
 * downstream check. */
static int rd_number(sCfgReader *r, int32_t *out)
{
    return Json_ReadI32(r, out);
}

/* --- validators -------------------------------------------------------- */

/** The same alphabet pack_cfg.c accepts for a pack name, because a member IS
 *  a pack name and the two must agree or a legal pack could never be a legal
 *  member. */
static int name_is_legal(const char *s)
{
    uint32_t i;

    if ((s == NULL) || (s[0] == '\0')) {
        return 0;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        const char c = s[i];

        if (((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) ||
            ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-')) {
            continue;
        }
        return 0;
    }
    return (i <= (CLUSTER_NAME_LEN - 1u)) ? 1 : 0;
}

/** The profile is an OPAQUE TOKEN.  It is handed to the frame source verbatim
 *  and never compared here — R4.8 asks for it in this record and R4.5 forbids
 *  this module knowing what a CAN dialect is, and opacity is what satisfies
 *  both.  sPackCfgEntry.bind uses the identical trick. */
static int profile_is_legal(const char *s)
{
    uint32_t i;

    if (s == NULL) {
        return 0;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        if (((unsigned char)s[i] < 0x20u) || ((unsigned char)s[i] > 0x7Eu)) {
            return 0;
        }
    }
    return (i <= (CLUSTER_PROFILE_LEN - 1u)) ? 1 : 0;
}

/* --- the tune block ---------------------------------------------------- */

static int parse_tune(sCfgReader *r, sClusterTune *t, sClusterCfgResult *res)
{
    char key[CFG_TOK_MAX];
    char c;

    if (!rd_expect(r, '{')) {
        SetFailure(res, -1, "tune", "expected an object");
        return 0;
    }
    if (!rd_skip_ws(r) || !rd_peek(r, &c)) {
        SetFailure(res, -1, "tune", "truncated");
        return 0;
    }
    if (c == '}') {
        rd_take(r);
        return 1;                       /* present and empty: all defaults   */
    }

    for (;;) {
        int32_t v;

        if (!rd_string(r, key, sizeof(key))) {
            SetFailure(res, -1, "tune", "bad key");
            return 0;
        }
        if (!rd_expect(r, ':')) {
            SetFailure(res, -1, key, "expected ':'");
            return 0;
        }
        if (!rd_number(r, &v)) {
            SetFailure(res, -1, key, "expected an integer");
            return 0;
        }
        if (v < 0) {
            SetFailure(res, -1, key, "must not be negative");
            return 0;
        }

        if (strcmp(key, "limitMax_mA") == 0) {
            if (((uint32_t)v == 0u) || ((uint32_t)v > CLUSTER_LIMIT_MAX_MA)) {
                /* A configured ceiling on the emitted value.  The limit
                 * arithmetic is uint64 throughout and does not need it. */
                SetFailure(res, -1, key, "must be in (0, 1000000] mA");
                return 0;
            }
            t->limitMax_mA = (uint32_t)v;
        } else if (strcmp(key, "voltDiverge_mV") == 0) {
            t->voltDiverge_mV = (uint32_t)v;
        } else if (strcmp(key, "elecMaxAge_ms") == 0) {
            if (((uint32_t)v == 0u) || ((uint32_t)v > 0xFFFFu)) {
                SetFailure(res, -1, key, "must be in (0, 65535] ms");
                return 0;
            }
            t->elecMaxAge_ms = (uint16_t)v;
        } else if (strcmp(key, "safetyMargin_pm") == 0) {
            /* 1000 IS PERMITTED AND MEANS "NO MARGIN", which is a legitimate
             * (if brave) operator choice: the bound target <= sum(L_i) holds
             * at 1000 exactly as it does at 900. */
            if (((uint32_t)v == 0u) || ((uint32_t)v > 1000u)) {
                SetFailure(res, -1, key, "must be in (0, 1000] per-mille");
                return 0;
            }
            t->safetyMargin_pm = (uint16_t)v;
        } else if (strcmp(key, "lowLoadFloor_pm") == 0) {
            /* ZERO IS PERMITTED and means "always measure".  It is not
             * dangerous — loadMax_pm > 0 is checked separately, so a bus with
             * nothing flowing still predicts — but it does mean trusting a
             * share ratio taken at any current at all. */
            if ((uint32_t)v >= CLUSTER_LOW_LOAD_FLOOR_MAX_PM) {
                SetFailure(res, -1, key, "must be below 500 per-mille");
                return 0;
            }
            t->lowLoadFloor_pm = (uint16_t)v;
        } else if (strcmp(key, "predictDecay_pm") == 0) {
            if (((uint32_t)v == 0u) || ((uint32_t)v > 1000u)) {
                SetFailure(res, -1, key, "must be in (0, 1000] per-mille");
                return 0;
            }
            t->predictDecay_pm = (uint16_t)v;
        } else if (strcmp(key, "chargeDerate_pm") == 0) {
            if (((uint32_t)v == 0u) || ((uint32_t)v > 1000u)) {
                SetFailure(res, -1, key, "must be in (0, 1000] per-mille");
                return 0;
            }
            t->chargeDerate_pm = (uint16_t)v;
        } else if (strcmp(key, "dischargeDerate_pm") == 0) {
            if (((uint32_t)v == 0u) || ((uint32_t)v > 1000u)) {
                SetFailure(res, -1, key, "must be in (0, 1000] per-mille");
                return 0;
            }
            t->dischargeDerate_pm = (uint16_t)v;
        } else if (strcmp(key, "socDiverge_pm") == 0) {
            if ((uint32_t)v > 1000u) {
                SetFailure(res, -1, key, "must be at most 1000 per-mille");
                return 0;
            }
            t->socDiverge_pm = (uint16_t)v;
        } else if (strcmp(key, "shareDiverge_pm") == 0) {
            if ((uint32_t)v > 1000u) {
                SetFailure(res, -1, key, "must be at most 1000 per-mille");
                return 0;
            }
            t->shareDiverge_pm = (uint16_t)v;
        } else {
            SetFailure(res, -1, key, "unknown key");
            return 0;
        }

        if (!rd_skip_ws(r) || !rd_bump(r, &c)) {
            SetFailure(res, -1, "tune", "truncated");
            return 0;
        }
        if (c == '}') {
            return 1;
        }
        if (c != ',') {
            SetFailure(res, -1, "tune", "expected ',' or '}'");
            return 0;
        }
    }
}

/* Exported functions -------------------------------------------------------*/

void ClusterCfg_Defaults(sClusterTune *tune)
{
    if (tune == NULL) {
        return;
    }
    (void)memset(tune, 0, sizeof(*tune));
    tune->limitMax_mA        = CLUSTER_DFLT_LIMIT_MAX_MA;
    tune->voltDiverge_mV     = CLUSTER_DFLT_VOLT_DIVERGE_MV;
    tune->elecMaxAge_ms      = CLUSTER_DFLT_ELEC_MAX_AGE_MS;
    tune->safetyMargin_pm    = CLUSTER_DFLT_SAFETY_MARGIN_PM;
    tune->lowLoadFloor_pm    = CLUSTER_DFLT_LOW_LOAD_FLOOR_PM;
    tune->predictDecay_pm    = CLUSTER_DFLT_PREDICT_DECAY_PM;
    tune->chargeDerate_pm    = CLUSTER_DFLT_CHARGE_DERATE_PM;
    tune->dischargeDerate_pm = CLUSTER_DFLT_DISCHARGE_DERATE_PM;
    tune->socDiverge_pm      = CLUSTER_DFLT_SOC_DIVERGE_PM;
    tune->shareDiverge_pm    = CLUSTER_DFLT_SHARE_DIVERGE_PM;
}

int ClusterCfg_Parse(fClusterByteSource src, void *srcCtx, sClusterCfg *out,
                     sClusterCfgResult *res)
{
    sCfgReader r;
    char       key[CFG_TOK_MAX];
    char       c;

    if ((src == NULL) || (out == NULL)) {
        SetFailure(res, -1, "json", "no source");
        return cluErr_badArg;
    }

    Json_ReaderInit(&r, src, srcCtx);
    (void)memset(out, 0, sizeof(*out));
    ClusterCfg_Defaults(&out->tune);
    if (res != NULL) {
        (void)memset(res, 0, sizeof(*res));
        res->memberIdx = -1;
    }

    if (!rd_expect(&r, '{')) {
        SetFailure(res, -1, "json", "expected an object");
        return cluErr_badArg;
    }

    for (;;) {
        if (!rd_string(&r, key, sizeof(key))) {
            SetFailure(res, -1, "json", "bad key");
            return cluErr_badArg;
        }
        if (!rd_expect(&r, ':')) {
            SetFailure(res, -1, key, "expected ':'");
            return cluErr_badArg;
        }

        if (strcmp(key, "version") == 0) {
            int32_t v;

            if (!rd_number(&r, &v) || (v <= 0)) {
                SetFailure(res, -1, "version", "bad version");
                return cluErr_badArg;
            }
            if (v > (int32_t)CLUSTER_CFG_VERSION) {
                /* Reading a newer document with this image's field meanings
                 * would misread a tunable that bounds a current limit. */
                SetFailure(res, -1, "version",
                           "unsupported document version");
                return cluErr_badArg;
            }
            out->version = (uint16_t)v;
        } else if (strcmp(key, "profile") == 0) {
            if (!rd_string(&r, out->profile, sizeof(out->profile)) ||
                !profile_is_legal(out->profile)) {
                SetFailure(res, -1, "profile", "not a legal profile token");
                return cluErr_badArg;
            }
        } else if (strcmp(key, "members") == 0) {
            if (!rd_expect(&r, '[')) {
                SetFailure(res, -1, "members", "expected an array");
                return cluErr_badArg;
            }
            if (!rd_skip_ws(&r) || !rd_peek(&r, &c)) {
                SetFailure(res, -1, "json", "truncated");
                return cluErr_badArg;
            }
            if (c == ']') {
                rd_take(&r);        /* an empty cluster is a valid statement */
            } else {
                for (;;) {
                    uint8_t i;

                    if (out->count >= CLUSTER_PACK_MAX) {
                        SetFailure(res, (int)out->count, "members",
                                   "more than CLUSTER_PACK_MAX");
                        return cluErr_badArg;
                    }
                    if (!rd_string(&r, out->member[out->count],
                                   CLUSTER_NAME_LEN) ||
                        !name_is_legal(out->member[out->count])) {
                        SetFailure(res, (int)out->count, "members",
                                   "not a legal pack name");
                        return cluErr_badArg;
                    }
                    /* TWO SLOTS NAMING ONE PACK would count its amp-hours
                     * twice and let one battery bind the limit against
                     * itself. */
                    for (i = 0u; i < out->count; i++) {
                        if (strcmp(out->member[i],
                                   out->member[out->count]) == 0) {
                            SetFailure(res, (int)out->count, "members",
                                       "duplicate member name");
                            return cluErr_badArg;
                        }
                    }
                    out->count++;
                    if (res != NULL) {
                        res->members = out->count;
                    }

                    if (!rd_skip_ws(&r) || !rd_bump(&r, &c)) {
                        SetFailure(res, -1, "json", "truncated");
                        return cluErr_badArg;
                    }
                    if (c == ']') {
                        break;
                    }
                    if (c != ',') {
                        SetFailure(res, -1, "members",
                                   "expected ',' or ']'");
                        return cluErr_badArg;
                    }
                }
            }
        } else if (strcmp(key, "tune") == 0) {
            if (!parse_tune(&r, &out->tune, res)) {
                return cluErr_badArg;
            }
        } else {
            SetFailure(res, -1, key, "unknown key");
            return cluErr_badArg;
        }

        if (!rd_skip_ws(&r) || !rd_bump(&r, &c)) {
            SetFailure(res, -1, "json", "truncated");
            return cluErr_badArg;
        }
        if (c == '}') {
            break;
        }
        if (c != ',') {
            SetFailure(res, -1, "json", "expected ',' or '}'");
            return cluErr_badArg;
        }
    }

    if (r.ioErr != 0u) {
        SetFailure(res, -1, "json", "source error");
        return cluErr_badArg;
    }

    /* NO CROSS-FIELD RULE SURVIVES REVISION 3.  Every tune field is bounded
     * on its own, and the limit's safety property (target <= sum(L_i)) is a
     * property of the arithmetic rather than of any pair of settings. */
    if (out->version == 0u) {
        out->version = CLUSTER_CFG_VERSION;
    }
    if (res != NULL) {
        res->ok      = 1;
        res->members = out->count;
    }
    return cluErr_ok;
}

/**
 * @brief Push one formatted fragment at the sink.
 * @retval cluErr_ok, cluErr_badArg if it did not fit or the sink refused
 * @note Bounded, not clamped: a fragment that does not fit is a bug in this
 *       function's buffer sizing, not something to truncate silently into a
 *       document somebody will try to upload again.
 */
static int emit(fClusterByteSink sink, void *ctx, const char *fmt, ...)
{
    char    line[CFG_LINE_MAX];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if ((n < 0) || (n >= (int)sizeof(line))) {
        return cluErr_badArg;
    }
    return (sink(ctx, line, (uint32_t)n) < 0) ? cluErr_badArg : cluErr_ok;
}

int ClusterCfg_Serialize(const sClusterCfg *cfg, fClusterByteSink sink,
                         void *ctx)
{
    char    profileEsc[CLU_PROFILE_ESC_LEN];
    uint8_t i;

    if ((cfg == NULL) || (sink == NULL)) {
        return cluErr_badArg;
    }

    /* Operator text, and the parser accepts escapes in it, so it is escaped on
     * the way out or the document will not re-upload. */
    (void)Json_Escape(profileEsc, sizeof(profileEsc), cfg->profile);

    if (emit(sink, ctx, "{\"version\":%u,\"profile\":\"%s\",\"members\":[",
             (unsigned)cfg->version, profileEsc) != cluErr_ok) {
        return cluErr_badArg;
    }

    for (i = 0u; i < cfg->count; i++) {
        char nameEsc[CLU_NAME_ESC_LEN];

        (void)Json_Escape(nameEsc, sizeof(nameEsc), cfg->member[i]);
        if (emit(sink, ctx, "%s\"%s\"", (i == 0u) ? "" : ",",
                 nameEsc) != cluErr_ok) {
            return cluErr_badArg;
        }
    }

    if (emit(sink, ctx,
             "],\"tune\":{"
             "\"limitMax_mA\":%lu,"
             "\"voltDiverge_mV\":%lu,"
             "\"elecMaxAge_ms\":%u,\"safetyMargin_pm\":%u,"
             "\"lowLoadFloor_pm\":%u,\"predictDecay_pm\":%u,"
             "\"chargeDerate_pm\":%u,"
             "\"dischargeDerate_pm\":%u,\"socDiverge_pm\":%u,"
             "\"shareDiverge_pm\":%u}}",
             (unsigned long)cfg->tune.limitMax_mA,
             (unsigned long)cfg->tune.voltDiverge_mV,
             (unsigned)cfg->tune.elecMaxAge_ms,
             (unsigned)cfg->tune.safetyMargin_pm,
             (unsigned)cfg->tune.lowLoadFloor_pm,
             (unsigned)cfg->tune.predictDecay_pm,
             (unsigned)cfg->tune.chargeDerate_pm,
             (unsigned)cfg->tune.dischargeDerate_pm,
             (unsigned)cfg->tune.socDiverge_pm,
             (unsigned)cfg->tune.shareDiverge_pm) != cluErr_ok) {
        return cluErr_badArg;
    }
    return cluErr_ok;
}

/* --- the eight enum-name accessors ---------------------------------------
 *
 * WRITTEN AS A SWITCH WITH THE FALLBACK *AFTER* IT, never in a `default:` —
 * a `default:` satisfies -Wswitch and so defeats the whole mechanism.  Each
 * switches on (eType)v and NOT on the uint8_t parameter, because
 * -Werror=switch does nothing at all on a uint8_t.  With the error scoped
 * onto this file in CMakeLists.txt (and onto tests/'s copy), adding an
 * enumerator without a name FAILS THE BUILD rather than shipping a "?" for
 * somebody to find in the field.
 *
 * They live in the CONFIG file for the same reason pack_cfg.c holds the pack
 * module's: this is the file tests/ links, and pack.c / cluster.c are not.
 *
 * THESE STRINGS ARE EMITTED INTO JSON by /api/cluster/status.  None contains
 * a '"' or a '\', so none needs escaping at the call site; a future wording
 * that quoted an operator-supplied token would produce malformed JSON on a
 * live endpoint.  Keep them plain, or escape them where they are emitted —
 * the host test asserts it.
 *
 * The bit-mask accessors take a uint32_t MASK WITH ONE BIT SET, not an index:
 * their enums have no _last sentinel and never will, so an index would be a
 * second numbering to keep in step with the first.
 */

const char *Cluster_CondName(uint8_t cond)
{
    switch ((eClusterCondition)cond) {
    case cluCond_unprovisioned: return "unprovisioned";
    case cluCond_absent:        return "absent";
    case cluCond_degraded:      return "degraded";
    case cluCond_online:        return "online";
    case cluCond_last:          break;
    }
    return "?";
}

const char *Cluster_MemberStateName(uint8_t state)
{
    switch ((eClusterMemberState)state) {
    case cluMember_unresolved:    return "unresolved";
    case cluMember_absent:        return "absent";
    case cluMember_stale:         return "stale";
    case cluMember_present:       return "present";
    case cluMember_participating: return "participating";
    case cluMember_last:          break;
    }
    return "?";
}

const char *Cluster_MemberWhyName(uint8_t why)
{
    switch ((eClusterMemberWhy)why) {
    case cluWhy_none:            return "participating";
    case cluWhy_nameUnresolved:  return "no configured pack answers to that name";
    case cluWhy_notOnline:       return "the pack is not online";
    case cluWhy_electricalStale: return "the electrical group is too old to use";
    case cluWhy_noLimitCap:      return "the pack publishes no current limits";
    case cluWhy_zeroLimits:      return "both of its current limits are zero";
    case cluWhy_switchesOpen:    return "both of its switches are open";
    case cluWhy_implausible:     return "a reading was outside the plausible domain";
    case cluWhy_last:            break;
    }
    return "?";
}

const char *Cluster_LoopStateName(uint8_t state)
{
    switch ((eClusterLoopState)state) {
    case cluLoop_idle:      return "idle";
    case cluLoop_measured:  return "measured";
    case cluLoop_predicted: return "predicted";
    case cluLoop_last:      break;
    }
    return "?";
}

const char *Cluster_LimitWhyName(uint8_t why)
{
    switch ((eClusterLimitWhy)why) {
    case cluLimitWhy_noParticipant: return "noParticipant";
    case cluLimitWhy_forbidden:     return "forbidden";
    case cluLimitWhy_measured:      return "measured";
    case cluLimitWhy_predicted:     return "predicted";
    case cluLimitWhy_ceiling:       return "ceiling";
    case cluLimitWhy_last:          break;
    }
    return "?";
}

const char *Cluster_AlarmName(uint32_t bit)
{
    switch ((eClusterAlarm)bit) {
    case cluAlarm_noMembers:        return "noMembers";
    case cluAlarm_allOffline:       return "allOffline";
    case cluAlarm_memberLost:       return "memberLost";
    case cluAlarm_socDiverge:       return "socDiverge";
    case cluAlarm_shareDiverge:     return "shareDiverge";
    case cluAlarm_busSplit:         return "busSplit";
    case cluAlarm_circulating:      return "circulating";
    case cluAlarm_chargeForbidden:  return "chargeForbidden";
    case cluAlarm_dischargeForbid:  return "dischargeForbidden";
    case cluAlarm_voltLimitMissing: return "voltLimitMissing";
    case cluAlarm_nameUnresolved:   return "nameUnresolved";
    case cluAlarm_implausible:      return "implausible";
    case cluAlarm_packCfgChanged:   return "packCfgChanged";
    }
    return "?";
}

const char *Cluster_MemberFlagName(uint32_t bit)
{
    switch ((eClusterMemberFlag)bit) {
    case cluMemFlag_bindingCharge:    return "bindingCharge";
    case cluMemFlag_bindingDischarge: return "bindingDischarge";
    case cluMemFlag_socOutlier:       return "socOutlier";
    case cluMemFlag_shareOutlier:     return "shareOutlier";
    case cluMemFlag_circulating:      return "circulating";
    case cluMemFlag_implausible:      return "implausible";
    case cluMemFlag_limitSaturated:   return "limitSaturated";
    }
    return "?";
}

const char *Cluster_FieldName(uint32_t bit)
{
    switch ((eClusterField)bit) {
    case cluField_voltage:            return "voltage";
    case cluField_current:            return "current";
    case cluField_soc:                return "soc";
    case cluField_soh:                return "soh";
    case cluField_chargeLimit:        return "chargeLimit";
    case cluField_dischargeLimit:     return "dischargeLimit";
    case cluField_chargeVoltLimit:    return "chargeVoltLimit";
    case cluField_dischargeVoltLimit: return "dischargeVoltLimit";
    case cluField_temperature:        return "temperature";
    case cluField_switches:           return "switches";
    }
    return "?";
}

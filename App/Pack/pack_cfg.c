/*
 * pack_cfg.c
 *
 * The uploaded pack configuration: streaming parse, re-serialise, name
 * tables (docs/design_battery_pack.md §12).
 *
 * LIBC ONLY, and deliberately so: this file is compiled by tests/ as well as
 * by the firmware, and it must never learn what a flash area is.  Persistence
 * lives in pack.c.
 *
 * The parser is a hand-rolled pull parser for one fixed schema, fed from an
 * abstract byte source — the HTTP body cursor on the device, a memory buffer
 * in tests.  RAM cost is one small lookahead buffer regardless of document
 * size, and UNKNOWN KEYS ARE REJECTED so a stale config fails by name instead
 * of losing a setting silently.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_cfg.h"

#include "json.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define CFG_TOK_MAX     32u     /* longest key/word we ever need to hold     */
/* Longest fragment the serialiser emits: the pack header, with `name` and
 * `bind` at their worst-case escape expansion of six bytes per byte in.  A
 * stack local inside emit(), not a static -- .bss is the tight region. */
#define CFG_LINE_MAX    384u

/* Escaped worst case is six bytes out per byte in (\u00XX). */
#define PACK_NAME_ESC_LEN   (((PACK_NAME_LEN - 1u) * 6u) + 1u)
#define PACK_BIND_ESC_LEN   (((PACK_BIND_LEN - 1u) * 6u) + 1u)

/* Private types ------------------------------------------------------------*/

/* The tokenizer is Shared/Json (docs/task_json_module.md §3.2).  The rd_*
 * wrappers below keep this file's 1-or-0 convention -- and its habit of
 * peeking at the next structural character before deciding what to parse --
 * without a second copy of a JSON reader behind them. */
typedef sJsonReader sCfgReader;

/* Private variables --------------------------------------------------------*/

/* The static blueprint ceilings a `commands` block is checked against at
 * PARSE time.  A per-instance bind narrows further at run time; this is the
 * limit the parser can see without a transport (§12). */
static const sPackCmdBound s_jkBounds[] = {
    { 0,      1,      packCmd_chargeEnable    },
    { 0,      1,      packCmd_dischargeEnable },
    { 0,      1,      packCmd_balanceEnable   },
    { 0, 500000,      packCmd_chargeLimit     },  /* 500 A, the JK's own max */
    { 0, 500000,      packCmd_dischargeLimit  },
};

static const sPackTypeLimits s_limits[packType_last] = {
    [packType_jkBms] = {
        s_jkBounds,
        PACK_CMD_BIT(packCmd_chargeEnable)    |
        PACK_CMD_BIT(packCmd_dischargeEnable) |
        PACK_CMD_BIT(packCmd_balanceEnable)   |
        PACK_CMD_BIT(packCmd_chargeLimit)     |
        PACK_CMD_BIT(packCmd_dischargeLimit),
        (uint8_t)(sizeof(s_jkBounds) / sizeof(s_jkBounds[0])),
    },
    /* A Pylontech-speaking pack accepts nothing inbound in the dialect as
     * implemented — §18 item 2 is the open question, not a guess made here. */
    [packType_pylontech] = { NULL, 0u, 0u },
};

static const char *const s_typeNames[packType_last] = {
    [packType_jkBms]     = "jkbms",
    [packType_pylontech] = "pylontech",
};

static const char *const s_chemNames[packChem_last] = {
    [packChem_lfp]   = "lfp",
    [packChem_liIon] = "liion",
    [packChem_lto]   = "lto",
};

static const char *const s_cmdNames[packCmd_last] = {
    [packCmd_chargeEnable]    = "chargeEnable",
    [packCmd_dischargeEnable] = "dischargeEnable",
    [packCmd_balanceEnable]   = "balanceEnable",
    [packCmd_chargeLimit]     = "chargeLimit",
    [packCmd_dischargeLimit]  = "dischargeLimit",
};

/* Private function prototypes ----------------------------------------------*/

/* Local, deliberately not PackFsm_FindBound: tests/ links this file on its
 * own, and a config parser has no business depending on the condition FSM. */
static const sPackCmdBound *find_bound(const sPackCmdBound *bounds,
                                       uint8_t count, ePackCmdId cmd)
{
    uint8_t i;

    if (bounds == NULL) {
        return NULL;
    }
    for (i = 0u; i < count; i++) {
        if (bounds[i].cmd == cmd) {
            return &bounds[i];
        }
    }
    return NULL;
}

static void SetFailure(sPackCfgResult *res, int packIdx, const char *field,
                       const char *reason);

/* Private functions --------------------------------------------------------*/

static void SetFailure(sPackCfgResult *res, int packIdx, const char *field,
                       const char *reason)
{
    if (res == NULL) {
        return;
    }
    res->ok      = 0;
    res->packIdx = packIdx;
    (void)snprintf(res->field, sizeof(res->field), "%s", field);
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

/** Consume a byte rd_peek() has already looked at. */
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

/* A JSON string.  Escapes ARE handled now: this file used to accept `\` into
 * the field, which then went to flash and came back out raw into every
 * response that named the pack (docs/task_json_module.md §1.3). */
static int rd_string(sCfgReader *r, char *out, uint32_t cap)
{
    return Json_ReadString(r, out, cap);
}

/* REJECTS rather than wraps.  Signed overflow is undefined behaviour, and a
 * wrapped value is worse than a rejected one: a 14-digit nameplate_ah once
 * wrapped into a small positive number and passed every downstream check. */
static int rd_number(sCfgReader *r, int32_t *out)
{
    return Json_ReadI32(r, out);
}

static int rd_bool(sCfgReader *r, int *out)
{
    return Json_ReadBool(r, out);
}

/* --- validators -------------------------------------------------------- */

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
    return (i <= PACK_CFG_NAME_MAX_CHARS) ? 1 : 0;
}

/* The core validates SHAPE only.  It does not know that a jkbms token looks
 * like "<port>:<slaveAddr>" and must not learn: syntax is the core's, meaning
 * is the type's (§12).  A well-formed token naming nothing real is accepted
 * here and fails later at bind() as packWhy_noBinding. */
static int bind_is_legal(const char *s)
{
    uint32_t i;

    if ((s == NULL) || (s[0] == '\0')) {
        return 0;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        if (((unsigned char)s[i] < 0x20u) || ((unsigned char)s[i] > 0x7Eu)) {
            return 0;
        }
    }
    return (i <= (PACK_BIND_LEN - 1u)) ? 1 : 0;
}

/* --- the commands block ------------------------------------------------ */

static int parse_commands(sCfgReader *r, sPackCfgEntry *e, int idx,
                          sPackCfgResult *res)
{
    const sPackTypeLimits *lim = PackCfg_TypeLimits(e->typeId);
    char                   key[CFG_TOK_MAX];
    char                   c;

    /* PRESENT means allowlist: unlisted is forbidden.  That is what makes
     * "block absent" (allow all) and "block present but empty" (allow none)
     * two different, useful statements (§12). */
    e->cmdAllow   = 0u;
    e->boundCount = 0u;

    if (!rd_expect(r, '{')) {
        SetFailure(res, idx, "commands", "expected an object");
        return 0;
    }
    if (!rd_skip_ws(r) || !rd_peek(r, &c)) {
        SetFailure(res, idx, "commands", "truncated");
        return 0;
    }
    if (c == '}') {
        rd_take(r);
        return 1;                       /* present and empty: allow nothing */
    }

    for (;;) {
        ePackCmdId  cmd;
        int         enabled = 1;
        int32_t     lo      = 0;
        int32_t     hi      = 0;
        int         haveDomain = 0;

        if (!rd_string(r, key, sizeof(key))) {
            SetFailure(res, idx, "commands", "bad key");
            return 0;
        }
        if (PackCfg_CmdIdFromName(key, &cmd) != packErr_ok) {
            SetFailure(res, idx, key, "not a command");
            return 0;
        }
        if (!rd_expect(r, ':')) {
            SetFailure(res, idx, key, "expected ':'");
            return 0;
        }
        if (!rd_skip_ws(r) || !rd_peek(r, &c)) {
            SetFailure(res, idx, key, "truncated");
            return 0;
        }

        if (c == '{') {
            /* { "min_a": .., "max_a": .. } — amps in the document, mA in the
             * bound, because no float crosses this API. */
            char sub[CFG_TOK_MAX];
            int  haveLo = 0;
            int  haveHi = 0;

            rd_take(r);
            for (;;) {
                int32_t v;

                if (!rd_string(r, sub, sizeof(sub)) || !rd_expect(r, ':') ||
                    !rd_number(r, &v)) {
                    SetFailure(res, idx, key, "bad domain");
                    return 0;
                }
                /* rd_number rejects an overflowing literal, but amps ->
                 * milliamps multiplies by 1000 and can overflow all over
                 * again -- and the ceiling check below then runs on the
                 * WRAPPED value, so {"min_a":4294968} was accepted as
                 * 0.704 A.  Signed overflow is UB and the build has no
                 * -fwrapv, so bound the operand, not the product. */
                if ((v < -PACK_CFG_MAX_AMPS) || (v > PACK_CFG_MAX_AMPS)) {
                    SetFailure(res, idx, sub, "out of range");
                    return 0;
                }
                if (strcmp(sub, "min_a") == 0) {
                    lo = v * 1000; haveLo = 1;
                } else if (strcmp(sub, "max_a") == 0) {
                    hi = v * 1000; haveHi = 1;
                } else {
                    SetFailure(res, idx, sub, "unknown key");
                    return 0;
                }
                if (!rd_skip_ws(r) || !rd_bump(r, &c)) {
                    SetFailure(res, idx, key, "truncated");
                    return 0;
                }
                if (c == '}') {
                    break;
                }
                if (c != ',') {
                    SetFailure(res, idx, key, "expected ',' or '}'");
                    return 0;
                }
            }
            if (!haveLo || !haveHi || (lo > hi)) {
                SetFailure(res, idx, key, "bad domain");
                return 0;
            }
            haveDomain = 1;
        } else if ((c == 't') || (c == 'f')) {
            if (!rd_bool(r, &enabled)) {
                SetFailure(res, idx, key, "expected true or false");
                return 0;
            }
        } else {
            SetFailure(res, idx, key, "expected a bool or a domain");
            return 0;
        }

        if (enabled != 0) {
            if ((lim == NULL) ||
                ((lim->cmdsMax & PACK_CMD_BIT(cmd)) == 0u)) {
                SetFailure(res, idx, key, "this type does not offer it");
                return 0;
            }
            /* A repeated command key is a malformed document, not a
             * silent overwrite: two domains for one command give the
             * operator no way to know which one is enforced. */
            if ((e->cmdAllow & PACK_CMD_BIT(cmd)) != 0u) {
                SetFailure(res, idx, key, "duplicate command");
                return 0;
            }
            e->cmdAllow |= PACK_CMD_BIT(cmd);

            if (haveDomain) {
                const sPackCmdBound *ceil =
                    find_bound(lim->bounds, lim->count, cmd);

                /* NARROWING ONLY.  Wider than the blueprint is a 422 here,
                 * never a silent clamp (§12). */
                if ((ceil == NULL) ||
                    (lo < ceil->min_scaled) || (hi > ceil->max_scaled)) {
                    SetFailure(res, idx, key, "bound is wider than the type");
                    return 0;
                }
                /* BOUND THE WRITE.  boundCount rises once per key carrying
                 * a domain, and a JSON object may repeat a key as often as it
                 * likes -- so without this the operator's document decides how
                 * far past a 6-element array to write.  pack_jkbms.c's
                 * equivalent loop has always had this guard; this one did not.
                 * Proven with ASan: ~87 repeats escapes sPackCfg entirely. */
                if (e->boundCount >= (uint8_t)packCmd_last) {
                    SetFailure(res, idx, key, "too many command domains");
                    return 0;
                }
                e->bounds[e->boundCount].cmd        = cmd;
                e->bounds[e->boundCount].min_scaled = lo;
                e->bounds[e->boundCount].max_scaled = hi;
                e->boundCount++;
            }
        }

        if (!rd_skip_ws(r) || !rd_bump(r, &c)) {
            SetFailure(res, idx, "commands", "truncated");
            return 0;
        }
        if (c == '}') {
            return 1;
        }
        if (c != ',') {
            SetFailure(res, idx, "commands", "expected ',' or '}'");
            return 0;
        }
    }
}

/* --- one pack object --------------------------------------------------- */

static int parse_pack(sCfgReader *r, sPackCfg *cfg, int idx,
                      sPackCfgResult *res)
{
    sPackCfgEntry *e = &cfg->pack[idx];
    char           key[CFG_TOK_MAX];
    char           c;
    int            haveType = 0;
    int            haveStale = 0;
    uint8_t        i;

    memset(e, 0, sizeof(*e));
    e->cmdAllow = 0xFFFFFFFFu;      /* absent `commands` = allow everything */

    if (!rd_expect(r, '{')) {
        SetFailure(res, idx, "packs", "expected an object");
        return 0;
    }

    for (;;) {
        if (!rd_string(r, key, sizeof(key))) {
            SetFailure(res, idx, "json", "bad key");
            return 0;
        }
        if (!rd_expect(r, ':')) {
            SetFailure(res, idx, key, "expected ':'");
            return 0;
        }

        if (strcmp(key, "name") == 0) {
            if (!rd_string(r, e->name, sizeof(e->name)) ||
                !name_is_legal(e->name)) {
                SetFailure(res, idx, "name", "not a legal name");
                return 0;
            }
        } else if (strcmp(key, "type") == 0) {
            char t[CFG_TOK_MAX];

            if (!rd_string(r, t, sizeof(t)) ||
                (PackCfg_TypeIdFromName(t, &e->typeId) != packErr_ok)) {
                SetFailure(res, idx, "type", "unknown type");
                return 0;
            }
            haveType = 1;
        } else if (strcmp(key, "bind") == 0) {
            if (!rd_string(r, e->bind, sizeof(e->bind)) ||
                !bind_is_legal(e->bind)) {
                SetFailure(res, idx, "bind", "not a legal bind token");
                return 0;
            }
        } else if (strcmp(key, "chemistry") == 0) {
            char t[CFG_TOK_MAX];

            if (!rd_string(r, t, sizeof(t)) ||
                (PackCfg_ChemFromName(t, &e->chemistry) != packErr_ok)) {
                SetFailure(res, idx, "chemistry", "unknown chemistry");
                return 0;
            }
        } else if (strcmp(key, "nameplate_ah") == 0) {
            int32_t v;

            if (!rd_number(r, &v) || (v <= 0)) {
                SetFailure(res, idx, "nameplate_ah", "must be positive");
                return 0;
            }
            e->nameplate_mAh = (uint32_t)v * 1000u;
        } else if (strcmp(key, "cells") == 0) {
            int32_t v;

            if (!rd_number(r, &v) || (v <= 0) ||
                (v > (int32_t)PACK_CELLS_MAX)) {
                SetFailure(res, idx, "cells", "out of range");
                return 0;
            }
            e->cellCount = (uint8_t)v;
        } else if (strcmp(key, "staleAfter_ms") == 0) {
            int32_t v;

            if (!rd_number(r, &v) || (v <= 0)) {
                SetFailure(res, idx, "staleAfter_ms", "must be positive");
                return 0;
            }
            e->staleAfter_ms = (uint32_t)v;
            haveStale = 1;
        } else if (strcmp(key, "commands") == 0) {
            if (!haveType) {
                SetFailure(res, idx, "commands", "must follow \"type\"");
                return 0;
            }
            if (!parse_commands(r, e, idx, res)) {
                return 0;
            }
        } else {
            SetFailure(res, idx, key, "unknown key");
            return 0;
        }

        if (!rd_skip_ws(r) || !rd_bump(r, &c)) {
            SetFailure(res, idx, "json", "truncated");
            return 0;
        }
        if (c == '}') {
            break;
        }
        if (c != ',') {
            SetFailure(res, idx, "json", "expected ',' or '}'");
            return 0;
        }
    }

    if (!haveType) {
        SetFailure(res, idx, "type", "missing");
        return 0;
    }
    if (e->name[0] == '\0') {
        SetFailure(res, idx, "name", "missing");
        return 0;
    }
    if (e->bind[0] == '\0') {
        SetFailure(res, idx, "bind", "missing");
        return 0;
    }

    /* Names are the identity, so they must be unique (§12). */
    for (i = 0u; i < (uint8_t)idx; i++) {
        if (strcmp(cfg->pack[i].name, e->name) == 0) {
            SetFailure(res, idx, "name", "duplicate");
            return 0;
        }
        /* And so must the BIND TOKEN, for a different reason: two entries
         * naming one physical battery both match every sample it sends, so
         * the board reports two healthy packs where one exists and a summing
         * consumer doubles the site's capacity.  maxInstances cannot catch
         * this -- the collision is per-token, not per-type. */
        if (strcmp(cfg->pack[i].bind, e->bind) == 0) {
            SetFailure(res, idx, "bind", "duplicate");
            return 0;
        }
    }

    /* Per-type defaults, applied only where the document was silent. */
    if (!haveStale) {
        e->staleAfter_ms = (e->typeId == (uint8_t)packType_pylontech)
                         ? PACK_CFG_PYLON_STALE_MS : PACK_CFG_JK_STALE_MS;
    }
    return 1;
}

/* Exported functions -------------------------------------------------------*/

int PackCfg_Parse(fPackByteSource src, void *srcCtx, sPackCfg *out,
                  sPackCfgResult *res)
{
    sCfgReader r;
    char       key[CFG_TOK_MAX];
    char       c;

    if ((src == NULL) || (out == NULL)) {
        SetFailure(res, -1, "json", "no source");
        return packErr_badArg;
    }

    Json_ReaderInit(&r, src, srcCtx);
    memset(out, 0, sizeof(*out));
    if (res != NULL) {
        memset(res, 0, sizeof(*res));
        res->packIdx = -1;
    }

    if (!rd_expect(&r, '{')) {
        SetFailure(res, -1, "json", "expected an object");
        return packErr_badArg;
    }

    for (;;) {
        if (!rd_string(&r, key, sizeof(key))) {
            SetFailure(res, -1, "json", "bad key");
            return packErr_badArg;
        }
        if (!rd_expect(&r, ':')) {
            SetFailure(res, -1, key, "expected ':'");
            return packErr_badArg;
        }

        if (strcmp(key, "version") == 0) {
            int32_t v;

            if (!rd_number(&r, &v) || (v <= 0)) {
                SetFailure(res, -1, "version", "bad version");
                return packErr_badArg;
            }
            if (v > (int32_t)PACK_CFG_VERSION) {
                /* Reading a newer document with this image's field meanings
                 * would misread a pack's binding.  Refuse instead. */
                SetFailure(res, -1, "version", "unsupported document version");
                return packErr_badArg;
            }
            out->version = (uint16_t)v;
        } else if (strcmp(key, "packs") == 0) {
            if (!rd_expect(&r, '[')) {
                SetFailure(res, -1, "packs", "expected an array");
                return packErr_badArg;
            }
            if (!rd_skip_ws(&r) || !rd_peek(&r, &c)) {
                SetFailure(res, -1, "json", "truncated");
                return packErr_badArg;
            }
            if (c == ']') {
                rd_take(&r);        /* an empty fleet is a valid statement  */
            } else {
                for (;;) {
                    if (out->count >= PACK_MAX) {
                        SetFailure(res, out->count, "packs",
                                   "more than PACK_MAX");
                        return packErr_badArg;
                    }
                    if (!parse_pack(&r, out, (int)out->count, res)) {
                        return packErr_badArg;
                    }
                    out->count++;
                    if (res != NULL) {
                        res->packs = out->count;
                    }

                    if (!rd_skip_ws(&r) || !rd_bump(&r, &c)) {
                        SetFailure(res, -1, "json", "truncated");
                        return packErr_badArg;
                    }
                    if (c == ']') {
                        break;
                    }
                    if (c != ',') {
                        SetFailure(res, -1, "packs", "expected ',' or ']'");
                        return packErr_badArg;
                    }
                }
            }
        } else {
            SetFailure(res, -1, key, "unknown key");
            return packErr_badArg;
        }

        if (!rd_skip_ws(&r) || !rd_bump(&r, &c)) {
            SetFailure(res, -1, "json", "truncated");
            return packErr_badArg;
        }
        if (c == '}') {
            break;
        }
        if (c != ',') {
            SetFailure(res, -1, "json", "expected ',' or '}'");
            return packErr_badArg;
        }
    }

    if (r.ioErr != 0u) {
        SetFailure(res, -1, "json", "source error");
        return packErr_badArg;
    }
    if (out->version == 0u) {
        out->version = PACK_CFG_VERSION;
    }
    if (res != NULL) {
        res->ok    = 1;
        res->packs = out->count;
    }
    return packErr_ok;
}

/**
 * @brief Push one formatted fragment at the sink.
 * @retval 0 on success, packErr_badArg if it did not fit or the sink refused
 * @note Bounded, not clamped: a fragment that does not fit is a bug in this
 *       function's buffer sizing, not something to truncate silently into a
 *       document somebody will try to upload again.
 */
static int emit(fPackByteSink sink, void *ctx, const char *fmt, ...)
{
    char    line[CFG_LINE_MAX];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if ((n < 0) || (n >= (int)sizeof(line))) {
        return packErr_badArg;
    }
    return (sink(ctx, line, (uint32_t)n) < 0) ? packErr_badArg : packErr_ok;
}

int PackCfg_Serialize(const sPackCfg *cfg, fPackByteSink sink, void *ctx)
{
    uint8_t i;

    if ((cfg == NULL) || (sink == NULL)) {
        return packErr_badArg;
    }

    if (emit(sink, ctx, "{\"version\":%u,\"packs\":[",
             (unsigned)cfg->version) != packErr_ok) {
        return packErr_badArg;
    }

    for (i = 0u; i < cfg->count; i++) {
        const sPackCfgEntry *e = &cfg->pack[i];
        char                 nameEsc[PACK_NAME_ESC_LEN];
        char                 bindEsc[PACK_BIND_ESC_LEN];

        /* `name` and `bind` are the operator's text and the parser now
         * accepts escapes in both, so they are escaped on the way out or the
         * document will not re-upload (docs/task_json_module.md §1.3). */
        (void)Json_Escape(nameEsc, sizeof(nameEsc), e->name);
        (void)Json_Escape(bindEsc, sizeof(bindEsc), e->bind);

        if (emit(sink, ctx,
                 "%s{\"name\":\"%s\",\"type\":\"%s\",\"bind\":\"%s\","
                 "\"nameplate_ah\":%lu,\"cells\":%u,\"chemistry\":\"%s\","
                 "\"staleAfter_ms\":%lu",
                 (i == 0u) ? "" : ",",
                 nameEsc, PackCfg_TypeName(e->typeId), bindEsc,
                 (unsigned long)(e->nameplate_mAh / 1000u),
                 (unsigned)e->cellCount, PackCfg_ChemName(e->chemistry),
                 (unsigned long)e->staleAfter_ms) != packErr_ok) {
            return packErr_badArg;
        }

        /* Absent means "allow everything", so an all-ones mask re-serialises
         * as an omission rather than as a list — the document round-trips to
         * the same MEANING, which is what §12 asks of an export. */
        if (e->cmdAllow != 0xFFFFFFFFu) {
            uint32_t cmd;
            int      first = 1;

            if (emit(sink, ctx, ",\"commands\":{") != packErr_ok) {
                return packErr_badArg;
            }
            for (cmd = 1u; cmd < (uint32_t)packCmd_last; cmd++) {
                const sPackCmdBound *bd;
                int                  r;

                if ((e->cmdAllow & PACK_CMD_BIT(cmd)) == 0u) {
                    continue;
                }
                bd = find_bound(e->bounds, e->boundCount, (ePackCmdId)cmd);
                if (bd != NULL) {
                    r = emit(sink, ctx,
                             "%s\"%s\":{\"min_a\":%ld,\"max_a\":%ld}",
                             first ? "" : ",",
                             PackCfg_CmdName((ePackCmdId)cmd),
                             (long)(bd->min_scaled / 1000),
                             (long)(bd->max_scaled / 1000));
                } else {
                    r = emit(sink, ctx, "%s\"%s\":true",
                             first ? "" : ",",
                             PackCfg_CmdName((ePackCmdId)cmd));
                }
                if (r != packErr_ok) {
                    return packErr_badArg;
                }
                first = 0;
            }
            if (sink(ctx, "}", 1u) < 0) {
                return packErr_badArg;
            }
        }
        if (sink(ctx, "}", 1u) < 0) {
            return packErr_badArg;
        }
    }

    return (sink(ctx, "]}", 2u) < 0) ? packErr_badArg : packErr_ok;
}

const sPackTypeLimits *PackCfg_TypeLimits(uint8_t typeId)
{
    if (typeId >= (uint8_t)packType_last) {
        return NULL;
    }
    return &s_limits[typeId];
}

int PackCfg_TypeIdFromName(const char *name, uint8_t *out)
{
    uint8_t i;

    if ((name == NULL) || (out == NULL)) {
        return -1;
    }
    for (i = 0u; i < (uint8_t)packType_last; i++) {
        if ((s_typeNames[i] != NULL) && (strcmp(s_typeNames[i], name) == 0)) {
            *out = i;
            return packErr_ok;
        }
    }
    return packErr_notFound;
}

const char *PackCfg_TypeName(uint8_t typeId)
{
    if (typeId >= (uint8_t)packType_last) {
        return NULL;
    }
    return s_typeNames[typeId];
}

int PackCfg_ChemFromName(const char *name, uint8_t *out)
{
    uint8_t i;

    if ((name == NULL) || (out == NULL)) {
        return -1;
    }
    for (i = 0u; i < (uint8_t)packChem_last; i++) {
        if ((s_chemNames[i] != NULL) && (strcmp(s_chemNames[i], name) == 0)) {
            *out = i;
            return packErr_ok;
        }
    }
    return packErr_notFound;
}

const char *PackCfg_ChemName(uint8_t chem)
{
    if (chem >= (uint8_t)packChem_last) {
        return NULL;
    }
    return s_chemNames[chem];
}

int PackCfg_CmdIdFromName(const char *name, ePackCmdId *out)
{
    uint32_t i;

    if ((name == NULL) || (out == NULL)) {
        return -1;
    }
    for (i = 1u; i < (uint32_t)packCmd_last; i++) {
        if ((s_cmdNames[i] != NULL) && (strcmp(s_cmdNames[i], name) == 0)) {
            *out = (ePackCmdId)i;
            return packErr_ok;
        }
    }
    return packErr_notFound;
}

const char *PackCfg_CmdName(ePackCmdId cmd)
{
    if (((uint32_t)cmd == 0u) || ((uint32_t)cmd >= (uint32_t)packCmd_last)) {
        return NULL;
    }
    return s_cmdNames[cmd];
}

/* --- runtime enum names -------------------------------------------------
 *
 * ePackCondition, ePackAbsentReason and ePackSwitch are RUNTIME state, not
 * configuration -- none of them is persisted or parsed back, so unlike the
 * three tables above these have no *FromName() half and their wording is free
 * to improve.  They live here anyway for one reason: this file is linked into
 * tests/test_pack_cfg while pack.c is not, so this is the only placement that
 * lets a host test assert every enumerator has a name.  pack.c re-exports
 * them as Pack_*Name() so a consumer never includes the config parser to ask
 * what a condition is called.
 *
 * WRITTEN AS A SWITCH WITH THE FALLBACK *AFTER* IT, not in a `default:`.
 * `C coding standard.md` asks for a default in every switch; its intent is
 * that an unhandled case must not pass silently, and -Werror=switch (scoped
 * to this file in CMakeLists.txt) serves that intent better -- it makes a new
 * enumerator without a name a BUILD FAILURE rather than a "?" discovered in
 * the field.  A `default:` would defeat -Wswitch entirely, which is exactly
 * why NvDbCfg_ModeName is unprotected today.
 *
 * THESE STRINGS ARE EMITTED INTO JSON by /api/pack/status.  None contains a
 * '"' or a '\', so none needs escaping at the call site.  A future wording
 * that quotes an operator-supplied token -- `bind key "x" matched nothing` --
 * would produce malformed JSON on a live endpoint with no test to catch it;
 * keep them plain, or escape them where they are emitted.
 */

const char *PackCfg_CondName(uint8_t cond)
{
    switch ((ePackCondition)cond) {
    case packCond_absent: return "absent";
    case packCond_stale:  return "stale";
    case packCond_online: return "online";
    case packCond_last:   break;
    }
    return "?";
}

const char *PackCfg_WhyName(uint8_t why)
{
    switch ((ePackAbsentReason)why) {
    case packWhy_none:            return "online";
    case packWhy_noType:          return "no such type in this firmware";
    case packWhy_typeUnavailable: return "type present but not usable in this build";
    case packWhy_noBinding:       return "bind key matched no device, or more than one";
    case packWhy_notPolled:       return "resolved, but no live plan reads it";
    case packWhy_noReply:         return "never answered";
    case packWhy_last:            break;
    }
    return "?";
}

/** ONE BIT OF ePackAlarm, by name.
 *
 *  A MASK WITH ONE BIT SET, not an index: ePackAlarm is a bit-flag enum with
 *  no _last sentinel and never will have one, so an index would be a second
 *  numbering to keep in step with the first.
 *
 *  This existed nowhere until 2026-09-07 and `alarms` reached HTTP and the CLI
 *  as a bare integer -- the identical defect ePackAbsentReason had, where the
 *  most diagnostic field the module publishes was readable only by someone
 *  holding the header.  On a tunnel-only board that is backwards.
 */
const char *PackCfg_AlarmName(uint32_t bit)
{
    switch ((ePackAlarm)bit) {
    case packAlarm_cellOverVoltage:   return "cellOverVoltage";
    case packAlarm_cellUnderVoltage:  return "cellUnderVoltage";
    case packAlarm_packOverVoltage:   return "packOverVoltage";
    case packAlarm_packUnderVoltage:  return "packUnderVoltage";
    case packAlarm_overTemperature:   return "overTemperature";
    case packAlarm_underTemperature:  return "underTemperature";
    case packAlarm_chargeOverCurrent: return "chargeOverCurrent";
    case packAlarm_dischargeOverCur:  return "dischargeOverCurrent";
    case packAlarm_cellImbalance:     return "cellImbalance";
    case packAlarm_internalFault:     return "internalFault";
    case packAlarm_protectionOpen:    return "protectionOpen";
    }
    return "?";
}

const char *PackCfg_SwitchName(uint8_t sw)
{
    switch ((ePackSwitch)sw) {
    /* "?" and not "unknown": the type cannot report it, which is the same
     * answer an out-of-range value gets, and the CLI has spelled it this way
     * since before these were public. */
    case packSwitch_unknown: return "?";
    case packSwitch_open:    return "open";
    case packSwitch_closed:  return "closed";
    case packSwitch_last:    break;
    }
    return "?";
}

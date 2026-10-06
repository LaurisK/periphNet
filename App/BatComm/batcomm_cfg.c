/*
 * batcomm_cfg.c
 *
 * The battery-communication configuration — streaming parse, re-serialise,
 * bounds — and all six BatComm_*Name()/FromName() accessors
 * (docs/design_battery_comm.md §5).
 *
 * LIBC ONLY, and deliberately so: this file is compiled by tests/ as well as
 * by the firmware, and it must never learn what a flash area or a CAN cell
 * is.  Persistence lives in batcomm.c.
 *
 * The parser is a hand-rolled pull parser for one fixed schema over
 * Shared/Json's one tokenizer, fed from an abstract byte source — the HTTP
 * body cursor on the device, a memory buffer in tests.  UNKNOWN KEYS ARE
 * REJECTED so a stale document fails by name instead of losing a setting
 * silently.  cluster_cfg.c is the precedent, down to the rd_* wrappers.
 *
 * EVERY NAME ACCESSOR IS A SWITCH WHOSE FALLBACK SITS AFTER THE SWITCH, never
 * in a `default:` — a `default:` satisfies -Wswitch and defeats the whole
 * mechanism.  CMake scopes -Werror=switch onto this one file, so adding an
 * enumerator without a name fails the build instead of shipping a "?" for
 * somebody to find in the field.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/BatComm/batcomm_cfg.h"
#include "App/BatComm/batcomm_frame.h"

#include "json.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define CFG_TOK_MAX     32u     /* longest key/word we ever need to hold     */
#define CFG_LINE_MAX    256u

#define BAT_NAME_ESC_LEN    (((BATCOMM_NAME_LEN - 1u) * 6u) + 1u)

/* Private types ------------------------------------------------------------*/

typedef sJsonReader sCfgReader;

/* Private function prototypes ----------------------------------------------*/

static void SetFailure(sBatCommCfgResult *res, const char *field,
                       const char *reason);
static int  rd_peek(sCfgReader *r, char *out);
static int  rd_bump(sCfgReader *r, char *out);
static void rd_take(sCfgReader *r);
static int  rd_skip_ws(sCfgReader *r);
static int  rd_expect(sCfgReader *r, char want);
static int  name_is_legal(const char *s);
static int  parse_tune(sCfgReader *r, sBatCommTune *t,
                       sBatCommCfgResult *res);
static int  emit(fBatCommByteSink sink, void *ctx, const char *fmt, ...);

/* Private functions --------------------------------------------------------*/

static void SetFailure(sBatCommCfgResult *res, const char *field,
                       const char *reason)
{
    if (res == NULL) {
        return;
    }
    res->ok = 0;
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

/* --- validators -------------------------------------------------------- */

/** The same alphabet pack_cfg.c accepts for a pack name, because this IS a
 *  pack name and the two must agree or a legal pack could never be named
 *  here. */
static int name_is_legal(const char *s)
{
    uint32_t i;

    if ((s == NULL) || (s[0] == '\0')) {
        return 0;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        const char c = s[i];

        if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
            ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-')) {
            continue;
        }
        return 0;
    }
    return (i < BATCOMM_NAME_LEN) ? 1 : 0;
}

static int parse_tune(sCfgReader *r, sBatCommTune *t, sBatCommCfgResult *res)
{
    char key[CFG_TOK_MAX];
    char c;

    if (!rd_expect(r, '{')) {
        SetFailure(res, "tune", "expected an object");
        return 0;
    }
    if (!rd_skip_ws(r) || !rd_peek(r, &c)) {
        SetFailure(res, "tune", "truncated");
        return 0;
    }
    if (c == '}') {
        rd_take(r);
        return 1;                       /* present and empty: all defaults   */
    }

    for (;;) {
        int32_t v;

        if (!Json_ReadString(r, key, sizeof(key))) {
            SetFailure(res, "tune", "bad key");
            return 0;
        }
        if (!rd_expect(r, ':')) {
            SetFailure(res, key, "expected ':'");
            return 0;
        }
        if (!Json_ReadI32(r, &v)) {
            SetFailure(res, key, "expected an integer");
            return 0;
        }
        if (v < 0) {
            SetFailure(res, key, "must not be negative");
            return 0;
        }

        if (strcmp(key, "slot_ms") == 0) {
            /* THE FLOOR IS THE BRIDGE'S, not a preference: the bridge
             * refuses a source period below 50 ms, and a configuration the
             * bridge would refuse must fail HERE, where the operator can see
             * which key was wrong. */
            if ((v < 50) || (v > 1000)) {
                SetFailure(res, key, "must be in [50, 1000] ms");
                return 0;
            }
            t->slot_ms = (uint16_t)v;
        } else if (strcmp(key, "slots") == 0) {
            if ((v < 1) || (v > (int32_t)BATCOMM_SLOTS_MAX)) {
                SetFailure(res, key, "must be in [1, 32]");
                return 0;
            }
            t->slots = (uint16_t)v;
        } else if (strcmp(key, "currentScale_mA") == 0) {
            if ((v <= 0) || (v > 10000)) {
                SetFailure(res, key, "must be in (0, 10000] mA per count");
                return 0;
            }
            t->currentScale_mA = (uint16_t)v;
        } else if (strcmp(key, "capacityScale_mAh") == 0) {
            if ((v <= 0) || (v > 10000)) {
                SetFailure(res, key, "must be in (0, 10000] mAh per count");
                return 0;
            }
            t->capacityScale_mAh = (uint16_t)v;
        } else if (strcmp(key, "staleTrip") == 0) {
            if ((v < 1) || (v > 60)) {
                SetFailure(res, key, "must be in [1, 60] cycles");
                return 0;
            }
            t->staleTrip = (uint16_t)v;
        } else if (strcmp(key, "invertCurrent") == 0) {
            t->invertCurrent = (uint8_t)((v != 0) ? 1 : 0);
        } else if (strcmp(key, "emitAlarms") == 0) {
            t->emitAlarms = (uint8_t)((v != 0) ? 1 : 0);
        } else {
            SetFailure(res, key, "unknown tune key");
            return 0;
        }

        if (!rd_skip_ws(r) || !rd_bump(r, &c)) {
            SetFailure(res, "tune", "truncated");
            return 0;
        }
        if (c == '}') {
            return 1;
        }
        if (c != ',') {
            SetFailure(res, "tune", "expected ',' or '}'");
            return 0;
        }
    }
}

/**
 * @brief Push one formatted fragment at the sink.
 * @note Bounded, not clamped: a fragment that does not fit is a bug in this
 *       function's buffer sizing, not something to truncate silently into a
 *       document somebody will try to upload again.
 */
static int emit(fBatCommByteSink sink, void *ctx, const char *fmt, ...)
{
    char    line[CFG_LINE_MAX];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if ((n < 0) || (n >= (int)sizeof(line))) {
        return batErr_badArg;
    }
    return (sink(ctx, line, (uint32_t)n) < 0) ? batErr_badArg : batErr_ok;
}

/* Exported functions -------------------------------------------------------*/

void BatCommCfg_Defaults(sBatCommCfg *cfg)
{
    if (cfg == NULL) {
        return;
    }
    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->version                 = (uint16_t)BATCOMM_CFG_VERSION;
    cfg->proto                   = (uint8_t)batProto_none;
    cfg->source                  = (uint8_t)batSrc_none;
    cfg->fallback                = (uint8_t)batFallback_bridge;
    cfg->tune.slot_ms            = (uint16_t)BATCOMM_DFLT_SLOT_MS;
    cfg->tune.slots              = (uint16_t)BATCOMM_DFLT_SLOTS;
    cfg->tune.currentScale_mA    = (uint16_t)BATCOMM_DFLT_CURRENT_SCALE_MA;
    cfg->tune.capacityScale_mAh  = (uint16_t)BATCOMM_DFLT_CAPACITY_SCALE_MAH;
    cfg->tune.staleTrip          = (uint16_t)BATCOMM_DFLT_STALE_TRIP;
    cfg->tune.invertCurrent      = 0u;   /* contract 1: UNMEASURED, so off   */
    cfg->tune.emitAlarms         = 1u;
}

int BatCommCfg_Parse(fBatCommByteSource src, void *srcCtx, sBatCommCfg *out,
                     sBatCommCfgResult *res)
{
    sCfgReader r;
    char       key[CFG_TOK_MAX];
    char       word[CFG_TOK_MAX];
    char       c;
    int        sawEnabled = 0;

    if ((src == NULL) || (out == NULL)) {
        SetFailure(res, "json", "no source");
        return batErr_badArg;
    }

    Json_ReaderInit(&r, src, srcCtx);
    BatCommCfg_Defaults(out);
    if (res != NULL) {
        (void)memset(res, 0, sizeof(*res));
    }

    if (!rd_expect(&r, '{')) {
        SetFailure(res, "json", "expected an object");
        return batErr_badArg;
    }

    for (;;) {
        if (!Json_ReadString(&r, key, sizeof(key))) {
            SetFailure(res, "json", "bad key");
            return batErr_badArg;
        }
        if (!rd_expect(&r, ':')) {
            SetFailure(res, key, "expected ':'");
            return batErr_badArg;
        }

        if (strcmp(key, "version") == 0) {
            int32_t v;

            if (!Json_ReadI32(&r, &v) || (v <= 0)) {
                SetFailure(res, "version", "bad version");
                return batErr_badArg;
            }
            if (v > (int32_t)BATCOMM_CFG_VERSION) {
                /* Reading a newer document with this image's field meanings
                 * would misread a scale that multiplies a current. */
                SetFailure(res, "version", "unsupported document version");
                return batErr_badArg;
            }
            out->version = (uint16_t)v;
        } else if (strcmp(key, "protocol") == 0) {
            eBatCommProto p;

            if (!Json_ReadString(&r, word, sizeof(word)) ||
                (BatComm_ProtoFromName(word, &p) != batErr_ok) ||
                (p == batProto_none)) {
                SetFailure(res, "protocol", "unknown dialect");
                return batErr_badArg;
            }
            out->proto = (uint8_t)p;
        } else if (strcmp(key, "peripheral") == 0) {
            if (!Json_ReadString(&r, word, sizeof(word))) {
                SetFailure(res, "peripheral", "expected a string");
                return batErr_badArg;
            }
            if (strcmp(word, "can1") == 0) {
                out->inverterBus = 0u;
            } else if (strcmp(word, "can2") == 0) {
                out->inverterBus = 1u;
            } else {
                SetFailure(res, "peripheral", "must be \"can1\" or \"can2\"");
                return batErr_badArg;
            }
        } else if (strcmp(key, "source") == 0) {
            if (!Json_ReadString(&r, word, sizeof(word))) {
                SetFailure(res, "source", "expected a string");
                return batErr_badArg;
            }
            if (strcmp(word, "cluster") == 0) {
                out->source = (uint8_t)batSrc_cluster;
            } else if (strcmp(word, "pack") == 0) {
                out->source = (uint8_t)batSrc_pack;
            } else {
                SetFailure(res, "source", "must be \"cluster\" or \"pack\"");
                return batErr_badArg;
            }
        } else if (strcmp(key, "pack") == 0) {
            if (!Json_ReadString(&r, out->packName, sizeof(out->packName)) ||
                !name_is_legal(out->packName)) {
                SetFailure(res, "pack", "not a legal pack name");
                return batErr_badArg;
            }
        } else if (strcmp(key, "fallback") == 0) {
            if (!Json_ReadString(&r, word, sizeof(word))) {
                SetFailure(res, "fallback", "expected a string");
                return batErr_badArg;
            }
            if (strcmp(word, "bridge") == 0) {
                out->fallback = (uint8_t)batFallback_bridge;
            } else if (strcmp(word, "silent") == 0) {
                out->fallback = (uint8_t)batFallback_silent;
            } else {
                SetFailure(res, "fallback",
                           "must be \"bridge\" or \"silent\"");
                return batErr_badArg;
            }
        } else if (strcmp(key, "enabled") == 0) {
            int b;

            if (!Json_ReadBool(&r, &b)) {
                SetFailure(res, "enabled", "expected true or false");
                return batErr_badArg;
            }
            out->enabled = (uint8_t)((b != 0) ? 1 : 0);
            sawEnabled = 1;
        } else if (strcmp(key, "batteryBusIsInput") == 0) {
            int b;

            if (!Json_ReadBool(&r, &b)) {
                SetFailure(res, "batteryBusIsInput",
                           "expected true or false");
                return batErr_badArg;
            }
            out->batteryBusIsInput = (uint8_t)((b != 0) ? 1 : 0);
        } else if (strcmp(key, "tune") == 0) {
            if (!parse_tune(&r, &out->tune, res)) {
                return batErr_badArg;
            }
        } else {
            SetFailure(res, key, "unknown key");
            return batErr_badArg;
        }

        if (!rd_skip_ws(&r) || !rd_bump(&r, &c)) {
            SetFailure(res, "json", "truncated");
            return batErr_badArg;
        }
        if (c == '}') {
            break;
        }
        if (c != ',') {
            SetFailure(res, "json", "expected ',' or '}'");
            return batErr_badArg;
        }
    }

    /* --- cross-field rules, all of them refusals rather than repairs ----- */

    if (out->proto == (uint8_t)batProto_none) {
        SetFailure(res, "protocol", "required");
        return batErr_badArg;
    }
    if (out->source == (uint8_t)batSrc_none) {
        SetFailure(res, "source", "required");
        return batErr_badArg;
    }
    /* A NAMED PACK WITH source "cluster" IS A CONTRADICTION, not a harmless
     * leftover: it is exactly what an operator writes when they meant to
     * switch source and edited one line of two. */
    if ((out->source == (uint8_t)batSrc_pack) &&
        (out->packName[0] == '\0')) {
        SetFailure(res, "pack", "required when source is \"pack\"");
        return batErr_badArg;
    }
    if ((out->source == (uint8_t)batSrc_cluster) &&
        (out->packName[0] != '\0')) {
        SetFailure(res, "pack", "not permitted when source is \"cluster\"");
        return batErr_badArg;
    }
    /* A CYCLE SHORTER THAN THE PROFILE'S FRAME COUNT would silently drop the
     * frames that fell off the end, which is the worst way to lose 0x70D.
     * The check lives here rather than in the encoder because the encoder is
     * told a slot, not a cycle. */
    if (out->tune.slots < (uint16_t)BatFrame_SlotCount((eBatCommProto)out->proto)) {
        SetFailure(res, "slots", "fewer slots than the profile has frames");
        return batErr_badArg;
    }
    /* DEFAULT ENABLED, so a document that says what to speak and where does
     * not silently do nothing.  Turning it off is an explicit act. */
    if (sawEnabled == 0) {
        out->enabled = 1u;
    }
    if (res != NULL) {
        res->ok = 1;
    }
    return batErr_ok;
}

int BatCommCfg_Serialize(const sBatCommCfg *cfg, fBatCommByteSink sink,
                         void *ctx)
{
    char nameEsc[BAT_NAME_ESC_LEN];

    if ((cfg == NULL) || (sink == NULL)) {
        return batErr_badArg;
    }

    /* Operator text, and the parser accepts escapes in it, so it is escaped
     * on the way out or the document will not re-upload. */
    (void)Json_Escape(nameEsc, sizeof(nameEsc), cfg->packName);

    if (emit(sink, ctx,
             "{\"version\":%u,\"protocol\":\"%s\",\"peripheral\":\"%s\","
             "\"source\":\"%s\"",
             (unsigned)cfg->version,
             BatComm_ProtoName((eBatCommProto)cfg->proto),
             (cfg->inverterBus == 0u) ? "can1" : "can2",
             BatComm_SourceName((eBatCommSource)cfg->source)) != batErr_ok) {
        return batErr_badArg;
    }
    if (cfg->packName[0] != '\0') {
        if (emit(sink, ctx, ",\"pack\":\"%s\"", nameEsc) != batErr_ok) {
            return batErr_badArg;
        }
    }
    return emit(sink, ctx,
                ",\"enabled\":%s,\"fallback\":\"%s\","
                "\"batteryBusIsInput\":%s,"
                "\"tune\":{\"slot_ms\":%u,\"slots\":%u,"
                "\"currentScale_mA\":%u,\"capacityScale_mAh\":%u,"
                "\"staleTrip\":%u,\"invertCurrent\":%u,\"emitAlarms\":%u}}",
                (cfg->enabled != 0u) ? "true" : "false",
                BatComm_FallbackName((eBatCommFallback)cfg->fallback),
                (cfg->batteryBusIsInput != 0u) ? "true" : "false",
                (unsigned)cfg->tune.slot_ms,
                (unsigned)cfg->tune.slots,
                (unsigned)cfg->tune.currentScale_mA,
                (unsigned)cfg->tune.capacityScale_mAh,
                (unsigned)cfg->tune.staleTrip,
                (unsigned)cfg->tune.invertCurrent,
                (unsigned)cfg->tune.emitAlarms);
}

/* --- the name accessors ---------------------------------------------------
 * A switch with the fallback AFTER it, never a `default:` — see the file
 * preamble.  Range-check and return "?", never NULL: both adapters pass the
 * result straight into a %s.
 * ------------------------------------------------------------------------*/

const char *BatComm_ProtoName(eBatCommProto proto)
{
    switch (proto) {
    case batProto_none:     return "none";
    case batProto_dynessLv: return "dyness_lv";
    case batProto_pylonLv:  return "pylon_lv";
    case batProto_last:     break;
    }
    return "?";
}

const char *BatComm_SourceName(eBatCommSource src)
{
    switch (src) {
    case batSrc_none:    return "none";
    case batSrc_cluster: return "cluster";
    case batSrc_pack:    return "pack";
    case batSrc_last:    break;
    }
    return "?";
}

const char *BatComm_FallbackName(eBatCommFallback fb)
{
    switch (fb) {
    case batFallback_bridge: return "bridge";
    case batFallback_silent: return "silent";
    case batFallback_last:   break;
    }
    return "?";
}

const char *BatComm_StateName(eBatCommState st)
{
    switch (st) {
    case batState_disabled: return "disabled";
    case batState_idle:     return "idle";
    case batState_emitting: return "emitting";
    case batState_holdoff:  return "holdoff";
    case batState_fallback: return "fallback";
    case batState_last:     break;
    }
    return "?";
}

const char *BatComm_WhyName(eBatCommWhy why)
{
    switch (why) {
    case batWhy_ok:             return "ok";
    case batWhy_unprovisioned:  return "no configuration stored";
    case batWhy_disabled:       return "disabled by configuration";
    case batWhy_notArmed:       return "not holding the bridge source slot";
    case batWhy_notBmsMode:     return "the bridge is not in bms mode";
    case batWhy_sourceNotReady: return "the source has never published";
    case batWhy_sourceStale:    return "the source snapshot is stale";
    case batWhy_sourceBusy:     return "the source could not be read";
    case batWhy_packNotFound:   return "the configured pack name matched no "
                                       "pack";
    case batWhy_packNotOnline:  return "the pack is not online";
    case batWhy_last:           break;
    }
    return "?";
}

int BatComm_ProtoFromName(const char *name, eBatCommProto *out)
{
    if ((name == NULL) || (out == NULL)) {
        return batErr_badArg;
    }
    if (strcmp(name, "dyness_lv") == 0) {
        *out = batProto_dynessLv;
        return batErr_ok;
    }
    if (strcmp(name, "pylon_lv") == 0) {
        *out = batProto_pylonLv;
        return batErr_ok;
    }
    if (strcmp(name, "none") == 0) {
        *out = batProto_none;
        return batErr_ok;
    }
    return batErr_notFound;
}

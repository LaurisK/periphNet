/*
 * nvdb_config.c
 *
 * JSON <-> sNvDbLayoutCfg, and the read-back rendering.
 *
 * A pull-parser for a fixed, tiny schema: a name, a version, a mode and a
 * table of sizes keyed by user name.  Unknown keys are rejected rather than
 * ignored, because a typo in a layout is a mistake worth hearing about
 * immediately and the cost of ignoring it is a user silently given no space
 * at all.
 *
 * THE TOKENIZER IS Shared/Json (docs/task_json_module.md §3.2, §6.3).  This
 * file was the buffered one of the three the module replaced, and the
 * question was whether it would fit a streaming shape at all.  It does: the
 * only thing it wanted from having the whole document in memory was a byte
 * offset for error reporting, and the reader carries one.  A buffered caller
 * hands it Json_MemRead over what it already holds -- so one tokenizer, not
 * two with a reason.
 */

/* Includes -----------------------------------------------------------------*/
#include "nvdb_config.h"
#include "nvdb.h"

#include "json.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define TOK_MAX     32u     /* longest key/value token + NUL                  */

/* Private types ------------------------------------------------------------*/

typedef struct {
    sJsonReader     rd;
    sJsonMemSource  mem;
    sNvDbCfgError  *err;
    int             failed;
} sParser;

/* Private function prototypes ----------------------------------------------*/

static int      Fail(sParser *p, const char *field, const char *reason);
static eJsonTok NextToken(sParser *p, char *text, uint32_t textSize);
static int      Expect(sParser *p, eJsonTok want, const char *what);

/* Private functions --------------------------------------------------------*/

/**
 * @brief Record where a parse gave up and stop.
 * @param  p - parser state
 * @param  field - the key that offended
 * @param  reason - what was wrong with it
 * @retval -1 always, so a caller can `return Fail(...)`
 */
static int Fail(sParser *p, const char *field, const char *reason)
{
    if (0 == p->failed && NULL != p->err) {
        strncpy(p->err->field, field, NVDB_CFG_ERR_LEN - 1u);
        p->err->field[NVDB_CFG_ERR_LEN - 1u] = '\0';
        strncpy(p->err->reason, reason, NVDB_CFG_ERR_LEN - 1u);
        p->err->reason[NVDB_CFG_ERR_LEN - 1u] = '\0';
        p->err->offset_bytes = p->rd.offset_bytes;
    }
    p->failed = 1;
    return -1;
}

/**
 * @brief Read the next token, reporting a syntax failure as this module does.
 * @param  p - parser state
 * @param  text - receives the token text for strings and numbers
 * @param  textSize - capacity of `text`
 * @retval the token kind, jsonTok_err after Fail() has been recorded
 */
static eJsonTok NextToken(sParser *p, char *text, uint32_t textSize)
{
    const eJsonTok t = Json_Next(&p->rd, text, textSize);

    if (jsonTok_err == t) {
        (void)Fail(p, "json",
                   (NULL != p->rd.reason) ? p->rd.reason : "syntax error");
    }
    return t;
}

static int Expect(sParser *p, eJsonTok want, const char *what)
{
    char text[TOK_MAX];

    if (NextToken(p, text, sizeof(text)) != want) {
        return Fail(p, "json", what);
    }
    return 0;
}

/* Exported functions -------------------------------------------------------*/

/**
 * @brief Render occupancy and wear, for a person looking at a board.
 * @param  scan - observe occupancy; reads every area, so it is slow
 * @param  out - destination
 * @param  cap_bytes - capacity of `out`
 * @retval bytes written excluding the NUL, or 0 if it did not fit
 * @note Everything in here is reporting only.  A reader who treats
 *       `eraseCntMax` as a reason to move something has misunderstood it:
 *       nvDb never does, and that is what keeps the counters cheap.
 */
uint32_t NvDbCfg_RenderUsage(bool scan, char *out, uint32_t cap_bytes)
{
    sNvDbMediumUsage med;
    size_t           pos   = 0u;
    uint32_t         i     = 0u;
    int              first = 1;

    if (NULL == out || 0u == cap_bytes) {
        return 0u;
    }
    out[0] = '\0';

    if (nvdbRes_ok != NvDb_GetMediumUsage(&med)) {
        return 0u;
    }

    pos = Json_Cat(out, cap_bytes, pos,
                   "{\"medium\":%lu,\"allocated\":%lu,\"freeSpace\":%lu,"
                   "\"unitSize\":%lu,\"units\":%lu,\"eraseMax\":%lu,"
                   "\"eraseTotal\":%lu,\"eraseUnsaved\":%lu,"
                   "\"scanned\":%s,\"users\":{",
                   (unsigned long)med.medium_bytes,
                   (unsigned long)med.allocated_bytes,
                   (unsigned long)med.freeSpace_bytes,
                   (unsigned long)med.unitSize_bytes,
                   (unsigned long)med.units,
                   (unsigned long)med.eraseCntMax,
                   (unsigned long)med.eraseCntTotal,
                   (unsigned long)med.eraseCntUnsaved,
                   scan ? "true" : "false");

    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        const char *name = NvDb_UserName((eNvDbUser)i);
        sNvDbUsage  u;

        if (NULL == name ||
            nvdbRes_ok != NvDb_GetUsage((eNvDbUser)i, scan, &u) ||
            0u == u.size_bytes) {
            continue;
        }
        /* User names are compile-time identifiers, not operator text, so
         * there is nothing here to escape. */
        pos = Json_Cat(out, cap_bytes, pos,
                       "%s\"%s\":{\"size\":%lu,\"occupied\":%lu,"
                       "\"units\":%lu,\"eraseMax\":%lu,\"eraseTotal\":%lu}",
                       first ? "" : ",", name,
                       (unsigned long)u.size_bytes,
                       (unsigned long)u.occupied_bytes,
                       (unsigned long)u.units,
                       (unsigned long)u.eraseCntMax,
                       (unsigned long)u.eraseCntTotal);
        first = 0;
    }

    pos = Json_Cat(out, cap_bytes, pos, "}}");

    return (pos >= cap_bytes) ? 0u : (uint32_t)pos;
}

/**
 * @brief The two spellings of an apply mode.
 * @param  mode - normal or forced
 * @retval a static string; "?" for anything else
 */
const char *NvDbCfg_ModeName(eNvDbApplyMode mode)
{
    switch (mode) {
    case nvdbMode_normal: return "normal";
    case nvdbMode_forced: return "forced";
    default: break;
    }
    return "?";
}

/**
 * @brief Parse a layout document into the structure an operator supplies.
 * @param  json - the document
 * @param  len_bytes - its length, or 0 for a NUL-terminated string
 * @param  out - the parsed layout; sizes for users the document omits stay 0
 * @param  err - filled on failure; may be NULL
 * @retval 0 on success, -1 with `err` describing the first problem
 * @note `freeSpace` is read-only and supplying it is a parse error: it is an
 *       answer the board gives, never an input, and silently ignoring it
 *       would let an operator believe they had asked for something.
 */
int NvDbCfg_Parse(const char *json, uint32_t len_bytes, sNvDbLayoutCfg *out,
                  sNvDbCfgError *err)
{
    sParser  p;
    char     key[TOK_MAX];
    char     val[TOK_MAX];
    int      sawName = 0;
    int      sawVer  = 0;
    int      sawUsers = 0;

    if (NULL == json || NULL == out) {
        return -1;
    }

    memset(&p, 0, sizeof(p));
    Json_MemSourceInit(&p.mem, json,
                       (0u != len_bytes) ? len_bytes : (uint32_t)strlen(json));
    Json_ReaderInit(&p.rd, Json_MemRead, &p.mem);
    p.err       = err;
    if (NULL != err) {
        memset(err, 0, sizeof(*err));
    }

    memset(out, 0, sizeof(*out));
    out->operation = nvdbMode_normal;

    if (0 != Expect(&p, jsonTok_lBrace, "expected '{'")) {
        return -1;
    }

    for (;;) {
        eJsonTok t = NextToken(&p, key, sizeof(key));

        if (jsonTok_rBrace == t) {
            break;
        }
        if (jsonTok_comma == t) {
            continue;
        }
        if (jsonTok_string != t) {
            return Fail(&p, "json", "expected a key");
        }
        if (0 != Expect(&p, jsonTok_colon, "expected ':'")) {
            return -1;
        }

        if (0 == strcmp(key, "name")) {
            if (jsonTok_string != NextToken(&p, val, sizeof(val))) {
                return Fail(&p, "name", "expected a string");
            }
            if (strlen(val) >= NVDB_LAYOUT_NAME_LEN) {
                return Fail(&p, "name", "too long");
            }
            strncpy(out->name, val, NVDB_LAYOUT_NAME_LEN - 1u);
            sawName = 1;
        } else if (0 == strcmp(key, "version")) {
            uint32_t v = 0u;
            if (jsonTok_number != NextToken(&p, val, sizeof(val)) ||
                0 != Json_ToU32(val, &v) || v > 0xFFFFu) {
                return Fail(&p, "version", "expected 0..65535");
            }
            out->version = (uint16_t)v;
            sawVer = 1;
        } else if (0 == strcmp(key, "operation")) {
            if (jsonTok_string != NextToken(&p, val, sizeof(val))) {
                return Fail(&p, "operation", "expected a string");
            }
            if (0 == strcmp(val, "normal")) {
                out->operation = nvdbMode_normal;
            } else if (0 == strcmp(val, "forced")) {
                out->operation = nvdbMode_forced;
            } else {
                return Fail(&p, "operation", "expected normal or forced");
            }
        } else if (0 == strcmp(key, "freeSpace")) {
            return Fail(&p, "freeSpace", "read-only, reported not supplied");
        } else if (0 == strcmp(key, "users")) {
            if (0 != Expect(&p, jsonTok_lBrace, "expected '{' after users")) {
                return -1;
            }
            for (;;) {
                eJsonTok      ut   = NextToken(&p, key, sizeof(key));
                eNvDbUser user = nvdbUser_undefined;
                uint32_t  size = 0u;

                if (jsonTok_rBrace == ut) {
                    break;
                }
                if (jsonTok_comma == ut) {
                    continue;
                }
                if (jsonTok_string != ut) {
                    return Fail(&p, "users", "expected a user name");
                }
                user = NvDb_UserByName(key);
                if (nvdbUser_undefined == user) {
                    return Fail(&p, key, "no user is called that");
                }
                if (0 != Expect(&p, jsonTok_colon, "expected ':'")) {
                    return -1;
                }
                if (jsonTok_number != NextToken(&p, val, sizeof(val)) ||
                    0 != Json_ToU32(val, &size)) {
                    return Fail(&p, key, "expected a size in bytes");
                }
                if (0u != out->size_bytes[user]) {
                    return Fail(&p, key, "named twice");
                }
                out->size_bytes[user] = size;
            }
            sawUsers = 1;
        } else {
            return Fail(&p, key, "unknown key");
        }
    }

    if (0 == sawName) {
        return Fail(&p, "name", "missing");
    }
    if (0 == sawVer) {
        return Fail(&p, "version", "missing");
    }
    if (0 == sawUsers) {
        return Fail(&p, "users", "missing");
    }

    if (Json_SkipWs(&p.rd) >= 0) {
        return Fail(&p, "json", "trailing content");
    }
    return 0;
}

/**
 * @brief Render the read-back form: the layout in force plus what it cost.
 * @param  info - the layout in force, free space and the onboarding state
 * @param  status - the persisted outcome of the last application; may be NULL
 * @param  out - destination
 * @param  cap_bytes - capacity of `out`
 * @retval bytes written excluding the NUL, or 0 if it did not fit
 * @note Deliberately almost the same shape as what is supplied, so a config
 *       and its read-back can be diffed by eye.
 */
uint32_t NvDbCfg_Render(const sNvDbLayoutInfo *info, const sNvDbStatus *status,
                        char *out, uint32_t cap_bytes)
{
    size_t   pos   = 0u;
    uint32_t i     = 0u;
    int      first = 1;

    if (NULL == info || NULL == out || 0u == cap_bytes) {
        return 0u;
    }
    out[0] = '\0';

    /* `name` came out of an uploaded layout document, so it is escaped;
     * user names below did not. */
    pos = Json_Cat(out, cap_bytes, pos, "{\"name\":\"");
    pos = Json_CatEscaped(out, cap_bytes, pos, info->name);
    pos = Json_Cat(out, cap_bytes, pos, "\",\"version\":%lu,\"users\":{",
                   (unsigned long)info->version);

    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        const char *name = NvDb_UserName((eNvDbUser)i);

        if (NULL == name) {
            continue;
        }
        pos = Json_Cat(out, cap_bytes, pos, "%s\"%s\":%lu",
                       first ? "" : ",", name,
                       (unsigned long)info->size_bytes[i]);
        first = 0;
    }

    pos = Json_Cat(out, cap_bytes, pos,
                   "},\"freeSpace\":%lu,\"onboarding\":\"%s\"",
                   (unsigned long)info->freeSpace_bytes,
                   (nvdbOnboard_validated == info->onboarding) ? "validated" :
                   (nvdbOnboard_forced == info->onboarding)    ? "forced"
                                                               : "none");

    if (nvdbOnboard_none != info->onboarding &&
        nvdbOnboard_undefined != info->onboarding) {
        pos = Json_Cat(out, cap_bytes, pos, ",\"received\":{\"name\":\"");
        pos = Json_CatEscaped(out, cap_bytes, pos, info->received.name);
        pos = Json_Cat(out, cap_bytes, pos,
                       "\",\"version\":%lu,\"operation\":\"%s\"}",
                       (unsigned long)info->received.version,
                       NvDbCfg_ModeName(info->received.operation));
    }

    if (NULL != status) {
        pos = Json_Cat(out, cap_bytes, pos,
                       ",\"nvdbVer\":%lu,\"lastApplyMode\":\"%s\","
                       "\"lastApplyResult\":%lu",
                       (unsigned long)status->nvdbVer,
                       NvDbCfg_ModeName(status->lastApplyMode),
                       (unsigned long)status->lastApplyResult);
    }

    pos = Json_Cat(out, cap_bytes, pos, "}");

    return (pos >= cap_bytes) ? 0u : (uint32_t)pos;
}

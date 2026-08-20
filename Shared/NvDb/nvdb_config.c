/*
 * nvdb_config.c
 *
 * JSON <-> sNvDbLayoutCfg, and the read-back rendering.
 *
 * A hand-rolled pull-parser for a fixed, tiny schema: a name, a version, a
 * mode and a table of sizes keyed by user name.  Unknown keys are rejected
 * rather than ignored, because a typo in a layout is a mistake worth hearing
 * about immediately and the cost of ignoring it is a user silently given no
 * space at all.
 */

/* Includes -----------------------------------------------------------------*/
#include "nvdb_config.h"
#include "nvdb.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define TOK_MAX     32u     /* longest key/value token + NUL                  */

/* Private types ------------------------------------------------------------*/

typedef enum {
    tok_undefined = 0,
    tok_lBrace, tok_rBrace,
    tok_colon, tok_comma,
    tok_string, tok_number,
    tok_eof, tok_err,
    tok_last
} eTok;

typedef struct {
    const char     *src;
    uint32_t        len_bytes;
    uint32_t        pos;
    sNvDbCfgError  *err;
    int             failed;
} sParser;

/* Private function prototypes ----------------------------------------------*/

static int      Fail(sParser *p, const char *field, const char *reason);
static void     SkipWs(sParser *p);
static eTok     NextToken(sParser *p, char *text, uint32_t textSize);
static int      Expect(sParser *p, eTok want, const char *what);
static int      ParseU32(const char *text, uint32_t *out);
static uint32_t Append(char *out, uint32_t cap, uint32_t pos, const char *str);
static uint32_t AppendU32(char *out, uint32_t cap, uint32_t pos, uint32_t v);

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
        p->err->offset_bytes = p->pos;
    }
    p->failed = 1;
    return -1;
}

static void SkipWs(sParser *p)
{
    while (p->pos < p->len_bytes) {
        char c = p->src[p->pos];
        if (' ' != c && '\t' != c && '\r' != c && '\n' != c) {
            return;
        }
        p->pos++;
    }
}

/**
 * @brief Read the next token; string/number text lands NUL-terminated.
 * @param  p - parser state
 * @param  text - receives the token text for strings and numbers
 * @param  textSize - capacity of `text`
 * @retval the token kind, tok_err after Fail() has been recorded
 */
static eTok NextToken(sParser *p, char *text, uint32_t textSize)
{
    char c = 0;

    text[0] = '\0';
    SkipWs(p);
    if (p->pos >= p->len_bytes) {
        return tok_eof;
    }

    c = p->src[p->pos++];
    switch (c) {
    case '{': return tok_lBrace;
    case '}': return tok_rBrace;
    case ':': return tok_colon;
    case ',': return tok_comma;
    default: break;
    }

    if ('"' == c) {
        uint32_t n = 0u;
        for (;;) {
            if (p->pos >= p->len_bytes) {
                (void)Fail(p, "json", "unterminated string");
                return tok_err;
            }
            c = p->src[p->pos++];
            if ('"' == c) {
                break;
            }
            if ('\\' == c) {
                (void)Fail(p, "json", "string escapes not supported");
                return tok_err;
            }
            if (n + 1u >= textSize) {
                (void)Fail(p, "json", "string too long");
                return tok_err;
            }
            text[n++] = c;
        }
        text[n] = '\0';
        return tok_string;
    }

    if (c >= '0' && c <= '9') {
        uint32_t n = 0u;
        text[n++] = c;
        while (p->pos < p->len_bytes &&
               p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
            if (n + 1u >= textSize) {
                (void)Fail(p, "json", "number too long");
                return tok_err;
            }
            text[n++] = p->src[p->pos++];
        }
        text[n] = '\0';
        return tok_number;
    }

    (void)Fail(p, "json", "unexpected character");
    return tok_err;
}

static int Expect(sParser *p, eTok want, const char *what)
{
    char text[TOK_MAX];

    if (NextToken(p, text, sizeof(text)) != want) {
        return Fail(p, "json", what);
    }
    return 0;
}

/**
 * @brief Parse an unsigned decimal, refusing anything that would wrap.
 * @param  text - NUL-terminated digits
 * @param  out - the value
 * @retval 0 on success, -1 otherwise
 */
static int ParseU32(const char *text, uint32_t *out)
{
    uint32_t v = 0u;
    uint32_t i = 0u;

    if ('\0' == text[0]) {
        return -1;
    }
    for (i = 0u; '\0' != text[i]; i++) {
        uint32_t d = (uint32_t)(text[i] - '0');
        if (v > ((0xFFFFFFFFu - d) / 10u)) {
            return -1;
        }
        v = (v * 10u) + d;
    }
    *out = v;
    return 0;
}

static uint32_t Append(char *out, uint32_t cap, uint32_t pos, const char *str)
{
    if (pos >= cap) {
        return cap;         /* already saturated — an empty `str` would
                             * otherwise write the NUL at out[cap] */
    }
    while ('\0' != *str) {
        if (pos + 1u >= cap) {
            return cap;                 /* saturate; the caller checks       */
        }
        out[pos++] = *str++;
    }
    out[pos] = '\0';
    return pos;
}

static uint32_t AppendU32(char *out, uint32_t cap, uint32_t pos, uint32_t v)
{
    char     digits[12];
    uint32_t n = 0u;

    if (0u == v) {
        digits[n++] = '0';
    }
    while (0u != v) {
        digits[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (0u != n) {
        if (pos + 1u >= cap) {
            return cap;
        }
        out[pos++] = digits[--n];
    }
    out[pos] = '\0';
    return pos;
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
    uint32_t         pos   = 0u;
    uint32_t         i     = 0u;
    int              first = 1;

    if (NULL == out || 0u == cap_bytes) {
        return 0u;
    }
    out[0] = '\0';

    if (nvdbRes_ok != NvDb_GetMediumUsage(&med)) {
        return 0u;
    }

    pos = Append(out, cap_bytes, pos, "{\"medium\":");
    pos = AppendU32(out, cap_bytes, pos, med.medium_bytes);
    pos = Append(out, cap_bytes, pos, ",\"allocated\":");
    pos = AppendU32(out, cap_bytes, pos, med.allocated_bytes);
    pos = Append(out, cap_bytes, pos, ",\"freeSpace\":");
    pos = AppendU32(out, cap_bytes, pos, med.freeSpace_bytes);
    pos = Append(out, cap_bytes, pos, ",\"unitSize\":");
    pos = AppendU32(out, cap_bytes, pos, med.unitSize_bytes);
    pos = Append(out, cap_bytes, pos, ",\"units\":");
    pos = AppendU32(out, cap_bytes, pos, med.units);
    pos = Append(out, cap_bytes, pos, ",\"eraseMax\":");
    pos = AppendU32(out, cap_bytes, pos, med.eraseCntMax);
    pos = Append(out, cap_bytes, pos, ",\"eraseTotal\":");
    pos = AppendU32(out, cap_bytes, pos, med.eraseCntTotal);
    pos = Append(out, cap_bytes, pos, ",\"eraseUnsaved\":");
    pos = AppendU32(out, cap_bytes, pos, med.eraseCntUnsaved);
    pos = Append(out, cap_bytes, pos, ",\"scanned\":");
    pos = Append(out, cap_bytes, pos, scan ? "true" : "false");
    pos = Append(out, cap_bytes, pos, ",\"users\":{");

    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        const char *name = NvDb_UserName((eNvDbUser)i);
        sNvDbUsage  u;

        if (NULL == name ||
            nvdbRes_ok != NvDb_GetUsage((eNvDbUser)i, scan, &u) ||
            0u == u.size_bytes) {
            continue;
        }
        if (0 == first) {
            pos = Append(out, cap_bytes, pos, ",");
        }
        first = 0;
        pos = Append(out, cap_bytes, pos, "\"");
        pos = Append(out, cap_bytes, pos, name);
        pos = Append(out, cap_bytes, pos, "\":{\"size\":");
        pos = AppendU32(out, cap_bytes, pos, u.size_bytes);
        pos = Append(out, cap_bytes, pos, ",\"occupied\":");
        pos = AppendU32(out, cap_bytes, pos, u.occupied_bytes);
        pos = Append(out, cap_bytes, pos, ",\"units\":");
        pos = AppendU32(out, cap_bytes, pos, u.units);
        pos = Append(out, cap_bytes, pos, ",\"eraseMax\":");
        pos = AppendU32(out, cap_bytes, pos, u.eraseCntMax);
        pos = Append(out, cap_bytes, pos, ",\"eraseTotal\":");
        pos = AppendU32(out, cap_bytes, pos, u.eraseCntTotal);
        pos = Append(out, cap_bytes, pos, "}");
    }

    pos = Append(out, cap_bytes, pos, "}}");

    return (pos >= cap_bytes) ? 0u : pos;
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
    p.src       = json;
    p.len_bytes = (0u != len_bytes) ? len_bytes : (uint32_t)strlen(json);
    p.err       = err;
    if (NULL != err) {
        memset(err, 0, sizeof(*err));
    }

    memset(out, 0, sizeof(*out));
    out->operation = nvdbMode_normal;

    if (0 != Expect(&p, tok_lBrace, "expected '{'")) {
        return -1;
    }

    for (;;) {
        eTok t = NextToken(&p, key, sizeof(key));

        if (tok_rBrace == t) {
            break;
        }
        if (tok_comma == t) {
            continue;
        }
        if (tok_string != t) {
            return Fail(&p, "json", "expected a key");
        }
        if (0 != Expect(&p, tok_colon, "expected ':'")) {
            return -1;
        }

        if (0 == strcmp(key, "name")) {
            if (tok_string != NextToken(&p, val, sizeof(val))) {
                return Fail(&p, "name", "expected a string");
            }
            if (strlen(val) >= NVDB_LAYOUT_NAME_LEN) {
                return Fail(&p, "name", "too long");
            }
            strncpy(out->name, val, NVDB_LAYOUT_NAME_LEN - 1u);
            sawName = 1;
        } else if (0 == strcmp(key, "version")) {
            uint32_t v = 0u;
            if (tok_number != NextToken(&p, val, sizeof(val)) ||
                0 != ParseU32(val, &v) || v > 0xFFFFu) {
                return Fail(&p, "version", "expected 0..65535");
            }
            out->version = (uint16_t)v;
            sawVer = 1;
        } else if (0 == strcmp(key, "operation")) {
            if (tok_string != NextToken(&p, val, sizeof(val))) {
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
            if (0 != Expect(&p, tok_lBrace, "expected '{' after users")) {
                return -1;
            }
            for (;;) {
                eTok      ut   = NextToken(&p, key, sizeof(key));
                eNvDbUser user = nvdbUser_undefined;
                uint32_t  size = 0u;

                if (tok_rBrace == ut) {
                    break;
                }
                if (tok_comma == ut) {
                    continue;
                }
                if (tok_string != ut) {
                    return Fail(&p, "users", "expected a user name");
                }
                user = NvDb_UserByName(key);
                if (nvdbUser_undefined == user) {
                    return Fail(&p, key, "no user is called that");
                }
                if (0 != Expect(&p, tok_colon, "expected ':'")) {
                    return -1;
                }
                if (tok_number != NextToken(&p, val, sizeof(val)) ||
                    0 != ParseU32(val, &size)) {
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

    SkipWs(&p);
    if (p.pos != p.len_bytes) {
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
    uint32_t pos   = 0u;
    uint32_t i     = 0u;
    int      first = 1;

    if (NULL == info || NULL == out || 0u == cap_bytes) {
        return 0u;
    }
    out[0] = '\0';

    pos = Append(out, cap_bytes, pos, "{\"name\":\"");
    pos = Append(out, cap_bytes, pos, info->name);
    pos = Append(out, cap_bytes, pos, "\",\"version\":");
    pos = AppendU32(out, cap_bytes, pos, info->version);
    pos = Append(out, cap_bytes, pos, ",\"users\":{");

    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        const char *name = NvDb_UserName((eNvDbUser)i);

        if (NULL == name) {
            continue;
        }
        if (0 == first) {
            pos = Append(out, cap_bytes, pos, ",");
        }
        first = 0;
        pos = Append(out, cap_bytes, pos, "\"");
        pos = Append(out, cap_bytes, pos, name);
        pos = Append(out, cap_bytes, pos, "\":");
        pos = AppendU32(out, cap_bytes, pos, info->size_bytes[i]);
    }

    pos = Append(out, cap_bytes, pos, "},\"freeSpace\":");
    pos = AppendU32(out, cap_bytes, pos, info->freeSpace_bytes);

    pos = Append(out, cap_bytes, pos, ",\"onboarding\":\"");
    switch (info->onboarding) {
    case nvdbOnboard_validated: pos = Append(out, cap_bytes, pos, "validated"); break;
    case nvdbOnboard_forced:    pos = Append(out, cap_bytes, pos, "forced");    break;
    default:                    pos = Append(out, cap_bytes, pos, "none");      break;
    }
    pos = Append(out, cap_bytes, pos, "\"");

    if (nvdbOnboard_none != info->onboarding &&
        nvdbOnboard_undefined != info->onboarding) {
        pos = Append(out, cap_bytes, pos, ",\"received\":{\"name\":\"");
        pos = Append(out, cap_bytes, pos, info->received.name);
        pos = Append(out, cap_bytes, pos, "\",\"version\":");
        pos = AppendU32(out, cap_bytes, pos, info->received.version);
        pos = Append(out, cap_bytes, pos, ",\"operation\":\"");
        pos = Append(out, cap_bytes, pos,
                     NvDbCfg_ModeName(info->received.operation));
        pos = Append(out, cap_bytes, pos, "\"}");
    }

    if (NULL != status) {
        pos = Append(out, cap_bytes, pos, ",\"nvdbVer\":");
        pos = AppendU32(out, cap_bytes, pos, status->nvdbVer);
        pos = Append(out, cap_bytes, pos, ",\"lastApplyMode\":\"");
        pos = Append(out, cap_bytes, pos,
                     NvDbCfg_ModeName(status->lastApplyMode));
        pos = Append(out, cap_bytes, pos, "\",\"lastApplyResult\":");
        pos = AppendU32(out, cap_bytes, pos, (uint32_t)status->lastApplyResult);
    }

    pos = Append(out, cap_bytes, pos, "}");

    return (pos >= cap_bytes) ? 0u : pos;
}

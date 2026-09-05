/*
 * json_read.c
 *
 * The streaming reader.  One tokenizer over a byte source, replacing the
 * three private ones this project had grown (Shared/Modbus's lex_*,
 * App/Pack's rd_*, Shared/NvDb's SkipWs/NextToken/Expect).
 *
 * None of the three handled backslash escapes -- two rejected them outright
 * and the third let `\` through into the field it was reading, so a name
 * containing one was stored and then re-emitted raw into every response that
 * mentioned it.  Escape decoding is therefore the one capability added here
 * rather than ported, and it is what closes the round trip with
 * Json_CatEscaped() at the other end.
 *
 * Two layers, both used:
 *   - bytes  (Json_Peek / Json_Get / Json_SkipWs) for a caller that looks
 *     ahead at a structural character before deciding what to parse;
 *   - tokens (Json_Next / Json_Expect / Json_Read*) for a caller that walks
 *     a grammar.
 */

/* Includes -----------------------------------------------------------------*/

#include "json.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define TEXT_MIN_SIZE       2u      /* one character plus the NUL           */

/* Scratch for a token whose text the caller does not want.  Big enough that a
 * misplaced string or number still reports "unexpected token" rather than
 * "string too long", which would name the wrong defect. */
#define TOK_SCRATCH         32u

/* Private function prototypes ----------------------------------------------*/

static int      Fill(sJsonReader *r);
static eJsonTok Fail(sJsonReader *r, const char *reason);
static int      HexDigit(int ch);
static int      ReadEscape(sJsonReader *r, char *out, uint32_t *outLen);
static eJsonTok ScanString(sJsonReader *r, char *text, uint32_t textSize);
static eJsonTok ScanNumber(sJsonReader *r, int first, char *text,
                           uint32_t textSize);
static eJsonTok ScanLiteral(sJsonReader *r, int first);

/* Private functions --------------------------------------------------------*/

static int Fill(sJsonReader *r)
{
    int n;

    if ((r->eof != 0u) || (r->ioErr != 0u)) {
        return 0;
    }
    n = r->src(r->srcCtx, r->window, JSON_WINDOW_LEN);
    if (n < 0) {
        r->ioErr = 1u;
        return 0;
    }
    if (n == 0) {
        r->eof = 1u;
        return 0;
    }
    r->winLen = (uint32_t)n;
    r->winPos = 0u;

    return 1;
}

static eJsonTok Fail(sJsonReader *r, const char *reason)
{
    if (r->reason == NULL) {
        r->reason = reason;         /* first failure is the informative one */
    }
    return jsonTok_err;
}

static int HexDigit(int ch)
{
    if ((ch >= '0') && (ch <= '9')) {
        return ch - '0';
    }
    if ((ch >= 'a') && (ch <= 'f')) {
        return (ch - 'a') + 10;
    }
    if ((ch >= 'A') && (ch <= 'F')) {
        return (ch - 'A') + 10;
    }
    return -1;
}

/**
 * @brief Decode the escape whose backslash has already been consumed.
 * @param  r - reader
 * @param  out - receives up to 3 bytes of UTF-8
 * @param  outLen - how many were produced
 * @retval 1 on success, 0 after Fail() has been recorded
 *
 * \uXXXX is decoded to UTF-8.  A surrogate half is passed through as U+FFFD
 * rather than paired: no schema here carries one, and a reader that silently
 * produced a lone surrogate would hand invalid UTF-8 to a consumer.
 */
static int ReadEscape(sJsonReader *r, char *out, uint32_t *outLen)
{
    int      ch = Json_Get(r);
    uint32_t cp = 0u;
    int      i;

    *outLen = 1u;

    switch (ch) {
    case '"':  out[0] = '"';  return 1;
    case '\\': out[0] = '\\'; return 1;
    case '/':  out[0] = '/';  return 1;
    case 'b':  out[0] = '\b'; return 1;
    case 'f':  out[0] = '\f'; return 1;
    case 'n':  out[0] = '\n'; return 1;
    case 'r':  out[0] = '\r'; return 1;
    case 't':  out[0] = '\t'; return 1;
    case 'u':  break;
    default:
        (void)Fail(r, "bad escape");
        return 0;
    }

    for (i = 0; i < 4; i++) {
        const int d = HexDigit(Json_Get(r));

        if (d < 0) {
            (void)Fail(r, "bad \\u escape");
            return 0;
        }
        cp = (cp << 4) | (uint32_t)d;
    }

    if ((cp >= 0xD800u) && (cp <= 0xDFFFu)) {
        cp = 0xFFFDu;               /* lone surrogate -> replacement char   */
    }

    if (cp < 0x80u) {
        out[0] = (char)cp;
        *outLen = 1u;
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        *outLen = 2u;
    } else {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        *outLen = 3u;
    }

    return 1;
}

/** The opening quote has already been consumed. */
static eJsonTok ScanString(sJsonReader *r, char *text, uint32_t textSize)
{
    uint32_t n = 0u;

    for (;;) {
        char     dec[4];
        uint32_t len = 1u;
        uint32_t i;
        int      ch = Json_Get(r);

        if (ch < 0) {
            return Fail(r, (r->ioErr != 0u) ? "read error"
                                            : "unterminated string");
        }
        if (ch == '"') {
            break;
        }
        if (ch == '\\') {
            if (ReadEscape(r, dec, &len) == 0) {
                return jsonTok_err;
            }
        } else if ((uint8_t)ch < 0x20u) {
            return Fail(r, "control character in string");
        } else {
            dec[0] = (char)ch;
        }

        if ((n + len + 1u) > textSize) {
            return Fail(r, "string too long");
        }
        for (i = 0u; i < len; i++) {
            text[n++] = dec[i];
        }
    }
    text[n] = '\0';

    return jsonTok_string;
}

/** `first` is the '-' or digit already consumed. */
static eJsonTok ScanNumber(sJsonReader *r, int first, char *text,
                           uint32_t textSize)
{
    uint32_t n = 0u;

    text[n++] = (char)first;
    for (;;) {
        const int ch = Json_Peek(r);

        if ((ch == '.') || ((ch >= '0') && (ch <= '9'))) {
            if ((n + 1u) >= textSize) {
                return Fail(r, "number too long");
            }
            text[n++] = (char)Json_Get(r);
        } else if ((ch == 'e') || (ch == 'E')) {
            /* Every schema here is integer or fixed-point.  Accepting an
             * exponent would mean deciding what 1e3 means in a field whose
             * domain is scaled integers, so it is refused, not rounded. */
            return Fail(r, "exponent notation not supported");
        } else {
            break;
        }
    }
    text[n] = '\0';

    return jsonTok_number;
}

/** `first` is the 't', 'f' or 'n' already consumed. */
static eJsonTok ScanLiteral(sJsonReader *r, int first)
{
    const char *rest;
    eJsonTok    tok;

    if (first == 't') {
        rest = "rue";
        tok  = jsonTok_true;
    } else if (first == 'f') {
        rest = "alse";
        tok  = jsonTok_false;
    } else {
        rest = "ull";
        tok  = jsonTok_null;
    }

    while (*rest != '\0') {
        if (Json_Get(r) != (int)*rest) {
            return Fail(r, "bad literal");
        }
        rest++;
    }

    return tok;
}

/* Exported functions -------------------------------------------------------*/

void Json_ReaderInit(sJsonReader *r, fJsonByteSource src, void *srcCtx)
{
    (void)memset(r, 0, sizeof(*r));
    r->src    = src;
    r->srcCtx = srcCtx;
}

int Json_Peek(sJsonReader *r)
{
    if (r->winPos >= r->winLen) {
        if (Fill(r) == 0) {
            return -1;
        }
    }
    return (int)r->window[r->winPos];
}

int Json_Get(sJsonReader *r)
{
    const int ch = Json_Peek(r);

    if (ch >= 0) {
        r->winPos++;
        r->offset_bytes++;
    }
    return ch;
}

int Json_SkipWs(sJsonReader *r)
{
    int ch;

    for (;;) {
        ch = Json_Peek(r);
        if ((ch != ' ') && (ch != '\t') && (ch != '\r') && (ch != '\n')) {
            return ch;
        }
        r->winPos++;
        r->offset_bytes++;
    }
}

eJsonTok Json_Next(sJsonReader *r, char *text, uint32_t textSize)
{
    int ch;

    if ((text == NULL) || (textSize < TEXT_MIN_SIZE)) {
        return Fail(r, "token buffer too small");
    }
    text[0] = '\0';

    (void)Json_SkipWs(r);
    ch = Json_Get(r);
    if (ch < 0) {
        if (r->ioErr != 0u) {
            return Fail(r, "read error");
        }
        return jsonTok_eof;
    }

    switch (ch) {
    case '{': return jsonTok_lBrace;
    case '}': return jsonTok_rBrace;
    case '[': return jsonTok_lBracket;
    case ']': return jsonTok_rBracket;
    case ':': return jsonTok_colon;
    case ',': return jsonTok_comma;
    case '"': return ScanString(r, text, textSize);
    default:  break;
    }

    if ((ch == '-') || ((ch >= '0') && (ch <= '9'))) {
        return ScanNumber(r, ch, text, textSize);
    }
    if ((ch == 't') || (ch == 'f') || (ch == 'n')) {
        return ScanLiteral(r, ch);
    }

    return Fail(r, "unexpected character");
}

int Json_Expect(sJsonReader *r, eJsonTok want)
{
    char text[TOK_SCRATCH];

    if (Json_Next(r, text, sizeof(text)) != want) {
        (void)Fail(r, "unexpected token");
        return -1;
    }
    return 0;
}

int Json_ReadString(sJsonReader *r, char *out, uint32_t cap)
{
    return (Json_Next(r, out, cap) == jsonTok_string) ? 1 : 0;
}

int Json_ReadI32(sJsonReader *r, int32_t *out)
{
    char text[16];

    if (Json_Next(r, text, sizeof(text)) != jsonTok_number) {
        return 0;
    }
    return (Json_ToI32(text, out) == 0) ? 1 : 0;
}

int Json_ReadBool(sJsonReader *r, int *out)
{
    char text[TOK_SCRATCH];

    switch (Json_Next(r, text, sizeof(text))) {
    case jsonTok_true:  *out = 1; return 1;
    case jsonTok_false: *out = 0; return 1;
    default:            break;
    }
    return 0;
}

int Json_ToI32(const char *text, int32_t *out)
{
    int32_t     v   = 0;
    int         neg = 0;
    int         any = 0;
    const char *p   = text;

    if (*p == '-') {
        neg = 1;
        p++;
    }
    for (; *p != '\0'; p++) {
        const int32_t d = (int32_t)(*p - '0');

        if ((*p < '0') || (*p > '9')) {
            return -1;              /* integers only: '.' lands here too    */
        }
        /* REJECT rather than wrap.  Signed overflow is undefined behaviour,
         * and a wrapped value is worse than a rejected one: a 14-digit
         * nameplate_ah once wrapped into a small positive number and passed
         * every downstream check. */
        if (v > ((INT32_MAX - d) / 10)) {
            return -1;
        }
        v = (v * 10) + d;
        any = 1;
    }
    if (any == 0) {
        return -1;
    }
    *out = neg ? -v : v;

    return 0;
}

int Json_ToU32(const char *text, uint32_t *out)
{
    uint32_t    v   = 0u;
    int         any = 0;
    const char *p   = text;

    for (; *p != '\0'; p++) {
        const uint32_t d = (uint32_t)(*p - '0');

        if ((*p < '0') || (*p > '9')) {
            return -1;
        }
        if (v > ((0xFFFFFFFFu - d) / 10u)) {
            return -1;
        }
        v = (v * 10u) + d;
        any = 1;
    }
    if (any == 0) {
        return -1;
    }
    *out = v;

    return 0;
}

void Json_MemSourceInit(sJsonMemSource *m, const void *data,
                        uint32_t len_bytes)
{
    m->data      = (const uint8_t *)data;
    m->len_bytes = len_bytes;
    m->pos       = 0u;
}

int Json_MemRead(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sJsonMemSource *m = (sJsonMemSource *)ctx;
    uint32_t        n = m->len_bytes - m->pos;

    if (n > maxLen) {
        n = maxLen;
    }
    if (n > 0u) {
        (void)memcpy(buf, &m->data[m->pos], n);
        m->pos += n;
    }
    return (int)n;
}

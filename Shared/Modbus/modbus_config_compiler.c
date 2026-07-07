#include "modbus_config_compiler.h"
#include "modbus_units.h"
#include "image_mgmt.h"
#include "w25q128.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ==========================================================================
 * Static state — the compiler runs on the 4 KB HTTP task stack, so nothing
 * big may live on the stack. Not reentrant by design (see header).
 * ========================================================================== */

#define LEX_WINDOW      128u   /* refill window over the byte source */
#define TOK_MAX         32u    /* longest key/value token + NUL      */
#define SCALE_POW_MAX   6      /* |scalePow10| ceiling               */

typedef enum {
    TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_COLON, TOK_COMMA, TOK_STRING, TOK_NUMBER,
    TOK_TRUE, TOK_FALSE, TOK_EOF, TOK_ERR,
} eTok;

typedef struct {
    /* lexer */
    fMbByteSource src;
    void         *srcCtx;
    uint8_t       window[LEX_WINDOW];
    uint32_t      winLen;
    uint32_t      winPos;
    int           eof;
    int           ioErr;

    /* flash writer (stream starts at regionBase + MODBUS_LUT_HEADER_SIZE) */
    uint32_t      base;
    uint32_t      streamOff;         /* bytes of stream written so far */
    uint8_t       page[W25Q128_PAGE_SIZE];
    uint32_t      pageLen;
    uint32_t      nextEraseOff;      /* region offset of next unerased sector */
    uint32_t      crc;
    void        (*kick)(void);
    int           flashErr;

    /* progress / bounds */
    int           devIdx, txnIdx, ptIdx;
    uint16_t      txnsTotal;
    uint16_t      pointsTotal;

    sMbCompileResult *res;
} sCompiler;

/* On the STM32 target the compiler state lives in CCM (CPU-only memory —
 * fine here: the SPI flash driver does polled transfers, and both buffers
 * are fully written by MbCfgCompile before any read). Host tests build
 * without the section attribute. */
#if defined(STM32F407xx)
#define MB_COMPILER_BSS __attribute__((section(".ccmram")))
#else
#define MB_COMPILER_BSS
#endif

MB_COMPILER_BSS static sCompiler s_c;

/* Per-transaction point buffer: the transaction record carries the derived
 * register count and precedes its points in the stream. */
MB_COMPILER_BSS static sModbusPointRecord s_ptBuf[MB_MAX_POINTS_PER_TXN];

/* ==========================================================================
 * Failure reporting — only the first failure is recorded
 * ========================================================================== */

static int fail(const char *field, const char *reason)
{
    sMbCompileResult *r = s_c.res;

    if (r->reason[0] == '\0') {
        r->deviceIdx = s_c.devIdx;
        r->txnIdx    = s_c.txnIdx;
        r->pointIdx  = s_c.ptIdx;
        snprintf(r->field, sizeof(r->field), "%s", field);
        snprintf(r->reason, sizeof(r->reason), "%s", reason);
    }
    return -1;
}

/* ==========================================================================
 * Lexer
 * ========================================================================== */

static int lex_peek(void)
{
    if (s_c.winPos >= s_c.winLen) {
        if (s_c.eof || s_c.ioErr) {
            return -1;
        }
        int n = s_c.src(s_c.srcCtx, s_c.window, LEX_WINDOW);
        if (n < 0) {
            s_c.ioErr = 1;
            return -1;
        }
        if (n == 0) {
            s_c.eof = 1;
            return -1;
        }
        s_c.winLen = (uint32_t)n;
        s_c.winPos = 0;
    }
    return s_c.window[s_c.winPos];
}

static int lex_get(void)
{
    int ch = lex_peek();
    if (ch >= 0) {
        s_c.winPos++;
    }
    return ch;
}

static void lex_skip_ws(void)
{
    int ch;
    while ((ch = lex_peek()) == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        s_c.winPos++;
    }
}

/* Read the next token; string/number text lands NUL-terminated in `text`. */
static eTok next_token(char *text, uint32_t textSize)
{
    lex_skip_ws();

    int ch = lex_get();
    if (ch < 0) {
        if (s_c.ioErr) {
            fail("json", "read error");
            return TOK_ERR;
        }
        return TOK_EOF;
    }

    switch (ch) {
    case '{': return TOK_LBRACE;
    case '}': return TOK_RBRACE;
    case '[': return TOK_LBRACKET;
    case ']': return TOK_RBRACKET;
    case ':': return TOK_COLON;
    case ',': return TOK_COMMA;
    default: break;
    }

    if (ch == '"') {
        uint32_t n = 0;
        for (;;) {
            ch = lex_get();
            if (ch < 0) {
                fail("json", "unterminated string");
                return TOK_ERR;
            }
            if (ch == '"') {
                break;
            }
            if (ch == '\\') {
                fail("json", "string escapes not supported");
                return TOK_ERR;
            }
            if (n + 1 >= textSize) {
                fail("json", "string too long");
                return TOK_ERR;
            }
            text[n++] = (char)ch;
        }
        text[n] = '\0';
        return TOK_STRING;
    }

    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        uint32_t n = 0;
        text[n++] = (char)ch;
        for (;;) {
            ch = lex_peek();
            if (ch == '.' || (ch >= '0' && ch <= '9')) {
                if (n + 1 >= textSize) {
                    fail("json", "number too long");
                    return TOK_ERR;
                }
                text[n++] = (char)lex_get();
            } else if (ch == 'e' || ch == 'E') {
                fail("json", "exponent notation not supported");
                return TOK_ERR;
            } else {
                break;
            }
        }
        text[n] = '\0';
        return TOK_NUMBER;
    }

    if (ch == 't' || ch == 'f') {
        const char *rest = (ch == 't') ? "rue" : "alse";
        while (*rest) {
            if (lex_get() != *rest++) {
                fail("json", "bad literal");
                return TOK_ERR;
            }
        }
        return (ch == 't') ? TOK_TRUE : TOK_FALSE;
    }

    fail("json", "unexpected character");
    return TOK_ERR;
}

static int expect(eTok want, const char *what)
{
    char text[TOK_MAX];
    eTok t = next_token(text, sizeof(text));
    if (t == TOK_ERR) {
        return -1;
    }
    if (t != want) {
        return fail("json", what);
    }
    return 0;
}

/* ==========================================================================
 * Number conversion (text -> integer domains; no floating point)
 * ========================================================================== */

static int parse_i32(const char *text, int32_t *out)
{
    int32_t     v = 0;
    int         neg = 0;
    const char *p = text;

    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (*p == '\0' || strchr(p, '.') != NULL) {
        return -1;                       /* integers only */
    }
    for (; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        if (v > (INT32_MAX - (*p - '0')) / 10) {
            return -1;
        }
        v = v * 10 + (*p - '0');
    }
    *out = neg ? -v : v;
    return 0;
}

static int parse_bounded(const char *text, int32_t lo, int32_t hi, int32_t *out)
{
    int32_t v;
    if (parse_i32(text, &v) != 0 || v < lo || v > hi) {
        return -1;
    }
    *out = v;
    return 0;
}

/* scale must be an exact power of ten: one '1' digit, all other digits '0'.
 * "10" -> 1, "1" -> 0, "0.1" -> -1, "0.01" -> -2 ... */
static int parse_scale_pow10(const char *text, int8_t *out)
{
    const char *dot = strchr(text, '.');
    int         ones = 0;
    int         pow = 0;

    if (text[0] == '-' || text[0] == '\0') {
        return -1;
    }

    for (const char *p = text; *p; p++) {
        if (*p == '.') {
            if (p != dot) {
                return -1;
            }
            continue;
        }
        if (*p == '1') {
            ones++;
            /* digits after the '1': before the dot each adds +1,
             * after the dot the '1' position sets a negative power */
            if (!dot || p < dot) {
                pow = (int)((dot ? dot : text + strlen(text)) - p) - 1;
            } else {
                pow = -(int)(p - dot);
            }
        } else if (*p != '0') {
            return -1;
        }
    }

    if (ones != 1 || pow < -SCALE_POW_MAX || pow > SCALE_POW_MAX) {
        return -1;
    }
    *out = (int8_t)pow;
    return 0;
}

/* ==========================================================================
 * Flash writer — page-buffered, lazy sector erase, incremental CRC.
 * Stream data begins at regionBase + MODBUS_LUT_HEADER_SIZE; the header
 * page is written last (see fw_finish).
 * ========================================================================== */

static int fw_erase_up_to(uint32_t regionOff)
{
    while (s_c.nextEraseOff <= regionOff) {
        if (s_c.kick) {
            s_c.kick();
        }
        if (W25Q128_EraseSector(s_c.base + s_c.nextEraseOff) != W25Q128_OK) {
            s_c.flashErr = 1;
            return fail("flash", "sector erase failed");
        }
        s_c.nextEraseOff += W25Q128_SECTOR_SIZE;
    }
    return 0;
}

static int fw_flush_page(void)
{
    if (s_c.pageLen == 0u) {
        return 0;
    }

    uint32_t regionOff = MODBUS_LUT_HEADER_SIZE + s_c.streamOff - s_c.pageLen;

    if (fw_erase_up_to(regionOff + s_c.pageLen - 1u) != 0) {
        return -1;
    }
    if (W25Q128_WritePage(s_c.base + regionOff, s_c.page,
                          s_c.pageLen) != W25Q128_OK) {
        s_c.flashErr = 1;
        return fail("flash", "page write failed");
    }
    s_c.pageLen = 0;
    return 0;
}

static int fw_write(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (MODBUS_LUT_HEADER_SIZE + s_c.streamOff + len > EXT_FLASH_MODBUS_LUT_SIZE) {
        return fail("config", "compiled stream exceeds region size");
    }

    s_c.crc = ImgMgmt_Crc32Update(s_c.crc, p, len);

    while (len > 0u) {
        uint32_t n = W25Q128_PAGE_SIZE - s_c.pageLen;
        if (n > len) {
            n = len;
        }
        memcpy(&s_c.page[s_c.pageLen], p, n);
        s_c.pageLen   += n;
        s_c.streamOff += n;
        p   += n;
        len -= n;
        if (s_c.pageLen == W25Q128_PAGE_SIZE && fw_flush_page() != 0) {
            return -1;
        }
    }
    return 0;
}

static int fw_finish(void)
{
    if (fw_flush_page() != 0) {
        return -1;
    }

    sModbusLutHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic     = MODBUS_LUT_MAGIC;
    hdr.version   = MODBUS_LUT_VERSION;
    hdr.streamLen = s_c.streamOff;
    hdr.crc32     = ImgMgmt_Crc32Final(s_c.crc);

    if (W25Q128_WritePage(s_c.base, (const uint8_t *)&hdr,
                          sizeof(hdr)) != W25Q128_OK) {
        s_c.flashErr = 1;
        return fail("flash", "header write failed");
    }
    return 0;
}

/* ==========================================================================
 * Schema helpers
 * ========================================================================== */

static int name_chars_ok(const char *s)
{
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) {
            return 0;
        }
    }
    return 1;
}

static int decode_type_from_string(const char *s, uint8_t *out)
{
    static const struct { const char *str; uint8_t type; } map[] = {
        { "u16",        MB_DECODE_U16 },
        { "s16",        MB_DECODE_S16 },
        { "u32_be",     MB_DECODE_U32_BE },
        { "u32_le",     MB_DECODE_U32_LE },
        { "s32_be",     MB_DECODE_S32_BE },
        { "s32_le",     MB_DECODE_S32_LE },
        { "float32_be", MB_DECODE_FLOAT32_BE },
        { "float32_le", MB_DECODE_FLOAT32_LE },
        { "bitfield",   MB_DECODE_BITFIELD },
        { "ascii",      MB_DECODE_ASCII },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].str, s) == 0) {
            *out = map[i].type;
            return 0;
        }
    }
    return -1;
}

/* ==========================================================================
 * Point object
 * ========================================================================== */

typedef struct {
    sModbusPointRecord rec;
    int hasOffset, hasDecode, hasScale, hasUnit, hasName;
    int hasLength, hasWriteMin, hasWriteMax, writable;
} sPointCtx;

static int parse_publish_object(sPointCtx *pc)
{
    char key[TOK_MAX], val[TOK_MAX];

    if (expect(TOK_LBRACE, "expected '{' for publish") != 0) {
        return -1;
    }

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == TOK_RBRACE) {
            return 0;
        }
        if (t == TOK_COMMA) {
            continue;
        }
        if (t != TOK_STRING) {
            return fail("publish", "expected key string");
        }
        if (expect(TOK_COLON, "expected ':'") != 0) {
            return -1;
        }
        if (next_token(val, sizeof(val)) != TOK_NUMBER) {
            return fail("publish", "expected number");
        }

        int32_t v;
        if (strcmp(key, "threshold") == 0) {
            if (parse_bounded(val, 0, UINT16_MAX, &v) != 0) {
                return fail("threshold", "must be 0..65535 (scaled-int)");
            }
            pc->rec.publishThreshold = (uint16_t)v;
        } else if (strcmp(key, "heartbeatS") == 0) {
            if (parse_bounded(val, 0, UINT16_MAX, &v) != 0) {
                return fail("heartbeatS", "must be 0..65535");
            }
            pc->rec.publishHeartbeatS = (uint16_t)v;
        } else {
            return fail(key, "unknown publish key");
        }
    }
}

static int parse_point(sModbusPointRecord *out)
{
    char      key[TOK_MAX], val[TOK_MAX];
    sPointCtx pc;
    int32_t   v;

    memset(&pc, 0, sizeof(pc));
    pc.rec.writeMin = INT16_MIN;         /* default: no range validation */
    pc.rec.writeMax = INT16_MAX;

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == TOK_RBRACE) {
            break;
        }
        if (t == TOK_COMMA) {
            continue;
        }
        if (t != TOK_STRING) {
            return fail("json", "expected key string in point");
        }
        if (expect(TOK_COLON, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "offset") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_bounded(val, 0, MB_MAX_REGS_PER_TXN - 1, &v) != 0) {
                return fail("offset", "must be 0..124");
            }
            pc.rec.offset = (uint16_t)v;
            pc.hasOffset = 1;
        } else if (strcmp(key, "decodeType") == 0) {
            if (next_token(val, sizeof(val)) != TOK_STRING ||
                decode_type_from_string(val, &pc.rec.decodeType) != 0) {
                return fail("decodeType", "unknown decode type");
            }
            pc.hasDecode = 1;
        } else if (strcmp(key, "scale") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_scale_pow10(val, &pc.rec.scalePow10) != 0) {
                return fail("scale", "must be an exact power of ten");
            }
            pc.hasScale = 1;
        } else if (strcmp(key, "unit") == 0) {
            if (next_token(val, sizeof(val)) != TOK_STRING) {
                return fail("unit", "expected string");
            }
            const sMbUnitInfo *u = MbUnits_FromString(val);
            if (!u) {
                return fail("unit", "unknown unit string");
            }
            pc.rec.unit = u->code;
            pc.hasUnit = 1;
        } else if (strcmp(key, "name") == 0) {
            if (next_token(val, sizeof(val)) != TOK_STRING ||
                val[0] == '\0' || strlen(val) >= MB_POINT_NAME_LEN ||
                !name_chars_ok(val)) {
                return fail("name", "1..23 chars of [A-Za-z0-9_-]");
            }
            strncpy(pc.rec.name, val, MB_POINT_NAME_LEN - 1);
            pc.hasName = 1;
        } else if (strcmp(key, "length") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_bounded(val, 1, MB_MAX_POINTS_PER_TXN, &v) != 0) {
                return fail("length", "must be 1..24 registers");
            }
            pc.rec.length = (uint8_t)v;
            pc.hasLength = 1;
        } else if (strcmp(key, "writable") == 0) {
            eTok bt = next_token(val, sizeof(val));
            if (bt != TOK_TRUE && bt != TOK_FALSE) {
                return fail("writable", "expected true/false");
            }
            pc.writable = (bt == TOK_TRUE);
        } else if (strcmp(key, "writeMin") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_bounded(val, INT16_MIN, INT16_MAX, &v) != 0) {
                return fail("writeMin", "must fit int16 (scaled-int domain)");
            }
            pc.rec.writeMin = (int16_t)v;
            pc.hasWriteMin = 1;
        } else if (strcmp(key, "writeMax") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_bounded(val, INT16_MIN, INT16_MAX, &v) != 0) {
                return fail("writeMax", "must fit int16 (scaled-int domain)");
            }
            pc.rec.writeMax = (int16_t)v;
            pc.hasWriteMax = 1;
        } else if (strcmp(key, "publish") == 0) {
            if (parse_publish_object(&pc) != 0) {
                return -1;
            }
        } else {
            return fail(key, "unknown point key");
        }
    }

    /* Cross-field rules (design §4) */
    if (!pc.hasOffset || !pc.hasDecode || !pc.hasScale ||
        !pc.hasUnit || !pc.hasName) {
        return fail("point", "offset/decodeType/scale/unit/name required");
    }
    if (pc.rec.decodeType == MB_DECODE_ASCII) {
        if (!pc.hasLength) {
            return fail("length", "required for ascii points");
        }
    } else if (pc.hasLength) {
        return fail("length", "only valid for ascii points");
    }
    if (!pc.writable && (pc.hasWriteMin || pc.hasWriteMax)) {
        return fail("writeMin", "only valid with writable:true");
    }
    if (pc.hasWriteMin && pc.hasWriteMax &&
        pc.rec.writeMin > pc.rec.writeMax) {
        return fail("writeMin", "writeMin exceeds writeMax");
    }
    if (pc.writable) {
        if (MbRecords_RegWidth(pc.rec.decodeType, pc.rec.length) != 1u) {
            return fail("writable", "writable needs a 1-register type");
        }
        pc.rec.flags |= MB_POINT_FLAG_WRITABLE;
    } else {
        /* Ignored for read-only points — canonical zeroes keep the
         * compiled stream deterministic. */
        pc.rec.writeMin = 0;
        pc.rec.writeMax = 0;
    }

    uint8_t width = MbRecords_RegWidth(pc.rec.decodeType, pc.rec.length);
    if ((uint32_t)pc.rec.offset + width > MB_MAX_REGS_PER_TXN) {
        return fail("offset", "offset+width exceeds 125-register ceiling");
    }

    *out = pc.rec;
    return 0;
}

/* ==========================================================================
 * Transaction object (buffers its points; record precedes them on flash)
 * ========================================================================== */

static int parse_transaction(void)
{
    char     key[TOK_MAX], val[TOK_MAX];
    int32_t  v;
    sModbusTransactionRecord txn;
    int      hasStart = 0, hasFc = 0, hasPeriod = 0;
    int      nPoints = 0;

    memset(&txn, 0, sizeof(txn));

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == TOK_RBRACE) {
            break;
        }
        if (t == TOK_COMMA) {
            continue;
        }
        if (t != TOK_STRING) {
            return fail("json", "expected key string in transaction");
        }
        if (expect(TOK_COLON, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "startAddr") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_bounded(val, 0, UINT16_MAX, &v) != 0) {
                return fail("startAddr", "must be 0..65535");
            }
            txn.startAddr = (uint16_t)v;
            hasStart = 1;
        } else if (strcmp(key, "functionCode") == 0) {
            if (next_token(val, sizeof(val)) != TOK_STRING) {
                return fail("functionCode", "expected string");
            }
            if (strcmp(val, "holding") == 0) {
                txn.functionCode = MB_FC_HOLDING;
            } else if (strcmp(val, "input") == 0) {
                txn.functionCode = MB_FC_INPUT;
            } else {
                return fail("functionCode", "must be \"holding\" or \"input\"");
            }
            hasFc = 1;
        } else if (strcmp(key, "readPeriodS") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_bounded(val, 1, UINT16_MAX, &v) != 0) {
                return fail("readPeriodS", "must be 1..65535");
            }
            txn.readPeriodS = (uint16_t)v;
            hasPeriod = 1;
        } else if (strcmp(key, "points") == 0) {
            if (expect(TOK_LBRACKET, "expected '[' for points") != 0) {
                return -1;
            }
            for (;;) {
                eTok pt = next_token(val, sizeof(val));
                if (pt == TOK_RBRACKET) {
                    break;
                }
                if (pt == TOK_COMMA) {
                    continue;
                }
                if (pt != TOK_LBRACE) {
                    return fail("points", "expected point object");
                }
                s_c.ptIdx = nPoints;
                if (nPoints >= MB_MAX_POINTS_PER_TXN) {
                    return fail("points", "too many points in transaction");
                }
                if (s_c.pointsTotal >= MB_MAX_POINTS_TOTAL) {
                    return fail("points", "config exceeds total point budget");
                }
                if (parse_point(&s_ptBuf[nPoints]) != 0) {
                    return -1;
                }
                nPoints++;
                s_c.pointsTotal++;
            }
            s_c.ptIdx = -1;
        } else {
            return fail(key, "unknown transaction key");
        }
    }

    if (!hasStart || !hasFc || !hasPeriod) {
        return fail("transaction", "startAddr/functionCode/readPeriodS required");
    }
    if (nPoints == 0) {
        return fail("points", "transaction has no points");
    }

    /* Derive the register block length (never authored, design §4) */
    uint32_t count = 1;
    for (int i = 0; i < nPoints; i++) {
        uint32_t end = (uint32_t)s_ptBuf[i].offset +
                       MbRecords_RegWidth(s_ptBuf[i].decodeType,
                                          s_ptBuf[i].length);
        if (end > count) {
            count = end;
        }
    }
    if (count > MB_MAX_REGS_PER_TXN) {
        return fail("count", "derived register count exceeds 125");
    }
    txn.count = (uint8_t)count;

    /* Emit: transaction record, its points, point sentinel */
    if (fw_write(&txn, sizeof(txn)) != 0) {
        return -1;
    }
    for (int i = 0; i < nPoints; i++) {
        if (fw_write(&s_ptBuf[i], sizeof(s_ptBuf[i])) != 0) {
            return -1;
        }
    }
    sModbusPointRecord ptEnd;
    memset(&ptEnd, 0, sizeof(ptEnd));
    return fw_write(&ptEnd, sizeof(ptEnd));
}

/* ==========================================================================
 * Device object
 * ========================================================================== */

static int parse_device(void)
{
    char    key[TOK_MAX], val[TOK_MAX];
    int32_t v;
    sModbusDeviceRecord dev;
    int     hasAddr = 0, hasPrefix = 0, devWritten = 0;
    int     nTxns = 0;

    memset(&dev, 0, sizeof(dev));

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == TOK_RBRACE) {
            break;
        }
        if (t == TOK_COMMA) {
            continue;
        }
        if (t != TOK_STRING) {
            return fail("json", "expected key string in device");
        }
        if (expect(TOK_COLON, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "slaveAddr") == 0) {
            if (next_token(val, sizeof(val)) != TOK_NUMBER ||
                parse_bounded(val, 1, 247, &v) != 0) {
                return fail("slaveAddr", "must be 1..247");
            }
            dev.slaveAddr = (uint8_t)v;
            hasAddr = 1;
        } else if (strcmp(key, "topicPrefix") == 0) {
            if (next_token(val, sizeof(val)) != TOK_STRING ||
                val[0] == '\0' || strlen(val) >= MB_TOPIC_PREFIX_LEN ||
                !name_chars_ok(val)) {
                return fail("topicPrefix", "1..15 chars of [A-Za-z0-9_-]");
            }
            strncpy(dev.topicPrefix, val, MB_TOPIC_PREFIX_LEN - 1);
            hasPrefix = 1;
        } else if (strcmp(key, "transactions") == 0) {
            /* Device record must be on flash before its transactions */
            if (!hasAddr || !hasPrefix) {
                return fail("transactions",
                            "slaveAddr/topicPrefix must precede transactions");
            }
            if (!devWritten) {
                if (fw_write(&dev, sizeof(dev)) != 0) {
                    return -1;
                }
                devWritten = 1;
            }
            if (expect(TOK_LBRACKET, "expected '[' for transactions") != 0) {
                return -1;
            }
            for (;;) {
                eTok tt = next_token(val, sizeof(val));
                if (tt == TOK_RBRACKET) {
                    break;
                }
                if (tt == TOK_COMMA) {
                    continue;
                }
                if (tt != TOK_LBRACE) {
                    return fail("transactions", "expected transaction object");
                }
                s_c.txnIdx = nTxns;
                if (nTxns >= MB_MAX_TXNS_PER_DEVICE) {
                    return fail("transactions", "too many transactions in device");
                }
                if (s_c.txnsTotal >= MB_MAX_TXNS_TOTAL) {
                    return fail("transactions",
                                "config exceeds total transaction budget");
                }
                if (parse_transaction() != 0) {
                    return -1;
                }
                nTxns++;
                s_c.txnsTotal++;
                s_c.ptIdx = -1;
            }
            s_c.txnIdx = -1;
        } else {
            return fail(key, "unknown device key");
        }
    }

    if (!hasAddr || !hasPrefix) {
        return fail("device", "slaveAddr and topicPrefix required");
    }
    if (nTxns == 0) {
        return fail("transactions", "device has no transactions");
    }

    /* Transaction sentinel closes the device */
    sModbusTransactionRecord txnEnd;
    memset(&txnEnd, 0, sizeof(txnEnd));
    return fw_write(&txnEnd, sizeof(txnEnd));
}

/* ==========================================================================
 * Top level
 * ========================================================================== */

static int parse_config(void)
{
    char key[TOK_MAX], val[TOK_MAX];
    int  nDevices = 0;

    if (expect(TOK_LBRACE, "expected '{'") != 0) {
        return -1;
    }
    if (next_token(key, sizeof(key)) != TOK_STRING ||
        strcmp(key, "devices") != 0) {
        return fail("json", "expected \"devices\" key");
    }
    if (expect(TOK_COLON, "expected ':'") != 0 ||
        expect(TOK_LBRACKET, "expected '[' for devices") != 0) {
        return -1;
    }

    for (;;) {
        eTok t = next_token(val, sizeof(val));
        if (t == TOK_RBRACKET) {
            break;
        }
        if (t == TOK_COMMA) {
            continue;
        }
        if (t != TOK_LBRACE) {
            return fail("devices", "expected device object");
        }
        s_c.devIdx = nDevices;
        if (nDevices >= MB_MAX_DEVICES) {
            return fail("devices", "too many devices");
        }
        if (parse_device() != 0) {
            return -1;
        }
        nDevices++;
        s_c.txnIdx = -1;
        s_c.ptIdx  = -1;
    }
    s_c.devIdx = -1;

    if (nDevices == 0) {
        return fail("devices", "config has no devices");
    }

    if (expect(TOK_RBRACE, "expected '}'") != 0) {
        return -1;
    }
    lex_skip_ws();
    if (lex_peek() >= 0) {
        return fail("json", "trailing data after config");
    }
    if (s_c.ioErr) {
        return fail("json", "read error");
    }

    /* Device sentinel ends the whole config */
    sModbusDeviceRecord devEnd;
    memset(&devEnd, 0, sizeof(devEnd));
    if (fw_write(&devEnd, sizeof(devEnd)) != 0) {
        return -1;
    }

    s_c.res->counts.devices      = (uint8_t)nDevices;
    s_c.res->counts.transactions = (uint8_t)s_c.txnsTotal;
    s_c.res->counts.points       = s_c.pointsTotal;
    return 0;
}

int MbCfgCompile(fMbByteSource src, void *srcCtx, uint32_t regionBase,
                 void (*kick)(void), sMbCompileResult *res)
{
    if (!src || !res ||
        (regionBase != EXT_FLASH_MODBUS_LUT_A_ADDR &&
         regionBase != EXT_FLASH_MODBUS_LUT_B_ADDR)) {
        return -1;
    }

    memset(res, 0, sizeof(*res));
    res->deviceIdx = -1;
    res->txnIdx    = -1;
    res->pointIdx  = -1;

    memset(&s_c, 0, sizeof(s_c));
    s_c.src     = src;
    s_c.srcCtx  = srcCtx;
    s_c.base    = regionBase;
    s_c.kick    = kick;
    s_c.crc     = ImgMgmt_Crc32Init();
    s_c.res     = res;
    s_c.devIdx  = -1;
    s_c.txnIdx  = -1;
    s_c.ptIdx   = -1;

    /* Kill any previous config in this region up front: a compile that
     * fails halfway must not leave a stale-but-valid header behind. */
    if (kick) {
        kick();
    }
    if (W25Q128_EraseSector(regionBase) != W25Q128_OK) {
        fail("flash", "sector erase failed");
        return -1;
    }
    s_c.nextEraseOff = W25Q128_SECTOR_SIZE;

    if (parse_config() != 0 || fw_finish() != 0) {
        return -1;
    }

    res->ok = 1;
    return 0;
}

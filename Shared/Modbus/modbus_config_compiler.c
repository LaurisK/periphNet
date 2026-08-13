/**
 * @file    modbus_config_compiler.c
 * @brief   Streaming JSON -> record-stream compiler (v2), see the header.
 *
 * ONE STREAMING PASS, and THE PASS IS THE VALIDATION (docs/modbus.md §7.4).
 * The parser reads a stream it cannot rewind and holds no index, which is why
 * every reference is a dense authored id, why section order is forced, and why
 * names link nothing.  Each section references only what precedes it —
 * capabilities, then devices (checking capId), then plans (checking capId,
 * every deviceId against the device count AND against that device's own
 * capability, and every pointId against the named capability's point count) —
 * so every count a range check needs is a running total already in hand.
 *
 * Ids are AUTHORED even though position is identity, and the compiler rejects
 * a run that is not 0, 1, 2 ...: that is the whole point of authoring them.
 * An insertion or deletion that would silently re-point every reference
 * downstream becomes a compile error naming the object where the run breaks
 * (§1.2, "author the redundancy that makes silence loud").
 */

#include "modbus_config_compiler.h"
#include "modbus_blocks.h"
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

#define PT_BITMAP_BYTES ((MB_MAX_POINTS_TOTAL + 7u) / 8u)

typedef enum {
    tok_lBrace, tok_rBrace, tok_lBracket, tok_rBracket,
    tok_colon, tok_comma, tok_string, tok_number,
    tok_true, tok_false, tok_eof, tok_err,
    tok_last                       /* sentinel */
} eTok;

typedef struct {
    /* lexer */
    fModbusByteSource src;
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
    int           dryRun;            /* verify: count, do not write */

    /* progress, for the failure path */
    int           capIdx, devIdx, planIdx, subIdx;

    /* running totals — everything a range check needs (§7.2) */
    sModbusConfigCounts counts;
    uint16_t      capPointCount[MB_MAX_CAPABILITIES];
    uint16_t      capPointBase[MB_MAX_CAPABILITIES];  /* into the bitmaps */
    uint8_t       devCap[MB_MAX_DEVICES];
    uint8_t       planSlotSeen;      /* bitmask: a slot may appear once   */
    int           lastPlanId;        /* plans are authored ascending      */
    uint16_t      ttEntriesTotal;

    /* Which points may be READ, so a time table listing a write-only point
     * is a compile error (§3.2) — indexed by capPointBase[cap] + ptOrd. */
    uint8_t       ptReadable[PT_BITMAP_BYTES];
    /* Points already claimed by a time table OF THE CURRENT PLAN. */
    uint8_t       ptSeen[PT_BITMAP_BYTES];

    sModbusCompileResult *res;
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

/* ==========================================================================
 * Failure reporting — only the first failure is recorded
 * ========================================================================== */

static int fail(const char *field, const char *reason)
{
    sModbusCompileResult *r = s_c.res;

    if (r->reason[0] == '\0') {
        r->capIdx  = s_c.capIdx;
        r->devIdx  = s_c.devIdx;
        r->planIdx = s_c.planIdx;
        r->subIdx  = s_c.subIdx;
        r->counts  = s_c.counts;
        snprintf(r->field, sizeof(r->field), "%s", field);
        snprintf(r->reason, sizeof(r->reason), "%s", reason);
    }
    return -1;
}

static inline void bit_set(uint8_t *bits, uint16_t i)
{
    bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
}

static inline int bit_get(const uint8_t *bits, uint16_t i)
{
    return (bits[i / 8u] >> (i % 8u)) & 1u;
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
            return tok_err;
        }
        return tok_eof;
    }

    switch (ch) {
    case '{': return tok_lBrace;
    case '}': return tok_rBrace;
    case '[': return tok_lBracket;
    case ']': return tok_rBracket;
    case ':': return tok_colon;
    case ',': return tok_comma;
    default: break;
    }

    if (ch == '"') {
        uint32_t n = 0;
        for (;;) {
            ch = lex_get();
            if (ch < 0) {
                fail("json", "unterminated string");
                return tok_err;
            }
            if (ch == '"') {
                break;
            }
            if (ch == '\\') {
                fail("json", "string escapes not supported");
                return tok_err;
            }
            if (n + 1 >= textSize) {
                fail("json", "string too long");
                return tok_err;
            }
            text[n++] = (char)ch;
        }
        text[n] = '\0';
        return tok_string;
    }

    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        uint32_t n = 0;
        text[n++] = (char)ch;
        for (;;) {
            ch = lex_peek();
            if (ch == '.' || (ch >= '0' && ch <= '9')) {
                if (n + 1 >= textSize) {
                    fail("json", "number too long");
                    return tok_err;
                }
                text[n++] = (char)lex_get();
            } else if (ch == 'e' || ch == 'E') {
                fail("json", "exponent notation not supported");
                return tok_err;
            } else {
                break;
            }
        }
        text[n] = '\0';
        return tok_number;
    }

    if (ch == 't' || ch == 'f') {
        const char *rest = (ch == 't') ? "rue" : "alse";
        while (*rest) {
            if (lex_get() != *rest++) {
                fail("json", "bad literal");
                return tok_err;
            }
        }
        return (ch == 't') ? tok_true : tok_false;
    }

    fail("json", "unexpected character");
    return tok_err;
}

static int expect(eTok want, const char *what)
{
    char text[TOK_MAX];
    eTok t = next_token(text, sizeof(text));
    if (t == tok_err) {
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
    if (s_c.dryRun) {
        return 0;
    }
    while (s_c.nextEraseOff <= regionOff) {
        if (s_c.kick) {
            s_c.kick();
        }
        if (W25Q128_EraseSector(s_c.base + s_c.nextEraseOff) != w25q_ok) {
            s_c.flashErr = 1;
            return fail("flash", "sector erase failed");
        }
        s_c.nextEraseOff += W25Q128_SECTOR_SIZE;
    }
    return 0;
}

static int fw_flush_page(void)
{
    if (s_c.dryRun) {
        s_c.pageLen = 0;
        return 0;
    }
    if (s_c.pageLen == 0u) {
        return 0;
    }

    uint32_t regionOff = MODBUS_LUT_HEADER_SIZE + s_c.streamOff - s_c.pageLen;

    if (fw_erase_up_to(regionOff + s_c.pageLen - 1u) != 0) {
        return -1;
    }
    if (W25Q128_WritePage(s_c.base + regionOff, s_c.page,
                          s_c.pageLen) != w25q_ok) {
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
    if (s_c.dryRun) {
        return 0;                    /* nothing was written, so nothing to
                                        make valid */
    }

    sModbusLutHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic     = MODBUS_LUT_MAGIC;
    hdr.version   = MODBUS_LUT_VERSION;
    hdr.streamLen = s_c.streamOff;
    hdr.crc32     = ImgMgmt_Crc32Final(s_c.crc);

    if (W25Q128_WritePage(s_c.base, (const uint8_t *)&hdr,
                          sizeof(hdr)) != w25q_ok) {
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
        { "u16",        mbDecode_u16 },
        { "s16",        mbDecode_s16 },
        { "u32_be",     mbDecode_u32Be },
        { "u32_le",     mbDecode_u32Le },
        { "s32_be",     mbDecode_s32Be },
        { "s32_le",     mbDecode_s32Le },
        { "float32_be", mbDecode_float32Be },
        { "float32_le", mbDecode_float32Le },
        { "bitfield",   mbDecode_bitfield },
        { "ascii",      mbDecode_ascii },
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
 * Enum <-> string tables.  Both directions are needed by the exporter too, so
 * they are the single name<->code table the format depends on.
 * ========================================================================== */

static int port_from_string(const char *s, uint8_t *out)
{
    if (strcmp(s, "rs485") == 0) { *out = mbPort_rs485; return 0; }
    if (strcmp(s, "test") == 0)  { *out = mbPort_test;  return 0; }
    return -1;
}

static int format_from_string(const char *s, uint8_t *out)
{
    if (strcmp(s, "8N1") == 0) { *out = mbFmt_8N1; return 0; }
    if (strcmp(s, "8E1") == 0) { *out = mbFmt_8E1; return 0; }
    if (strcmp(s, "8O1") == 0) { *out = mbFmt_8O1; return 0; }
    if (strcmp(s, "8N2") == 0) { *out = mbFmt_8N2; return 0; }
    return -1;
}

static int fc_from_string(const char *s, uint8_t *out)
{
    if (strcmp(s, "holding") == 0) { *out = mbFc_holding; return 0; }
    if (strcmp(s, "input") == 0)   { *out = mbFc_input;   return 0; }
    return -1;
}

/* "r" (default), "w", "rw" — what the SILICON supports (§3.2). */
static int access_from_string(const char *s, uint8_t *out)
{
    if (strcmp(s, "r") == 0)  { *out = MB_PT_READ; return 0; }
    if (strcmp(s, "w") == 0)  { *out = MB_PT_WRITE; return 0; }
    if (strcmp(s, "rw") == 0) { *out = (uint8_t)(MB_PT_READ | MB_PT_WRITE);
                                return 0; }
    return -1;
}

/* Decode-type value range, for checking authored write bounds. */
static int decode_range(uint8_t decodeType, int32_t *lo, int32_t *hi)
{
    switch (decodeType) {
    case mbDecode_u16:
    case mbDecode_bitfield:  *lo = 0;          *hi = UINT16_MAX; return 0;
    case mbDecode_s16:       *lo = INT16_MIN;  *hi = INT16_MAX;  return 0;
    case mbDecode_u32Be:
    case mbDecode_u32Le:     *lo = 0;          *hi = INT32_MAX;  return 0;
    case mbDecode_s32Be:
    case mbDecode_s32Le:
    case mbDecode_float32Be:
    case mbDecode_float32Le: *lo = INT32_MIN;  *hi = INT32_MAX;  return 0;
    default:                 return -1;        /* ascii has no numeric range */
    }
}

/* ==========================================================================
 * Point object — inside a capability, after its blocks
 * ========================================================================== */

typedef struct {
    sModbusPointRecord rec;
    int hasAddr, hasDecode, hasScale, hasUnit, hasName;
    int hasLength, hasWriteMin, hasWriteMax, hasAccess;
} sPointCtx;

static int parse_point(const sModbusCapabilityRecord *cap,
                       const sModbusBlockRecord *blocks,
                       sModbusPointRecord *out)
{
    char      key[TOK_MAX], val[TOK_MAX];
    sPointCtx pc;
    int32_t   v;
    int       idSeen = 0;

    memset(&pc, 0, sizeof(pc));
    pc.rec.flags = MB_PT_READ;               /* access defaults to "r" */

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == tok_rBrace) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_string) {
            return fail("json", "expected point key");
        }
        if (expect(tok_colon, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "id") == 0) {
            if (next_token(val, sizeof(val)) != tok_number ||
                parse_i32(val, &v) != 0) {
                return fail("id", "expected number");
            }
            /* Dense: an id that does not continue the run is where an
             * insertion silently re-pointed everything after it. */
            if (v != s_c.subIdx) {
                return fail("id", "point ids must run 0, 1, 2 ...");
            }
            idSeen = 1;
            continue;
        }

        t = next_token(val, sizeof(val));
        if (t == tok_err) {
            return -1;
        }

        if (strcmp(key, "addr") == 0) {
            if (t != tok_number || parse_bounded(val, 0, UINT16_MAX, &v) != 0) {
                return fail("addr", "must be 0..65535");
            }
            pc.rec.addr = (uint16_t)v;
            pc.hasAddr = 1;
        } else if (strcmp(key, "fc") == 0) {
            if (t != tok_string || fc_from_string(val, &pc.rec.functionCode) != 0) {
                return fail("fc", "must be \"holding\" or \"input\"");
            }
        } else if (strcmp(key, "decodeType") == 0) {
            if (t != tok_string ||
                decode_type_from_string(val, &pc.rec.decodeType) != 0) {
                return fail("decodeType", "unknown decode type");
            }
            pc.hasDecode = 1;
        } else if (strcmp(key, "scale") == 0) {
            if (t != tok_number ||
                parse_scale_pow10(val, &pc.rec.scalePow10) != 0) {
                return fail("scale", "must be an exact power of ten");
            }
            pc.hasScale = 1;
        } else if (strcmp(key, "unit") == 0) {
            const sMbUnitInfo *u = (t == tok_string) ? MbUnits_FromString(val)
                                                     : NULL;
            if (u == NULL) {
                return fail("unit", "unknown unit");
            }
            pc.rec.unit = u->code;
            pc.hasUnit = 1;
        } else if (strcmp(key, "name") == 0) {
            if (t != tok_string || val[0] == '\0' ||
                strlen(val) >= MB_POINT_NAME_LEN || !name_chars_ok(val)) {
                return fail("name", "1..23 chars of [A-Za-z0-9_-]");
            }
            snprintf(pc.rec.name, sizeof(pc.rec.name), "%s", val);
            pc.hasName = 1;
        } else if (strcmp(key, "length") == 0) {
            if (t != tok_number || parse_bounded(val, 1, 64, &v) != 0) {
                return fail("length", "must be 1..64 registers");
            }
            pc.rec.length = (uint8_t)v;
            pc.hasLength = 1;
        } else if (strcmp(key, "access") == 0) {
            if (t != tok_string || access_from_string(val, &pc.rec.flags) != 0) {
                return fail("access", "must be \"r\", \"w\" or \"rw\"");
            }
            pc.hasAccess = 1;
        } else if (strcmp(key, "writeMin") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0) {
                return fail("writeMin", "must be an int32 (scaled-int domain)");
            }
            pc.rec.writeMin = v;
            pc.hasWriteMin = 1;
        } else if (strcmp(key, "writeMax") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0) {
                return fail("writeMax", "must be an int32 (scaled-int domain)");
            }
            pc.rec.writeMax = v;
            pc.hasWriteMax = 1;
        } else {
            /* An old config carrying "publish" fails BY NAME rather than
             * silently losing a setting (§6). */
            return fail(key, "unknown point key");
        }
    }

    if (!idSeen)      return fail("id", "missing");
    if (!pc.hasAddr)  return fail("addr", "missing");
    if (!pc.hasDecode) return fail("decodeType", "missing");
    if (!pc.hasName)  return fail("name", "missing");
    if (!pc.hasScale) pc.rec.scalePow10 = 0;
    if (!pc.hasUnit)  pc.rec.unit = MB_UNIT_NONE;
    if (pc.rec.functionCode == 0u) {
        return fail("fc", "missing");
    }

    if (pc.rec.decodeType == mbDecode_ascii) {
        if (!pc.hasLength) {
            return fail("length", "required for ascii points");
        }
    } else if (pc.hasLength) {
        return fail("length", "only valid for ascii points");
    }

    /* Access rules, both enforced at compile because they are facts about the
     * silicon that a request must never be able to talk its way past (§3.2). */
    if ((pc.rec.flags & MB_PT_WRITE) != 0u) {
        if (pc.rec.functionCode != mbFc_holding) {
            return fail("access", "w/rw requires fc \"holding\"");
        }
        /* Under FC06 a write lands exactly one register, so a writable point
         * must be a 1-register type; FC16 has no such limit. */
        if (cap->writeFc == 6u &&
            MbRecords_RegWidth(pc.rec.decodeType, pc.rec.length) != 1u) {
            return fail("access", "writeFc 6 writes one register only");
        }
    } else if (pc.hasWriteMin || pc.hasWriteMax) {
        return fail("writeMin", "write bounds need write access");
    }

    if (pc.hasWriteMin != pc.hasWriteMax) {
        return fail("writeMin", "authoring one bound requires the other");
    }
    if (pc.hasWriteMin) {
        int32_t lo, hi;
        if (pc.rec.writeMin > pc.rec.writeMax) {
            return fail("writeMin", "writeMin > writeMax");
        }
        if (decode_range(pc.rec.decodeType, &lo, &hi) != 0) {
            return fail("writeMin", "ascii points take no write bounds");
        }
        if (pc.rec.writeMin < lo || pc.rec.writeMax > hi) {
            return fail("writeMin", "bound outside the decode type's range");
        }
        pc.rec.flags |= MB_PT_BOUNDED;
    } else {
        /* Absent bounds mean UNCONSTRAINED, not forbidden — the flag says so,
         * and the record keeps exactly what the author wrote (§4.6). */
        pc.rec.writeMin = 0;
        pc.rec.writeMax = 0;
    }

    /* The point must lie inside a declared block and end before that block's
     * regs: the compiler is the third thing authoring blocks buys (§3.2). */
    {
        uint8_t  width  = MbRecords_RegWidth(pc.rec.decodeType, pc.rec.length);
        uint8_t  stride = MbRecords_Stride(cap);
        int      bi     = MbBlocks_Find(cap, blocks, cap->blockCount,
                                        pc.rec.addr);
        uint16_t offRegs;

        if (width == 0u) {
            return fail("decodeType", "unknown register width");
        }
        if (bi < 0) {
            return fail("addr", "outside every declared block");
        }
        offRegs = (uint16_t)((pc.rec.addr - blocks[bi].base) / stride);
        if ((uint32_t)offRegs + width > blocks[bi].regs) {
            return fail("addr", "point ends past its block's regs");
        }
        if (width > MbRecords_MaxReadRegs(cap)) {
            return fail("length", "point is wider than maxReadRegs");
        }
    }

    *out = pc.rec;
    return 0;
}

/* ==========================================================================
 * Capability object
 * ========================================================================== */

static int parse_blocks(sModbusCapabilityRecord *cap,
                        sModbusBlockRecord *blocks)
{
    char    val[TOK_MAX];
    int32_t v;

    if (expect(tok_lBracket, "expected '[' for blocks") != 0) {
        return -1;
    }

    for (;;) {
        eTok t = next_token(val, sizeof(val));
        if (t == tok_rBracket) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_lBrace) {
            return fail("blocks", "expected block object");
        }
        if (cap->blockCount >= MB_MAX_BLOCKS_PER_CAP) {
            return fail("blocks", "too many blocks");
        }

        sModbusBlockRecord b = { 0, 0 };
        int haveBase = 0, haveRegs = 0;

        for (;;) {
            char key[TOK_MAX];
            t = next_token(key, sizeof(key));
            if (t == tok_rBrace) {
                break;
            }
            if (t == tok_comma) {
                continue;
            }
            if (t != tok_string ||
                expect(tok_colon, "expected ':'") != 0) {
                return fail("blocks", "expected block key");
            }
            if (next_token(val, sizeof(val)) != tok_number) {
                return fail("blocks", "expected number");
            }
            if (strcmp(key, "base") == 0) {
                if (parse_bounded(val, 0, UINT16_MAX, &v) != 0) {
                    return fail("base", "must be 0..65535");
                }
                b.base = (uint16_t)v;
                haveBase = 1;
            } else if (strcmp(key, "regs") == 0) {
                if (parse_bounded(val, 1, UINT16_MAX, &v) != 0) {
                    return fail("regs", "must be 1..65535");
                }
                b.regs = (uint16_t)v;
                haveRegs = 1;
            } else {
                return fail(key, "unknown block key");
            }
        }
        if (!haveBase || !haveRegs) {
            return fail("blocks", "block needs base and regs");
        }
        blocks[cap->blockCount++] = b;
    }

    return (cap->blockCount > 0) ? 0 : fail("blocks", "at least one required");
}

static int parse_capability(void)
{
    char                    key[TOK_MAX], val[TOK_MAX];
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];
    int32_t                 v;
    int                     idSeen = 0, haveBlocks = 0, havePoints = 0;

    memset(&cap, 0, sizeof(cap));
    memset(blocks, 0, sizeof(blocks));
    cap.addrStride  = 1;
    cap.writeFc     = 6;
    cap.maxReadRegs = MB_MAX_REGS_PER_READ;

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == tok_rBrace) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_string) {
            return fail("json", "expected capability key");
        }
        if (expect(tok_colon, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "blocks") == 0) {
            if (havePoints) {
                return fail("blocks", "blocks must precede points");
            }
            if (parse_blocks(&cap, blocks) != 0) {
                return -1;
            }
            haveBlocks = 1;
            continue;
        }

        if (strcmp(key, "points") == 0) {
            /* Points come last: the capability record carries blockCount and
             * is written before its blocks, and every point is checked
             * against those blocks and the dialect. */
            if (!haveBlocks) {
                return fail("points", "blocks must precede points");
            }
            s_c.capPointBase[s_c.capIdx] = s_c.counts.points;
            if (!idSeen) {
                return fail("id", "must precede points");
            }
            if (cap.name[0] == '\0') {
                return fail("name", "must precede points");
            }

            if (fw_write(&cap, sizeof(cap)) != 0 ||
                fw_write(blocks, sizeof(sModbusBlockRecord) * cap.blockCount) != 0) {
                return -1;
            }

            if (expect(tok_lBracket, "expected '[' for points") != 0) {
                return -1;
            }
            s_c.subIdx = 0;
            for (;;) {
                t = next_token(val, sizeof(val));
                if (t == tok_rBracket) {
                    break;
                }
                if (t == tok_comma) {
                    continue;
                }
                if (t != tok_lBrace) {
                    return fail("points", "expected point object");
                }
                if (s_c.counts.points >= MB_MAX_POINTS_TOTAL) {
                    return fail("points", "too many points");
                }

                sModbusPointRecord pt;
                if (parse_point(&cap, blocks, &pt) != 0) {
                    return -1;
                }
                if (fw_write(&pt, sizeof(pt)) != 0) {
                    return -1;
                }
                if ((pt.flags & MB_PT_READ) != 0u) {
                    bit_set(s_c.ptReadable, s_c.counts.points);
                }
                s_c.counts.points++;
                s_c.subIdx++;
            }

            {   /* point sentinel */
                sModbusPointRecord end;
                memset(&end, 0, sizeof(end));
                if (fw_write(&end, sizeof(end)) != 0) {
                    return -1;
                }
            }
            s_c.capPointCount[s_c.capIdx] = (uint16_t)s_c.subIdx;
            s_c.subIdx  = -1;
            havePoints  = 1;
            continue;
        }

        t = next_token(val, sizeof(val));
        if (t == tok_err) {
            return -1;
        }

        if (strcmp(key, "id") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0) {
                return fail("id", "expected number");
            }
            if (v != s_c.capIdx) {
                return fail("id", "capability ids must run 0, 1, 2 ...");
            }
            idSeen = 1;
        } else if (strcmp(key, "name") == 0) {
            if (t != tok_string || val[0] == '\0' ||
                strlen(val) >= MB_NAME_LEN || !name_chars_ok(val)) {
                return fail("name", "1..15 chars of [A-Za-z0-9_-]");
            }
            snprintf(cap.name, sizeof(cap.name), "%s", val);
        } else if (strcmp(key, "addrStride") == 0) {
            if (t != tok_number || parse_bounded(val, 1, 255, &v) != 0) {
                return fail("addrStride", "must be 1..255 address units/reg");
            }
            cap.addrStride = (uint8_t)v;
        } else if (strcmp(key, "writeFc") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0 ||
                (v != 6 && v != 16)) {
                return fail("writeFc", "must be 6 or 16");
            }
            cap.writeFc = (uint8_t)v;
        } else if (strcmp(key, "maxReadRegs") == 0) {
            if (t != tok_number ||
                parse_bounded(val, 1, MB_MAX_REGS_PER_READ, &v) != 0) {
                return fail("maxReadRegs", "must be 1..125");
            }
            cap.maxReadRegs = (uint16_t)v;
        } else {
            return fail(key, "unknown capability key");
        }
    }

    if (!idSeen)     return fail("id", "missing");
    if (!haveBlocks) return fail("blocks", "missing");
    if (!havePoints) return fail("points", "missing");
    return 0;
}

/* ==========================================================================
 * Device object — the communication data, and the ONLY per-device facts
 * ========================================================================== */

static int parse_device(void)
{
    char                key[TOK_MAX], val[TOK_MAX];
    sModbusDeviceRecord dev;
    int32_t             v;
    int                 idSeen = 0, haveSlave = 0, haveCap = 0, havePrefix = 0;

    memset(&dev, 0, sizeof(dev));

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == tok_rBrace) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_string) {
            return fail("json", "expected device key");
        }
        if (expect(tok_colon, "expected ':'") != 0) {
            return -1;
        }
        t = next_token(val, sizeof(val));
        if (t == tok_err) {
            return -1;
        }

        if (strcmp(key, "id") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0) {
                return fail("id", "expected number");
            }
            if (v != s_c.devIdx) {
                return fail("id", "device ids must run 0, 1, 2 ...");
            }
            idSeen = 1;
        } else if (strcmp(key, "slaveAddr") == 0) {
            if (t != tok_number || parse_bounded(val, 1, 247, &v) != 0) {
                return fail("slaveAddr", "must be 1..247");
            }
            dev.slaveAddr = (uint8_t)v;
            haveSlave = 1;
        } else if (strcmp(key, "capability") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0) {
                return fail("capability", "expected number");
            }
            if (v < 0 || v >= (int32_t)s_c.counts.capabilities) {
                return fail("capability", "no such capability id");
            }
            dev.capId = (uint16_t)v;
            haveCap = 1;
        } else if (strcmp(key, "baud") == 0) {
            int code;
            if (t != tok_number || parse_i32(val, &v) != 0 ||
                (code = MbRecords_CodeFromBaud((uint32_t)v)) < 0) {
                return fail("baud", "not one of the supported rates");
            }
            dev.baudCode = (uint8_t)code;
        } else if (strcmp(key, "format") == 0) {
            if (t != tok_string || format_from_string(val, &dev.format) != 0) {
                return fail("format", "must be 8N1, 8E1, 8O1 or 8N2");
            }
        } else if (strcmp(key, "port") == 0) {
            if (t != tok_string || port_from_string(val, &dev.portId) != 0) {
                return fail("port", "this firmware has no such port");
            }
        } else if (strcmp(key, "topicPrefix") == 0) {
            if (t != tok_string || val[0] == '\0' ||
                strlen(val) >= MB_TOPIC_PREFIX_LEN || !name_chars_ok(val)) {
                return fail("topicPrefix", "1..15 chars of [A-Za-z0-9_-]");
            }
            snprintf(dev.topicPrefix, sizeof(dev.topicPrefix), "%s", val);
            havePrefix = 1;
        } else {
            return fail(key, "unknown device key");
        }
    }

    if (!idSeen)     return fail("id", "missing");
    if (!haveSlave)  return fail("slaveAddr", "missing");
    if (!haveCap)    return fail("capability", "missing");
    if (!havePrefix) return fail("topicPrefix", "missing");

    s_c.devCap[s_c.devIdx] = (uint8_t)dev.capId;
    return fw_write(&dev, sizeof(dev));
}

/* ==========================================================================
 * Plan object — what we WATCH, and the only section an edit rewrites
 * ========================================================================== */

static int parse_time_tables(const sModbusPlanRecord *plan)
{
    char     val[TOK_MAX];
    int32_t  v;
    uint16_t ptCount = s_c.capPointCount[plan->capId];
    uint16_t ptBase  = s_c.capPointBase[plan->capId];
    int      ttIdx   = 0;

    if (expect(tok_lBracket, "expected '[' for timeTables") != 0) {
        return -1;
    }

    for (;;) {
        eTok t = next_token(val, sizeof(val));
        if (t == tok_rBracket) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_lBrace) {
            return fail("timeTables", "expected time table object");
        }
        if (ttIdx >= MB_MAX_TIME_TABLES_PER_PLAN) {
            return fail("timeTables", "too many time tables");
        }

        s_c.subIdx = ttIdx;

        sModbusTimeTableRecord tt;
        uint16_t ids[MB_MAX_TT_ENTRIES_PER_TABLE];
        int      idSeen = 0, havePeriod = 0, havePoints = 0;

        memset(&tt, 0, sizeof(tt));

        for (;;) {
            char key[TOK_MAX];
            t = next_token(key, sizeof(key));
            if (t == tok_rBrace) {
                break;
            }
            if (t == tok_comma) {
                continue;
            }
            if (t != tok_string) {
                return fail("timeTables", "expected key");
            }
            if (expect(tok_colon, "expected ':'") != 0) {
                return -1;
            }

            if (strcmp(key, "points") == 0) {
                if (expect(tok_lBracket, "expected '[' for points") != 0) {
                    return -1;
                }
                for (;;) {
                    t = next_token(val, sizeof(val));
                    if (t == tok_rBracket) {
                        break;
                    }
                    if (t == tok_comma) {
                        continue;
                    }
                    if (t != tok_number || parse_i32(val, &v) != 0) {
                        return fail("points", "expected point id");
                    }
                    if (v < 0 || v >= (int32_t)ptCount) {
                        return fail("points", "no such point id in capability");
                    }
                    if (!bit_get(s_c.ptReadable, (uint16_t)(ptBase + v))) {
                        /* A w point cannot be read, so watching it is
                         * incoherent (§3.2). */
                        return fail("points", "write-only point in a time table");
                    }
                    if (bit_get(s_c.ptSeen, (uint16_t)(ptBase + v))) {
                        return fail("points", "point listed twice in this plan");
                    }
                    bit_set(s_c.ptSeen, (uint16_t)(ptBase + v));

                    if (tt.entryCount >= (sizeof(ids) / sizeof(ids[0]))) {
                        return fail("points", "too many points in one table");
                    }
                    if (s_c.ttEntriesTotal >= MB_MAX_TT_ENTRIES_TOTAL) {
                        return fail("points", "too many time-table entries");
                    }
                    ids[tt.entryCount++] = (uint16_t)v;
                    s_c.ttEntriesTotal++;
                }
                havePoints = 1;
                continue;
            }

            t = next_token(val, sizeof(val));
            if (t == tok_err) {
                return -1;
            }
            if (strcmp(key, "id") == 0) {
                if (t != tok_number || parse_i32(val, &v) != 0) {
                    return fail("id", "expected number");
                }
                if (v != ttIdx) {
                    return fail("id", "time table ids must run 0, 1, 2 ...");
                }
                idSeen = 1;
            } else if (strcmp(key, "everySec") == 0) {
                if (t != tok_number || parse_i32(val, &v) != 0 || v < 1) {
                    return fail("everySec", "must be >= 1");
                }
                tt.period_sec = (uint32_t)v;
                havePeriod = 1;
            } else {
                return fail(key, "unknown time table key");
            }
        }

        if (!idSeen)     return fail("id", "missing");
        if (!havePeriod) return fail("everySec", "missing");
        if (!havePoints || tt.entryCount == 0u) {
            return fail("points", "a time table needs at least one point");
        }

        if (fw_write(&tt, sizeof(tt)) != 0 ||
            fw_write(ids, sizeof(uint16_t) * tt.entryCount) != 0) {
            return -1;
        }
        ttIdx++;
    }

    s_c.subIdx = -1;

    {   /* time-table sentinel */
        sModbusTimeTableRecord end;
        memset(&end, 0, sizeof(end));
        return fw_write(&end, sizeof(end));
    }
}

static int parse_plan(void)
{
    char              key[TOK_MAX], val[TOK_MAX];
    sModbusPlanRecord plan;
    int32_t           v;
    int               idSeen = 0, haveCap = 0, haveTables = 0;

    memset(&plan, 0, sizeof(plan));
    memset(s_c.ptSeen, 0, sizeof(s_c.ptSeen));

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == tok_rBrace) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_string) {
            return fail("json", "expected plan key");
        }
        if (expect(tok_colon, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "devices") == 0) {
            if (!haveCap) {
                return fail("devices", "capability must precede devices");
            }
            if (expect(tok_lBracket, "expected '[' for devices") != 0) {
                return -1;
            }
            for (;;) {
                t = next_token(val, sizeof(val));
                if (t == tok_rBracket) {
                    break;
                }
                if (t == tok_comma) {
                    continue;
                }
                if (t != tok_number || parse_i32(val, &v) != 0) {
                    return fail("devices", "expected device id");
                }
                if (v < 0 || v >= (int32_t)s_c.counts.devices) {
                    return fail("devices", "no such device id");
                }
                /* A plan whose device set reaches outside its own capability
                 * is a compile error, so a mismatch is impossible (§3.1). */
                if (s_c.devCap[v] != plan.capId) {
                    return fail("devices", "device implements another capability");
                }
                plan.devices |= (uint8_t)(1u << v);
            }
            continue;
        }

        if (strcmp(key, "timeTables") == 0) {
            if (!idSeen || !haveCap || plan.name[0] == '\0') {
                return fail("timeTables", "id, name and capability must precede it");
            }
            if (fw_write(&plan, sizeof(plan)) != 0 ||
                parse_time_tables(&plan) != 0) {
                return -1;
            }
            haveTables = 1;
            continue;
        }

        t = next_token(val, sizeof(val));
        if (t == tok_err) {
            return -1;
        }

        if (strcmp(key, "id") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0) {
                return fail("id", "expected number");
            }
            /* A plan id is a SLOT, not a position: unique, 0..7, gaps allowed
             * — but authored ascending, which is how the stream stays in slot
             * order with no blank records (§3.5, §7.2). */
            if (v < 0 || v >= MB_MAX_PLANS) {
                return fail("id", "plan id must be 0..7");
            }
            if (s_c.planSlotSeen & (uint8_t)(1u << v)) {
                return fail("id", "duplicate plan id");
            }
            if (v <= s_c.lastPlanId) {
                return fail("id", "plan ids must be authored ascending");
            }
            s_c.planSlotSeen |= (uint8_t)(1u << v);
            s_c.lastPlanId    = (int)v;
            plan.planId       = (uint8_t)v;
            idSeen = 1;
        } else if (strcmp(key, "name") == 0) {
            if (t != tok_string || val[0] == '\0' ||
                strlen(val) >= MB_NAME_LEN || !name_chars_ok(val)) {
                return fail("name", "1..15 chars of [A-Za-z0-9_-]");
            }
            snprintf(plan.name, sizeof(plan.name), "%s", val);
        } else if (strcmp(key, "capability") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0) {
                return fail("capability", "expected number");
            }
            if (v < 0 || v >= (int32_t)s_c.counts.capabilities) {
                return fail("capability", "no such capability id");
            }
            plan.capId = (uint16_t)v;
            haveCap = 1;
        } else {
            return fail(key, "unknown plan key");
        }
    }

    if (!idSeen)  return fail("id", "missing");
    if (!haveCap) return fail("capability", "missing");

    if (!haveTables) {
        /* "timeTables may be empty" — declared but watching nothing (§6). */
        sModbusTimeTableRecord end;
        memset(&end, 0, sizeof(end));
        if (fw_write(&plan, sizeof(plan)) != 0 ||
            fw_write(&end, sizeof(end)) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ==========================================================================
 * Top level — three arrays, in dependency order, and that order is FORCED
 * ========================================================================== */

static int parse_array_of(const char *what, int (*parseOne)(void), int *idx,
                          uint8_t *count, uint8_t max)
{
    char val[TOK_MAX];

    if (expect(tok_lBracket, "expected '[' for section") != 0) {
        return -1;
    }
    for (;;) {
        eTok t = next_token(val, sizeof(val));
        if (t == tok_rBracket) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_lBrace) {
            return fail(what, "expected object");
        }
        if (*count >= max) {
            return fail(what, "too many");
        }
        *idx = (int)*count;
        if (parseOne() != 0) {
            return -1;
        }
        (*count)++;
    }
    *idx = -1;
    return 0;
}

static int parse_config(void)
{
    char key[TOK_MAX];
    int  seenCaps = 0, seenDevs = 0, seenPlans = 0;

    if (expect(tok_lBrace, "expected '{'") != 0) {
        return -1;
    }

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == tok_rBrace) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_string) {
            return fail("json", "expected section key");
        }
        if (expect(tok_colon, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "capabilities") == 0) {
            if (seenDevs || seenPlans) {
                return fail("capabilities", "must come first");
            }
            /* capPointBase is a running total, so a plan's point-id range
             * check indexes the right slice of the readable bitmap. */
            if (parse_array_of("capabilities", parse_capability, &s_c.capIdx,
                               &s_c.counts.capabilities,
                               MB_MAX_CAPABILITIES) != 0) {
                return -1;
            }
            {   /* capability sentinel */
                sModbusCapabilityRecord end;
                memset(&end, 0, sizeof(end));
                if (fw_write(&end, sizeof(end)) != 0) {
                    return -1;
                }
            }
            seenCaps = 1;
        } else if (strcmp(key, "devices") == 0) {
            if (!seenCaps) {
                return fail("devices", "capabilities must come first");
            }
            if (seenPlans) {
                return fail("devices", "devices must precede plans");
            }
            if (parse_array_of("devices", parse_device, &s_c.devIdx,
                               &s_c.counts.devices, MB_MAX_DEVICES) != 0) {
                return -1;
            }
            {   /* device sentinel */
                sModbusDeviceRecord end;
                memset(&end, 0, sizeof(end));
                if (fw_write(&end, sizeof(end)) != 0) {
                    return -1;
                }
            }
            seenDevs = 1;
        } else if (strcmp(key, "plans") == 0) {
            if (!seenCaps || !seenDevs) {
                return fail("plans", "capabilities and devices must precede plans");
            }
            if (parse_array_of("plans", parse_plan, &s_c.planIdx,
                               &s_c.counts.plans, MB_MAX_PLANS) != 0) {
                return -1;
            }
            {   /* plan sentinel — the end of the stream */
                sModbusPlanRecord end;
                memset(&end, 0, sizeof(end));
                if (fw_write(&end, sizeof(end)) != 0) {
                    return -1;
                }
            }
            seenPlans = 1;
        } else {
            return fail(key, "unknown top-level key");
        }
    }

    if (!seenCaps) {
        return fail("capabilities", "missing");
    }

    /* Sentinels for the sections that were never opened, so the stream is
     * always a complete, walkable v2 stream. */
    if (!seenDevs) {
        sModbusDeviceRecord end;
        memset(&end, 0, sizeof(end));
        if (fw_write(&end, sizeof(end)) != 0) {
            return -1;
        }
    }
    if (!seenPlans) {
        sModbusPlanRecord end;
        memset(&end, 0, sizeof(end));
        if (fw_write(&end, sizeof(end)) != 0) {
            return -1;
        }
    }

    if (next_token(key, sizeof(key)) != tok_eof) {
        return fail("json", "trailing data after config");
    }
    return 0;
}

/* One entry point, two modes.  VERIFY IS THIS SAME PASS with the record writes
 * going to a counting sink instead of flash (docs/modbus.md §4.9) — there is
 * never a second validator to keep in sync, which is the whole reason verify
 * is cheap enough to have. */
static int compile_or_verify(fModbusByteSource src, void *srcCtx,
                             uint32_t regionBase, void (*kick)(void),
                             sModbusCompileResult *res, int dryRun)
{
    if (!src || !res ||
        (!dryRun && regionBase != EXT_FLASH_MODBUS_LUT_A_ADDR &&
         regionBase != EXT_FLASH_MODBUS_LUT_B_ADDR)) {
        return -1;
    }

    memset(res, 0, sizeof(*res));
    res->capIdx  = -1;
    res->devIdx  = -1;
    res->planIdx = -1;
    res->subIdx  = -1;

    memset(&s_c, 0, sizeof(s_c));
    s_c.src        = src;
    s_c.srcCtx     = srcCtx;
    s_c.base       = regionBase;
    s_c.kick       = kick;
    s_c.crc        = ImgMgmt_Crc32Init();
    s_c.res        = res;
    s_c.capIdx     = -1;
    s_c.devIdx     = -1;
    s_c.planIdx    = -1;
    s_c.subIdx     = -1;
    s_c.lastPlanId = -1;
    s_c.dryRun     = dryRun;

    /* Kill any previous config in this region up front: a compile that
     * fails halfway must not leave a stale-but-valid header behind.  A verify
     * touches no flash at all, which is exactly what it is for: an upload
     * consumes the inactive region on success AND on failure, and that region
     * holds the previous config. */
    if (!dryRun) {
        if (kick) {
            kick();
        }
        if (W25Q128_EraseSector(regionBase) != w25q_ok) {
            fail("flash", "sector erase failed");
            return -1;
        }
        s_c.nextEraseOff = W25Q128_SECTOR_SIZE;
    }

    if (parse_config() != 0 || fw_finish() != 0) {
        return -1;
    }

    res->ok     = 1;
    res->counts = s_c.counts;
    return 0;
}

int MbCfgCompile(fModbusByteSource src, void *srcCtx, uint32_t regionBase,
                 void (*kick)(void), sModbusCompileResult *res)
{
    return compile_or_verify(src, srcCtx, regionBase, kick, res, 0);
}

/* ==========================================================================
 * One plan object, standalone (docs/modbus.md §8.1)
 *
 * A plan body is one element of the config's plans[] array, so the same JSON
 * an operator would paste into a config is what the plan endpoints take.
 *
 * This shares the SCHEMA with parse_plan above — the key names and their
 * shapes — but not the semantics: an uploaded plan is range-checked against
 * counts the compiler is accumulating, while a standalone one is checked
 * against the counts already in flash by MbCfgPlans_Validate.  Same rule list,
 * two sources for the numbers, which is what §7.4 asks for.
 * ========================================================================== */

MB_COMPILER_BSS static uint16_t s_planIds[MB_MAX_TT_ENTRIES_TOTAL / 2];
MB_COMPILER_BSS static sModbusTimeTableSpec s_planTables[MB_MAX_TIME_TABLES_PER_PLAN];
MB_COMPILER_BSS static char s_planName[MB_NAME_LEN];

int MbCfgParsePlan(fModbusByteSource src, void *srcCtx,
                   sModbusPlanSpec *out, int *outId,
                   sModbusCompileResult *res)
{
    char    key[TOK_MAX], val[TOK_MAX];
    int32_t v;
    int     idSeen = 0, haveCap = 0;
    uint8_t tableCount = 0;
    uint16_t at = 0;

    if (!src || !out || !outId || !res) {
        return -1;
    }

    memset(res, 0, sizeof(*res));
    res->capIdx = -1;
    res->devIdx = -1;
    res->planIdx = -1;
    res->subIdx = -1;

    memset(&s_c, 0, sizeof(s_c));
    s_c.src    = src;
    s_c.srcCtx = srcCtx;
    s_c.res    = res;
    s_c.dryRun = 1;
    s_c.capIdx = -1;
    s_c.devIdx = -1;
    s_c.planIdx = -1;
    s_c.subIdx = -1;

    memset(out, 0, sizeof(*out));
    memset(s_planName, 0, sizeof(s_planName));
    *outId = -1;

    if (expect(tok_lBrace, "expected '{'") != 0) {
        return -1;
    }

    for (;;) {
        eTok t = next_token(key, sizeof(key));
        if (t == tok_rBrace) {
            break;
        }
        if (t == tok_comma) {
            continue;
        }
        if (t != tok_string) {
            return fail("json", "expected plan key");
        }
        if (expect(tok_colon, "expected ':'") != 0) {
            return -1;
        }

        if (strcmp(key, "devices") == 0) {
            if (expect(tok_lBracket, "expected '[' for devices") != 0) {
                return -1;
            }
            for (;;) {
                t = next_token(val, sizeof(val));
                if (t == tok_rBracket) {
                    break;
                }
                if (t == tok_comma) {
                    continue;
                }
                if (t != tok_number || parse_i32(val, &v) != 0 ||
                    v < 0 || v >= MB_MAX_DEVICES) {
                    return fail("devices", "expected device id 0..7");
                }
                out->devices |= (uint8_t)(1u << v);
            }
            continue;
        }

        if (strcmp(key, "timeTables") == 0) {
            if (expect(tok_lBracket, "expected '[' for timeTables") != 0) {
                return -1;
            }
            for (;;) {
                t = next_token(val, sizeof(val));
                if (t == tok_rBracket) {
                    break;
                }
                if (t == tok_comma) {
                    continue;
                }
                if (t != tok_lBrace) {
                    return fail("timeTables", "expected time table object");
                }
                if (tableCount >= MB_MAX_TIME_TABLES_PER_PLAN) {
                    return fail("timeTables", "too many time tables");
                }

                uint32_t period = 0;
                uint16_t first  = at;

                for (;;) {
                    char k2[TOK_MAX];
                    t = next_token(k2, sizeof(k2));
                    if (t == tok_rBrace) {
                        break;
                    }
                    if (t == tok_comma) {
                        continue;
                    }
                    if (t != tok_string ||
                        expect(tok_colon, "expected ':'") != 0) {
                        return fail("timeTables", "expected key");
                    }
                    if (strcmp(k2, "points") == 0) {
                        if (expect(tok_lBracket, "expected '['") != 0) {
                            return -1;
                        }
                        for (;;) {
                            t = next_token(val, sizeof(val));
                            if (t == tok_rBracket) {
                                break;
                            }
                            if (t == tok_comma) {
                                continue;
                            }
                            if (t != tok_number || parse_i32(val, &v) != 0 ||
                                v < 0 || v > UINT16_MAX) {
                                return fail("points", "expected point id");
                            }
                            if (at >= (sizeof(s_planIds) / sizeof(s_planIds[0]))) {
                                return fail("points", "too many points");
                            }
                            s_planIds[at++] = (uint16_t)v;
                        }
                        continue;
                    }
                    t = next_token(val, sizeof(val));
                    if (t == tok_err) {
                        return -1;
                    }
                    if (strcmp(k2, "everySec") == 0) {
                        if (t != tok_number || parse_i32(val, &v) != 0 ||
                            v < 1) {
                            return fail("everySec", "must be >= 1");
                        }
                        period = (uint32_t)v;
                    } else if (strcmp(k2, "id") != 0) {
                        return fail(k2, "unknown time table key");
                    }
                }

                s_planTables[tableCount].period_sec = period;
                s_planTables[tableCount].points     = &s_planIds[first];
                s_planTables[tableCount].count      = (uint16_t)(at - first);
                tableCount++;
            }
            continue;
        }

        t = next_token(val, sizeof(val));
        if (t == tok_err) {
            return -1;
        }

        if (strcmp(key, "id") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0 ||
                v < 0 || v >= MB_MAX_PLANS) {
                return fail("id", "plan id must be 0..7");
            }
            *outId = (int)v;
            idSeen = 1;
        } else if (strcmp(key, "name") == 0) {
            if (t != tok_string || val[0] == '\0' ||
                strlen(val) >= MB_NAME_LEN || !name_chars_ok(val)) {
                return fail("name", "1..15 chars of [A-Za-z0-9_-]");
            }
            snprintf(s_planName, sizeof(s_planName), "%s", val);
        } else if (strcmp(key, "capability") == 0) {
            if (t != tok_number || parse_i32(val, &v) != 0 || v < 0) {
                return fail("capability", "expected number");
            }
            out->capId = (uint16_t)v;
            haveCap = 1;
        } else {
            return fail(key, "unknown plan key");
        }
    }

    (void)idSeen;                    /* id is optional: PUT takes it in the URL */
    if (!haveCap) {
        return fail("capability", "missing");
    }
    if (s_planName[0] == '\0') {
        return fail("name", "missing");
    }

    out->name       = s_planName;
    out->tables     = s_planTables;
    out->tableCount = tableCount;

    if (next_token(key, sizeof(key)) != tok_eof) {
        return fail("json", "trailing data after plan");
    }

    res->ok = 1;
    return 0;
}

int MbCfgVerify(fModbusByteSource src, void *srcCtx,
                sModbusCompileResult *res)
{
    return compile_or_verify(src, srcCtx, 0u, NULL, res, 1);
}

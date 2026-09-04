/*
 * json.h
 *
 * THE JSON module: one clamped writer and one streaming reader, shared by
 * every part of the application that persists a configuration or answers a
 * query (docs/task_json_module.md).
 *
 * It is deliberately small.  This is not a general-purpose library: the test
 * of every function here is that at least two existing call sites need it.
 *
 * THE WRITER EXISTS BECAUSE OF A DEFECT CLASS, not for tidiness.  The idiom
 * it replaces was
 *
 *     n += snprintf(&buf[n], CAP - n, ...);
 *
 * and snprintf() returns the length it WOULD have written.  `n` is therefore
 * an unclamped high-water estimate rather than a position: once it passes
 * CAP, `CAP - n` underflows in an unsigned type to ~4.29 billion and is
 * handed to the next snprintf() as its size limit, at an address already past
 * the end of the buffer.  Json_Cat() saturates instead, so the arithmetic
 * that produced the overrun cannot be written.
 *
 * `pos == cap` is the caller's "it did not fit" signal, which is what lets a
 * loop emit whole objects or none — see the roll-back idiom in
 * App/Http/http_server.c.  A buffer that saturated exactly on the last byte
 * reports the same thing; costing one dropped element is the cheap side of
 * that trade.
 *
 * APPLICATION-ONLY Shared/ code.  It must never enter SHARED_SOURCES: the
 * 32 KB bootloader has no JSON in it and never will.
 */

#ifndef SHARED_JSON_JSON_H_
#define SHARED_JSON_JSON_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes -----------------------------------------------------------------*/

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* Exported defines ---------------------------------------------------------*/

/** Refill window over the byte source.  The reader never needs more than one
 *  byte of lookahead, so this only trades call count against RAM. */
#define JSON_WINDOW_LEN     64u

/* Exported types -----------------------------------------------------------*/

/** Pull bytes into `buf`; return the count, 0 at end of input, <0 on error.
 *  The same shape as fModbusByteSource / fPackByteSource, deliberately. */
typedef int (*fJsonByteSource)(void *ctx, uint8_t *buf, uint32_t maxLen);

typedef enum {
    jsonTok_lBrace,
    jsonTok_rBrace,
    jsonTok_lBracket,
    jsonTok_rBracket,
    jsonTok_colon,
    jsonTok_comma,
    jsonTok_string,
    jsonTok_number,
    jsonTok_true,
    jsonTok_false,
    jsonTok_null,
    jsonTok_eof,
    jsonTok_err,
    jsonTok_last                    /* sentinel - intentionally lowercase   */
} eJsonTok;

/** Reader state.  Small enough for a 4 KB task stack; the Modbus compiler
 *  keeps its copy in CCM only because the rest of its state is there. */
typedef struct {
    fJsonByteSource src;
    void           *srcCtx;
    const char     *reason;         /* why the last token was jsonTok_err   */
    uint32_t        offset_bytes;   /* bytes consumed, for error reporting  */
    uint32_t        winLen;
    uint32_t        winPos;
    uint8_t         eof;
    uint8_t         ioErr;
    uint8_t         window[JSON_WINDOW_LEN];
} sJsonReader;

/** Byte source over a buffer already in memory, for a caller that has the
 *  whole document (Shared/NvDb/nvdb_config.c). */
typedef struct {
    const uint8_t  *data;
    uint32_t        len_bytes;
    uint32_t        pos;
} sJsonMemSource;

/* Exported functions -------------------------------------------------------*/

/* --- writer ------------------------------------------------------------- */

/**
 * @brief Append a formatted fragment, clamping instead of overrunning.
 * @param  buf - destination
 * @param  cap - its capacity in bytes, NUL included
 * @param  pos - the current position, as returned by the previous call
 * @param  fmt - printf format
 * @retval the new position, saturating at `cap`
 */
size_t Json_Cat(char *buf, size_t cap, size_t pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/**
 * @brief Append `str` escaped as the BODY of a JSON string - the caller
 *        writes the surrounding quotes.
 * @param  buf - destination
 * @param  cap - its capacity in bytes, NUL included
 * @param  pos - the current position
 * @param  str - the raw text; NULL is treated as empty
 * @retval the new position, saturating at `cap`
 *
 * A truncation never splits an escape sequence: the output is always a
 * prefix of a well-formed string body.
 */
size_t Json_CatEscaped(char *buf, size_t cap, size_t pos, const char *str);

/**
 * @brief Escape `src` into a standalone NUL-terminated buffer.
 * @param  dst - destination
 * @param  dstCap - its capacity in bytes, NUL included
 * @param  src - the raw text; NULL is treated as empty
 * @retval the length written, NUL excluded
 *
 * For the many call sites whose value is one field of a long format string:
 * escape once into a small local, keep the "%s".
 */
size_t Json_Escape(char *dst, size_t dstCap, const char *src);

/* --- reader: byte layer ------------------------------------------------- */

void Json_ReaderInit(sJsonReader *r, fJsonByteSource src, void *srcCtx);

/** @retval the next byte without consuming it, or -1 at EOF / on error. */
int Json_Peek(sJsonReader *r);

/** @retval the next byte, consuming it, or -1 at EOF / on error. */
int Json_Get(sJsonReader *r);

/** @brief Consume whitespace. @retval the next byte (not consumed), or -1. */
int Json_SkipWs(sJsonReader *r);

/* --- reader: token layer ------------------------------------------------ */

/**
 * @brief Read the next token; string and number text lands NUL-terminated
 *        in `text`, with escapes already decoded.
 * @param  r - reader
 * @param  text - receives the token text (may be NULL only if the caller
 *                knows the token is structural, which it cannot - so don't)
 * @param  textSize - capacity of `text`
 * @retval the token kind; jsonTok_err leaves `r->reason` set
 */
eJsonTok Json_Next(sJsonReader *r, char *text, uint32_t textSize);

/**
 * @brief Read the next token and require it to be `want`.
 * @retval 0 on match, -1 otherwise (`r->reason` set)
 */
int Json_Expect(sJsonReader *r, eJsonTok want);

/** @brief Read a string value into a bounded buffer. @retval 1 ok, 0 not. */
int Json_ReadString(sJsonReader *r, char *out, uint32_t cap);

/** @brief Read an integer value. @retval 1 ok, 0 not. */
int Json_ReadI32(sJsonReader *r, int32_t *out);

/** @brief Read a true/false value into 1/0. @retval 1 ok, 0 not. */
int Json_ReadBool(sJsonReader *r, int *out);

/* --- reader: text conversion -------------------------------------------- */

/** @brief Decimal text (optionally signed) to int32_t, refusing anything
 *         that would wrap and anything that is not all digits.
 *  @retval 0 on success, -1 otherwise */
int Json_ToI32(const char *text, int32_t *out);

/** @brief Decimal text to uint32_t, same refusals plus a leading '-'.
 *  @retval 0 on success, -1 otherwise */
int Json_ToU32(const char *text, uint32_t *out);

/* --- reader: in-memory source ------------------------------------------- */

void Json_MemSourceInit(sJsonMemSource *m, const void *data,
                        uint32_t len_bytes);

/** @brief fJsonByteSource over an sJsonMemSource. */
int Json_MemRead(void *ctx, uint8_t *buf, uint32_t maxLen);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_JSON_JSON_H_ */

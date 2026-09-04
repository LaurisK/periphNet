/*
 * json_write.c
 *
 * The clamped writer.  Lifted from json_cat() in App/Http/http_server.c,
 * which was written to fix exactly one instance of the overrun described in
 * json.h and then never reached the other 30.
 */

/* Includes -----------------------------------------------------------------*/

#include "json.h"

#include <stdio.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

/** Longest escape this writer can emit: \u00XX. */
#define ESC_MAX_LEN         6u

/* Private function prototypes ----------------------------------------------*/

static uint32_t EscapeOne(char ch, char *out);

/* Private functions --------------------------------------------------------*/

/**
 * @brief Render one raw byte as it must appear inside a JSON string.
 * @param  ch - the raw byte
 * @param  out - receives up to ESC_MAX_LEN bytes, NOT NUL-terminated
 * @retval how many bytes were written
 *
 * Bytes at or above 0x80 pass through: the input is assumed to be UTF-8 and
 * re-encoding it as \u escapes would mean decoding it first, which is more
 * than any caller here needs.
 */
static uint32_t EscapeOne(char ch, char *out)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t     b = (uint8_t)ch;

    switch (ch) {
    case '"':  out[0] = '\\'; out[1] = '"';  return 2u;
    case '\\': out[0] = '\\'; out[1] = '\\'; return 2u;
    case '\b': out[0] = '\\'; out[1] = 'b';  return 2u;
    case '\f': out[0] = '\\'; out[1] = 'f';  return 2u;
    case '\n': out[0] = '\\'; out[1] = 'n';  return 2u;
    case '\r': out[0] = '\\'; out[1] = 'r';  return 2u;
    case '\t': out[0] = '\\'; out[1] = 't';  return 2u;
    default:   break;
    }

    if (b < 0x20u) {
        out[0] = '\\';
        out[1] = 'u';
        out[2] = '0';
        out[3] = '0';
        out[4] = hex[(b >> 4) & 0x0Fu];
        out[5] = hex[b & 0x0Fu];
        return ESC_MAX_LEN;
    }

    out[0] = ch;
    return 1u;
}

/* Exported functions -------------------------------------------------------*/

size_t Json_Cat(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    va_list ap;
    int     n;

    if ((buf == NULL) || (pos >= cap)) {
        return cap;                 /* full: swallow, never wrap            */
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return pos;
    }
    pos += (size_t)n;

    return (pos > cap) ? cap : pos;  /* truncated is fine; overrunning is not */
}

size_t Json_CatEscaped(char *buf, size_t cap, size_t pos, const char *str)
{
    char esc[ESC_MAX_LEN];

    if ((buf == NULL) || (pos >= cap)) {
        return cap;
    }
    if (str == NULL) {
        buf[pos] = '\0';
        return pos;
    }

    while (*str != '\0') {
        const uint32_t len = EscapeOne(*str, esc);
        uint32_t       i;

        /* +1 for the NUL this leaves behind, so the result is always a
         * usable C string and an escape is never cut in half. */
        if ((pos + len + 1u) > cap) {
            buf[pos] = '\0';
            return cap;
        }
        for (i = 0u; i < len; i++) {
            buf[pos++] = esc[i];
        }
        str++;
    }
    buf[pos] = '\0';

    return pos;
}

size_t Json_Escape(char *dst, size_t dstCap, const char *src)
{
    if ((dst == NULL) || (dstCap == 0u)) {
        return 0u;
    }
    dst[0] = '\0';
    (void)Json_CatEscaped(dst, dstCap, 0u, src);

    /* Json_CatEscaped reports `cap` when it saturated, which is a position it
     * never wrote to -- the NUL is below it.  Ask the string itself. */
    return strlen(dst);
}

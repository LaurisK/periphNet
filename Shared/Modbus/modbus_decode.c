#include "modbus_decode.h"

#include <string.h>
#include <stdio.h>

/* 10^0..10^9 for integer scaling (int64 headroom used in conversions) */
static const int64_t s_pow10[] = {
    1, 10, 100, 1000, 10000, 100000,
    1000000, 10000000, 100000000, 1000000000,
};

static int32_t clamp_i64(int64_t v)
{
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

int32_t MbDecode_Scaled(const sModbusPointRecord *p, const uint16_t *regs)
{
    uint32_t u;

    switch (p->decodeType) {
    case mbDecode_u16:
    case mbDecode_bitfield:
        return (int32_t)regs[0];
    case mbDecode_s16:
        return (int32_t)(int16_t)regs[0];
    case mbDecode_u32Be:
        u = ((uint32_t)regs[0] << 16) | regs[1];
        return clamp_i64((int64_t)u);
    case mbDecode_u32Le:
        u = ((uint32_t)regs[1] << 16) | regs[0];
        return clamp_i64((int64_t)u);
    case mbDecode_s32Be:
        u = ((uint32_t)regs[0] << 16) | regs[1];
        return (int32_t)u;
    case mbDecode_s32Le:
        u = ((uint32_t)regs[1] << 16) | regs[0];
        return (int32_t)u;
    case mbDecode_float32Be:
    case mbDecode_float32Le: {
        /* The single permitted float step (design §8): decode the IEEE754
         * wire value, quantize into the scaled-int domain, never let float
         * propagate further. */
        u = (p->decodeType == mbDecode_float32Be)
                ? (((uint32_t)regs[0] << 16) | regs[1])
                : (((uint32_t)regs[1] << 16) | regs[0]);
        float f;
        memcpy(&f, &u, sizeof(f));

        int   pow = p->scalePow10;
        float scaled = f;
        if (pow > 0) {
            scaled = f / (float)s_pow10[pow];
        } else if (pow < 0) {
            scaled = f * (float)s_pow10[-pow];
        }
        if (scaled >= 2147483647.0f) {
            return INT32_MAX;
        }
        if (scaled <= -2147483648.0f) {
            return INT32_MIN;
        }
        return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    }
    default:
        return 0;
    }
}

int MbDecode_Ascii(const sModbusPointRecord *p, const uint16_t *regs,
                   char *out, size_t outSize)
{
    size_t chars = (size_t)p->length * 2u;

    if (outSize < chars + 1u) {
        return -1;
    }

    for (uint8_t i = 0; i < p->length; i++) {
        out[2u * i]      = (char)(regs[i] >> 8);
        out[2u * i + 1u] = (char)(regs[i] & 0xFFu);
    }
    out[chars] = '\0';

    /* Trim trailing NULs/spaces vendors pad with */
    size_t n = strlen(out);
    while (n > 0u && out[n - 1u] == ' ') {
        out[--n] = '\0';
    }
    return (int)n;
}

int MbFormat_Scaled(char *buf, size_t size, int32_t scaled, int8_t pow10)
{
    if (pow10 >= 0) {
        /* Append pow10 zeros; int64 print without %lld: value fits since
         * |scaled| <= 2^31 and pow10 <= 6 -> print digits manually */
        int n = snprintf(buf, size, "%ld", (long)scaled);
        if (n < 0 || (size_t)n >= size) {
            return n;
        }
        for (int i = 0; i < pow10; i++) {
            if ((size_t)n + 1u >= size) {
                return n;
            }
            buf[n++] = '0';
        }
        buf[n] = '\0';
        return n;
    }

    int64_t div  = s_pow10[-pow10];
    int64_t a    = (scaled < 0) ? -(int64_t)scaled : (int64_t)scaled;
    long    ip   = (long)(a / div);
    long    fp   = (long)(a % div);

    return snprintf(buf, size, "%s%ld.%0*ld",
                    (scaled < 0) ? "-" : "", ip, -pow10, fp);
}

int MbParse_Scaled(const char *text, int8_t pow10, int32_t *scaled)
{
    const char *p = text;
    int         neg = 0;
    int64_t     ip = 0;
    int64_t     fp = 0;
    int         fracDigits = 0;
    int         anyDigit = 0;

    if (!text || !scaled) {
        return -1;
    }

    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    for (; *p >= '0' && *p <= '9'; p++) {
        ip = ip * 10 + (*p - '0');
        if (ip > INT32_MAX) {
            return -1;
        }
        anyDigit = 1;
    }
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++) {
            /* Extra digits beyond the scale's resolution must be zero */
            if (fracDigits >= ((pow10 < 0) ? -pow10 : 0)) {
                if (*p != '0') {
                    return -1;
                }
                continue;
            }
            fp = fp * 10 + (*p - '0');
            fracDigits++;
            anyDigit = 1;
        }
    }
    if (*p != '\0' || !anyDigit) {
        return -1;
    }

    int64_t v;
    if (pow10 >= 0) {
        /* Value must be a multiple of 10^pow10 */
        if (fp != 0) {
            return -1;
        }
        if (ip % s_pow10[pow10] != 0) {
            return -1;
        }
        v = ip / s_pow10[pow10];
    } else {
        int64_t div = s_pow10[-pow10];
        /* Right-pad missing fraction digits with zeros */
        for (int i = fracDigits; i < -pow10; i++) {
            fp *= 10;
        }
        v = ip * div + fp;
    }

    if (neg) {
        v = -v;
    }
    if (v > INT32_MAX || v < INT32_MIN) {
        return -1;
    }
    *scaled = (int32_t)v;
    return 0;
}

int MbEncode_Scaled(const sModbusPointRecord *p, int32_t scaled,
                    uint16_t *regs)
{
    uint32_t u;

    if (p == NULL || regs == NULL) {
        return -1;
    }

    switch (p->decodeType) {
    case mbDecode_u16:
    case mbDecode_bitfield:
        if (scaled < 0 || scaled > (int32_t)UINT16_MAX) {
            return -1;
        }
        regs[0] = (uint16_t)scaled;
        return 1;

    case mbDecode_s16:
        if (scaled < INT16_MIN || scaled > INT16_MAX) {
            return -1;
        }
        regs[0] = (uint16_t)(int16_t)scaled;
        return 1;

    case mbDecode_u32Be:
    case mbDecode_u32Le:
        if (scaled < 0) {
            return -1;
        }
        u = (uint32_t)scaled;
        if (p->decodeType == mbDecode_u32Be) {
            regs[0] = (uint16_t)(u >> 16);
            regs[1] = (uint16_t)(u & 0xFFFFu);
        } else {
            regs[0] = (uint16_t)(u & 0xFFFFu);
            regs[1] = (uint16_t)(u >> 16);
        }
        return 2;

    case mbDecode_s32Be:
    case mbDecode_s32Le:
        u = (uint32_t)scaled;
        if (p->decodeType == mbDecode_s32Be) {
            regs[0] = (uint16_t)(u >> 16);
            regs[1] = (uint16_t)(u & 0xFFFFu);
        } else {
            regs[0] = (uint16_t)(u & 0xFFFFu);
            regs[1] = (uint16_t)(u >> 16);
        }
        return 2;

    default:
        /* ascii has no numeric inverse, and a float32 point's scaled value has
         * already lost its exponent — neither is writable, and the compiler
         * has no reason to allow one to be. */
        return -1;
    }
}

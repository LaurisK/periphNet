#include "modbus_config_export.h"
#include "modbus_config_store.h"
#include "modbus_units.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Runs on the HTTP task (4 KB stack) — keep the format buffer static. */
static char s_line[192];

static int emit(fMbByteSink sink, void *ctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(s_line)) {
        return -1;
    }
    return sink(ctx, s_line, (uint32_t)n);
}

static const char *decode_type_str(uint8_t type)
{
    switch (type) {
    case MB_DECODE_U16:        return "u16";
    case MB_DECODE_S16:        return "s16";
    case MB_DECODE_U32_BE:     return "u32_be";
    case MB_DECODE_U32_LE:     return "u32_le";
    case MB_DECODE_S32_BE:     return "s32_be";
    case MB_DECODE_S32_LE:     return "s32_le";
    case MB_DECODE_FLOAT32_BE: return "float32_be";
    case MB_DECODE_FLOAT32_LE: return "float32_le";
    case MB_DECODE_BITFIELD:   return "bitfield";
    case MB_DECODE_ASCII:      return "ascii";
    default:                   return "?";
    }
}

/* "1", "10", "0.01" ... from a scalePow10 exponent */
static void scale_str(int8_t pow10, char *buf, size_t size)
{
    size_t n = 0;

    if (pow10 >= 0) {
        buf[n++] = '1';
        for (int i = 0; i < pow10 && n + 1u < size; i++) {
            buf[n++] = '0';
        }
    } else {
        buf[n++] = '0';
        buf[n++] = '.';
        for (int i = 1; i < -pow10 && n + 1u < size; i++) {
            buf[n++] = '0';
        }
        buf[n++] = '1';
    }
    buf[n] = '\0';
}

static int export_point(fMbByteSink sink, void *ctx,
                        const sModbusPointRecord *pt, int first)
{
    const sMbUnitInfo *unit = MbUnits_FromCode(pt->unit);
    char scale[12];

    if (!unit) {
        return -1;
    }
    scale_str(pt->scalePow10, scale, sizeof(scale));

    if (emit(sink, ctx,
             "%s{\"offset\":%u,\"decodeType\":\"%s\",\"scale\":%s,"
             "\"unit\":\"%s\",\"name\":\"%s\"",
             first ? "" : ",",
             pt->offset, decode_type_str(pt->decodeType), scale,
             unit->str, pt->name) != 0) {
        return -1;
    }

    if (pt->decodeType == MB_DECODE_ASCII &&
        emit(sink, ctx, ",\"length\":%u", pt->length) != 0) {
        return -1;
    }

    if (pt->publishThreshold != 0u || pt->publishHeartbeatS != 0u) {
        if (emit(sink, ctx, ",\"publish\":{") != 0) {
            return -1;
        }
        int comma = 0;
        if (pt->publishThreshold != 0u) {
            if (emit(sink, ctx, "\"threshold\":%u",
                     pt->publishThreshold) != 0) {
                return -1;
            }
            comma = 1;
        }
        if (pt->publishHeartbeatS != 0u) {
            if (emit(sink, ctx, "%s\"heartbeatS\":%u",
                     comma ? "," : "", pt->publishHeartbeatS) != 0) {
                return -1;
            }
        }
        if (emit(sink, ctx, "}") != 0) {
            return -1;
        }
    }

    if (pt->flags & MB_POINT_FLAG_WRITABLE) {
        if (emit(sink, ctx, ",\"writable\":true") != 0) {
            return -1;
        }
        /* INT16_MIN/MAX is the compiled "no range validation" default —
         * omit it on export so the round trip stays canonical */
        if (pt->writeMin != INT16_MIN || pt->writeMax != INT16_MAX) {
            if (emit(sink, ctx, ",\"writeMin\":%d,\"writeMax\":%d",
                     pt->writeMin, pt->writeMax) != 0) {
                return -1;
            }
        }
    }

    return emit(sink, ctx, "}");
}

int MbCfgExport(uint32_t regionBase, fMbByteSink sink, void *ctx)
{
    sMbCfgCursor             c;
    sModbusDeviceRecord      dev;
    sModbusTransactionRecord txn;
    sModbusPointRecord       pt;
    int                      r, rt, rp;
    int                      firstDev = 1;

    if (!sink || MbCfg_Open(regionBase, &c) != 0) {
        return -1;
    }

    if (emit(sink, ctx, "{\"devices\":[") != 0) {
        return -1;
    }

    while ((r = MbCfg_NextDevice(&c, &dev)) == 1) {
        if (emit(sink, ctx,
                 "%s{\"slaveAddr\":%u,\"topicPrefix\":\"%s\","
                 "\"transactions\":[",
                 firstDev ? "" : ",", dev.slaveAddr, dev.topicPrefix) != 0) {
            return -1;
        }
        firstDev = 0;

        int firstTxn = 1;
        while ((rt = MbCfg_NextTransaction(&c, &txn)) == 1) {
            if (emit(sink, ctx,
                     "%s{\"startAddr\":%u,\"functionCode\":\"%s\","
                     "\"readPeriodS\":%u,\"points\":[",
                     firstTxn ? "" : ",", txn.startAddr,
                     (txn.functionCode == MB_FC_HOLDING) ? "holding" : "input",
                     txn.readPeriodS) != 0) {
                return -1;
            }
            firstTxn = 0;

            int firstPt = 1;
            while ((rp = MbCfg_NextPoint(&c, &pt)) == 1) {
                if (export_point(sink, ctx, &pt, firstPt) != 0) {
                    return -1;
                }
                firstPt = 0;
            }
            if (rp != 0 || emit(sink, ctx, "]}") != 0) {
                return -1;
            }
        }
        if (rt != 0 || emit(sink, ctx, "]}") != 0) {
            return -1;
        }
    }
    if (r != 0) {
        return -1;
    }

    return emit(sink, ctx, "]}");
}

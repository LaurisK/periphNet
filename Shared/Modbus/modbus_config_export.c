#include "modbus_config_export.h"
#include "modbus_config_store.h"
#include "modbus_units.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Runs on the HTTP task (4 KB stack) — keep the format buffer static. */
static char s_line[192];

static int emit(fModbusByteSink sink, void *ctx, const char *fmt, ...)
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
    case mbDecode_u16:        return "u16";
    case mbDecode_s16:        return "s16";
    case mbDecode_u32Be:     return "u32_be";
    case mbDecode_u32Le:     return "u32_le";
    case mbDecode_s32Be:     return "s32_be";
    case mbDecode_s32Le:     return "s32_le";
    case mbDecode_float32Be: return "float32_be";
    case mbDecode_float32Le: return "float32_le";
    case mbDecode_bitfield:   return "bitfield";
    case mbDecode_ascii:      return "ascii";
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

static const char *fc_str(uint8_t fc)
{
    return (fc == mbFc_holding) ? "holding" : "input";
}

static const char *port_str(uint8_t portId)
{
    return (portId == mbPort_test) ? "test" : "rs485";
}

static const char *format_str(uint8_t fmt)
{
    switch (fmt) {
    case mbFmt_8E1: return "8E1";
    case mbFmt_8O1: return "8O1";
    case mbFmt_8N2: return "8N2";
    default:        return "8N1";
    }
}

static int export_point(fModbusByteSink sink, void *ctx,
                        const sModbusPointRecord *pt, uint16_t id)
{
    const sMbUnitInfo *unit = MbUnits_FromCode(pt->unit);
    char scale[12];

    if (!unit) {
        return -1;
    }
    scale_str(pt->scalePow10, scale, sizeof(scale));

    if (emit(sink, ctx,
             "%s{\"id\":%u,\"addr\":%u,\"fc\":\"%s\",\"decodeType\":\"%s\","
             "\"scale\":%s,\"unit\":\"%s\",\"name\":\"%s\"",
             (id == 0u) ? "" : ",",
             id, pt->addr, fc_str(pt->functionCode),
             decode_type_str(pt->decodeType), scale,
             unit->str, pt->name) != 0) {
        return -1;
    }

    if (pt->decodeType == mbDecode_ascii &&
        emit(sink, ctx, ",\"length\":%u", pt->length) != 0) {
        return -1;
    }

    /* access defaults to "r", so only w/rw is emitted. */
    if ((pt->flags & MB_PT_WRITE) != 0u) {
        if (emit(sink, ctx, ",\"access\":\"%s\"",
                 (pt->flags & MB_PT_READ) ? "rw" : "w") != 0) {
            return -1;
        }
        /* Bounds are emitted only if the author WROTE them: expanding absent
         * bounds to the type range would export keys nobody authored, and the
         * round trip is exactly what catches that (§9). */
        if ((pt->flags & MB_PT_BOUNDED) != 0u &&
            emit(sink, ctx, ",\"writeMin\":%ld,\"writeMax\":%ld",
                 (long)pt->writeMin, (long)pt->writeMax) != 0) {
            return -1;
        }
    }

    return emit(sink, ctx, "}");
}

static int export_capability(fModbusByteSink sink, void *ctx, sMbCfgCursor *c,
                             const sModbusCapabilityRecord *cap, uint16_t id)
{
    sModbusBlockRecord blocks[MB_MAX_BLOCKS_PER_CAP];
    sModbusPointRecord pt;
    uint16_t           ptId = 0;
    int                rp;

    if (cap->blockCount > MB_MAX_BLOCKS_PER_CAP ||
        MbCfg_ReadBlocks(c, blocks, cap->blockCount) != 0) {
        return -1;
    }

    if (emit(sink, ctx,
             "%s{\"id\":%u,\"name\":\"%s\",\"addrStride\":%u,\"writeFc\":%u,"
             "\"maxReadRegs\":%u,\"blocks\":[",
             (id == 0u) ? "" : ",",
             id, cap->name, MbRecords_Stride(cap), cap->writeFc,
             MbRecords_MaxReadRegs(cap)) != 0) {
        return -1;
    }

    for (uint8_t i = 0; i < cap->blockCount; i++) {
        if (emit(sink, ctx, "%s{\"base\":%u,\"regs\":%u}",
                 (i == 0u) ? "" : ",", blocks[i].base, blocks[i].regs) != 0) {
            return -1;
        }
    }

    if (emit(sink, ctx, "],\"points\":[") != 0) {
        return -1;
    }
    while ((rp = MbCfg_NextPoint(c, &pt)) == 1) {
        if (export_point(sink, ctx, &pt, ptId) != 0) {
            return -1;
        }
        ptId++;
    }
    if (rp != 0) {
        return -1;
    }

    return emit(sink, ctx, "]}");
}

static int export_plan(fModbusByteSink sink, void *ctx, sMbCfgCursor *c,
                       const sModbusPlanRecord *plan, int first)
{
    sModbusTimeTableRecord tt;
    uint16_t               ids[MB_MAX_TT_ENTRIES_PER_TABLE];
    int                    ttId = 0;
    int                    rt;
    int                    firstDev = 1;

    if (emit(sink, ctx,
             "%s{\"id\":%u,\"name\":\"%s\",\"capability\":%u,\"devices\":[",
             first ? "" : ",", plan->planId, plan->name, plan->capId) != 0) {
        return -1;
    }
    for (uint8_t d = 0; d < MB_MAX_DEVICES; d++) {
        if ((plan->devices & (uint8_t)(1u << d)) == 0u) {
            continue;
        }
        if (emit(sink, ctx, "%s%u", firstDev ? "" : ",", d) != 0) {
            return -1;
        }
        firstDev = 0;
    }
    if (emit(sink, ctx, "],\"timeTables\":[") != 0) {
        return -1;
    }

    while ((rt = MbCfg_NextTimeTable(c, &tt)) == 1) {
        if (tt.entryCount > (sizeof(ids) / sizeof(ids[0])) ||
            MbCfg_ReadPointIds(c, ids, tt.entryCount) != 0) {
            return -1;
        }
        if (emit(sink, ctx, "%s{\"id\":%u,\"everySec\":%lu,\"points\":[",
                 (ttId == 0) ? "" : ",", ttId,
                 (unsigned long)tt.period_sec) != 0) {
            return -1;
        }
        for (uint16_t i = 0; i < tt.entryCount; i++) {
            if (emit(sink, ctx, "%s%u", (i == 0u) ? "" : ",", ids[i]) != 0) {
                return -1;
            }
        }
        if (emit(sink, ctx, "]}") != 0) {
            return -1;
        }
        ttId++;
    }
    if (rt != 0) {
        return -1;
    }

    return emit(sink, ctx, "]}");
}

int MbCfgExport(eNvDbUser region, fModbusByteSink sink, void *ctx)
{
    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusDeviceRecord     dev;
    sModbusPlanRecord       plan;
    uint16_t                capId = 0;
    uint8_t                 devId = 0;
    int                     r;
    int                     firstDev = 1, firstPlan = 1;

    if (!sink || MbCfg_Open(region, &c) != 0) {
        return -1;
    }

    if (emit(sink, ctx, "{\"capabilities\":[") != 0) {
        return -1;
    }
    while ((r = MbCfg_NextCapability(&c, &cap)) == 1) {
        if (export_capability(sink, ctx, &c, &cap, capId) != 0) {
            return -1;
        }
        capId++;
    }
    if (r != 0 || emit(sink, ctx, "],\"devices\":[") != 0) {
        return -1;
    }

    while ((r = MbCfg_NextDevice(&c, &dev)) == 1) {
        if (emit(sink, ctx,
                 "%s{\"id\":%u,\"slaveAddr\":%u,\"capability\":%u,"
                 "\"baud\":%lu,\"format\":\"%s\",\"port\":\"%s\","
                 "\"topicPrefix\":\"%s\"}",
                 firstDev ? "" : ",",
                 devId, dev.slaveAddr, dev.capId,
                 (unsigned long)MbRecords_BaudFromCode(dev.baudCode),
                 format_str(dev.format), port_str(dev.portId),
                 dev.topicPrefix) != 0) {
            return -1;
        }
        firstDev = 0;
        devId++;
    }
    if (r != 0 || emit(sink, ctx, "],\"plans\":[") != 0) {
        return -1;
    }

    while ((r = MbCfg_NextPlan(&c, &plan)) == 1) {
        if (export_plan(sink, ctx, &c, &plan, firstPlan) != 0) {
            return -1;
        }
        firstPlan = 0;
    }
    if (r != 0) {
        return -1;
    }

    return emit(sink, ctx, "]}");
}

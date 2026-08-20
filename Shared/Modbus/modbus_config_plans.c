/**
 * @file    modbus_config_plans.c
 * @brief   Runtime plan edits — see modbus_config_plans.h.
 */

#include "modbus_config_plans.h"
#include "modbus_config_store.h"
#include "image_mgmt.h"
#include "nvdb.h"

#include <stddef.h>
#include <string.h>

#define PT_BITMAP_BYTES ((MB_MAX_POINTS_TOTAL + 7u) / 8u)

/* Static, like the compiler's: this runs on whichever task drives the edit and
 * nothing big may sit on its stack.  Serialized by the module — only one edit
 * is in flight at a time (docs/modbus.md §4.8). */
/* How much rewritten stream is buffered before it goes to nvDb.  A buffer,
 * not a page: where the medium's boundaries fall is nvDb's business. */
#define MB_PLANS_WRITE_CHUNK 256u

#if defined(STM32F407xx)
#define MB_PLANS_BSS __attribute__((section(".ccmram")))
#else
#define MB_PLANS_BSS
#endif

typedef struct {
    eNvDbUser region;                /* destination region                  */
    uint32_t  regionMax;             /* bytes it can hold                   */
    uint32_t  streamOff;
    uint8_t   page[MB_PLANS_WRITE_CHUNK];
    uint32_t  pageLen;
    uint32_t  crc;
    void    (*kick)(void);
} sWriter;

MB_PLANS_BSS static sWriter s_w;
MB_PLANS_BSS static uint8_t s_ptSeen[PT_BITMAP_BYTES];

/* ==========================================================================
 * Validation — the plan-shaped subset of the compiler's rules (§7.4)
 * ========================================================================== */

static inline void bit_set(uint8_t *bits, uint16_t i)
{
    bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
}

static inline int bit_get(const uint8_t *bits, uint16_t i)
{
    return (bits[i / 8u] >> (i % 8u)) & 1u;
}

eModbusPlanErr MbCfgPlans_Validate(eNvDbUser region, const sModbusPlanSpec *spec)
{
    sModbusConfigCounts     counts;
    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusPointRecord      pt;
    uint16_t                ptCount = 0;

    if (spec == NULL || spec->name == NULL || spec->name[0] == '\0' ||
        strlen(spec->name) >= MB_NAME_LEN ||
        spec->tableCount > MB_MAX_TIME_TABLES_PER_PLAN ||
        (spec->tableCount > 0u && spec->tables == NULL)) {
        return mbPlan_errBadArg;
    }
    if (MbCfg_Count(region, &counts) != 0) {
        return mbPlan_errStream;
    }
    if (spec->capId >= counts.capabilities) {
        return mbPlan_errNoCap;
    }

    /* Every device in the set must exist AND implement this capability, so a
     * plan whose device set reaches outside its own capability is impossible
     * (§3.1). */
    for (uint8_t d = 0; d < 8u; d++) {
        sModbusDeviceRecord dev;

        if ((spec->devices & (uint8_t)(1u << d)) == 0u) {
            continue;
        }
        if (d >= counts.devices || MbCfg_FindDevice(region, d, &dev) != 0) {
            return mbPlan_errNoDevice;
        }
        if (dev.capId != spec->capId) {
            return mbPlan_errDeviceCap;
        }
    }

    /* Count the capability's points, and remember which of them may be read. */
    memset(s_ptSeen, 0, sizeof(s_ptSeen));
    if (MbCfg_OpenCapability(region, spec->capId, &c, &cap, NULL, 0) != 0) {
        return mbPlan_errNoCap;
    }

    uint8_t readable[PT_BITMAP_BYTES];
    memset(readable, 0, sizeof(readable));
    while (MbCfg_NextPoint(&c, &pt) == 1) {
        if (ptCount >= MB_MAX_POINTS_TOTAL) {
            return mbPlan_errStream;
        }
        if ((pt.flags & MB_PT_READ) != 0u) {
            bit_set(readable, ptCount);
        }
        ptCount++;
    }

    for (uint8_t t = 0; t < spec->tableCount; t++) {
        const sModbusTimeTableSpec *tt = &spec->tables[t];

        if (tt->period_sec < 1u) {
            return mbPlan_errPeriod;
        }
        if (tt->count == 0u || tt->count > MB_MAX_TT_ENTRIES_PER_TABLE ||
            tt->points == NULL) {
            return mbPlan_errBadArg;
        }
        for (uint16_t i = 0; i < tt->count; i++) {
            uint16_t id = tt->points[i];

            if (id >= ptCount) {
                return mbPlan_errNoPoint;
            }
            if (!bit_get(readable, id)) {
                return mbPlan_errWriteOnly;
            }
            if (bit_get(s_ptSeen, id)) {
                return mbPlan_errDuplicate;
            }
            bit_set(s_ptSeen, id);
        }
    }

    return mbPlan_ok;
}

/* ==========================================================================
 * Region writer — the compiler's, minus the JSON
 * ========================================================================== */

static int w_flush_page(void)
{
    if (s_w.pageLen == 0u) {
        return 0;
    }

    uint32_t regionOff = MODBUS_LUT_HEADER_SIZE + s_w.streamOff - s_w.pageLen;

    if (s_w.kick) {
        s_w.kick();
    }
    if (NvDb_Write(s_w.region, s_w.page, regionOff,
                   s_w.pageLen) != nvdbRes_ok) {
        return -1;
    }
    s_w.pageLen = 0;
    return 0;
}

static int w_write(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (MODBUS_LUT_HEADER_SIZE + s_w.streamOff + len > s_w.regionMax) {
        return -1;
    }
    s_w.crc = ImgMgmt_Crc32Update(s_w.crc, p, len);

    while (len > 0u) {
        uint32_t n = MB_PLANS_WRITE_CHUNK - s_w.pageLen;
        if (n > len) {
            n = len;
        }
        memcpy(&s_w.page[s_w.pageLen], p, n);
        s_w.pageLen   += n;
        s_w.streamOff += n;
        p   += n;
        len -= n;
        if (s_w.pageLen == MB_PLANS_WRITE_CHUNK && w_flush_page() != 0) {
            return -1;
        }
    }
    return 0;
}

/* The header goes LAST, so a torn rewrite never yields a valid region. */
static int w_finish(void)
{
    sModbusLutHeader hdr;

    if (w_flush_page() != 0) {
        return -1;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic     = MODBUS_LUT_MAGIC;
    hdr.version   = MODBUS_LUT_VERSION;
    hdr.streamLen = s_w.streamOff;
    hdr.crc32     = ImgMgmt_Crc32Final(s_w.crc);

    return (NvDb_Write(s_w.region, &hdr, 0u,
                       (uint32_t)sizeof(hdr)) == nvdbRes_ok) ? 0 : -1;
}

/* ==========================================================================
 * Rewrite
 * ========================================================================== */

/* Copy `len` bytes of the source region's stream, from offset `off`. */
static int copy_span(eNvDbUser srcRegion, uint32_t off, uint32_t len)
{
    uint8_t buf[128];

    while (len > 0u) {
        uint32_t n = (len > sizeof(buf)) ? sizeof(buf) : len;

        if (NvDb_Read(srcRegion, buf, MODBUS_LUT_HEADER_SIZE + off,
                      n) != nvdbRes_ok) {
            return -1;
        }
        if (w_write(buf, n) != 0) {
            return -1;
        }
        off += n;
        len -= n;
    }
    return 0;
}

/* Emit one plan from the spec: the record, its time tables, the sentinel. */
static int emit_spec(uint8_t slot, const sModbusPlanSpec *spec)
{
    sModbusPlanRecord rec;

    memset(&rec, 0, sizeof(rec));
    {
        size_t n = strlen(spec->name);
        if (n >= MB_NAME_LEN) {
            n = MB_NAME_LEN - 1u;
        }
        memcpy(rec.name, spec->name, n);
    }
    rec.capId   = spec->capId;
    rec.planId  = slot;
    rec.devices = spec->devices;

    if (w_write(&rec, sizeof(rec)) != 0) {
        return -1;
    }

    for (uint8_t t = 0; t < spec->tableCount; t++) {
        sModbusTimeTableRecord tt;

        memset(&tt, 0, sizeof(tt));
        tt.period_sec = spec->tables[t].period_sec;
        tt.entryCount = spec->tables[t].count;
        if (w_write(&tt, sizeof(tt)) != 0 ||
            w_write(spec->tables[t].points,
                    (uint32_t)tt.entryCount * sizeof(uint16_t)) != 0) {
            return -1;
        }
    }

    sModbusTimeTableRecord end;
    memset(&end, 0, sizeof(end));
    return w_write(&end, sizeof(end));
}

/* Copy one plan and its body verbatim from the source cursor. */
static int copy_plan(eNvDbUser srcRegion, sMbCfgCursor *c,
                     const sModbusPlanRecord *rec)
{
    sModbusTimeTableRecord tt;
    int                    r;

    if (w_write(rec, sizeof(*rec)) != 0) {
        return -1;
    }
    while ((r = MbCfg_NextTimeTable(c, &tt)) == 1) {
        uint32_t idsOff = c->off;
        uint32_t idsLen = (uint32_t)tt.entryCount * sizeof(uint16_t);

        if (MbCfg_ReadPointIds(c, NULL, tt.entryCount) != 0 ||
            w_write(&tt, sizeof(tt)) != 0 ||
            copy_span(srcRegion, idsOff, idsLen) != 0) {
            return -1;
        }
    }
    if (r != 0) {
        return -1;
    }

    sModbusTimeTableRecord end;
    memset(&end, 0, sizeof(end));
    return w_write(&end, sizeof(end));
}

eModbusPlanErr MbCfgPlans_Rewrite(eNvDbUser srcRegion, eNvDbUser dstRegion,
                                  uint8_t slot, const sModbusPlanSpec *spec,
                                  void (*kick)(void))
{
    sMbCfgCursor      c;
    sModbusPlanRecord plan;
    uint32_t          planSectionOff;
    int               emitted = 0;
    uint32_t          dstSize = 0u;
    int               r;

    if (slot >= MB_MAX_PLANS || srcRegion == dstRegion) {
        return mbPlan_errBadArg;
    }
    if (spec != NULL) {
        eModbusPlanErr v = MbCfgPlans_Validate(srcRegion, spec);
        if (v != mbPlan_ok) {
            return v;
        }
    }

    /* Where the plan section starts: everything before it is copied byte for
     * byte, which is what makes an untouched section provably untouched. */
    if (MbCfg_SeekPlans(srcRegion, &c) != 0) {
        return mbPlan_errStream;
    }
    planSectionOff = c.off;

    if (NvDb_GetSize(dstRegion, &dstSize) != nvdbRes_ok ||
        dstSize <= MODBUS_LUT_HEADER_SIZE) {
        return mbPlan_errFlash;
    }

    memset(&s_w, 0, sizeof(s_w));
    s_w.region    = dstRegion;
    s_w.regionMax = dstSize;
    s_w.crc       = ImgMgmt_Crc32Init();
    s_w.kick      = kick;

    if (kick) {
        kick();
    }
    /* Stops the destination validating before a single byte of the new
     * stream is written, and hands its erases to the collector. */
    if (MbCfgStore_EraseRegion(dstRegion) != 0) {
        return mbPlan_errFlash;
    }

    if (copy_span(srcRegion, 0u, planSectionOff) != 0) {
        return mbPlan_errFull;
    }

    /* Plans in ascending slot order, the edited one substituted in place. */
    while ((r = MbCfg_NextPlan(&c, &plan)) == 1) {
        if (plan.planId >= MB_MAX_PLANS) {
            return mbPlan_errStream;
        }
        if (plan.planId == slot) {
            /* Replaced or deleted: consume the old body either way. */
            sModbusTimeTableRecord tt;
            int rt;
            while ((rt = MbCfg_NextTimeTable(&c, &tt)) == 1) {
                if (MbCfg_ReadPointIds(&c, NULL, tt.entryCount) != 0) {
                    return mbPlan_errStream;
                }
            }
            if (rt != 0) {
                return mbPlan_errStream;
            }
            if (spec != NULL && emit_spec(slot, spec) != 0) {
                return mbPlan_errFull;
            }
            emitted = 1;
            continue;
        }

        /* A create lands before the first higher slot, so order is kept. */
        if (spec != NULL && !emitted && plan.planId > slot) {
            if (emit_spec(slot, spec) != 0) {
                return mbPlan_errFull;
            }
            emitted = 1;
        }
        if (copy_plan(srcRegion, &c, &plan) != 0) {
            return mbPlan_errFull;
        }
    }
    if (r != 0) {
        return mbPlan_errStream;
    }

    if (spec != NULL && !emitted && emit_spec(slot, spec) != 0) {
        return mbPlan_errFull;
    }

    sModbusPlanRecord end;
    memset(&end, 0, sizeof(end));
    if (w_write(&end, sizeof(end)) != 0 || w_finish() != 0) {
        return mbPlan_errFlash;
    }
    return mbPlan_ok;
}

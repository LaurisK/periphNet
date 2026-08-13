/**
 * @file    modbus_config_plans.c
 * @brief   Runtime plan edits — see modbus_config_plans.h.
 */

#include "modbus_config_plans.h"
#include "modbus_config_store.h"
#include "image_mgmt.h"
#include "w25q128.h"

#include <stddef.h>
#include <string.h>

#define PT_BITMAP_BYTES ((MB_MAX_POINTS_TOTAL + 7u) / 8u)

/* Static, like the compiler's: this runs on whichever task drives the edit and
 * nothing big may sit on its stack.  Serialized by the module — only one edit
 * is in flight at a time (docs/modbus.md §4.8). */
#if defined(STM32F407xx)
#define MB_PLANS_BSS __attribute__((section(".ccmram")))
#else
#define MB_PLANS_BSS
#endif

typedef struct {
    uint32_t base;                   /* destination region                  */
    uint32_t streamOff;
    uint8_t  page[W25Q128_PAGE_SIZE];
    uint32_t pageLen;
    uint32_t nextEraseOff;
    uint32_t crc;
    void   (*kick)(void);
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

eModbusPlanErr MbCfgPlans_Validate(uint32_t base, const sModbusPlanSpec *spec)
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
    if (MbCfg_Count(base, &counts) != 0) {
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
        if (d >= counts.devices || MbCfg_FindDevice(base, d, &dev) != 0) {
            return mbPlan_errNoDevice;
        }
        if (dev.capId != spec->capId) {
            return mbPlan_errDeviceCap;
        }
    }

    /* Count the capability's points, and remember which of them may be read. */
    memset(s_ptSeen, 0, sizeof(s_ptSeen));
    if (MbCfg_OpenCapability(base, spec->capId, &c, &cap, NULL, 0) != 0) {
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

static int w_erase_up_to(uint32_t regionOff)
{
    while (s_w.nextEraseOff <= regionOff) {
        if (s_w.kick) {
            s_w.kick();
        }
        if (W25Q128_EraseSector(s_w.base + s_w.nextEraseOff) != w25q_ok) {
            return -1;
        }
        s_w.nextEraseOff += W25Q128_SECTOR_SIZE;
    }
    return 0;
}

static int w_flush_page(void)
{
    if (s_w.pageLen == 0u) {
        return 0;
    }

    uint32_t regionOff = MODBUS_LUT_HEADER_SIZE + s_w.streamOff - s_w.pageLen;

    if (w_erase_up_to(regionOff + s_w.pageLen) != 0) {
        return -1;
    }
    if (W25Q128_WritePage(s_w.base + regionOff, s_w.page,
                          s_w.pageLen) != w25q_ok) {
        return -1;
    }
    s_w.pageLen = 0;
    return 0;
}

static int w_write(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (MODBUS_LUT_HEADER_SIZE + s_w.streamOff + len >
        EXT_FLASH_MODBUS_LUT_SIZE) {
        return -1;
    }
    s_w.crc = ImgMgmt_Crc32Update(s_w.crc, p, len);

    while (len > 0u) {
        uint32_t n = W25Q128_PAGE_SIZE - s_w.pageLen;
        if (n > len) {
            n = len;
        }
        memcpy(&s_w.page[s_w.pageLen], p, n);
        s_w.pageLen   += n;
        s_w.streamOff += n;
        p   += n;
        len -= n;
        if (s_w.pageLen == W25Q128_PAGE_SIZE && w_flush_page() != 0) {
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

    return (W25Q128_WritePage(s_w.base, (const uint8_t *)&hdr,
                              sizeof(hdr)) == w25q_ok) ? 0 : -1;
}

/* ==========================================================================
 * Rewrite
 * ========================================================================== */

/* Copy `len` bytes of the source region's stream, from offset `off`. */
static int copy_span(uint32_t srcBase, uint32_t off, uint32_t len)
{
    uint8_t buf[128];

    while (len > 0u) {
        uint32_t n = (len > sizeof(buf)) ? sizeof(buf) : len;

        if (W25Q128_Read(srcBase + MODBUS_LUT_HEADER_SIZE + off,
                         buf, n) != w25q_ok) {
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
static int copy_plan(uint32_t srcBase, sMbCfgCursor *c,
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
            copy_span(srcBase, idsOff, idsLen) != 0) {
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

eModbusPlanErr MbCfgPlans_Rewrite(uint32_t srcBase, uint32_t dstBase,
                                  uint8_t slot, const sModbusPlanSpec *spec,
                                  void (*kick)(void))
{
    sMbCfgCursor      c;
    sModbusPlanRecord plan;
    uint32_t          planSectionOff;
    int               emitted = 0;
    int               r;

    if (slot >= MB_MAX_PLANS || srcBase == dstBase) {
        return mbPlan_errBadArg;
    }
    if (spec != NULL) {
        eModbusPlanErr v = MbCfgPlans_Validate(srcBase, spec);
        if (v != mbPlan_ok) {
            return v;
        }
    }

    /* Where the plan section starts: everything before it is copied byte for
     * byte, which is what makes an untouched section provably untouched. */
    if (MbCfg_SeekPlans(srcBase, &c) != 0) {
        return mbPlan_errStream;
    }
    planSectionOff = c.off;

    memset(&s_w, 0, sizeof(s_w));
    s_w.base = dstBase;
    s_w.crc  = ImgMgmt_Crc32Init();
    s_w.kick = kick;

    if (kick) {
        kick();
    }
    if (W25Q128_EraseSector(dstBase) != w25q_ok) {
        return mbPlan_errFlash;
    }
    s_w.nextEraseOff = W25Q128_SECTOR_SIZE;

    if (copy_span(srcBase, 0u, planSectionOff) != 0) {
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
        if (copy_plan(srcBase, &c, &plan) != 0) {
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

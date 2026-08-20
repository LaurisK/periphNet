#include "modbus_config_store.h"
#include "image_mgmt.h"
#include "nvdb.h"

#include <stddef.h>
#include <string.h>

/* Offset of the flags field inside sMbSelector — flags live outside the CRC
 * so swap_pending can be bit-cleared without erasing the sector. */
#define SEL_FLAGS_OFFSET  offsetof(sMbSelector, flags)

/* CRC of the record stream is verified in 256-byte chunks. */
#define REGION_SCAN_CHUNK 256u

/* Cached active-region index — all mutations go through this module, and a
 * word-sized read/write is atomic, so tasks may query it lock-free. */
static uint32_t s_activeRegion;

/* ==========================================================================
 * Selector header
 * ========================================================================== */

static uint32_t sel_header_crc(const sMbSelector *sel)
{
    return ImgMgmt_Crc32((const uint8_t *)sel,
                         offsetof(sMbSelector, header_crc32));
}

static int sel_read(sMbSelector *sel)
{
    if (NvDb_Read(nvdbUser_modbusSelector, sel, 0u,
                  (uint32_t)sizeof(*sel)) != nvdbRes_ok) {
        return -1;
    }

    if (sel->magic != MODBUS_SEL_MAGIC ||
        sel->version != MODBUS_SEL_VERSION ||
        sel->activeRegion > 1u ||
        sel->header_crc32 != sel_header_crc(sel)) {
        return -1;
    }

    return 0;
}

static int sel_write(const sMbSelector *sel)
{
    /* Rewriting the whole record sets bits that are already clear, so nvDb
     * does the erase-and-write-back for us.  Whether that costs an erase is
     * its business, not this module's. */
    if (NvDb_Write(nvdbUser_modbusSelector, sel, 0u,
                   (uint32_t)sizeof(*sel)) != nvdbRes_ok) {
        return -1;
    }
    return 0;
}

static eNvDbUser region_user(uint32_t index)
{
    return (index == 0u) ? nvdbUser_modbusLutA : nvdbUser_modbusLutB;
}

/* The record stream a region can hold, minus its header. */
static uint32_t region_stream_max(eNvDbUser region)
{
    uint32_t size = 0u;

    if (NvDb_GetSize(region, &size) != nvdbRes_ok ||
        size <= MODBUS_LUT_HEADER_SIZE) {
        return 0u;
    }
    return size - MODBUS_LUT_HEADER_SIZE;
}

/* ==========================================================================
 * Init — validate selector, recover from blank/corrupt sector
 * ========================================================================== */

int MbCfgStore_Init(void)
{
    sMbSelector sel;

    if (sel_read(&sel) == 0) {
        s_activeRegion = sel.activeRegion;
        return 0;
    }

    /* Blank or corrupt (e.g. power cut during CommitSwap): prefer whichever
     * region holds a valid config so a previously working setup survives. */
    uint32_t active = 0u;
    if (!MbCfgStore_RegionValid(nvdbUser_modbusLutA) &&
        MbCfgStore_RegionValid(nvdbUser_modbusLutB)) {
        active = 1u;
    }

    memset(&sel, 0, sizeof(sel));
    sel.magic        = MODBUS_SEL_MAGIC;
    sel.version      = MODBUS_SEL_VERSION;
    sel.activeRegion = active;
    sel.header_crc32 = sel_header_crc(&sel);
    sel.flags.word   = 0xFFFFFFFFu;      /* no swap pending */

    if (sel_write(&sel) != 0) {
        return -1;
    }

    s_activeRegion = active;
    return 0;
}

eNvDbUser MbCfgStore_ActiveRegion(void)
{
    return region_user(s_activeRegion);
}

eNvDbUser MbCfgStore_InactiveRegion(void)
{
    return region_user(s_activeRegion ^ 1u);
}

/* ==========================================================================
 * Region validity — header sanity + stream CRC
 * ========================================================================== */

bool MbCfgStore_RegionValid(eNvDbUser region)
{
    sModbusLutHeader hdr;

    if (NvDb_Read(region, &hdr, 0u, (uint32_t)sizeof(hdr)) != nvdbRes_ok) {
        return false;
    }

    if (hdr.magic != MODBUS_LUT_MAGIC ||
        hdr.version != MODBUS_LUT_VERSION ||
        hdr.streamLen == 0u ||
        hdr.streamLen > region_stream_max(region)) {
        return false;
    }

    uint32_t crc = ImgMgmt_Crc32Init();
    uint32_t off = 0u;
    while (off < hdr.streamLen) {
        uint8_t  chunk[REGION_SCAN_CHUNK];
        uint32_t n = hdr.streamLen - off;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (NvDb_Read(region, chunk, MODBUS_LUT_HEADER_SIZE + off,
                      n) != nvdbRes_ok) {
            return false;
        }
        crc = ImgMgmt_Crc32Update(crc, chunk, n);
        off += n;
    }

    return ImgMgmt_Crc32Final(crc) == hdr.crc32;
}

/* ==========================================================================
 * Swap flag — NOR bit-clear (arm) / sector rewrite (commit)
 * ========================================================================== */

int MbCfgStore_SetSwapPending(void)
{
    if (!MbCfgStore_RegionValid(MbCfgStore_InactiveRegion())) {
        return -1;                       /* nothing valid to swap to */
    }

    sMbSelFlags flags;
    if (NvDb_Read(nvdbUser_modbusSelector, &flags, SEL_FLAGS_OFFSET,
                  (uint32_t)sizeof(flags)) != nvdbRes_ok) {
        return -1;
    }

    /* Clearing one bit of a word that is otherwise unchanged: nvDb programs
     * that in place, so arming a swap still costs no erase and is still
     * atomic — which is the whole reason the flags sit outside the CRC. */
    flags.bits.swap_pending = 0;
    if (NvDb_Write(nvdbUser_modbusSelector, &flags, SEL_FLAGS_OFFSET,
                   (uint32_t)sizeof(flags)) != nvdbRes_ok) {
        return -1;
    }
    return 0;
}

bool MbCfgStore_IsSwapPending(void)
{
    sMbSelFlags flags;

    if (NvDb_Read(nvdbUser_modbusSelector, &flags, SEL_FLAGS_OFFSET,
                  (uint32_t)sizeof(flags)) != nvdbRes_ok) {
        return false;
    }
    return flags.bits.swap_pending == 0;
}

int MbCfgStore_CommitSwap(void)
{
    sMbSelector sel;

    memset(&sel, 0, sizeof(sel));
    sel.magic        = MODBUS_SEL_MAGIC;
    sel.version      = MODBUS_SEL_VERSION;
    sel.activeRegion = s_activeRegion ^ 1u;
    sel.header_crc32 = sel_header_crc(&sel);
    sel.flags.word   = 0xFFFFFFFFu;      /* pending consumed */

    if (sel_write(&sel) != 0) {
        return -1;
    }

    s_activeRegion = sel.activeRegion;
    return 0;
}

int MbCfgStore_ClearSwapPending(void)
{
    sMbSelector sel;

    memset(&sel, 0, sizeof(sel));
    sel.magic        = MODBUS_SEL_MAGIC;
    sel.version      = MODBUS_SEL_VERSION;
    sel.activeRegion = s_activeRegion;   /* disarm only — no flip */
    sel.header_crc32 = sel_header_crc(&sel);
    sel.flags.word   = 0xFFFFFFFFu;      /* pending discarded */

    return sel_write(&sel);
}

/* ==========================================================================
 * Region erase — "invalid is erased, not repaired" (docs/modbus.md §4.2)
 * ========================================================================== */

int MbCfgStore_EraseRegion(eNvDbUser region)
{
    /* "Invalid is erased, not repaired" has to hold the instant this
     * returns, and nvDb's delete is eventual.  Clearing the header's magic
     * is a bit-clear, so it lands synchronously and costs nothing; the wipe
     * then reclaims the region in the background, which is what makes the
     * next upload's writes cheap. */
    uint32_t magic = 0u;

    if (NvDb_Write(region, &magic, offsetof(sModbusLutHeader, magic),
                   (uint32_t)sizeof(magic)) != nvdbRes_ok) {
        return -1;
    }
    return (NvDb_Wipe(region, NULL) == nvdbRes_ok) ? 0 : -1;
}

/* ==========================================================================
 * Record cursor
 * ========================================================================== */

int MbCfg_Open(eNvDbUser region, sMbCfgCursor *c)
{
    sModbusLutHeader hdr;

    if (!c) {
        return -1;
    }

    if (NvDb_Read(region, &hdr, 0u, (uint32_t)sizeof(hdr)) != nvdbRes_ok ||
        hdr.magic != MODBUS_LUT_MAGIC ||
        hdr.version != MODBUS_LUT_VERSION ||
        hdr.streamLen == 0u ||
        hdr.streamLen > region_stream_max(region)) {
        return -1;
    }

    c->region    = region;
    c->off       = 0u;
    c->streamLen = hdr.streamLen;
    return 0;
}

/* Read `size` bytes of stream; 1 = ok, -1 = stream overrun/read error. */
static int cursor_read(sMbCfgCursor *c, void *dst, uint32_t size)
{
    if (c->off + size > c->streamLen) {
        return -1;
    }
    if (dst != NULL &&
        NvDb_Read(c->region, dst, MODBUS_LUT_HEADER_SIZE + c->off,
                  size) != nvdbRes_ok) {
        return -1;
    }
    c->off += size;
    return 1;
}

int MbCfg_NextCapability(sMbCfgCursor *c, sModbusCapabilityRecord *cap)
{
    if (cursor_read(c, cap, sizeof(*cap)) != 1) {
        return -1;
    }
    return (cap->name[0] == '\0') ? 0 : 1;
}

int MbCfg_NextPoint(sMbCfgCursor *c, sModbusPointRecord *p)
{
    if (cursor_read(c, p, sizeof(*p)) != 1) {
        return -1;
    }
    return (p->decodeType == mbDecode_end) ? 0 : 1;
}

int MbCfg_NextDevice(sMbCfgCursor *c, sModbusDeviceRecord *d)
{
    if (cursor_read(c, d, sizeof(*d)) != 1) {
        return -1;
    }
    return (d->slaveAddr == 0u) ? 0 : 1;
}

int MbCfg_NextPlan(sMbCfgCursor *c, sModbusPlanRecord *p)
{
    if (cursor_read(c, p, sizeof(*p)) != 1) {
        return -1;
    }
    return (p->name[0] == '\0') ? 0 : 1;
}

int MbCfg_NextTimeTable(sMbCfgCursor *c, sModbusTimeTableRecord *t)
{
    if (cursor_read(c, t, sizeof(*t)) != 1) {
        return -1;
    }
    return (t->entryCount == 0u) ? 0 : 1;
}

int MbCfg_ReadBlocks(sMbCfgCursor *c, sModbusBlockRecord *out, uint8_t count)
{
    uint32_t bytes = (uint32_t)count * sizeof(sModbusBlockRecord);

    if (count == 0u) {
        return 0;
    }
    return (cursor_read(c, out, bytes) == 1) ? 0 : -1;
}

int MbCfg_ReadPointIds(sMbCfgCursor *c, uint16_t *out, uint16_t count)
{
    uint32_t bytes = (uint32_t)count * sizeof(uint16_t);

    if (count == 0u) {
        return 0;
    }
    return (cursor_read(c, out, bytes) == 1) ? 0 : -1;
}

/* ==========================================================================
 * Section navigation
 *
 * Every one of these is a linear scan from the top of the stream.  That is the
 * cost of a self-describing format with no index, and it is deliberate: the
 * alternative is stored offsets, which are a second thing that can disagree
 * with the records (docs/modbus.md §7.2).
 * ========================================================================== */

/* Consume one capability's blocks and points; the capability record itself has
 * already been read.  Counts points into *points if non-NULL. */
static int skip_capability_body(sMbCfgCursor *c,
                                const sModbusCapabilityRecord *cap,
                                uint16_t *points)
{
    sModbusPointRecord pt;
    int r;

    if (MbCfg_ReadBlocks(c, NULL, cap->blockCount) != 0) {
        return -1;
    }
    while ((r = MbCfg_NextPoint(c, &pt)) == 1) {
        if (points != NULL) {
            (*points)++;
        }
    }
    return (r == 0) ? 0 : -1;
}

/* Consume one plan's time tables and their id arrays; the plan record itself
 * has already been read.  Counts tables into *tables if non-NULL. */
static int skip_plan_body(sMbCfgCursor *c, uint8_t *tables)
{
    sModbusTimeTableRecord tt;
    int r;

    while ((r = MbCfg_NextTimeTable(c, &tt)) == 1) {
        if (MbCfg_ReadPointIds(c, NULL, tt.entryCount) != 0) {
            return -1;
        }
        if (tables != NULL) {
            (*tables)++;
        }
    }
    return (r == 0) ? 0 : -1;
}

int MbCfg_SeekDevices(eNvDbUser region, sMbCfgCursor *c)
{
    sModbusCapabilityRecord cap;
    int r;

    if (MbCfg_Open(region, c) != 0) {
        return -1;
    }
    while ((r = MbCfg_NextCapability(c, &cap)) == 1) {
        if (skip_capability_body(c, &cap, NULL) != 0) {
            return -1;
        }
    }
    return (r == 0) ? 0 : -1;
}

int MbCfg_SeekPlans(eNvDbUser region, sMbCfgCursor *c)
{
    sModbusDeviceRecord dev;
    int r;

    if (MbCfg_SeekDevices(region, c) != 0) {
        return -1;
    }
    while ((r = MbCfg_NextDevice(c, &dev)) == 1) { }
    return (r == 0) ? 0 : -1;
}

int MbCfg_Count(eNvDbUser region, sModbusConfigCounts *out)
{
    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusDeviceRecord     dev;
    sModbusPlanRecord       plan;
    sModbusConfigCounts     counts = { 0, 0, 0, 0 };
    int                     r;

    if (!out || MbCfg_Open(region, &c) != 0) {
        return -1;
    }

    while ((r = MbCfg_NextCapability(&c, &cap)) == 1) {
        counts.capabilities++;
        if (skip_capability_body(&c, &cap, &counts.points) != 0) {
            return -1;
        }
    }
    if (r != 0) {
        return -1;
    }

    while ((r = MbCfg_NextDevice(&c, &dev)) == 1) {
        counts.devices++;
    }
    if (r != 0) {
        return -1;
    }

    while ((r = MbCfg_NextPlan(&c, &plan)) == 1) {
        counts.plans++;
        if (skip_plan_body(&c, NULL) != 0) {
            return -1;
        }
    }
    if (r != 0) {
        return -1;
    }

    *out = counts;
    return 0;
}

int MbCfg_OpenCapability(eNvDbUser region, uint16_t capId, sMbCfgCursor *c,
                         sModbusCapabilityRecord *cap,
                         sModbusBlockRecord *blocks, uint8_t maxBlocks)
{
    uint16_t ord = 0;
    int      r;

    if (!c || !cap || MbCfg_Open(region, c) != 0) {
        return -1;
    }

    while ((r = MbCfg_NextCapability(c, cap)) == 1) {
        if (ord == capId) {
            uint8_t n = cap->blockCount;

            if (blocks != NULL && n > 0u) {
                /* Read what fits, skip the rest — a caller that sized its
                 * array to MB_MAX_BLOCKS_PER_CAP never hits this. */
                uint8_t take = (n > maxBlocks) ? maxBlocks : n;
                if (MbCfg_ReadBlocks(c, blocks, take) != 0 ||
                    MbCfg_ReadBlocks(c, NULL, (uint8_t)(n - take)) != 0) {
                    return -1;
                }
            } else if (MbCfg_ReadBlocks(c, NULL, n) != 0) {
                return -1;
            }
            return 0;                    /* cursor is at the first point */
        }
        if (skip_capability_body(c, cap, NULL) != 0) {
            return -1;
        }
        ord++;
    }
    return -1;
}

int MbCfg_FindDevice(eNvDbUser region, uint8_t devOrd, sModbusDeviceRecord *out)
{
    sMbCfgCursor c;
    uint8_t      ord = 0;

    if (!out || MbCfg_SeekDevices(region, &c) != 0) {
        return -1;
    }
    while (MbCfg_NextDevice(&c, out) == 1) {
        if (ord == devOrd) {
            return 0;
        }
        ord++;
    }
    return -1;
}

int MbCfg_FindPoint(eNvDbUser region, uint16_t capId, uint16_t ptOrd,
                    sModbusPointRecord *out)
{
    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    uint16_t                ord = 0;

    if (!out || MbCfg_OpenCapability(region, capId, &c, &cap, NULL, 0) != 0) {
        return -1;
    }
    while (MbCfg_NextPoint(&c, out) == 1) {
        if (ord == ptOrd) {
            return 0;
        }
        ord++;
    }
    return -1;
}

int MbCfg_ReadPlanHeaders(eNvDbUser region, sMbPlanHeader *out)
{
    sMbCfgCursor      c;
    sModbusPlanRecord plan;
    int               count = 0;
    int               r;

    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(sMbPlanHeader) * MB_MAX_PLANS);

    if (MbCfg_SeekPlans(region, &c) != 0) {
        return -1;
    }

    while ((r = MbCfg_NextPlan(&c, &plan)) == 1) {
        uint8_t tables = 0;

        if (skip_plan_body(&c, &tables) != 0) {
            return -1;
        }
        /* A stored slot outside the table is a corrupt stream, not a plan. */
        if (plan.planId >= MB_MAX_PLANS) {
            return -1;
        }
        out[plan.planId].rec        = plan;
        out[plan.planId].tableCount = tables;
        out[plan.planId].used       = 1u;
        count++;
    }
    return (r == 0) ? count : -1;
}

int MbCfg_ResolvePoint(uint8_t devOrd, uint16_t ptOrd, sMbPointLookup *out)
{
    eNvDbUser               region = MbCfgStore_ActiveRegion();
    sModbusDeviceRecord     dev;
    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    uint16_t                ord = 0;

    if (!out || MbCfg_FindDevice(region, devOrd, &dev) != 0) {
        return -1;
    }
    if (MbCfg_OpenCapability(region, dev.capId, &c, &cap, NULL, 0) != 0) {
        return -1;
    }

    while (MbCfg_NextPoint(&c, &out->point) == 1) {
        if (ord == ptOrd) {
            out->slaveAddr    = dev.slaveAddr;
            out->functionCode = out->point.functionCode;
            out->regAddr      = out->point.addr;
            out->capId        = dev.capId;
            out->addrStride   = MbRecords_Stride(&cap);
            out->writeFc      = cap.writeFc;
            out->portId       = dev.portId;
            out->baudCode     = dev.baudCode;
            out->format       = dev.format;
            return 0;
        }
        ord++;
    }
    return -1;
}

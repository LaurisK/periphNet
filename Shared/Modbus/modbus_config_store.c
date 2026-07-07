#include "modbus_config_store.h"
#include "image_mgmt.h"
#include "w25q128.h"

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
    if (W25Q128_Read(EXT_FLASH_MODBUS_SEL_ADDR,
                     (uint8_t *)sel, sizeof(*sel)) != W25Q128_OK) {
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
    if (W25Q128_EraseSector(EXT_FLASH_MODBUS_SEL_ADDR) != W25Q128_OK) {
        return -1;
    }
    if (W25Q128_WritePage(EXT_FLASH_MODBUS_SEL_ADDR,
                          (const uint8_t *)sel, sizeof(*sel)) != W25Q128_OK) {
        return -1;
    }
    return 0;
}

static uint32_t region_base(uint32_t index)
{
    return (index == 0u) ? EXT_FLASH_MODBUS_LUT_A_ADDR
                         : EXT_FLASH_MODBUS_LUT_B_ADDR;
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
    if (!MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR) &&
        MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_B_ADDR)) {
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

uint32_t MbCfgStore_ActiveBase(void)
{
    return region_base(s_activeRegion);
}

uint32_t MbCfgStore_InactiveBase(void)
{
    return region_base(s_activeRegion ^ 1u);
}

/* ==========================================================================
 * Region validity — header sanity + stream CRC
 * ========================================================================== */

bool MbCfgStore_RegionValid(uint32_t base)
{
    sModbusLutHeader hdr;

    if (W25Q128_Read(base, (uint8_t *)&hdr, sizeof(hdr)) != W25Q128_OK) {
        return false;
    }

    if (hdr.magic != MODBUS_LUT_MAGIC ||
        hdr.version != MODBUS_LUT_VERSION ||
        hdr.streamLen == 0u ||
        hdr.streamLen > EXT_FLASH_MODBUS_LUT_SIZE - MODBUS_LUT_HEADER_SIZE) {
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
        if (W25Q128_Read(base + MODBUS_LUT_HEADER_SIZE + off,
                         chunk, n) != W25Q128_OK) {
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
    if (!MbCfgStore_RegionValid(MbCfgStore_InactiveBase())) {
        return -1;                       /* nothing valid to swap to */
    }

    sMbSelFlags flags;
    if (W25Q128_Read(EXT_FLASH_MODBUS_SEL_ADDR + SEL_FLAGS_OFFSET,
                     (uint8_t *)&flags, sizeof(flags)) != W25Q128_OK) {
        return -1;
    }

    flags.bits.swap_pending = 0;
    if (W25Q128_WritePage(EXT_FLASH_MODBUS_SEL_ADDR + SEL_FLAGS_OFFSET,
                          (const uint8_t *)&flags,
                          sizeof(flags)) != W25Q128_OK) {
        return -1;
    }
    return 0;
}

bool MbCfgStore_IsSwapPending(void)
{
    sMbSelFlags flags;

    if (W25Q128_Read(EXT_FLASH_MODBUS_SEL_ADDR + SEL_FLAGS_OFFSET,
                     (uint8_t *)&flags, sizeof(flags)) != W25Q128_OK) {
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

/* ==========================================================================
 * Record cursor
 * ========================================================================== */

int MbCfg_Open(uint32_t base, sMbCfgCursor *c)
{
    sModbusLutHeader hdr;

    if (!c) {
        return -1;
    }

    if (W25Q128_Read(base, (uint8_t *)&hdr, sizeof(hdr)) != W25Q128_OK ||
        hdr.magic != MODBUS_LUT_MAGIC ||
        hdr.version != MODBUS_LUT_VERSION ||
        hdr.streamLen == 0u ||
        hdr.streamLen > EXT_FLASH_MODBUS_LUT_SIZE - MODBUS_LUT_HEADER_SIZE) {
        return -1;
    }

    c->base      = base;
    c->off       = 0u;
    c->streamLen = hdr.streamLen;
    return 0;
}

/* Read one whole record of `size` bytes; 1 = ok, -1 = stream overrun/error. */
static int cursor_read(sMbCfgCursor *c, void *rec, uint32_t size)
{
    if (c->off + size > c->streamLen) {
        return -1;
    }
    if (W25Q128_Read(c->base + MODBUS_LUT_HEADER_SIZE + c->off,
                     (uint8_t *)rec, size) != W25Q128_OK) {
        return -1;
    }
    c->off += size;
    return 1;
}

int MbCfg_NextDevice(sMbCfgCursor *c, sModbusDeviceRecord *d)
{
    int r = cursor_read(c, d, sizeof(*d));
    if (r != 1) {
        return -1;
    }
    return (d->slaveAddr == 0u) ? 0 : 1;
}

int MbCfg_NextTransaction(sMbCfgCursor *c, sModbusTransactionRecord *t)
{
    int r = cursor_read(c, t, sizeof(*t));
    if (r != 1) {
        return -1;
    }
    return (t->count == 0u) ? 0 : 1;
}

int MbCfg_NextPoint(sMbCfgCursor *c, sModbusPointRecord *p)
{
    int r = cursor_read(c, p, sizeof(*p));
    if (r != 1) {
        return -1;
    }
    return (p->decodeType == MB_DECODE_END) ? 0 : 1;
}

/* ==========================================================================
 * Whole-region helpers
 * ========================================================================== */

int MbCfg_Count(uint32_t base, sMbCfgCounts *out)
{
    sMbCfgCursor            c;
    sModbusDeviceRecord     dev;
    sModbusTransactionRecord txn;
    sModbusPointRecord      pt;
    sMbCfgCounts            counts = {0};

    if (!out || MbCfg_Open(base, &c) != 0) {
        return -1;
    }

    int r;
    while ((r = MbCfg_NextDevice(&c, &dev)) == 1) {
        counts.devices++;
        int rt;
        while ((rt = MbCfg_NextTransaction(&c, &txn)) == 1) {
            counts.transactions++;
            int rp;
            while ((rp = MbCfg_NextPoint(&c, &pt)) == 1) {
                counts.points++;
            }
            if (rp != 0) {
                return -1;
            }
        }
        if (rt != 0) {
            return -1;
        }
    }
    if (r != 0) {
        return -1;
    }

    *out = counts;
    return 0;
}

int MbCfg_FindWritablePoint(const char *topicPrefix, const char *name,
                            sMbPointLookup *out)
{
    sMbCfgCursor            c;
    sModbusDeviceRecord     dev;
    sModbusTransactionRecord txn;
    sModbusPointRecord      pt;

    if (!topicPrefix || !name || !out ||
        MbCfg_Open(MbCfgStore_ActiveBase(), &c) != 0) {
        return -1;
    }

    while (MbCfg_NextDevice(&c, &dev) == 1) {
        int devMatch = (strncmp(dev.topicPrefix, topicPrefix,
                                MB_TOPIC_PREFIX_LEN) == 0);
        while (MbCfg_NextTransaction(&c, &txn) == 1) {
            while (MbCfg_NextPoint(&c, &pt) == 1) {
                if (devMatch &&
                    (pt.flags & MB_POINT_FLAG_WRITABLE) != 0u &&
                    strncmp(pt.name, name, MB_POINT_NAME_LEN) == 0) {
                    out->slaveAddr = dev.slaveAddr;
                    out->regAddr   = (uint16_t)(txn.startAddr + pt.offset);
                    out->point     = pt;
                    return 0;
                }
            }
        }
    }

    return -1;
}

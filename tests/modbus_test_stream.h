/**
 * Helpers for hand-assembling Modbus config record streams (v2) into the NOR
 * flash mock — shared by test_modbus_store.c, test_modbus_compiler.c and
 * test_modbus_export.c.
 *
 * Stream order is the one the format forces (docs/modbus.md §7.1):
 * capabilities (each with its blocks then its points), devices, plans (each
 * with its time tables, every table followed by its point-id array).
 */
#ifndef MODBUS_TEST_STREAM_H
#define MODBUS_TEST_STREAM_H

#include "modbus_records.h"
#include "modbus_config_store.h"
#include "image_mgmt.h"
#include "w25q128.h"
#include "w25q128_mock.h"

#include <string.h>

typedef struct {
    uint8_t  buf[8192];
    uint32_t len;
} sTestStream;

static inline void ts_append(sTestStream *s, const void *rec, uint32_t size)
{
    memcpy(&s->buf[s->len], rec, size);
    s->len += size;
}

/* ---- capabilities ------------------------------------------------------ */

static inline void ts_capability(sTestStream *s, const char *name,
                                 uint8_t addrStride, uint8_t writeFc,
                                 uint16_t maxReadRegs, uint8_t blockCount)
{
    sModbusCapabilityRecord c;
    memset(&c, 0, sizeof(c));
    if (name) {
        strncpy(c.name, name, MB_NAME_LEN - 1);
    }
    c.addrStride  = addrStride;
    c.writeFc     = writeFc;
    c.maxReadRegs = maxReadRegs;
    c.blockCount  = blockCount;
    ts_append(s, &c, sizeof(c));
}

static inline void ts_block(sTestStream *s, uint16_t base, uint16_t regs)
{
    sModbusBlockRecord b = { base, regs };
    ts_append(s, &b, sizeof(b));
}

static inline sModbusPointRecord ts_mkpoint(uint8_t decodeType, uint16_t addr,
                                            uint8_t fc, int8_t scalePow10,
                                            uint8_t unit, const char *name)
{
    sModbusPointRecord p;
    memset(&p, 0, sizeof(p));
    p.decodeType   = decodeType;
    p.flags        = MB_PT_READ;
    p.functionCode = fc;
    p.addr         = addr;
    p.scalePow10   = scalePow10;
    p.unit         = unit;
    if (name) {
        strncpy(p.name, name, MB_POINT_NAME_LEN - 1);
    }
    return p;
}

static inline void ts_point(sTestStream *s, const sModbusPointRecord *p)
{
    ts_append(s, p, sizeof(*p));
}

/* ---- devices ----------------------------------------------------------- */

static inline void ts_device(sTestStream *s, uint8_t slaveAddr, uint16_t capId,
                             const char *topicPrefix)
{
    sModbusDeviceRecord d;
    memset(&d, 0, sizeof(d));
    d.slaveAddr = slaveAddr;
    d.capId     = capId;
    if (topicPrefix) {
        strncpy(d.topicPrefix, topicPrefix, MB_TOPIC_PREFIX_LEN - 1);
    }
    ts_append(s, &d, sizeof(d));
}

/* ---- plans ------------------------------------------------------------- */

static inline void ts_plan(sTestStream *s, uint8_t planId, const char *name,
                           uint16_t capId, uint8_t devices)
{
    sModbusPlanRecord p;
    memset(&p, 0, sizeof(p));
    if (name) {
        strncpy(p.name, name, MB_NAME_LEN - 1);
    }
    p.capId   = capId;
    p.planId  = planId;
    p.devices = devices;
    ts_append(s, &p, sizeof(p));
}

static inline void ts_time_table(sTestStream *s, uint32_t period_sec,
                                 const uint16_t *ids, uint16_t count)
{
    sModbusTimeTableRecord t;
    memset(&t, 0, sizeof(t));
    t.period_sec = period_sec;
    t.entryCount = count;
    ts_append(s, &t, sizeof(t));
    ts_append(s, ids, (uint32_t)count * sizeof(uint16_t));
}

/* ---- sentinels: full-size all-zero records ----------------------------- */

#define TS_END(fn, type)                          \
    static inline void fn(sTestStream *s)         \
    {                                             \
        type r;                                   \
        memset(&r, 0, sizeof(r));                 \
        ts_append(s, &r, sizeof(r));              \
    }

TS_END(ts_end_points,      sModbusPointRecord)
TS_END(ts_end_capabilities, sModbusCapabilityRecord)
TS_END(ts_end_devices,     sModbusDeviceRecord)
TS_END(ts_end_time_tables, sModbusTimeTableRecord)
TS_END(ts_end_plans,       sModbusPlanRecord)

/* Program a finished stream + valid header into a LUT region of the mock. */
static inline void ts_write_region(const sTestStream *s, uint32_t base)
{
    for (uint32_t off = 0; off < EXT_FLASH_MODBUS_LUT_SIZE;
         off += W25Q128_SECTOR_SIZE) {
        W25Q128_EraseSector(base + off);
    }

    for (uint32_t off = 0; off < s->len; off += 256u) {
        uint32_t n = s->len - off;
        if (n > 256u) {
            n = 256u;
        }
        W25Q128_WritePage(base + MODBUS_LUT_HEADER_SIZE + off,
                          &s->buf[off], n);
    }

    sModbusLutHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic     = MODBUS_LUT_MAGIC;
    hdr.version   = MODBUS_LUT_VERSION;
    hdr.streamLen = s->len;
    hdr.crc32     = ImgMgmt_Crc32(s->buf, s->len);
    W25Q128_WritePage(base, (const uint8_t *)&hdr, sizeof(hdr));
}

/* DLMS unit codes used by the worked example (values per modbus_units.c). */
#define TS_UNIT_V    35u
#define TS_UNIT_A    33u
#define TS_UNIT_W    27u
#define TS_UNIT_PCT  56u

/**
 * The §6 worked example, v2: two capabilities, three devices (two of them
 * SHARING one capability, which is the whole point of the split), and two
 * plans over the shared capability — one fast over both devices, one lazy over
 * the spare.  Capability 0 has one writable point.
 */
static inline void ts_build_worked_example(sTestStream *s)
{
    sModbusPointRecord p;
    static const uint16_t fast[]  = { 0, 1, 2 };
    static const uint16_t slow[]  = { 3 };
    static const uint16_t lazy[]  = { 0 };
    static const uint16_t meter[] = { 0 };

    s->len = 0;

    /* capability 0: "solis", stride 1, FC06 writes, one block */
    ts_capability(s, "solis", 1, 6, 125, 1);
    ts_block(s, 3132, 16);
    p = ts_mkpoint(mbDecode_u16, 3132, mbFc_input, -1, TS_UNIT_V,
                   "battery_voltage");
    ts_point(s, &p);
    p = ts_mkpoint(mbDecode_s16, 3133, mbFc_input, -1, TS_UNIT_A,
                   "battery_current");
    ts_point(s, &p);
    p = ts_mkpoint(mbDecode_u16, 3138, mbFc_input, 0, TS_UNIT_PCT,
                   "battery_soc");
    ts_point(s, &p);
    p = ts_mkpoint(mbDecode_u16, 3139, mbFc_holding, 0, TS_UNIT_PCT,
                   "overdischarge_soc_set");
    p.flags    = MB_PT_READ | MB_PT_WRITE | MB_PT_BOUNDED;
    p.writeMin = 5;
    p.writeMax = 40;
    ts_point(s, &p);
    ts_end_points(s);

    /* capability 1: "meter" */
    ts_capability(s, "meter", 1, 6, 125, 1);
    ts_block(s, 0, 8);
    p = ts_mkpoint(mbDecode_float32Be, 0, mbFc_input, 0, TS_UNIT_W, "power");
    ts_point(s, &p);
    ts_end_points(s);
    ts_end_capabilities(s);

    ts_device(s, 1, 0, "periphnet");
    ts_device(s, 3, 0, "spare");
    ts_device(s, 2, 1, "periphnet_meter");
    ts_end_devices(s);

    ts_plan(s, 0, "inv_fast", 0, 0x01);          /* device 0 only */
    ts_time_table(s, 5, fast, 3);
    ts_time_table(s, 60, slow, 1);
    ts_end_time_tables(s);

    ts_plan(s, 2, "inv_lazy", 0, 0x02);          /* slot 2: gaps are legal */
    ts_time_table(s, 300, lazy, 1);
    ts_end_time_tables(s);

    ts_plan(s, 3, "meter_plan", 1, 0x04);
    ts_time_table(s, 10, meter, 1);
    ts_end_time_tables(s);
    ts_end_plans(s);
}

#endif /* MODBUS_TEST_STREAM_H */

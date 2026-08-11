/**
 * Helpers for hand-assembling Modbus config record streams into the NOR
 * flash mock — shared by test_modbus_store.c and test_modbus_compiler.c.
 *
 * The worked-example builder mirrors the SHIPPED config schema, including the
 * publish threshold/heartbeat fields.  docs/modbus.md §4 documents the target
 * schema, which drops them (§2.4) and adds a per-device baud (§2.6); this
 * builder and the record layout change together at §2.16 step 6/8.
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
    uint8_t  buf[4096];
    uint32_t len;
} sTestStream;

static inline void ts_append(sTestStream *s, const void *rec, uint32_t size)
{
    memcpy(&s->buf[s->len], rec, size);
    s->len += size;
}

static inline void ts_device(sTestStream *s, uint8_t slaveAddr,
                             const char *topicPrefix)
{
    sModbusDeviceRecord d;
    memset(&d, 0, sizeof(d));
    d.slaveAddr = slaveAddr;
    if (topicPrefix) {
        strncpy(d.topicPrefix, topicPrefix, MB_TOPIC_PREFIX_LEN - 1);
    }
    ts_append(s, &d, sizeof(d));
}

static inline void ts_txn(sTestStream *s, uint8_t count, uint8_t fc,
                          uint16_t startAddr, uint16_t readPeriodS)
{
    sModbusTransactionRecord t;
    memset(&t, 0, sizeof(t));
    t.count        = count;
    t.functionCode = fc;
    t.startAddr    = startAddr;
    t.readPeriodS  = readPeriodS;
    ts_append(s, &t, sizeof(t));
}

static inline void ts_point(sTestStream *s, const sModbusPointRecord *p)
{
    ts_append(s, p, sizeof(*p));
}

static inline sModbusPointRecord ts_mkpoint(uint8_t decodeType,
                                            uint16_t offset,
                                            int8_t scalePow10, uint8_t unit,
                                            const char *name)
{
    sModbusPointRecord p;
    memset(&p, 0, sizeof(p));
    p.decodeType = decodeType;
    p.offset     = offset;
    p.scalePow10 = scalePow10;
    p.unit       = unit;
    if (name) {
        strncpy(p.name, name, MB_POINT_NAME_LEN - 1);
    }
    return p;
}

/* Sentinels are full-size all-zero records. */
static inline void ts_end_points(sTestStream *s)
{
    sModbusPointRecord p;
    memset(&p, 0, sizeof(p));
    ts_append(s, &p, sizeof(p));
}

static inline void ts_end_txns(sTestStream *s)
{
    sModbusTransactionRecord t;
    memset(&t, 0, sizeof(t));
    ts_append(s, &t, sizeof(t));
}

static inline void ts_end_devices(sTestStream *s)
{
    sModbusDeviceRecord d;
    memset(&d, 0, sizeof(d));
    ts_append(s, &d, sizeof(d));
}

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

/* The design-doc §2 worked example: 2 devices, 2 transactions, 5 points. */
static inline void ts_build_worked_example(sTestStream *s)
{
    sModbusPointRecord p;

    s->len = 0;

    ts_device(s, 1, "periphnet");
    /* count derived: max(offset + width) = 7 + 1 = 8 */
    ts_txn(s, 8, MB_FC_INPUT, 3132, 5);

    p = ts_mkpoint(MB_DECODE_U16, 0, -1, TS_UNIT_V, "battery_voltage");
    ts_point(s, &p);
    p = ts_mkpoint(MB_DECODE_S16, 1, -1, TS_UNIT_A, "battery_current");
    ts_point(s, &p);
    p = ts_mkpoint(MB_DECODE_U16, 6, 0, TS_UNIT_PCT, "battery_soc");
    p.publishThreshold  = 1;
    p.publishHeartbeatS = 300;
    ts_point(s, &p);
    p = ts_mkpoint(MB_DECODE_U16, 7, 0, TS_UNIT_PCT, "overdischarge_soc_set");
    p.flags    = MB_POINT_FLAG_WRITABLE;
    p.writeMin = 5;
    p.writeMax = 40;
    ts_point(s, &p);
    ts_end_points(s);
    ts_end_txns(s);

    ts_device(s, 2, "periphnet_meter");
    ts_txn(s, 2, MB_FC_INPUT, 0, 5);
    p = ts_mkpoint(MB_DECODE_FLOAT32_BE, 0, 0, TS_UNIT_W, "power");
    ts_point(s, &p);
    ts_end_points(s);
    ts_end_txns(s);

    ts_end_devices(s);
}

#endif /* MODBUS_TEST_STREAM_H */

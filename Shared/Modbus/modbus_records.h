/**
 * @file    modbus_records.h
 * @brief   Compiled Modbus register-config record layout (flash-resident).
 *
 * A configuration is a flat, sentinel-terminated stream of packed records
 * living in external flash — no stored pointers or offsets, a linear scan is
 * self-describing:
 *
 *   [Device][Txn][Point]...[Point{decodeType=0}]      <- point sentinel
 *           [Txn][Point]...[Point{decodeType=0}]
 *           [Txn{count=0}]                             <- transaction sentinel
 *   [Device]...
 *   [Device{slaveAddr=0}]                              <- device sentinel (end)
 *
 * Sentinels are full-size all-zero records, so readers always consume whole
 * records. slaveAddr 0 is the Modbus broadcast address (never a real slave),
 * count 0 is not a valid read length and decodeType 0 is reserved — all three
 * are safe end markers.
 *
 * Each 16 KB LUT region starts with a 256-byte header page (sModbusLutHeader,
 * CRC32 over the record stream that follows). The header is written last by
 * the compiler, so a torn upload never yields a valid region.
 *
 * Design: docs/modbus.md §3.2.
 */
#ifndef MODBUS_RECORDS_H_
#define MODBUS_RECORDS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Enums / flags
 * ========================================================================== */

typedef enum {
    MB_DECODE_END        = 0,   /* sentinel: end of this transaction's points */
    MB_DECODE_U16        = 1,
    MB_DECODE_S16        = 2,
    MB_DECODE_U32_BE     = 3,
    MB_DECODE_U32_LE     = 4,
    MB_DECODE_S32_BE     = 5,
    MB_DECODE_S32_LE     = 6,
    MB_DECODE_FLOAT32_BE = 7,
    MB_DECODE_FLOAT32_LE = 8,
    MB_DECODE_BITFIELD   = 9,
    MB_DECODE_ASCII      = 10,
} eModbusDecodeType;

typedef enum {
    MB_FC_HOLDING = 3,
    MB_FC_INPUT   = 4,
} eModbusFunctionCode;

#define MB_POINT_FLAG_WRITABLE   (1u << 0)

/* ==========================================================================
 * Records (all packed; sizes are part of the on-flash format)
 * ========================================================================== */

/* Point names must fit the existing Solis MQTT suffixes (longest is 21
 * chars, "overdischarge_soc_set") — the design doc's name[16] was too small
 * for its own worked example. */
#define MB_POINT_NAME_LEN        24    /* incl. NUL — max 23 usable chars */
#define MB_TOPIC_PREFIX_LEN      16    /* incl. NUL — max 15 usable chars */

typedef struct __attribute__((packed)) {
    uint8_t  decodeType;         /* eModbusDecodeType; 0 = end-of-points     */
    uint8_t  flags;              /* MB_POINT_FLAG_*; bits 1-7 reserved       */
    uint8_t  length;             /* ASCII register length; unused otherwise  */
    uint16_t offset;             /* relative to enclosing txn's startAddr    */
    int8_t   scalePow10;         /* value = raw * 10^scalePow10              */
    uint8_t  unit;               /* DLMS/COSEM physical-unit code            */
    uint16_t publishThreshold;   /* scaled-int magnitude; 0 = always publish */
    uint16_t publishHeartbeatS;  /* 0 = no forced republish                  */
    int16_t  writeMin;           /* scaled-int domain; ignored if !writable  */
    int16_t  writeMax;
    char     name[MB_POINT_NAME_LEN];  /* MQTT topic suffix, NUL-terminated  */
} sModbusPointRecord;            /* 39 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  count;              /* register block length; 0 = end-of-txns.
                                    Compiler-derived, never authored.        */
    uint8_t  functionCode;       /* eModbusFunctionCode                      */
    uint16_t startAddr;          /* wire register address                    */
    uint16_t readPeriodS;        /* poll interval, seconds                   */
} sModbusTransactionRecord;      /* 6 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;          /* 1-247; 0 = end-of-devices sentinel       */
    char     topicPrefix[MB_TOPIC_PREFIX_LEN];  /* MQTT ns + HA device name  */
} sModbusDeviceRecord;           /* 17 bytes */

_Static_assert(sizeof(sModbusPointRecord) == 39, "point record size");
_Static_assert(sizeof(sModbusTransactionRecord) == 6, "txn record size");
_Static_assert(sizeof(sModbusDeviceRecord) == 17, "device record size");

/* ==========================================================================
 * Region header (first page of each LUT region; stream starts after it)
 * ========================================================================== */

#define MODBUS_LUT_VERSION       1u
#define MODBUS_LUT_HEADER_SIZE   256u  /* one flash page reserved for header */

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* MODBUS_LUT_MAGIC "MBCF"                  */
    uint32_t version;            /* MODBUS_LUT_VERSION                       */
    uint32_t streamLen;          /* record stream length, bytes              */
    uint32_t crc32;              /* CRC32 over the record stream             */
} sModbusLutHeader;              /* 16 bytes */

/* ==========================================================================
 * Compiler-enforced bounds (validation limits, NOT RAM array dimensions —
 * except in the walker's fixed per-ordinal state arrays, design §8)
 * ========================================================================== */

#define MB_MAX_DEVICES           8
#define MB_MAX_TXNS_TOTAL        64
#define MB_MAX_POINTS_TOTAL      192
#define MB_MAX_TXNS_PER_DEVICE   16    /* soft cap, validation only */
#define MB_MAX_POINTS_PER_TXN    24    /* soft cap, validation only */
#define MB_MAX_REGS_PER_TXN      125   /* Modbus FC03/04 protocol ceiling */

/* Register width a point occupies on the wire; 0 = unknown decode type.
 * ASCII points carry their width in .length (author-specified). */
static inline uint8_t MbRecords_RegWidth(uint8_t decodeType, uint8_t asciiLen)
{
    switch (decodeType) {
    case MB_DECODE_U16:
    case MB_DECODE_S16:
    case MB_DECODE_BITFIELD:
        return 1;
    case MB_DECODE_U32_BE:
    case MB_DECODE_U32_LE:
    case MB_DECODE_S32_BE:
    case MB_DECODE_S32_LE:
    case MB_DECODE_FLOAT32_BE:
    case MB_DECODE_FLOAT32_LE:
        return 2;
    case MB_DECODE_ASCII:
        return asciiLen;
    default:
        return 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_RECORDS_H_ */

/**
 * @file    modbus_records.h
 * @brief   Compiled Modbus register-config record layout (flash-resident), v2.
 *
 * A configuration is a flat, sentinel-terminated stream of packed records in
 * external flash — no stored pointers or offsets, so a linear scan is
 * self-describing and nothing has to be RAM-resident:
 *
 *   [Header]
 *   [Capability][Block x blockCount][Point]...[Point{decodeType=0}]  <- points
 *   [Capability]...
 *   [Capability{name[0]=0}]                        <- capability sentinel
 *   [Device]...[Device{slaveAddr=0}]               <- device sentinel
 *   [Plan][TimeTable][pointId x entryCount]...
 *         [TimeTable{entryCount=0}]                <- time-table sentinel
 *   [Plan]...
 *   [Plan{name[0]=0}]                              <- plan sentinel (end)
 *
 * PLANS ARE LAST because they reference both capabilities and devices, and a
 * single pass can only check what precedes it (docs/modbus.md §7.2).  It also
 * puts the one editable section at the end, where a rewrite does least work.
 *
 * Sentinels are full-size all-zero records, so readers always consume whole
 * records.  slaveAddr 0 is the Modbus broadcast address (never a real slave),
 * decodeType 0 is reserved, and name[0] == 0 cannot be a legal display name.
 *
 * NO RECORD STORES ITS OWN ID, except a plan's.  Ids are dense ordinals equal
 * to array position, and a linear scan already knows the position of what it
 * is reading; the "id" in the JSON is an authoring assertion the compiler
 * checks, not a field it stores.  A planId is a SLOT rather than a position
 * (§3.5) — precisely so deleting a plan moves no other — so it is written
 * down, and a free slot is simply absent from the stream.
 *
 * Each 16 KB LUT region starts with a 256-byte header page (sModbusLutHeader,
 * CRC32 over the record stream that follows).  The header is written last by
 * the compiler, so a torn upload never yields a valid region.
 *
 * Design: docs/modbus.md §7.
 */
#ifndef MODBUS_RECORDS_H_
#define MODBUS_RECORDS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Enums
 *
 * ENUM VALUES MUST NEVER BE RENUMBERED: eModbusDecodeType is persisted in the
 * LUT, and eModbusPortId / eModbusLineFormat / the baud table are persisted on
 * the device record.  Appending is safe, prepending is not — which is why none
 * of them has a _undefined = 0.
 * ========================================================================== */

typedef enum {
    mbDecode_end        = 0,   /* sentinel: end of this capability's points */
    mbDecode_u16        = 1,
    mbDecode_s16        = 2,
    mbDecode_u32Be      = 3,
    mbDecode_u32Le      = 4,
    mbDecode_s32Be      = 5,
    mbDecode_s32Le      = 6,
    mbDecode_float32Be  = 7,
    mbDecode_float32Le  = 8,
    mbDecode_bitfield   = 9,
    mbDecode_ascii      = 10,
    mbDecode_last               /* sentinel — not a stored value */
} eModbusDecodeType;

typedef enum {
    mbFc_holding = 3,
    mbFc_input   = 4,
} eModbusFunctionCode;

/* A port is an enum, not an authored record: portId indexes the module's port
 * table, one slot per peripheral (docs/modbus.md §3.4).  It lives here because
 * the compiler, the exporter and whoever registers a driver all need the
 * name<->code table — but NOTHING OUTSIDE THE MODULE CHOOSES A PORT FOR A
 * DEVICE.  A port with no driver registered is disabled, structurally: there
 * is no mbPort_disabled. */
typedef enum {
    mbPort_rs485 = 0,
    mbPort_test  = 1,
    mbPort_last                 /* sentinel — not a stored value */
} eModbusPortId;

/* Line format is a DEVICE parameter: a slave's parity is a fact about that
 * slave (§3.4).  8E1 is RTU's spec default, so a config that can say 9600 but
 * not 8E1 supports the common variant and refuses the conformant one. */
typedef enum {
    mbFmt_8N1 = 0,              /* the widespread non-conformant variant */
    mbFmt_8E1 = 1,              /* the RTU spec default                  */
    mbFmt_8O1 = 2,
    mbFmt_8N2 = 3,
    mbFmt_last                  /* sentinel — not a stored value */
} eModbusLineFormat;

/* Point access, as the CAPABILITY declares it — what the silicon supports, not
 * what this deployment does with it (§3.2).  MB_PT_BOUNDED says the author
 * wrote writeMin/writeMax; absent bounds mean UNCONSTRAINED, not forbidden,
 * which is why it is a flag and not a magic pair of numbers (§4.6). */
#define MB_PT_READ      (1u << 0)
#define MB_PT_WRITE     (1u << 1)
#define MB_PT_BOUNDED   (1u << 2)

/* ==========================================================================
 * Records (all packed; sizes are part of the on-flash format)
 * ========================================================================== */

#define MB_POINT_NAME_LEN        24    /* incl. NUL — max 23 usable chars */
#define MB_NAME_LEN              16    /* capability / plan / topicPrefix */
#define MB_TOPIC_PREFIX_LEN      MB_NAME_LEN

/* What the hardware CAN DO: its dialect, its blocks and a flat list of points.
 * It describes the silicon and never varies, so it contains no periods — how
 * often to read a register is a fact about this deployment, and lives on a
 * plan (§3.1). */
typedef struct __attribute__((packed)) {
    char     name[MB_NAME_LEN];  /* display only; name[0] == 0 = end-of-caps */
    uint8_t  addrStride;         /* address units per register (§3.3); 0 = 1 */
    uint8_t  writeFc;            /* 6 = FC06, 16 = FC16 — a dialect fact     */
    uint16_t maxReadRegs;        /* per-request quantity cap; 0 = 125        */
    uint8_t  blockCount;         /* sModbusBlockRecord x blockCount follow   */
    uint8_t  reserved;
} sModbusCapabilityRecord;       /* 22 bytes */

/* An address range the slave implements.  A derived read may span anything
 * INSIDE one block, including addresses no point selects, but never two:
 * bridging an unselected address costs two bytes per register against a whole
 * extra round trip, while reaching outside a block risks the slave rejecting
 * the entire read with exception 2.  `regs` is also the read-span limit the
 * slave enforces (the JK's "quantity + wordOffset < 147"). */
typedef struct __attribute__((packed)) {
    uint16_t base;               /* first wire address of the block          */
    uint16_t regs;               /* length in REGISTERS, not address units   */
} sModbusBlockRecord;            /* 4 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  decodeType;         /* eModbusDecodeType; 0 = end-of-points     */
    uint8_t  flags;              /* MB_PT_READ|WRITE|BOUNDED                 */
    uint8_t  functionCode;       /* holding(3) | input(4) — per point, since
                                    they are different address spaces        */
    uint8_t  length;             /* ASCII register length; unused otherwise  */
    uint16_t addr;               /* wire address, VERBATIM as authored: the
                                    stride is applied by the engine, never
                                    baked in here (§3.3)                     */
    int8_t   scalePow10;         /* value = scaled * 10^scalePow10           */
    uint8_t  unit;               /* DLMS/COSEM physical-unit code            */
    int32_t  writeMin;           /* scaled-int domain; valid if MB_PT_BOUNDED*/
    int32_t  writeMax;
    char     name[MB_POINT_NAME_LEN];  /* MQTT topic suffix, NUL-terminated  */
} sModbusPointRecord;            /* 40 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;          /* 1-247; 0 = end-of-devices sentinel       */
    uint8_t  baudCode;           /* rate-table index; 0 = 9600               */
    uint8_t  portId;             /* eModbusPortId                            */
    uint8_t  format;             /* eModbusLineFormat; 0 = 8N1               */
    uint16_t capId;              /* the capability this slave implements     */
    char     topicPrefix[MB_TOPIC_PREFIX_LEN];   /* MQTT ns + HA device name */
} sModbusDeviceRecord;           /* 22 bytes */

/* What we WATCH: a capability, a set of its devices, and time tables.  Plans
 * are the one config object the system may change about itself while it runs
 * (§3.5), which is why a planId is a stored SLOT and free slots have no
 * record at all — a blank entry would collide with the name[0] sentinel and
 * truncate the section at the first hole. */
typedef struct __attribute__((packed)) {
    char     name[MB_NAME_LEN];  /* name[0] == 0 = end-of-plans              */
    uint16_t capId;              /* the capability its point ids index into  */
    uint8_t  planId;             /* SLOT 0..7 — stored, unlike every other id*/
    uint8_t  devices;            /* device set, one bit per deviceId         */
} sModbusPlanRecord;             /* 20 bytes */

typedef struct __attribute__((packed)) {
    uint32_t period_sec;         /* >=1; uint32 so 24 h is expressible       */
    uint16_t entryCount;         /* 0 = end-of-time-tables; uint16 pointIds
                                    follow this record, entryCount of them   */
} sModbusTimeTableRecord;        /* 6 bytes */

_Static_assert(sizeof(sModbusCapabilityRecord) == 22, "capability record size");
_Static_assert(sizeof(sModbusBlockRecord) == 4,       "block record size");
_Static_assert(sizeof(sModbusPointRecord) == 40,      "point record size");
_Static_assert(sizeof(sModbusDeviceRecord) == 22,     "device record size");
_Static_assert(sizeof(sModbusPlanRecord) == 20,       "plan record size");
_Static_assert(sizeof(sModbusTimeTableRecord) == 6,   "time table record size");

/* ==========================================================================
 * Region header (first page of each LUT region; stream starts after it)
 * ========================================================================== */

#define MODBUS_LUT_VERSION       2u
#define MODBUS_LUT_HEADER_SIZE   256u  /* one flash page reserved for header */

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* MODBUS_LUT_MAGIC "MBCF"                  */
    uint32_t version;            /* MODBUS_LUT_VERSION                       */
    uint32_t streamLen;          /* record stream length, bytes              */
    uint32_t crc32;              /* CRC32 over the record stream             */
} sModbusLutHeader;              /* 16 bytes */

/* ==========================================================================
 * Compiler-enforced bounds (docs/modbus.md §6)
 *
 * The caps count DISTINCT RECORDS, not instances: four packs on a 50-point
 * capability cost 50 points, not 200.  The point ceiling is a FLASH limit — a
 * point costs its 40 bytes of stream and nothing in RAM — and the real ceiling
 * is the 16 KB region, which the compiler reports as a region-full failure.
 * Two counts are pinned by masks instead: plans at 8 by the uint8_t
 * subscription mask, devices at 8 by the uint8_t device set on the plan
 * record.
 * ========================================================================== */

#define MB_MAX_DEVICES              8
#define MB_MAX_CAPABILITIES         8
#define MB_MAX_PLANS                8    /* planMask is a uint8_t */
#define MB_MAX_POINTS_TOTAL         384
#define MB_MAX_TT_ENTRIES_TOTAL     384
#define MB_MAX_TT_ENTRIES_PER_TABLE 64   /* one derivation's working set */
#define MB_MAX_TIME_TABLES_PER_PLAN 8
#define MB_MAX_BLOCKS_PER_CAP       8
#define MB_MAX_READ_BLOCKS_PER_DEV  64
#define MB_MAX_REGS_PER_READ        125  /* Modbus FC03/04 protocol ceiling */

/* ==========================================================================
 * Config data types crossing the module boundary (docs/modbus.md §4.9)
 *
 * These are DATA, so consumers may name them; the flash accessors that
 * produce them (MbCfg_*, MbCfgStore_*, MbCfgCompile) stay behind
 * App/Modbus/modbus.h and consumers must not call them.
 * ========================================================================== */

/** Byte source for a streaming upload: >0 = bytes read, 0 = EOF, <0 = error */
typedef int (*fModbusByteSource)(void *ctx, uint8_t *buf, uint32_t maxLen);

/** Byte sink for a streaming export: 0 = continue, <0 = abort */
typedef int (*fModbusByteSink)(void *ctx, const char *data, uint32_t len);

typedef struct {
    uint8_t  capabilities;
    uint8_t  devices;
    uint8_t  plans;
    uint16_t points;
} sModbusConfigCounts;

typedef struct {
    int      ok;                 /* 1 = compiled, region marked valid       */
    int      capIdx;             /* first failure location; -1 = n/a        */
    int      devIdx;
    int      planIdx;
    int      subIdx;             /* point / time table / block within it    */
    char     field[24];          /* offending key, or "json" for syntax     */
    char     reason[64];
    sModbusConfigCounts counts;  /* what compiled before the failure        */
} sModbusCompileResult;

/* ==========================================================================
 * Plan specification (docs/modbus.md §4.3)
 *
 * What a plan edit CARRIES.  Plans are the one config object the system may
 * change about itself while it runs, so this shape is used by both the API and
 * the record→record rewrite — and validated against the same counts the
 * compiler checks an uploaded plan against, so a plan built through the API is
 * exactly as validated as one that arrived in an upload (§7.4).
 * ========================================================================== */

typedef struct {
    uint32_t        period_sec;
    const uint16_t *points;      /* pointIds into the plan's capability      */
    uint16_t        count;
} sModbusTimeTableSpec;

typedef struct {
    const char *name;
    const sModbusTimeTableSpec *tables;
    uint16_t    capId;
    uint8_t     tableCount;
    uint8_t     devices;         /* device set; must lie within capId        */
} sModbusPlanSpec;

/* ==========================================================================
 * Derived facts — pure functions of the records above, so they live here and
 * are host-tested (docs/modbus.md §2.2).  Nothing derived is ever STORED: two
 * things that can disagree is the hazard being avoided.
 * ========================================================================== */

/* Register width a point occupies on the wire; 0 = unknown decode type.
 * ASCII points carry their width in .length (author-specified). */
static inline uint8_t MbRecords_RegWidth(uint8_t decodeType, uint8_t asciiLen)
{
    switch (decodeType) {
    case mbDecode_u16:
    case mbDecode_s16:
    case mbDecode_bitfield:
        return 1;
    case mbDecode_u32Be:
    case mbDecode_u32Le:
    case mbDecode_s32Be:
    case mbDecode_s32Le:
    case mbDecode_float32Be:
    case mbDecode_float32Le:
        return 2;
    case mbDecode_ascii:
        return asciiLen;
    default:
        return 0;
    }
}

/* baudCode 0 is 9600 (the default, so an omitted key costs no special case);
 * the remaining entries are in ascending rate order.  APPEND-ONLY: a new rate
 * is appended, never inserted, because the code is persisted. */
static inline uint32_t MbRecords_BaudFromCode(uint8_t code)
{
    static const uint32_t rates[] = {
        9600u, 1200u, 2400u, 4800u, 19200u, 38400u, 57600u, 115200u
    };
    return (code < (sizeof(rates) / sizeof(rates[0]))) ? rates[code] : 9600u;
}

/* -1 if the rate is not in the table. */
static inline int MbRecords_CodeFromBaud(uint32_t baud)
{
    for (uint8_t c = 0; c < 8u; c++) {
        if (MbRecords_BaudFromCode(c) == baud) {
            return (int)c;
        }
    }
    return -1;
}

/* addrStride is a DIVISOR, not a multiplier (§3.3): JK registers are byte-
 * addressed, so consecutive registers sit at 0x1400, 0x1402, 0x1404 where a
 * standard slave puts them at 3000, 3001, 3002.  Registers are still 16-bit
 * and the wire's quantity field is still a register count — only the address
 * units differ.  Getting the domains backwards is silent: a 16-register JK
 * block spans an address delta of 32, so a contiguity test or a quantity
 * computed on raw addresses is wrong by exactly addrStride and still looks
 * plausible. */
static inline uint8_t MbRecords_Stride(const sModbusCapabilityRecord *cap)
{
    return (cap->addrStride == 0u) ? 1u : cap->addrStride;
}

static inline uint16_t MbRecords_MaxReadRegs(const sModbusCapabilityRecord *cap)
{
    return (cap->maxReadRegs == 0u) ? MB_MAX_REGS_PER_READ : cap->maxReadRegs;
}

/* Registers between two wire addresses of the same capability. */
static inline uint16_t MbRecords_RegSpan(const sModbusCapabilityRecord *cap,
                                         uint16_t fromAddr, uint16_t toAddr)
{
    return (uint16_t)((uint16_t)(toAddr - fromAddr) / MbRecords_Stride(cap));
}

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_RECORDS_H_ */

/**
 * @file    modbus_config_store.h
 * @brief   Modbus config flash store: A/B region selector + record cursor.
 *
 * Two 16 KB LUT regions hold compiled record streams (modbus_records.h).
 * The 4 KB selector sector says which region is active, following the
 * boot_status NOR pattern: a CRC-protected header plus a flags word OUTSIDE
 * the CRC so single bits (swap_pending) can be cleared without a sector
 * erase.
 *
 * Invariant: a region is never erased while it may still be walked — the
 * retiring region is only overwritten by the NEXT upload, so a reader that
 * races a swap still sees coherent (old) data.
 *
 * Shared-layer module: depends only on libc + the W25Q128 driver, so it is
 * host-testable over the NOR-faithful flash mock.
 */
#ifndef MODBUS_CONFIG_STORE_H_
#define MODBUS_CONFIG_STORE_H_

#include "modbus_records.h"
#include "bl_app_contract.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Selector sector layout
 * ========================================================================== */

#define MODBUS_SEL_VERSION  1u

typedef union {
    uint32_t word;                       /* erased = 0xFFFFFFFF             */
    struct __attribute__((packed)) {
        uint32_t swap_pending : 1;       /* 0 = apply requested             */
        uint32_t _reserved    : 31;
    } bits;
} sMbSelFlags;

typedef struct {
    uint32_t    magic;                   /* MODBUS_SEL_MAGIC "MBSL"         */
    uint32_t    version;                 /* MODBUS_SEL_VERSION              */
    uint32_t    activeRegion;            /* 0 = region A, 1 = region B      */
    uint32_t    header_crc32;            /* CRC32(magic..activeRegion)      */
    sMbSelFlags flags;                   /* outside CRC — bit-clearable!    */
} sMbSelector;

/* ==========================================================================
 * Selector / region management
 * ========================================================================== */

/* Validate the selector, recovering from a blank/corrupt sector (e.g. a
 * power cut during CommitSwap): prefer whichever region holds a valid
 * config. Idempotent; call before any other store function. */
int      MbCfgStore_Init(void);

uint32_t MbCfgStore_ActiveBase(void);
uint32_t MbCfgStore_InactiveBase(void);

/* Header magic/version/streamLen sanity + CRC32 over the record stream. */
bool     MbCfgStore_RegionValid(uint32_t base);

/* Arm the swap flag (NOR bit-clear, no erase). Refuses (-1) if the inactive
 * region does not hold a valid config. */
int      MbCfgStore_SetSwapPending(void);
bool     MbCfgStore_IsSwapPending(void);

/* Flip the active region and clear the pending flag (sector erase+rewrite).
 * Called by the walker at a lap boundary only. Never erases LUT regions. */
int      MbCfgStore_CommitSwap(void);

/* ==========================================================================
 * Record cursor — sequential walk of a region's record stream
 *
 * Call discipline: NextDevice, then per device loop NextTransaction, then
 * per transaction loop NextPoint until it returns 0. Points must always be
 * drained before the next NextTransaction call (the cursor is a plain
 * offset — records are not indexed).
 *
 * Return convention: 1 = record read, 0 = sentinel consumed (end of that
 * level), -1 = malformed stream / read error.
 * ========================================================================== */

typedef struct {
    uint32_t base;               /* region base address                     */
    uint32_t off;                /* current offset into the record stream   */
    uint32_t streamLen;          /* from the region header                  */
} sMbCfgCursor;

int MbCfg_Open(uint32_t base, sMbCfgCursor *c);   /* -1 if header invalid */
int MbCfg_NextDevice(sMbCfgCursor *c, sModbusDeviceRecord *d);
int MbCfg_NextTransaction(sMbCfgCursor *c, sModbusTransactionRecord *t);
int MbCfg_NextPoint(sMbCfgCursor *c, sModbusPointRecord *p);

/* ==========================================================================
 * Whole-region helpers
 * ========================================================================== */

typedef struct {
    uint8_t  devices;
    uint8_t  transactions;
    uint16_t points;
} sMbCfgCounts;

/* Full structural walk; -1 if the stream is malformed. */
int MbCfg_Count(uint32_t base, sMbCfgCounts *out);

typedef struct {
    uint8_t            slaveAddr;
    uint16_t           regAddr;   /* absolute: txn.startAddr + point.offset */
    sModbusPointRecord point;
} sMbPointLookup;

/* Find a writable point by device topicPrefix + point name in the ACTIVE
 * region (MQTT set-topic resolution). 0 = found, -1 = no match. */
int MbCfg_FindWritablePoint(const char *topicPrefix, const char *name,
                            sMbPointLookup *out);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONFIG_STORE_H_ */

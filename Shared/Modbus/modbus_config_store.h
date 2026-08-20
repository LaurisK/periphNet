/**
 * @file    modbus_config_store.h
 * @brief   Modbus config flash store: A/B region selector + record cursor.
 *
 * Two LUT regions hold compiled record streams (modbus_records.h).  A third
 * area, the selector, says which of them is active, following the boot_status
 * NOR pattern: a CRC-protected header plus a flags word OUTSIDE the CRC so
 * single bits (swap_pending) can be cleared without an erase — which nvDb
 * preserves, because a write it can program onto what is already there is
 * programmed in place.
 *
 * All three are nvDb users (nvdbUser_modbusLutA / LutB / modbusSelector).
 * A "region" here is therefore a user id, not an address: this module asks
 * nvDb for bytes at an offset and has no idea where they live.
 *
 * Invariant: a region is never erased while it may still be walked — the
 * retiring region is only overwritten by the NEXT upload or plan edit, so a
 * reader that races a swap still sees coherent (old) data.  The corollary is
 * what motivates verify (docs/modbus.md §4.9): the retiring region IS the
 * previous config, and any upload destroys it.
 *
 * Shared-layer module: depends only on libc + the W25Q128 driver, so it is
 * host-testable over the NOR-faithful flash mock.
 */
#ifndef MODBUS_CONFIG_STORE_H_
#define MODBUS_CONFIG_STORE_H_

#include "modbus_records.h"
#include "bl_app_contract.h"
#include "nvdb.h"

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

/* Which nvDb user holds the config in force, and which one an upload may
 * consume.  They swap; neither moves. */
eNvDbUser MbCfgStore_ActiveRegion(void);
eNvDbUser MbCfgStore_InactiveRegion(void);

/* Header magic/version/streamLen sanity + CRC32 over the record stream.
 * The version test is strict equality, which is what makes the v1 -> v2 move
 * a wipe rather than a migration (docs/modbus.md §11.2). */
bool     MbCfgStore_RegionValid(eNvDbUser region);

/* Erase a region's header page so it stops validating.  "Invalid is erased,
 * not repaired" (§4.2): one observable state instead of a spectrum of
 * partially-readable ones, and the region is immediately reusable. */
int      MbCfgStore_EraseRegion(eNvDbUser region);

/* Arm the swap flag (NOR bit-clear, no erase). Refuses (-1) if the inactive
 * region does not hold a valid config. */
int      MbCfgStore_SetSwapPending(void);
bool     MbCfgStore_IsSwapPending(void);

/* Flip the active region and clear the pending flag (sector erase+rewrite).
 * Called by the engine at a safe point only. Never erases LUT regions. */
int      MbCfgStore_CommitSwap(void);

/* Disarm a pending swap WITHOUT flipping the active region (sector
 * erase+rewrite, same cost as a commit).
 *
 * The invariant the engine relies on is "pending implies something valid to
 * swap to": it commits only when the inactive region validates, so a pending
 * flag left armed over an invalid region is never consumed and never clears.
 * Because the flag also refuses every compile, that state is a permanent
 * lockout of the config plane, persisted in flash across reboots.  Erasing a
 * region while a swap is armed is exactly how a board gets there, so whoever
 * invalidates the inactive region must disarm the flag in the same breath. */
int      MbCfgStore_ClearSwapPending(void);

/* ==========================================================================
 * Record cursor — sequential walk of a region's record stream
 *
 * Call discipline follows the stream's own order (modbus_records.h): for each
 * capability, read its blocks, then drain its points; then devices; then, per
 * plan, its time tables, each followed by its point-id array.  Records must
 * always be drained before moving to the next level — the cursor is a plain
 * offset, records are not indexed.
 *
 * Return convention: 1 = record read, 0 = sentinel consumed (end of that
 * level), -1 = malformed stream / read error.
 * ========================================================================== */

typedef struct {
    uint32_t  off;               /* current offset into the record stream   */
    uint32_t  streamLen;         /* from the region header                  */
    eNvDbUser region;            /* which nvDb user holds this stream       */
} sMbCfgCursor;

int MbCfg_Open(eNvDbUser region, sMbCfgCursor *c);  /* -1 if header invalid */

int MbCfg_NextCapability(sMbCfgCursor *c, sModbusCapabilityRecord *cap);
int MbCfg_NextPoint(sMbCfgCursor *c, sModbusPointRecord *p);
int MbCfg_NextDevice(sMbCfgCursor *c, sModbusDeviceRecord *d);
int MbCfg_NextPlan(sMbCfgCursor *c, sModbusPlanRecord *p);
int MbCfg_NextTimeTable(sMbCfgCursor *c, sModbusTimeTableRecord *t);

/* Payload arrays, not records: nothing refers to "block 2 of capability 1" or
 * to a time table's third id, so they are plain arrays behind a count on the
 * record that owns them (§7.2).  Pass out = NULL to skip. 0 = ok, -1 = error. */
int MbCfg_ReadBlocks(sMbCfgCursor *c, sModbusBlockRecord *out, uint8_t count);
int MbCfg_ReadPointIds(sMbCfgCursor *c, uint16_t *out, uint16_t count);

/* ==========================================================================
 * Whole-region helpers
 * ========================================================================== */

/* Full structural walk; -1 if the stream is malformed. */
int MbCfg_Count(eNvDbUser region, sModbusConfigCounts *out);

/* Position a cursor at the first device / first plan record. */
int MbCfg_SeekDevices(eNvDbUser region, sMbCfgCursor *c);
int MbCfg_SeekPlans(eNvDbUser region, sMbCfgCursor *c);

/* Load one capability and its blocks, leaving the cursor at its first point.
 * `blocks` may be NULL; it is filled with at most maxBlocks entries.
 * 0 = found, -1 = no such capability / malformed. */
int MbCfg_OpenCapability(eNvDbUser region, uint16_t capId, sMbCfgCursor *c,
                         sModbusCapabilityRecord *cap,
                         sModbusBlockRecord *blocks, uint8_t maxBlocks);

int MbCfg_FindDevice(eNvDbUser region, uint8_t devOrd, sModbusDeviceRecord *out);

/* One point of one capability, by its dense ordinal. */
int MbCfg_FindPoint(eNvDbUser region, uint16_t capId, uint16_t ptOrd,
                    sModbusPointRecord *out);

/* Plan headers are the ONE deliberate exception to "no record is RAM-resident"
 * (§3.5): ~160 B for all 8 slots, so PlanList/PlanGet can be synchronous and a
 * critical section can answer "is this plan active" without touching flash. */
typedef struct {
    sModbusPlanRecord rec;
    uint8_t           tableCount;
    uint8_t           used;      /* 0 = free slot: no record in the stream  */
} sMbPlanHeader;

/* Fills out[0..MB_MAX_PLANS-1] by slot, marking the free ones.
 * Returns the number of plans present, or -1. */
int MbCfg_ReadPlanHeaders(eNvDbUser region, sMbPlanHeader *out);

typedef struct {
    uint8_t            slaveAddr;
    uint8_t            functionCode;
    uint16_t           regAddr;   /* the point's wire address, VERBATIM —
                                     a write needs no stride arithmetic     */
    uint16_t           capId;
    uint8_t            addrStride;
    uint8_t            writeFc;   /* the capability's dialect (§3.3)        */
    uint8_t            portId;    /* where the device lives — config, never
                                     an API choice (§3.4)                   */
    uint8_t            baudCode;
    uint8_t            format;
    sModbusPointRecord point;
} sMbPointLookup;

/* There is no lookup BY NAME: a reading is addressed by {devOrd, ptOrd} and
 * nothing else (docs/modbus.md §4.10).  A consumer that starts from a string
 * — an MQTT set-topic, a CLI argument — resolves it against the catalogue it
 * was given, once, in the consumer, and never on a data path. */

/* Resolve {devOrd, ptOrd} in the ACTIVE region — the ordinal addressing the
 * module's API uses.  ptOrd indexes the DEVICE'S CAPABILITY, so several
 * devices sharing one capability answer the same ptOrd with the same point
 * and different slave addresses (§4.4).
 *
 * 0 = found, -1 = no such device or point. */
int MbCfg_ResolvePoint(uint8_t devOrd, uint16_t ptOrd, sMbPointLookup *out);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONFIG_STORE_H_ */

/**
 * @file    modbus_internal.h
 * @brief   Hooks between the module's own translation units.
 *
 * MODULE-INTERNAL (docs/modbus.md §2.2): nothing outside App/Modbus may
 * include this header.  Consumers include App/Modbus/modbus.h.
 *
 * Everything declared here runs on the MODBUS TASK.  The split it expresses is
 * the one §10 is sequenced around: modbus.c owns the API's semantics
 * (requests, and from step 2 the subscription table), the engine owns the bus
 * and calls in at the points where it can give the bus up.  When the walker is
 * replaced by the event-driven scheduler at step 10, these hooks stay and only
 * their caller changes.
 */

#ifndef MODBUS_INTERNAL_H_
#define MODBUS_INTERNAL_H_

#include "App/Modbus/modbus.h"
#include "App/Modbus/modbus_port.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Dispatch — the engine hands a decoded reading, a transaction outcome or a
 * config swap to whoever subscribed.  Modbus task only; delivery is the call.
 * ========================================================================== */

void ModbusDispatch_Sample(const sModbusPointDesc *pt, int32_t value,
                           const char *text);

void ModbusDispatch_Txn(uint8_t devOrd, uint8_t slaveAddr, uint16_t addr,
                        uint16_t regs, uint16_t elapsed_ms, int16_t err,
                        uint8_t planId, uint8_t timeTableId);

void ModbusDispatch_Config(uint8_t activeRegion,
                           const sModbusConfigCounts *counts);

/**
 * @brief  Is anything subscribed at all?
 *
 *         Work exists because someone asked for it (§1.2): a plan nobody
 *         subscribes to is not polled, so with no subscribers the engine puts
 *         nothing on the wire.
 */
int ModbusSub_AnyLive(void);

/* The OR of every live subscription's plan mask. */
uint8_t ModbusSub_PlanUnion(void);

/**
 * @brief  Post a catalogue replay to every live subscription.
 *
 *         Called by the engine after a config swap: mbEvt_config says the
 *         ordinals may mean something else now, and the catalogue that follows
 *         says what they mean (§4.5).
 */
void ModbusSub_CatalogueAll(void);

/**
 * @brief  Fill a point descriptor from a point record.
 *
 *         One place builds a descriptor, so a sample and a catalogue entry can
 *         never describe the same point differently.  `topicPrefix` and the
 *         record are BORROWED — the descriptor is only ever handed to a
 *         callback that must not retain it.
 */
void ModbusDesc_Build(sModbusPointDesc *d, const char *topicPrefix,
                      uint8_t devOrd, uint16_t ptOrd, uint32_t period_sec,
                      const sModbusPointRecord *pt);

/**
 * @brief  Complete any posted Modbus_Unsubscribe, ending with the consumer's
 *         mbEvt_released call.  Modbus task only — which is precisely why no
 *         dispatch can be in flight when the entry is cleared.
 */
void ModbusSub_Service(void);

/**
 * @brief  Frame verdict -> the API's code set.
 *
 *         Shared/ says what was wrong with the bytes; the API says what
 *         happened to an item or a sequence.  A slave that answers an
 *         exception IS answering, so its code is folded in per item (§4.6)
 *         rather than kept as one global "last exception" nobody can
 *         attribute — which is what `exc` carries here.
 */
int16_t ModbusErr_FromFrame(int frameErr, uint8_t exc);

/* ==========================================================================
 * The engine's side of the port table (modbus_port.c).  Module-internal: a
 * DRIVER sees modbus_port.h, a consumer sees neither.
 * ========================================================================== */

int16_t ModbusPort_Read(uint8_t portId, const sModbusPortParams *params,
                        uint8_t slave, uint8_t fc, uint16_t addr,
                        uint16_t count, uint16_t *regs);

int16_t ModbusPort_Write(uint8_t portId, const sModbusPortParams *params,
                         uint8_t slave, uint8_t writeFc, uint16_t reg,
                         const uint16_t *values, uint16_t count);

/* A port with no driver is disabled, structurally (§5.1). */
int  ModbusPort_IsRegistered(uint8_t portId);

void ModbusPort_SetMonitor(int enable);
int  ModbusPort_GetMonitor(void);

/**
 * @brief  Rebuild the resident plan-header table from the ACTIVE region.
 *
 *         Called at init and after every config swap — a swap is exactly the
 *         moment the table stops describing what is in flash (§3.5).
 */
void ModbusPlans_Refresh(void);

/* Plan slots whose device set names `devOrd`. */
uint8_t ModbusPlans_CoveringDevice(uint8_t devOrd);

/**
 * @brief  Carry out a claimed plan edit: rewrite the inactive region and arm
 *         the swap.  Modbus task only, because it is a flash write.
 *
 *         The claim already happened synchronously, which is what let the API
 *         call report mbErr_busy for a subscribed plan (§4.8).
 */
void ModbusPlans_Service(void);

/**
 * @brief  Run any pending request batch to completion and fire its callback.
 *
 *         Called by the engine at every opening between transactions — which
 *         is what keeps requests responsive without ever interleaving on a
 *         half-duplex wire (§5.2).  A batch runs whole: splitting it across
 *         openings would break "one sequence, one port, one baud".
 *
 *         Cheap when nothing is pending, so calling it often is the point.
 */
void ModbusReq_Service(void);

/**
 * @brief  Complete every outstanding request against the retiring config.
 *
 *         A config swap completes outstanding requests; it never drops them
 *         (§4.6) — "the callback always fires" is what lets a caller reclaim
 *         its item array.  Called by the engine immediately after it commits
 *         a swap, before it walks the new config.
 */
void ModbusReq_CompleteForSwap(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_INTERNAL_H_ */

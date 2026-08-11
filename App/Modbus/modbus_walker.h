/**
 * @file    modbus_walker.h
 * @brief   Generic Modbus poll scheduler (FreeRTOS task) — walks the
 *          flash-resident register config (Shared/Modbus) one full lap per
 *          100 ms tick, reads due transactions, decodes points and publishes
 *          them through the MQTT bridge. Replaces the hardcoded Solis
 *          poller (design docs/modbus.md §3.5).
 *
 * No RAM-resident config: records are read from external flash on demand.
 * Fixed RAM state is sized to the §6 bounds (~2 KB) regardless of the
 * actual config. Config hot-swap happens only at a lap boundary.
 */
#ifndef MODBUS_WALKER_H_
#define MODBUS_WALKER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t baud;                /* 0 = 9600                              */
    uint32_t responseTimeoutMs;   /* 0 = 1000                              */
} sModbusWalkerCfg;

void ModbusWalker_Start(const sModbusWalkerCfg *cfg);
void ModbusWalker_Stop(void);
int  ModbusWalker_IsRunning(void);

void ModbusWalker_SetBaud(uint32_t baud);      /* applies on next Start */
uint32_t ModbusWalker_GetBaud(void);

/* Queue a single-register write (FC06); drained by the walker between
 * transactions. 0 = queued, -1 = not running / slot full. */
int  ModbusWalker_WriteRegister(uint8_t slaveAddr, uint16_t reg,
                                uint16_t value);

/* Test hook: feed a decoded register block to the transaction identified by
 * {slaveAddr, startAddr} in the ACTIVE config and run the normal
 * decode/threshold/publish path synchronously (replaces the old
 * count-dispatched SolisPoller_InjectRegisters). 0 = ok, -1 = no matching
 * transaction / no valid config. */
int  ModbusWalker_InjectResponse(uint8_t slaveAddr, uint16_t startAddr,
                                 const uint16_t *regs, uint16_t count);

/* Clear publish/poll tracking so the next lap re-reads and re-publishes
 * everything (backs "mqtt publish now"). */
void ModbusWalker_ForceRepublish(void);

void ModbusWalker_LogStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_WALKER_H_ */

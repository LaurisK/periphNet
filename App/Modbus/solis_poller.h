/**
 * @file    solis_poller.h
 * @brief   Solis inverter Modbus poll scheduler
 *
 * Runs as a FreeRTOS task.  Polls Solis registers in fast (5 s) and slow
 * (60 s) groups and stores results in a shared sSolisData cache.
 */

#ifndef SOLIS_POLLER_H_
#define SOLIS_POLLER_H_

#include "App/Modbus/solis_registers.h"
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Configuration (runtime-adjustable via commands)
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  slaveAddr;        /* Modbus slave address (default 1) */
    uint32_t baud;             /* USART baud rate (default 9600) */
    uint32_t fastIntervalMs;   /* fast poll interval (default 5000) */
    uint32_t slowIntervalMs;   /* slow poll interval (default 60000) */
    uint32_t responseTimeoutMs;/* per-transaction timeout (default 1000) */
} sSolisPollerCfg;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief  Start the Modbus poller task.
 *         Initialises USART2 at the configured baud rate.
 */
void SolisPoller_Start(const sSolisPollerCfg *cfg);

/**
 * @brief  Stop the poller task and deinit USART2.
 */
void SolisPoller_Stop(void);

/**
 * @brief  Returns 1 if the poller task is running.
 */
int SolisPoller_IsRunning(void);

/**
 * @brief  Get pointer to the shared register cache.
 *         Data is updated by the poller task; read from MQTT task.
 *         Caller should treat as read-only (no mutex needed for
 *         aligned 32-bit reads on Cortex-M4).
 */
const sSolisData *SolisPoller_GetData(void);

/**
 * @brief  Get current poller configuration.
 */
const sSolisPollerCfg *SolisPoller_GetConfig(void);

/**
 * @brief  Update baud rate.  Takes effect on next poll cycle restart.
 */
void SolisPoller_SetBaud(uint32_t baud);

/**
 * @brief  Update slave address.
 */
void SolisPoller_SetSlaveAddr(uint8_t addr);

/**
 * @brief  Write a single holding register on the inverter.
 *         Thread-safe: queued and executed on the next poll cycle.
 * @return 0 if queued, -1 if queue full or not running
 */
int SolisPoller_WriteRegister(uint16_t reg, uint16_t value);

/**
 * @brief  Log current data to Trice output.
 */
void SolisPoller_LogData(void);

#endif /* SOLIS_POLLER_H_ */

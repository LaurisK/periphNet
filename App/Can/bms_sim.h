/**
 * @file    bms_sim.h
 * @brief   Pylontech BMS simulator — transmits battery CAN frames on CAN1
 */

#ifndef BMS_SIM_H_
#define BMS_SIM_H_

#include <stdint.h>

/** Initialise CAN1 for 500 kbps TX and start periodic BMS frame transmission. */
void BmsSim_Start(void);

/** Stop BMS simulator and de-init CAN1. */
void BmsSim_Stop(void);

/** Returns 1 if simulator is running. */
int BmsSim_IsRunning(void);

/**
 * Transmit one round of all Pylontech frames.
 * Call from a task/timer at ~1 Hz (PYLON_TX_INTERVAL_MS).
 */
void BmsSim_SendOnce(void);

/* --- Setpoint adjustment (for varying test data) --- */

void BmsSim_SetVoltage(float volts);
void BmsSim_SetCurrent(float amps);
void BmsSim_SetSoc(uint16_t pct);
void BmsSim_SetTemperature(float degC);

#endif /* BMS_SIM_H_ */

/**
 * @file    bms_sim.h
 * @brief   Pylontech BMS simulator — the board answering an inverter itself
 *
 * Occupies the CAN bridge's frame-source slot: it emits only in
 * canBrMode_bms, on whichever bus the inverter is on, and owns no peripheral.
 */

#ifndef BMS_SIM_H_
#define BMS_SIM_H_

#include <stdint.h>

/** Take the bridge source slot.  Emission starts when the mode does. */
void BmsSim_Start(void);

/** Release the bridge source slot. */
void BmsSim_Stop(void);

/** Returns 1 if simulator is running. */
int BmsSim_IsRunning(void);

/**
 * Transmit one round of all Pylontech frames on the inverter bus.
 * The bridge calls this at PYLON_TX_INTERVAL_MS in bms mode; the CLI calls it
 * directly to put one round on the wire by hand.
 */
void BmsSim_SendOnce(void);

/* --- Setpoint adjustment (for varying test data) --- */

void BmsSim_SetVoltage(float volts);
void BmsSim_SetCurrent(float amps);
void BmsSim_SetSoc(uint16_t pct);
void BmsSim_SetTemperature(float degC);

#endif /* BMS_SIM_H_ */

/**
 * @file    bms_reader.h
 * @brief   Pylontech BMS CAN reader — receives and parses battery frames on CAN2
 */

#ifndef BMS_READER_H_
#define BMS_READER_H_

#include "App/Can/pylontech.h"

/** Initialise CAN2 for 500 kbps RX with filters for Pylontech IDs. */
void BmsReader_Start(void);

/** Stop reader and de-init CAN2. */
void BmsReader_Stop(void);

/** Returns 1 if reader is running. */
int BmsReader_IsRunning(void);

/**
 * Poll for received CAN frames and parse them.
 * Call periodically (e.g., every 100 ms).
 */
void BmsReader_Poll(void);

/** Get pointer to parsed battery data (updated by BmsReader_Poll). */
const sPylonBatteryData *BmsReader_GetData(void);

/** Log current battery data via Trice. */
void BmsReader_LogData(void);

#endif /* BMS_READER_H_ */

/**
 * @file    bms_reader.h
 * @brief   Pylontech BMS CAN reader — parses the battery frame set for display
 *
 * A subscriber of App/Can/can_bus.c, not a peripheral owner: it asks for
 * 0x350-0x35F on whichever bus the bridge says the battery is on, so the bus
 * must already be up.
 */

#ifndef BMS_READER_H_
#define BMS_READER_H_

#include "App/Can/pylontech.h"

/** Subscribe to 0x350-0x35F on the battery bus.  Needs the bus running. */
void BmsReader_Start(void);

/** Drop the subscription.  Leaves the bus alone. */
void BmsReader_Stop(void);

/** Returns 1 if reader is running. */
int BmsReader_IsRunning(void);

/**
 * No-op: frames arrive by interrupt through the dispatcher.  Kept so a caller
 * may still say "refresh" before reading, and cost nothing.
 */
void BmsReader_Poll(void);

/** Get pointer to parsed battery data (updated by BmsReader_Poll). */
const sPylonBatteryData *BmsReader_GetData(void);

/** Log current battery data via Trice. */
void BmsReader_LogData(void);

#endif /* BMS_READER_H_ */

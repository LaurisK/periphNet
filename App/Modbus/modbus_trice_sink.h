/**
 * @file    modbus_trice_sink.h
 * @brief   Trice subscriber — every decoded reading, straight to the log.
 *
 * The cheapest possible consumer, and the one that proves data flows out
 * through the API with no MQTT in the picture (docs/modbus.md §10 step 3).
 * It does its work inline in the callback and allocates nothing, which is
 * exactly what §4.7 says a consumer that only logs should do.
 *
 * Driven by `modbus dump on|off`.
 */
#ifndef MODBUS_TRICE_SINK_H_
#define MODBUS_TRICE_SINK_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief  Attach (1) or detach (0) the sink.  Idempotent. */
void ModbusTriceSink_Set(int enable);

int  ModbusTriceSink_Get(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_TRICE_SINK_H_ */

/**
 * @file    modbus_config_export.h
 * @brief   Re-serialize a compiled LUT region back to author-facing JSON.
 *
 * Data-faithful, not byte-identical to the original upload (design §7): the
 * emitted JSON re-compiles to a byte-identical record stream. A pure
 * function of the region, so HTTP download can run it twice — once with a
 * counting sink for Content-Length, then with the socket sink.
 */
#ifndef MODBUS_CONFIG_EXPORT_H_
#define MODBUS_CONFIG_EXPORT_H_

#include "modbus_records.h"
#include "nvdb.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fModbusByteSink lives in modbus_records.h (docs/modbus.md §4.9). */

/* Walk the region and emit JSON. 0 = done, -1 = invalid region / sink abort. */
int MbCfgExport(eNvDbUser region, fModbusByteSink sink, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONFIG_EXPORT_H_ */

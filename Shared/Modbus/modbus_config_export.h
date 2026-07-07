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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receive a JSON fragment; return 0 to continue, <0 to abort the export. */
typedef int (*fMbByteSink)(void *ctx, const char *data, uint32_t len);

/* Walk the region and emit JSON. 0 = done, -1 = invalid region / sink abort. */
int MbCfgExport(uint32_t regionBase, fMbByteSink sink, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONFIG_EXPORT_H_ */

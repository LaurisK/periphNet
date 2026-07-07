/**
 * @file    modbus_config_compiler.h
 * @brief   Streaming JSON -> record-stream compiler (upload validation pass).
 *
 * Compiles the author-facing JSON config (design doc §2) directly into a LUT
 * region as it streams in — there is no full-document RAM buffer and no
 * separate dry-run: this pass IS the validation (§4). The region header is
 * written last, only if every rule passed, so a failed or torn upload never
 * leaves a valid region behind. The region's previous content is invalidated
 * up front (header sector erased first), so an aborted compile can never
 * resurrect a stale config either.
 *
 * The parser is a hand-rolled pull-parser for this fixed schema: bytes come
 * from an abstract source callback (HTTP connection cursor on the device, a
 * memory buffer in host tests), so RAM cost is a few static buffers
 * (~1.3 KB) regardless of config size. Keys may appear in any order; unknown
 * keys are rejected (typo safety). writeMin/writeMax and publish.threshold
 * are authored in the scaled-integer domain (= raw register domain).
 */
#ifndef MODBUS_CONFIG_COMPILER_H_
#define MODBUS_CONFIG_COMPILER_H_

#include "modbus_config_store.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pull up to maxLen bytes; returns >0 = bytes read, 0 = EOF, <0 = I/O error. */
typedef int (*fMbByteSource)(void *ctx, uint8_t *buf, uint32_t maxLen);

typedef struct {
    int          ok;          /* 1 = compiled, region marked valid          */
    int          deviceIdx;   /* first failure location; -1 = n/a           */
    int          txnIdx;
    int          pointIdx;
    char         field[24];   /* offending key, or "json" for syntax errors */
    char         reason[64];
    sMbCfgCounts counts;      /* filled on success                          */
} sMbCompileResult;

/* Compile JSON from `src` into the LUT region at `regionBase`.
 * `kick` (nullable) is called before each sector erase (IWDG).
 * Returns 0 and res->ok=1 on success; -1 with res describing the first
 * failure otherwise. Not reentrant (static parser state) — callers are the
 * HTTP task and one-time default provisioning, serialized by design. */
int MbCfgCompile(fMbByteSource src, void *srcCtx, uint32_t regionBase,
                 void (*kick)(void), sMbCompileResult *res);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONFIG_COMPILER_H_ */

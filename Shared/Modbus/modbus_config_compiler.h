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

/* fModbusByteSource and sModbusCompileResult live in modbus_records.h: the
 * result is data a consumer may name, MbCfgCompile() is a flash accessor it
 * may not call (docs/modbus.md §4.9). */

/* Compile JSON from `src` into the LUT region held by nvDb user `region`.
 * `kick` (nullable) is called before each sector erase (IWDG).
 * Returns 0 and res->ok=1 on success; -1 with res describing the first
 * failure otherwise. Not reentrant (static parser state) — callers are the
 * HTTP task and one-time default provisioning, serialized by design. */
int MbCfgCompile(fModbusByteSource src, void *srcCtx, eNvDbUser region,
                 void (*kick)(void), sModbusCompileResult *res);

/* The same pass with the record writes discarded: same source, same rules,
 * same *res, no flash touched (docs/modbus.md §4.9).  It exists because
 * "compile is validation" conflates two things — not ACTIVATING is already
 * true, but not WRITING is not: an upload consumes the inactive region
 * whether it succeeds or fails, and that region holds the previous config, so
 * a fat-fingered upload destroys the fallback while telling you it failed. */
int MbCfgVerify(fModbusByteSource src, void *srcCtx,
                sModbusCompileResult *res);

/* Parse ONE plan object — the same JSON an operator would paste into a
 * config's plans[] array (docs/modbus.md §8.1).  Fills `out` with pointers
 * into this module's static storage, valid until the next call, and `*outId`
 * with the authored slot or -1 if the body did not name one.
 *
 * Syntax and shape only: the semantic rules (capability exists, devices lie
 * within it, point ids are readable and unique) are MbCfgPlans_Validate's, so
 * there is one semantic validator reached two ways. */
int MbCfgParsePlan(fModbusByteSource src, void *srcCtx,
                   sModbusPlanSpec *out, int *outId,
                   sModbusCompileResult *res);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONFIG_COMPILER_H_ */

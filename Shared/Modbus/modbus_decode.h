/**
 * @file    modbus_decode.h
 * @brief   Point decode / quantize / format — pure functions, integer-only
 *          pipeline (design §8, §13).
 *
 * The scaled-integer domain: published value = scaledInt * 10^scalePow10,
 * i.e. for integer decode types scaledInt IS the raw register value, and
 * FC06 writes take scaledInt directly. float32_* wire values are quantized
 * into this domain immediately after decode (the single permitted float
 * step); threshold comparison, heartbeat and formatting stay pure integer.
 *
 * Frozen semantics for the odd types:
 *  - ASCII:    decoded via MbDecode_Ascii (2 chars per register, wire order,
 *              NUL-terminated).  A sample carries `text` and its `value` is
 *              UNSPECIFIED — nothing in the module computes one, and a
 *              consumer wanting change detection hashes the string itself.
 *  - BITFIELD: raw uint16 rendered as unsigned decimal.
 *
 * Neither gets an HA device_class or unit.  There is no change detection and
 * no publish policy anywhere: the module keeps no previous value (§1.2).
 *  - u32/s32 values are clamped to int32 for the scaled domain.
 */
#ifndef MODBUS_DECODE_H_
#define MODBUS_DECODE_H_

#include "modbus_records.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode one point from `regs`, which must point at the point's FIRST
 * register (caller indexes the response buffer with point->offset).
 * Not for ASCII points. */
int32_t MbDecode_Scaled(const sModbusPointRecord *p, const uint16_t *regs);

/* ASCII decode: writes up to 2*p->length chars + NUL; returns string length
 * or -1 if out is too small (needs 2*length+1). */
int MbDecode_Ascii(const sModbusPointRecord *p, const uint16_t *regs,
                   char *out, size_t outSize);

/* ENCODING IS DECODE'S INVERSE AND LIVES BESIDE IT (docs/modbus.md §5.3).
 * A write converts a scaled int32 back into 1..N registers per decodeType,
 * using the same word-order rules decode uses.  A value that does not fit the
 * point's register width is REJECTED, never silently truncated — the module
 * does not quietly move a value to the nearest legal one, exactly as §4.6
 * refuses to clamp an out-of-range request item.
 *
 * Returns the register count written, or -1 if the value does not fit (or the
 * type cannot be written at all: ascii and float32 have no defined inverse
 * here — a float32 point's scaled value has already lost the exponent). */
int MbEncode_Scaled(const sModbusPointRecord *p, int32_t scaled,
                    uint16_t *regs);

/* Render scaledInt * 10^pow10 as decimal text ("512",-1 -> "51.2";
 * "12",2 -> "1200"). Returns chars written (snprintf semantics). */
int MbFormat_Scaled(char *buf, size_t size, int32_t scaled, int8_t pow10);

/* Parse decimal text back into the scaled domain ("51.2",-1 -> 512).
 * Rejects values not representable at this scale ("51.25",-1) or out of
 * int32. Returns 0/-1. */
int MbParse_Scaled(const char *text, int8_t pow10, int32_t *scaled);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_DECODE_H_ */

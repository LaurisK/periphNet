/**
 * @file    modbus_blocks.h
 * @brief   Derived read blocks — what the engine actually puts on the wire.
 *
 * TRANSACTIONS ARE NOT AUTHORED (docs/modbus.md §3.2).  A capability is a flat
 * list of points and a time table is a list of point ids; the read blocks are
 * DERIVED when a plan goes live, by grouping the selected points by function
 * code and ascending address, splitting at the capability's maxReadRegs and
 * never crossing a declared block boundary.
 *
 * Read blocks are never STORED — they are a pure function of records already
 * in the stream, so caching them there would only create two things that can
 * disagree (§7.2).  Being a pure function of records and dialect constants is
 * also why this lives in Shared/ and is host-tested (§2.2, §9): reaching into
 * App/Modbus/ from the test build would make an engine internal a test
 * dependency.
 *
 * The engine supplies the spans (it read the point records out of flash); the
 * sorting, splitting and bounds-checking are here.
 */
#ifndef MODBUS_BLOCKS_H_
#define MODBUS_BLOCKS_H_

#include "modbus_records.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One selected point, as the engine reads it out of a point record. */
typedef struct {
    uint16_t addr;               /* wire address, verbatim as authored      */
    uint16_t ptOrd;              /* so the caller can map a reply back      */
    uint8_t  regs;               /* register width                          */
    uint8_t  fc;                 /* eModbusFunctionCode                     */
} sModbusPointSpan;

/* One frame's worth of read. */
typedef struct {
    uint16_t addr;               /* first wire address to request           */
    uint16_t regs;               /* quantity, in REGISTERS — the wire field */
    uint8_t  fc;
} sModbusReadBlock;

typedef enum {
    mbBlocks_ok            =  0,
    mbBlocks_errNoBlock    = -1, /* a point sits outside every declared block */
    mbBlocks_errPastBlock  = -2, /* a point ends past its block's regs        */
    mbBlocks_errTooMany    = -3, /* more read blocks than the caller allows   */
    mbBlocks_errBadArg     = -4,
} eModbusBlocksErr;

/**
 * @brief  Derive the read blocks covering `sel`.
 *
 *         `sel` is SORTED IN PLACE by (function code, ascending address) — it
 *         is the caller's scratch and is expected to be reused per (device,
 *         time table).  A block is closed when the function code changes, when
 *         the next point lies in a different declared block, or when adding it
 *         would exceed the capability's maxReadRegs.  Gaps INSIDE a declared
 *         block are bridged deliberately: two bytes per register beats a whole
 *         extra round trip.
 *
 * @return number of blocks written to `out`, or a negative eModbusBlocksErr
 */
int MbBlocks_Derive(const sModbusCapabilityRecord *cap,
                    const sModbusBlockRecord *blocks, uint8_t blockCount,
                    sModbusPointSpan *sel, uint16_t selCount,
                    sModbusReadBlock *out, uint16_t maxOut);

/**
 * @brief  Index of a point's first register within a read block's reply.
 *
 *         (pt.addr - block.addr) / addrStride — one of the three places the
 *         address domain and the register domain differ (§3.3).  Returns -1 if
 *         the point does not lie in the block.
 */
int MbBlocks_RegIndex(const sModbusCapabilityRecord *cap,
                      const sModbusReadBlock *blk, uint16_t ptAddr);

/**
 * @brief  Which declared block contains `addr`, or -1.
 */
int MbBlocks_Find(const sModbusCapabilityRecord *cap,
                  const sModbusBlockRecord *blocks, uint8_t blockCount,
                  uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_BLOCKS_H_ */

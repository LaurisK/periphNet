/**
 * @file    batcomm_frame.h
 * @brief   THE PURE ENCODER: one sBatCommIn, one slot, one frame.
 *          MODULE-INTERNAL (docs/design_battery_comm.md §4).
 *
 * LIBC ONLY.  No CAN, no RTOS, no flash, no float, ZERO FILE STATICS — the
 * whole input is the arguments.  That is what lets tests/test_batcomm.c
 * assert the captured PowerBrick payloads byte-for-byte on a host, which is
 * the only evidence available for a dialect nobody can re-measure without
 * standing in front of the cabinet.
 */

#ifndef BATCOMM_FRAME_H_
#define BATCOMM_FRAME_H_

/* Includes -----------------------------------------------------------------*/

#include "App/BatComm/batcomm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants -------------------------------------------------------*/

/* The identifiers the dyness_lv profile sources, in CYCLE ORDER — which is
 * the order the capture recorded and is part of the imitation, not an
 * accident of an array.  0x7FF is deliberately absent: it is 80 % of the
 * frames on that wire and NO HYPOTHESIS SURVIVES ITS DATA
 * (reference_dyness_can_capture_2026-09-05.md §4.1), so it is omitted rather
 * than replayed as a constant somebody would later mistake for a decode. */
#define BATCOMM_ID_INFO0        0x30Fu   /* observed all-zero for 24 h       */
#define BATCOMM_ID_LIMITS       0x351u
#define BATCOMM_ID_SOC          0x355u
#define BATCOMM_ID_MEASURE      0x356u
#define BATCOMM_ID_ALARM        0x359u
#define BATCOMM_ID_ALARM_EXT    0x35Au   /* observed all-zero                */
#define BATCOMM_ID_CHGCTRL      0x35Cu
#define BATCOMM_ID_MFGNAME      0x35Eu
#define BATCOMM_ID_REMAINING    0x35Fu
#define BATCOMM_ID_NAME0        0x370u
#define BATCOMM_ID_NAME1        0x371u
#define BATCOMM_ID_MODELINFO    0x70Du

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Has every group this pack ADVERTISES been delivered at least once?
 *
 * pack.h's rule: a field is meaningful only when its capability is set AND its
 * group has been delivered — `age_ms[] == PACK_AGE_NEVER` is "not read yet",
 * which is a different statement from "read as zero".  A capability is
 * advertised from bind, but a slow group (a JK's limits) arrives well after
 * the fast one, and mapping the gap onto the wire as zeros tells the inverter
 * to stop charging with a 0 V limit.  So until this returns 1 the source is
 * NOT READY and nothing is emitted (batcomm.h contract 2).
 *
 * Pure: reads only its argument, so the host tests drive it directly.
 *
 * @param  p - one pack's state
 * @retval 1 when every advertised group has been delivered, 0 otherwise
 */
int BatFrame_PackReady(const sPackState *p);

/**
 * @brief  How many leading slots of a cycle carry a frame.
 * @param  proto - the dialect
 * @retval the count, or 0 for an unknown dialect
 */
uint8_t BatFrame_SlotCount(eBatCommProto proto);

/**
 * @brief  Build the frame for one slot of one cycle.
 *
 * @param  proto - the dialect
 * @param  tune - scales and the sign bit; NULL is not accepted
 * @param  in - the numbers, with `fields` saying which of them mean anything
 * @param  slot - 0-based slot within the cycle
 * @param  out - destination
 * @retval 1 when a frame was produced, 0 when this slot emits nothing —
 *         an empty tail slot, an unknown dialect, or 0x351 WITHHELD because
 *         a voltage limit is invalid (batcomm.h contract 3)
 */
int BatFrame_Build(eBatCommProto proto, const sBatCommTune *tune,
                   const sBatCommIn *in, uint8_t slot, sBatCommFrame *out);

#ifdef __cplusplus
}
#endif

#endif /* BATCOMM_FRAME_H_ */

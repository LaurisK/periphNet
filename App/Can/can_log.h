/*
 * can_log.h
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * THE FLASH TRACE.  A ring of CAN frames in external flash, sized so that a
 * bus can be watched for days and read out in pieces while it keeps running.
 *
 * WHY IT CANNOT SIMPLY BE "EVERY FRAME".  A bridged Pylontech set is six
 * frames a second on each side plus the inverter's keepalive — about 14
 * records a second, or 2.4 million over two days.  At 16 bytes each that is
 * 38 MB, and the whole chip is 8 MB with 7.3 free.  So the policy is not a
 * knob for tuning, it is what makes the feature possible at all: log a frame
 * when its PAYLOAD CHANGED, at most once per `minGap_ms`, and at least once
 * per `heartbeat_ms` so a constant frame still proves it was there.  In the
 * Pylontech dialect only 0x356 (V/I/T) moves continuously, so that is roughly
 * one record a second and the 4 MB area holds about 60 hours.
 *
 * `canLogMode_all` exists for short captures where every frame matters, and
 * fills the same area in about five hours.  The status reply always reports
 * how much is held and how far back it reaches, so the trade is visible
 * rather than assumed.
 *
 * ON-MEDIUM SHAPE — 4 KB unit, 16 B record, 256 records per unit:
 *
 *   unit 0        area header (magic, version, record size, unit count)
 *   unit 1..N-1   [unit header][255 records]
 *
 * A unit header carries the unit's sequence number and the BOOT it belongs
 * to, and **every boot starts a fresh unit**. That costs up to 254 record
 * slots per reset, which is nothing, and buys two things worth much more:
 * a record's timestamp is unambiguous (`stamp_ms` is milliseconds since a
 * *known* boot), and finding the head after a reset is a scan of unit headers
 * with no intra-unit search and no partially-written unit to reason about.
 *
 * RECORDS ARE NUMBERED GLOBALLY and monotonically — `recNo = unitSeq * 255 +
 * index` — so a reader polls with `from=<last recNo + 1>` and never has to
 * think about the ring. A record whose unit has since been overwritten reads
 * back as absent, which is the honest answer to "you asked too late".
 */

#ifndef CAN_LOG_H_
#define CAN_LOG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes -----------------------------------------------------------------*/

#include <stdint.h>

#include "App/Can/can_bus.h"

/* Exported types -----------------------------------------------------------*/

/** Records per erasable unit, the header taking the first slot. */
#define CANLOG_RECS_PER_UNIT    255u

/** How many records the RAM staging ring holds between flushes.  The writer
 *  runs on defaultTask at 100 ms; 32 records is over four seconds of slack at
 *  the worst policy-limited rate. */
#define CANLOG_STAGE_DEPTH      32u

/** Default policy. */
#define CANLOG_MIN_GAP_MS       1000u
#define CANLOG_HEARTBEAT_MS     60000u

typedef enum {
    canLogMode_off = 0,     /* nothing is written                            */
    canLogMode_changes,     /* changed payloads, rate-limited + heartbeat    */
    canLogMode_all,         /* every frame — hours, not days                 */
    canLogMode_last
} eCanLogMode;

/** What the module is doing.  `provisioning` is the one-time erase of a fresh
 *  area, which is deferred to nvDb's collector and can take a while. */
typedef enum {
    canLogState_idle = 0,   /* no area, or none usable                       */
    canLogState_provisioning,
    canLogState_running,
    canLogState_last
} eCanLogState;

/** One logged frame.  16 bytes, 255 to a 4 KB unit. */
typedef struct {
    uint32_t stamp_ms;      /* HAL_GetTick() at capture; 0xFFFFFFFF = free   */
    uint16_t id;            /* 11-bit identifier, or the LOW 16 BITS of an
                               extended one — see `meta` bit 3               */
    uint8_t  meta;          /* b0 bus, b1 dir, b2 rtr, b3 ext, b4-7 dlc      */
    uint8_t  reserved;
    uint8_t  data[8];
} sCanLogRec;

#define CANLOG_META_BUS(m)      ((uint8_t)((m) & 0x01u))
#define CANLOG_META_DIR(m)      ((uint8_t)(((m) >> 1) & 0x01u))
#define CANLOG_META_RTR(m)      ((uint8_t)(((m) >> 2) & 0x01u))
#define CANLOG_META_EXT(m)      ((uint8_t)(((m) >> 3) & 0x01u))
#define CANLOG_META_DLC(m)      ((uint8_t)(((m) >> 4) & 0x0Fu))

typedef struct {
    uint32_t capacityRecs;      /* how many records the ring holds           */
    uint32_t oldestRec;         /* lowest recNo still readable               */
    uint32_t nextRec;           /* recNo the next captured frame will get    */
    uint32_t heldRecs;          /* nextRec - oldestRec                       */
    uint32_t droppedCnt;        /* frames the staging ring could not take    */
    uint32_t stalledCnt;        /* frames dropped waiting for an erase       */
    uint32_t writeErrCnt;
    uint32_t bootId;
    uint32_t minGap_ms;
    uint32_t heartbeat_ms;
    uint32_t areaSize_bytes;
    uint8_t  mode;              /* eCanLogMode                               */
    uint8_t  state;             /* eCanLogState                              */
    uint8_t  staged;            /* records waiting in RAM right now          */
} sCanLogStatus;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Adopt or provision the flash area and start capturing.
 *
 * Scans the unit headers for the newest sequence number, opens the unit after
 * it, and installs itself as the monitor's sink.  An area whose header is
 * absent or of the wrong version is wiped and rebuilt — its previous contents
 * were written by a format this build cannot read, and pretending otherwise
 * would produce plausible nonsense.
 *
 * @retval 0 on success, negative when the area is missing or unusable
 * @note   Task context, after NvDb_Init().  Blocks for the header scan.
 */
int CanLog_Init(void);

/**
 * @brief  Move staged records to flash, and open the next unit when due.
 * @note   defaultTask, every ~100 ms.  MAY BLOCK for a page program, and once
 *         per unit for a header write.  Never erases inline — the erase of
 *         the unit ahead is nvDb's collector's job and is requested a whole
 *         unit in advance.
 */
void CanLog_Service(void);

/**
 * @brief  Set the capture policy.
 * @param  mode - off, changes or all
 * @param  minGap_ms - per identifier, per bus; 0 for no limit
 * @param  heartbeat_ms - log an unchanged frame at least this often; 0 to
 *            never log one
 * @retval 0 on success, negative on a bad argument
 */
int CanLog_SetPolicy(eCanLogMode mode, uint32_t minGap_ms,
                     uint32_t heartbeat_ms);

/**
 * @brief  Copy the module's state and counters.
 * @param  out - destination
 * @retval 0 on success, negative on a bad argument
 */
int CanLog_GetStatus(sCanLogStatus *out);

/**
 * @brief  Read one record by its global number.
 * @param  recNo - as reported by CanLog_GetStatus
 * @param  out - destination
 * @retval 0 on success, negative when that record has been overwritten, has
 *            not been written yet, or the medium failed
 * @note   Task context.  BLOCKS on flash.
 */
int CanLog_ReadRec(uint32_t recNo, sCanLogRec *out);

/**
 * @brief  Throw the whole log away and start again at record 0.
 * @retval 0 on success, negative on failure
 * @note   Task context.  Returns immediately; the erase is nvDb's collector's
 *         and the module reports `provisioning` until it finishes.
 */
int CanLog_Wipe(void);

/** @brief  Mode name for the CLI and JSON; "?" when out of range. */
const char *CanLog_ModeName(eCanLogMode mode);

/**
 * @brief  Parse a mode name.
 * @param  name - "off", "changes" or "all"
 * @param  out - the parsed mode
 * @retval 0 on success, negative when the name matches nothing
 */
int CanLog_ModeFromName(const char *name, eCanLogMode *out);

/** @brief  State name for the CLI and JSON; "?" when out of range. */
const char *CanLog_StateName(eCanLogState state);

#ifdef __cplusplus
}
#endif

#endif /* CAN_LOG_H_ */

/**
 * @file    system.h
 * @brief   System services: reset-cause detection, SW watchdog, IWDG kick
 */

#ifndef APP_SYSTEM_H_
#define APP_SYSTEM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Reset cause flags (subset of RCC_CSR reset flags)
 */
typedef enum {
    sysRst_pin  = 0x01,  /*!< NRST pin reset     */
    sysRst_por  = 0x02,  /*!< Power-on reset     */
    sysRst_sw   = 0x04,  /*!< Software reset     */
    sysRst_iwdg = 0x08,  /*!< IWDG reset         */
    sysRst_wwdg = 0x10,  /*!< WWDG reset         */
    sysRst_bor  = 0x20,  /*!< BOR reset          */
    sysRst_lpwr = 0x40,  /*!< Low-power reset    */
} eResetCause;

/**
 * @brief Zero the NOLOAD .ccmram section (startup code does not touch CCM).
 *
 * Must be called once from main() BEFORE osKernelInitialize(): .ccmram
 * holds buffers (USB CDC, HTTP, Modbus walker) that are in use as
 * soon as the scheduler starts.
 */
void System_EarlyInit(void);

/**
 * @brief Initialise system services.
 *
 * Reads and clears RCC reset-cause flags, initialises TIM14 software
 * watchdog (12 s period before IWDG fires at ~16 s).
 * Must be called once, early in the default FreeRTOS task (Trice available).
 */
void System_Init(void);

/**
 * @brief Log previously saved reset cause via Trice.
 *
 * Call after Trice task has started.
 */
void System_LogResetCause(void);

/**
 * @brief Raw RCC_CSR reset-cause flags captured at boot by System_Init().
 */
uint32_t System_GetResetCause(void);

/**
 * @brief Refresh IWDG and restart TIM14 software watchdog counter.
 *
 * Call regularly (≤ 12 s interval) from the critical application task.
 * If not called for 12 s, TIM14 fires and Crash_GenerateReport is called.
 * If not called for ~16.4 s, IWDG resets the MCU.
 */
void KickIwdg(void);

/**
 * @brief Keep the watchdogs quiet during one long operation, without
 *        claiming the caller is the health of the system.
 *
 * KickIwdg() does two jobs: it feeds the watchdogs AND it records the gap
 * since the last feed, which is what System_GetIwdgStats() reports as "the
 * longest we ever went" — the early warning the reset itself never gives.
 * That statistic is only meaningful if it measures defaultTask's loop.
 *
 * A long operation on some other task — a 488 KB erase, a relayout, an
 * occupancy scan — still has to feed the hardware or the IWDG fires halfway
 * through.  It must not touch the statistic while doing so, or the number
 * silently becomes "how long since ANYBODY kicked", which is optimistic
 * exactly when it matters.
 */
void System_FeedWatchdogLongOp(void);

/**
 * @brief Watchdog margin: longest gap ever seen between KickIwdg() calls, and
 *        how long ago the last one was.
 *
 * Either pointer may be NULL.  The maximum ignores the very first kick (there
 * is no previous one to measure from).  Reported by the system monitor —
 * a maximum creeping towards the 16.4 s IWDG timeout is the early warning the
 * reset itself does not give.
 */
void System_GetIwdgStats(uint32_t *gapMax_ms, uint32_t *sinceKick_ms);

/**
 * @brief Clear the recorded IWDG gap maximum.
 */
void System_ResetIwdgStats(void);

/**
 * @brief Stop feeding the watchdog (for deliberate watchdog-trigger test).
 *
 * After calling this the device will reset in ≤ 16.4 s.
 */
void System_SetWatchdogTestMode(void);

/** Floor on a requested reboot delay: long enough for the HTTP reply that
 *  asked for it to leave the board and for Trice to flush. */
#define SYSTEM_REBOOT_MIN_DELAY_MS  500U

/**
 * @brief Arm a deferred reboot; defaultTask performs it when it comes due.
 *
 *        Deferred rather than immediate so the caller gets an answer first --
 *        a reset inside a request handler is indistinguishable from a crash
 *        at the other end of the connection.
 *
 * @param delay_ms  wait before resetting; floored at
 *                  SYSTEM_REBOOT_MIN_DELAY_MS
 */
void System_RequestReboot(uint32_t delay_ms);

/** @return 1 once an armed reboot has come due.  Polled by defaultTask. */
int System_RebootDue(void);

/* ---------------------------------------------------------------------------
 * The restart survivor block
 *
 * A reboot performed to "restore access so the cause can be investigated"
 * erases uptime, the tunnel's own view, the heap floor and the stale counters
 * — that is, exactly the evidence the investigation needs.  This is the black
 * box that stops it (docs/design_tunnel_watchdog.md §5.4, and the standing
 * complaint in docs/issue_wg_sodas_offline_2026-09-06.md §5.4).
 *
 * It lives in `.ccmnoinit`, which is:
 *   - OUTSIDE the _sccmram.._eccmram range, so System_Init()'s memset does not
 *     touch it — the same placement trick .ccmheap already uses;
 *   - untouched by the bootloader, which declares CCMRAM but places nothing
 *     there, so the record survives an OTA install as well as a plain reset;
 *   - deliberately NOT nvDb.  Flash would cost a layout change, a
 *     NVDB_TARGET_VER bump and wear, to persist something whose entire purpose
 *     is a *soft* reset.  A power cut reads back as "no record", which is
 *     correct: a power cut is not this mechanism.
 * ------------------------------------------------------------------------ */

/** Why the firmware restarted itself.  Persisted in the survivor block, so
 *  values are never renumbered. */
typedef enum {
    restartReason_none = 0,         /**< no record                           */
    restartReason_tunnelTerminal,   /**< recovery ladder rung 3              */
    restartReason_last
} eRestartReason;

/** ~64 B of "what the board looked like immediately before it reset itself". */
typedef struct {
    uint32_t reason;            /**< eRestartReason                          */
    uint32_t uptime_sec;
    uint32_t outageAge_ms;      /**< how long the hub had been silent        */
    uint32_t recoveries;        /**< WgLink_RecoveryCount()                  */
    uint32_t portRotations;     /**< WgLink_PortRotationCount()              */
    uint32_t localPort;         /**< the source port that was not working    */
    uint32_t heapFreeMin;
    uint32_t tasksStale;
    uint32_t rebootsUsed;       /**< the per-power-on budget, carried across  */
    uint32_t prevResetCause;    /**< RCC_CSR of the reset BEFORE this one     */
} sRestartRecord;

/**
 * @brief  Stamp the survivor block.  Call immediately before arming a reboot.
 *
 * Adds the magic and CRC itself, so a cold boot — where CCM holds whatever it
 * held — reads back as absent rather than as garbage.
 */
void System_RecordRestart(const sRestartRecord *rec);

/**
 * @brief  Read the record left by the previous restart.
 * @return 0 and fills @p out when a valid record is present, -1 otherwise
 *         (a power-on, or nothing ever recorded).
 */
int System_GetLastRestart(sRestartRecord *out);

/** @brief  Invalidate the record.  The board is not going to explain itself
 *          twice, and a stale record would misattribute the next outage. */
void System_ClearRestartRecord(void);

/** @brief  Name of a restart reason.  Never NULL; "?" when out of range. */
const char *System_RestartReasonName(uint32_t reason);

#ifdef __cplusplus
}
#endif

#endif /* APP_SYSTEM_H_ */

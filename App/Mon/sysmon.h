/**
 * @file    sysmon.h
 * @brief   System monitor — task liveness, stack, heap and CPU accounting
 *
 * One place to answer "is the firmware healthy?" without a debugger:
 *
 *  - **Liveness**: a task registers itself from inside its own body and then
 *    checks in once per loop iteration.  A task that stops completing its
 *    loop while still being scheduled (stuck on a peripheral, spinning on a
 *    state it will never leave) is invisible to the FreeRTOS scheduler and to
 *    the IWDG — it is exactly what a check-in deadline catches.
 *  - **Stacks**: the FreeRTOS high-water mark (minimum free words ever) for
 *    every task in the system, registered or not, against the configured
 *    depth where that is known.  An EthIf stack overflow already cost this
 *    project a silent crash loop (docs/issue_idle_iwdg_crashloop.md).
 *  - **Heap**: FreeRTOS heap_4 free / minimum-ever-free.
 *  - **CPU**: per-task share of a sampling window plus the idle share, from
 *    the FreeRTOS run-time stats clock (DWT cycle counter / 16).
 *  - **Watchdog margin**: the largest gap ever seen between KickIwdg() calls,
 *    i.e. how close the board actually comes to the 16.4 s IWDG timeout.
 *
 * Sampling runs in whatever task calls SysMon_Poll() — defaultTask, which is
 * also the task that kicks the IWDG.  That is deliberate: the one task the
 * monitor cannot report on is the one the hardware watchdog already covers.
 *
 * Nothing here resets or reboots the board.  The monitor reports; policy
 * belongs to the caller (SysMon_AllTasksAlive() is the hook for it).
 */

#ifndef APP_MON_SYSMON_H_
#define APP_MON_SYSMON_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/** Slots for tracked tasks.  The board runs ~12; the rest is headroom for
 *  tasks that come and go (mqtt) plus room to notice a leak. */
#define SYSMON_MAX_TASKS        16u

/** Sampling window.  Also the resolution of every CPU figure below. */
#define SYSMON_WINDOW_MS        1000u

/** A task with fewer than this many free stack words ever is reported as a
 *  warning — 256 B of margin is about one nested HAL call away from a fault. */
#define SYSMON_STACK_WARN_WORDS 64u

/** Returned by SysMon_TaskRegister() when no slot is free. */
#define SYSMON_ID_NONE          (-1)

/**
 * @brief Per-task snapshot, as of the last completed sampling window.
 */
typedef struct {
    const char *name;                 /*!< NUL-terminated, owned by SysMon   */
    uint32_t    runTime_ms;           /*!< CPU time since boot               */
    uint32_t    checkinCnt;           /*!< Loop iterations reported          */
    uint32_t    sinceCheckin_ms;      /*!< Age of the last check-in          */
    uint32_t    deadline_ms;          /*!< 0 = liveness not asserted         */
    uint32_t    staleCnt;             /*!< Deadline misses since boot        */
    uint16_t    stackSize_words;      /*!< 0 = configured depth unknown      */
    uint16_t    stackFreeMin_words;   /*!< FreeRTOS high-water mark          */
    uint16_t    cpuLoad_permille;     /*!< Share of the last window, 0..1000 */
    uint16_t    cpuPeak_permille;     /*!< Worst window since boot/reset     */
    uint8_t     priority;             /*!< Current (possibly inherited)      */
    uint8_t     state;                /*!< eTaskState                        */
    uint8_t     present;              /*!< 0 = task has been deleted         */
    uint8_t     stale;                /*!< 1 = past its check-in deadline    */
} sSysMonTaskInfo;

/**
 * @brief Whole-system snapshot, as of the last completed sampling window.
 */
typedef struct {
    uint32_t uptime_sec;
    uint32_t heapSize_bytes;          /*!< configTOTAL_HEAP_SIZE             */
    uint32_t heapFree_bytes;
    uint32_t heapFreeMin_bytes;       /*!< Minimum ever free                 */
    uint32_t iwdgGapMax_ms;           /*!< Longest gap between IWDG kicks    */
    uint32_t iwdgSinceKick_ms;
    uint32_t sampleCnt;               /*!< Windows completed                 */
    uint16_t cpuLoad_permille;        /*!< 1000 - idle share                 */
    uint16_t cpuPeakLoad_permille;
    uint16_t idle_permille;           /*!< Share the board spent idle        */
    uint8_t  taskCnt;                 /*!< Tracked slots, present or not     */
    uint8_t  staleCnt;                /*!< Tasks currently past deadline     */
    uint8_t  stackWarnCnt;            /*!< Tasks under SYSMON_STACK_WARN     */
    uint8_t  runTimeCounterOk;        /*!< 0 = DWT never advanced, CPU is 0  */
} sSysMonSummary;

/**
 * @brief Initialise the monitor.  Call once, before any task registers.
 */
void SysMon_Init(void);

/**
 * @brief Register the CALLING task for liveness and stack accounting.
 *
 * Call once from inside the task body — the caller's handle is what binds the
 * slot, so a task that is deleted and recreated (mqtt) gets a fresh one.
 *
 * @param stackSize_words  Configured depth, in words (osThreadAttr_t
 *                         stack_size is in BYTES — divide by 4).  Only used
 *                         to turn the high-water mark into a percentage.
 * @param deadline_ms      Longest tolerable gap between check-ins, or 0 for a
 *                         task that legitimately blocks forever (an accept()
 *                         or a notify-take with no timeout).  Such a task is
 *                         still tracked for stack and CPU.
 * @return Slot id for SysMon_TaskCheckin(), or SYSMON_ID_NONE.
 */
int8_t SysMon_TaskRegister(uint32_t stackSize_words, uint32_t deadline_ms);

/**
 * @brief Report that the task's loop completed an iteration.
 *
 * Cheap (two stores) and safe to call from the registered task only.
 * SYSMON_ID_NONE is ignored, so an unchecked register return is harmless.
 */
void SysMon_TaskCheckin(int8_t id);

/**
 * @brief Drive sampling.  Call regularly (≤ 100 ms) from one task.
 *
 * Does the real work once per SYSMON_WINDOW_MS and returns immediately in
 * between, so it is cheap to put in an existing housekeeping loop.
 */
void SysMon_Poll(void);

/**
 * @brief Copy the last whole-system snapshot.
 */
void SysMon_GetSummary(sSysMonSummary *out);

/**
 * @brief Copy up to @p max per-task snapshots.
 * @return Number written.
 */
uint8_t SysMon_GetTasks(sSysMonTaskInfo *out, uint8_t max);

/**
 * @brief true while no registered task is past its check-in deadline.
 *
 * The hook for making a stuck task fatal (stop kicking the IWDG).  Nothing
 * in the firmware does that today — a false positive would reboot a healthy
 * board, so that decision is left to the caller.
 */
bool SysMon_AllTasksAlive(void);

/**
 * @brief Clear peak CPU, the IWDG gap maximum and the stale counters.
 *
 * Stack high-water marks are owned by FreeRTOS and cannot be cleared.
 */
void SysMon_ResetPeaks(void);

/**
 * @brief Dump the current snapshot to Trice (one line per task).
 */
void SysMon_Report(void);

/**
 * @brief Short printable name for an eTaskState ("Run"/"Rdy"/"Blk"/...).
 */
const char *SysMon_StateName(uint8_t state);

/* --------------------------------------------------------------------------
 * FreeRTOS run-time stats clock — wired up by FreeRTOSConfig.h, not for
 * application use.  DWT cycle counter / 16 (10.5 MHz at 168 MHz core).
 * -------------------------------------------------------------------------- */

void     SysMon_RunTimeCounterInit(void);
uint32_t SysMon_RunTimeCounter(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MON_SYSMON_H_ */

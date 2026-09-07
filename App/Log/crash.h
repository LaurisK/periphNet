/**
 * @file    crash.h
 * @brief   Crash diagnostics, fault logging, and persistent crash storage
 *
 * Dumps CPU register snapshot, fault status registers, return-address
 * candidates scanned out of every stack, and the FreeRTOS task list via Trice
 * (UART DMA, PRIMASK-protected).
 * Safe to call from any exception handler (HardFault, NMI, BusFault, etc.)
 * and from interrupt context (SW watchdog TIM14).
 *
 * Crash reports are also persisted to external SPI flash for later
 * retrieval via HTTP API.
 */

#ifndef APP_LOG_CRASH_H_
#define APP_LOG_CRASH_H_

#include <stdint.h>
#include <stdbool.h>

/* "CRS2".  Bumped from "CRSH" when the R7-chain backtrace (bt_depth/bt_addr)
 * left the record: the layout moved, and a stale log decoded against the new
 * one would report a plausible-looking wrong call path rather than nothing. */
#define CRASH_LOG_MAGIC         0x43525332u   /* "CRS2" */
#define CRASH_LOG_MAX_SCAN      16   /* persisted LR candidates */
/* MUST be >= the number of FreeRTOS tasks.  uxTaskGetSystemState() returns
 * ZERO when the array it is handed is too small, so an undersized value does
 * not truncate the snapshot -- it silently discards all of it.  That is how
 * this log lost its whole task section: the system grew past 8 tasks (11 as
 * of Pd1.1.14) and nothing said so.  Keep it ahead of the task count. */
#define CRASH_LOG_MAX_TASKS     16

/**
 * @brief Crash / fault type identifiers
 */
typedef enum {
    crashType_hardFault,
    crashType_nmi,
    crashType_busFault,
    crashType_usageFault,
    crashType_memManage,
    crashType_swWatchdog,
    crashType_stackOverflow,
    crashType_assert,
    crashType_last          /* sentinel */
} eCrashType;

/**
 * @brief Task snapshot captured during crash
 */
typedef struct {
    char     name[16];
    uint32_t pc;
    uint32_t lr;
    uint16_t free_stack;       /* words */
    uint8_t  state;            /* eTaskState */
    uint8_t  reserved;
} sCrashTask;

/**
 * @brief Persistent crash log record (written to ext flash)
 */
typedef struct {
    uint32_t    magic;                              /* CRASH_LOG_MAGIC          */
    uint32_t    tick;                               /* HAL_GetTick() at crash   */
    uint8_t     crash_type;                         /* eCrashType               */
    uint8_t     task_count;                         /* tasks captured           */
    uint8_t     reserved[2];
    /* Stacked registers */
    uint32_t    r0, r1, r2, r3, r12, lr, pc, psr;
    uint32_t    sp, control, primask;
    /* Fault status registers */
    uint32_t    cfsr, hfsr, mmfar, bfar;
    /* Callee-saved registers of the FAULTING context, captured by
     * Crash_CaptureEntry() before any C code could disturb them.  Best effort
     * for r4_r11[3] (R7): the calling handler's own prologue may have pushed
     * over it, and with no frame pointer in the build there is no guaranteed
     * prologue shape to recover it from.  The other seven are exact. */
    uint32_t    r4_r11[8];
    /* THE call path.  Return-address candidates scanned out of every stack —
     * it depends on no frame chain, which is why it is the only mechanism
     * left (see crash.c scanAllStacks).  Expect stale LRs from returned calls
     * among them: this is a candidate set to filter, not an ordered trace. */
    uint32_t    scan_lr[CRASH_LOG_MAX_SCAN];
    uint8_t     scan_count;
    uint8_t     frame_on_msp;   /* 1 = fault taken in handler mode          */
    uint8_t     reserved2[2];
    /* Task name (for watchdog) */
    char        task_name[16];
    /* Task snapshots */
    sCrashTask  tasks[CRASH_LOG_MAX_TASKS];
    /* Integrity */
    uint32_t    crc32;
} sCrashLog;

/**
 * @brief Generate crash report (Trice output) and save to flash
 * @param type  Crash type (affects header message)
 */
/**
 * @brief Latch the faulting context's callee-saved registers and both stack
 *        pointers.
 *
 * MUST be the FIRST statement of a fault handler.  On exception entry the core
 * stacks only R0-R3, R12, LR, PC and xPSR — R4-R11 are callee-saved and still
 * hold the faulting context's values, so they are recoverable only before C
 * code reuses them.  They are diagnostic values only — nothing walks a frame
 * chain any more, so R7 carries no special meaning and is not singled out.
 *
 * Naked: no prologue may run ahead of the capture.  Clobbers only r0/r1, which
 * AAPCS already lets a callee destroy.
 */
void Crash_CaptureEntry(void);

void Crash_GenerateReport(eCrashType type);

/**
 * @brief Read saved crash log from external flash
 * @param log   Output buffer
 * @return true if a valid crash log was read
 */
bool Crash_ReadFromFlash(sCrashLog *log);

/**
 * @brief Erase the crash log sector in external flash
 */
void Crash_ClearFlash(void);

/**
 * @brief  The word for an eCrashType ("HardFault", "Assert", ...).
 * @param  type - eCrashType, e.g. sCrashLog.crash_type
 * @retval the word, or "Unknown" out of range.  Never NULL.
 * @note   Pure table lookup.  FAULT CONTEXT SAFE: no lock, no tick, no Trice,
 *         no RTOS call -- keep it that way, a crash-time summary is the
 *         obvious next caller.
 */
const char *Crash_TypeName(uint8_t type);

#endif /* APP_LOG_CRASH_H_ */

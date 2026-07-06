/**
 * @file    crash.h
 * @brief   Crash diagnostics, fault logging, and persistent crash storage
 *
 * Dumps CPU register snapshot, fault status registers, faulting-task
 * backtrace and FreeRTOS task list via Trice (UART DMA, PRIMASK-protected).
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

#define CRASH_LOG_MAGIC         0x43525348u   /* "CRSH" */
#define CRASH_LOG_MAX_BT_DEPTH  8
#define CRASH_LOG_MAX_TASKS     8

/**
 * @brief Crash / fault type identifiers
 */
typedef enum {
    CRASH_HARDFAULT,
    CRASH_NMI,
    CRASH_BUS_FAULT,
    CRASH_USAGE_FAULT,
    CRASH_MEM_MANAGE,
    CRASH_SW_WATCHDOG,
    CRASH_STACK_OVERFLOW,
    CRASH_ASSERT
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
    uint8_t     bt_depth;                           /* backtrace entries used   */
    uint8_t     task_count;                         /* tasks captured           */
    uint8_t     reserved;
    /* Stacked registers */
    uint32_t    r0, r1, r2, r3, r12, lr, pc, psr;
    uint32_t    sp, control, primask;
    /* Fault status registers */
    uint32_t    cfsr, hfsr, mmfar, bfar;
    /* Backtrace */
    uint32_t    bt_addr[CRASH_LOG_MAX_BT_DEPTH];
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

#endif /* APP_LOG_CRASH_H_ */

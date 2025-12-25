/**
 * @file    crash.h
 * @brief   Crash diagnostics and fault logging
 *
 * Dumps CPU register snapshot, fault status registers, faulting-task
 * backtrace and FreeRTOS task list via Trice (UART DMA, PRIMASK-protected).
 * Safe to call from any exception handler (HardFault, NMI, BusFault, etc.)
 * and from interrupt context (SW watchdog TIM14).
 */

#ifndef APP_LOG_CRASH_H_
#define APP_LOG_CRASH_H_

/**
 * @brief Crash / fault type identifiers
 */
typedef enum {
    CRASH_HARDFAULT,
    CRASH_NMI,
    CRASH_BUS_FAULT,
    CRASH_USAGE_FAULT,
    CRASH_MEM_MANAGE,
    CRASH_SW_WATCHDOG
} eCrashType;

/**
 * @brief Generate crash report and halt
 *
 * Captures CPU state (exception frame from PSP), prints register dump,
 * Cortex-M4 fault status registers (CFSR/HFSR/MMFAR/BFAR), faulting
 * backtrace, and full FreeRTOS task list with per-task backtraces.
 * Flushes all output via DMA polling before returning (function does not
 * return – caller must loop forever after calling it).
 *
 * @param type  Crash type (affects header message)
 */
void Crash_GenerateReport(eCrashType type);

#endif /* APP_LOG_CRASH_H_ */

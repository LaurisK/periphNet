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
 * holds buffers (USB CDC, MQTT, HTTP, Modbus walker) that are in use as
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

#ifdef __cplusplus
}
#endif

#endif /* APP_SYSTEM_H_ */

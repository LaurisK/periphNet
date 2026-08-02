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
    eRst_pin  = 0x01,  /*!< NRST pin reset     */
    eRst_por  = 0x02,  /*!< Power-on reset     */
    eRst_sw   = 0x04,  /*!< Software reset     */
    eRst_iwdg = 0x08,  /*!< IWDG reset         */
    eRst_wwdg = 0x10,  /*!< WWDG reset         */
    eRst_bor  = 0x20,  /*!< BOR reset          */
    eRst_lpwr = 0x40,  /*!< Low-power reset    */
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
 * @brief Stop feeding the watchdog (for deliberate watchdog-trigger test).
 *
 * After calling this the device will reset in ≤ 16.4 s.
 */
void System_SetWatchdogTestMode(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SYSTEM_H_ */

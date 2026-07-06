/**
 * @file    system.c
 * @brief   System services – reset cause, SW watchdog (TIM14), IWDG kick
 *
 * IWDG hardware timeout: ~16.4 s (PRESCALER_128, RELOAD=4095, LSI=32 kHz)
 * TIM14 software watchdog: 12 s (fires 4 s before IWDG → time to flush Trice)
 *
 * TIM14 timer clock: 84 MHz (APB1 = 42 MHz × 2 because APB1 divider != 1)
 *   PSC = 83999  →  84 MHz / 84 000 = 1 kHz
 *   ARR = 11999  →  1 kHz / 12 000 = 1/12 Hz → 12 s period
 */

#include "App/system.h"
#include "App/Log/crash.h"
#include "FreeRTOS.h"
#include "task.h"
#include "trice.h"
#include "iwdg.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOSConfig.h"   /* configLIBRARY_LOWEST_INTERRUPT_PRIORITY */

/* External IWDG handle (declared in Core/Src/iwdg.c) */
extern IWDG_HandleTypeDef hiwdg;

/* --------------------------------------------------------------------------
 * Private data
 * -------------------------------------------------------------------------- */

static TIM_HandleTypeDef s_htim14;
static uint32_t          s_resetCause    = 0U;
static volatile uint8_t  s_wdgTestMode   = 0U;

/* --------------------------------------------------------------------------
 * Private functions
 * -------------------------------------------------------------------------- */

static void tim14Init(void)
{
    __HAL_RCC_TIM14_CLK_ENABLE();

    s_htim14.Instance               = TIM14;
    s_htim14.Init.Prescaler         = 83999U;  /* 84 MHz / 84000 = 1 kHz  */
    s_htim14.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim14.Init.Period            = 11999U;  /* 1 kHz / 12000 = 12 s    */
    s_htim14.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim14.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&s_htim14) != HAL_OK) {
        /* Non-fatal: watchdog won't have SW pre-warning, IWDG still active */
        return;
    }

    /* Lowest NVIC priority so it doesn't preempt normal IRQs during flush */
    HAL_NVIC_SetPriority(TIM8_TRG_COM_TIM14_IRQn,
                         configLIBRARY_LOWEST_INTERRUPT_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(TIM8_TRG_COM_TIM14_IRQn);

    HAL_TIM_Base_Start_IT(&s_htim14);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void System_Init(void)
{
    /* Read and clear reset-cause flags before they are lost */
    s_resetCause = RCC->CSR;
    __HAL_RCC_CLEAR_RESET_FLAGS();

    tim14Init();
}

void System_LogResetCause(void)
{
    TRice("Reset cause (RCC_CSR=0x%08X):", s_resetCause);
    if (s_resetCause & RCC_CSR_PINRSTF)   trice(" PIN");
    if (s_resetCause & RCC_CSR_PORRSTF)   trice(" POR");
    if (s_resetCause & RCC_CSR_SFTRSTF)   trice(" SW");
    if (s_resetCause & RCC_CSR_IWDGRSTF)  trice(" IWDG");
    if (s_resetCause & RCC_CSR_WWDGRSTF)  trice(" WWDG");
    if (s_resetCause & RCC_CSR_BORRSTF)   trice(" BOR");
    if (s_resetCause & RCC_CSR_LPWRRSTF)  trice(" LPWR");
    trice("\n");
}

uint32_t System_GetResetCause(void)
{
    return s_resetCause;
}

void KickIwdg(void)
{
    if (s_wdgTestMode) {
        return;
    }
    HAL_IWDG_Refresh(&hiwdg);
    /* Reset TIM14 counter – period restarts from now */
    __HAL_TIM_SET_COUNTER(&s_htim14, 0U);
}

void System_SetWatchdogTestMode(void)
{
    s_wdgTestMode = 1U;
}

/* --------------------------------------------------------------------------
 * TIM14 IRQ handler (called from TIM8_TRG_COM_TIM14_IRQHandler in it.c)
 * -------------------------------------------------------------------------- */

void TIM14_PeriodElapsed_Callback(void)
{
    /* Software watchdog fired – 12 s without a KickIwdg call */
    Crash_GenerateReport(CRASH_SW_WATCHDOG);
    /* IWDG will reset the MCU in ~4 more seconds */
    while (1) {}
}

TIM_HandleTypeDef *System_GetTIM14Handle(void)
{
    return &s_htim14;
}

/* --------------------------------------------------------------------------
 * Fatal-error handlers (configASSERT / FreeRTOS stack overflow check)
 * -------------------------------------------------------------------------- */

/**
 * @brief configASSERT handler — record a crash report and reset.
 *
 * Callable from any context (task, ISR, masked). The reentry guard
 * prevents recursion if the crash reporter itself trips an assert.
 */
void App_AssertFailed(void)
{
    static volatile uint8_t s_inAssert = 0U;

    __disable_irq();
    if (s_inAssert == 0U) {
        s_inAssert = 1U;
        Crash_GenerateReport(CRASH_ASSERT);
    }
    NVIC_SystemReset();
}

/**
 * @brief FreeRTOS stack overflow hook (configCHECK_FOR_STACK_OVERFLOW = 2).
 *
 * Runs in PendSV context while pxCurrentTCB is still the offending
 * task, so the crash report captures the right task name and stacks.
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;

    __disable_irq();
    Crash_GenerateReport(CRASH_STACK_OVERFLOW);
    NVIC_SystemReset();
}

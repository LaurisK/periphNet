/**
 * @file    app_freertos.h
 * @brief   Application-level FreeRTOS task initialisation
 */

#ifndef APP_FREERTOS_H_
#define APP_FREERTOS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create auxiliary application FreeRTOS tasks.
 *
 * Call from MX_FREERTOS_Init() (Core/Src/freertos.c USER CODE Init).
 * Creates the triceTask (Normal+1 priority, 256 words).
 * The default task is created by CubeMX; its body calls App_DefaultTaskEntry().
 */
void App_FreertosInit(void);

/**
 * @brief Default task body – called from CubeMX's StartDefaultTask.
 *
 * MX_LWIP_Init() has already been called by CubeMX before this runs.
 * Initialises system services, then runs the heartbeat / watchdog / button loop.
 * Does NOT return.
 */
void App_DefaultTaskEntry(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_FREERTOS_H_ */

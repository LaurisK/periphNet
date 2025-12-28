/**
 ******************************************************************************
 * @file    trace.h
 * @brief   Trice TCP/IP trace server interface
 * @author  PeriphNet Project
 * @date    2024-12-28
 ******************************************************************************
 * @attention
 *
 * Trice trace functionality over TCP/IP
 * - TCP server on port 61486 (default trice port)
 * - Supports up to 2 simultaneous connections
 * - Periodic trace messages
 *
 ******************************************************************************
 */

#ifndef __TRACE_H
#define __TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "cmsis_os.h"

/* Trace server configuration */
#define TRACE_SERVER_PORT       61486       /* Default trice TCP port */
#define TRACE_MAX_CLIENTS       2           /* Maximum simultaneous connections */
#define TRACE_TASK_PRIORITY     (osPriorityNormal - 1)
#define TRACE_TASK_STACK_SIZE   512         /* Words */

/**
 * @brief  Initialize trace system
 * @note   Call early in main(), before scheduler starts
 * @retval None
 */
void trace_init(void);

/**
 ******************************************************************************
 * Trice Buffer Monitoring API
 * Use these functions to monitor Trice buffer activity
 ******************************************************************************
 */

/**
 * @brief  Check if new trace data is available
 * @return 1 if new data available, 0 otherwise
 * @note   Call trace_clear_data_flag() after processing to reset flag
 */
uint8_t trace_has_new_data(void);

/**
 * @brief  Get length of last trace buffer received from Trice
 * @return Length in bytes of last buffer
 */
size_t trace_get_last_buffer_length(void);

/**
 * @brief  Clear the new data available flag
 * @note   Call after processing new trace data
 */
void trace_clear_data_flag(void);

/**
 * @brief  Get current Trice half-buffer fill level
 * @return Number of bytes used in current half buffer
 * @note   Accesses Trice internal variables
 */
size_t trace_get_buffer_fill_level(void);

#ifdef __cplusplus
}
#endif

#endif /* __TRACE_H */

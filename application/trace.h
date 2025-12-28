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

#ifdef __cplusplus
}
#endif

#endif /* __TRACE_H */

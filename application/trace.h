#ifndef __TRACE_H
#define __TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "cmsis_os.h"

#define TRACE_SERVER_PORT       61486
#define TRACE_MAX_CLIENTS       2
#define TRACE_TASK_PRIORITY     (osPriorityNormal - 1)
#define TRACE_TASK_STACK_SIZE   512

void trace_init(void);
uint8_t trace_has_new_data(void);
size_t trace_get_last_buffer_length(void);
void trace_clear_data_flag(void);
size_t trace_get_buffer_fill_level(void);

#ifdef __cplusplus
}
#endif

#endif /* __TRACE_H */

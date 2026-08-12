#pragma once
#include <stdint.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"

#define TRICE_CONSUMER_UART  0
#define TRICE_CONSUMER_UDP   1
#define TRICE_CONSUMER_COUNT 2

void TriceConsumer_Init(void);
void TriceConsumer_Register(int id, TaskHandle_t task);

void TriceConsumer_Dispatch(const void *data, size_t len);
void TriceConsumer_Done(int id);
void TriceConsumer_ResetAll(void);

unsigned TriceConsumer_Pending(void);

/**
 * @brief  1 if a consumer task registered for @p id, 0 otherwise.
 *
 * A consumer whose task failed to start is indistinguishable from one that is
 * merely idle unless this is exposed: dispatch silently does nothing, and the
 * sink looks dead for no visible reason.
 */
int TriceConsumer_IsRegistered(int id);

const void *TriceConsumer_GetData(void);
size_t      TriceConsumer_GetLen(void);

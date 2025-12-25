/**
 * Mock FreeRTOS.h for host-native unit tests.
 * Maps pvPortMalloc/vPortFree to stdlib malloc/free.
 */
#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H

#include <stdlib.h>

#define pvPortMalloc(sz) malloc(sz)
#define vPortFree(ptr)   free(ptr)

#endif /* MOCK_FREERTOS_H */

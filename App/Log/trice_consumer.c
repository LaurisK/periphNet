#include "App/Log/trice_consumer.h"

static struct {
    const void   *data;
    size_t        len;
    uint32_t      pending_mask;
    TaskHandle_t  tasks[TRICE_CONSUMER_COUNT];
} s_mgr;

void TriceConsumer_Init(void)
{
    s_mgr.data         = NULL;
    s_mgr.len          = 0;
    s_mgr.pending_mask = 0;
    for (int i = 0; i < TRICE_CONSUMER_COUNT; i++) {
        s_mgr.tasks[i] = NULL;
    }
}

void TriceConsumer_Register(int id, TaskHandle_t task)
{
    if (id >= 0 && id < TRICE_CONSUMER_COUNT) {
        s_mgr.tasks[id] = task;
    }
}

void TriceConsumer_Dispatch(const void *data, size_t len)
{
    s_mgr.data = data;
    s_mgr.len  = len;
    s_mgr.pending_mask = TRICE_CONSUMER_MASK;

    if (s_mgr.tasks[TRICE_CONSUMER_UDP] != NULL) {
        xTaskNotifyGive(s_mgr.tasks[TRICE_CONSUMER_UDP]);
    }
}

void TriceConsumer_Done(int id)
{
    if (id >= 0 && id < TRICE_CONSUMER_COUNT) {
        s_mgr.pending_mask &= ~(1u << id);
    }
}

void TriceConsumer_ResetAll(void)
{
    s_mgr.pending_mask = 0;
}

unsigned TriceConsumer_Pending(void)
{
    return s_mgr.pending_mask;
}

int TriceConsumer_IsRegistered(int id)
{
    if (id < 0 || id >= TRICE_CONSUMER_COUNT) {
        return 0;
    }
    return (s_mgr.tasks[id] != NULL) ? 1 : 0;
}

const void *TriceConsumer_GetData(void)
{
    return s_mgr.data;
}

size_t TriceConsumer_GetLen(void)
{
    return s_mgr.len;
}

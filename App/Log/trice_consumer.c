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
    uint32_t mask = TRICE_CONSUMER_MASK;

    s_mgr.data = data;
    s_mgr.len  = len;

    /* THE UDP BIT IS SET ONLY WHEN THE NOTIFY IS ACTUALLY SENT.
     *
     * Only the consumer task clears that bit, so setting it while the task is
     * still NULL leaves a bit nobody will ever clear — and triceTask refuses
     * to call TriceTransfer() while anything is pending, so the whole trace
     * stops permanently after that one dispatch.  That is not a theoretical
     * window: Trice_UdpInit() arms UserNonBlockingDeferredWrite8AuxiliaryFn
     * BEFORE osThreadNew(), the new task shares defaultTask's priority so it
     * does not preempt its creator, and triceTask runs ABOVE both — so a
     * dispatch between the two is the expected interleaving, not a rare one.
     * Losing it costs the few log lines emitted before the task is up, which
     * nothing is listening for yet (the destination list is RAM-only and the
     * board has just booted); winning it cost sodas every trace for 11.8 days.
     *
     * The UART bit is deliberately NOT treated this way: it is cleared by the
     * USART3 TX-complete ISR (HAL_UART_TxCpltCallback), which needs no task
     * and no registration, so gating it on `tasks[]` would disable its
     * back-pressure instead of protecting it. */
    if (s_mgr.tasks[TRICE_CONSUMER_UDP] == NULL) {
        mask &= ~(1u << TRICE_CONSUMER_UDP);
    }

    s_mgr.pending_mask = mask;

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

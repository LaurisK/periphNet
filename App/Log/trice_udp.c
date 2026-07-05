#include "App/Log/trice_udp.h"
#include "App/Log/trice_usb.h"
#include "App/Log/trice_consumer.h"
#include "trice.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "cmsis_os.h"
#include <string.h>

static struct udp_pcb *s_trice_pcb;
static ip_addr_t       s_broadcast_addr;

static void Trice_UdpConsumerTask(void *arg);

void Trice_UdpWrite(const uint8_t *data, size_t len)
{
    if (s_trice_pcb == NULL || len == 0) {
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (p == NULL) {
        return;
    }
    memcpy(p->payload, data, len);

    LOCK_TCPIP_CORE();
    udp_sendto(s_trice_pcb, p, &s_broadcast_addr, TRICE_UDP_PORT);
    UNLOCK_TCPIP_CORE();

    pbuf_free(p);
}

static void Trice_AuxWrite(const uint8_t *data, size_t len)
{
    TriceConsumer_Dispatch(data, len);
    Trice_UsbWrite(data, len);
}

static void Trice_UdpConsumerTask(void *arg)
{
    TriceConsumer_Register(TRICE_CONSUMER_UDP, xTaskGetCurrentTaskHandle());

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        size_t len = TriceConsumer_GetLen();
        if (len) {
            const uint8_t *data = TriceConsumer_GetData();
            Trice_UdpWrite(data, len);
        }
        TriceConsumer_Done(TRICE_CONSUMER_UDP);
    }
}

void Trice_UdpInit(void)
{
    LOCK_TCPIP_CORE();
    s_trice_pcb = udp_new();
    UNLOCK_TCPIP_CORE();

    if (s_trice_pcb == NULL) {
        return;
    }

    IP4_ADDR(&s_broadcast_addr, 255, 255, 255, 255);

    extern Write8AuxiliaryFn_t UserNonBlockingDeferredWrite8AuxiliaryFn;
    UserNonBlockingDeferredWrite8AuxiliaryFn = Trice_AuxWrite;

    osThreadAttr_t attr = {
        .name       = "tudp",
        .stack_size = 256U * 4U,
        .priority   = (osPriority_t)(osPriorityNormal),
    };
    osThreadNew(Trice_UdpConsumerTask, NULL, &attr);
}

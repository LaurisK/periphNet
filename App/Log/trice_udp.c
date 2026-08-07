#include "App/Log/trice_udp.h"
#include "App/Log/trice_usb.h"
#include "App/Log/trice_consumer.h"
#include "trice.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "cmsis_os.h"
#include <string.h>

static struct udp_pcb *s_trice_pcb;
static ip_addr_t       s_dest_addr;

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
    /* Return value ignored on purpose: with a tunnel destination this fails
     * with ERR_RTE whenever the WireGuard netif is down.  Logging must never
     * be able to disturb anything, so a dropped log line is the correct
     * outcome — not a retry, and not an error path. */
    (void)udp_sendto(s_trice_pcb, p, &s_dest_addr, TRICE_UDP_PORT);
    UNLOCK_TCPIP_CORE();

    pbuf_free(p);
}

void Trice_UdpSetDest(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    LOCK_TCPIP_CORE();
    IP4_ADDR(&s_dest_addr, a, b, c, d);
    UNLOCK_TCPIP_CORE();
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

    IP4_ADDR(&s_dest_addr,
             TRICE_UDP_DEFAULT_DEST_0, TRICE_UDP_DEFAULT_DEST_1,
             TRICE_UDP_DEFAULT_DEST_2, TRICE_UDP_DEFAULT_DEST_3);

    extern Write8AuxiliaryFn_t UserNonBlockingDeferredWrite8AuxiliaryFn;
    UserNonBlockingDeferredWrite8AuxiliaryFn = Trice_AuxWrite;

    osThreadAttr_t attr = {
        .name       = "tudp",
        /* Runs the full lwIP raw TX path (udp_sendto → etharp →
         * low_level_output) under the core lock — 1 KB is too tight. */
        .stack_size = 512U * 4U,
        .priority   = (osPriority_t)(osPriorityNormal),
    };
    osThreadNew(Trice_UdpConsumerTask, NULL, &attr);
}

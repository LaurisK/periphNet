#include "App/Log/trice_udp.h"
#include "App/Log/trice_usb.h"
#include "App/Log/trice_consumer.h"
#include "trice.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "cmsis_os.h"
#include <string.h>

/** One destination slot.  @c seq doubles as the in-use marker (0 = free) and
 *  as the eviction order, so a full list drops its oldest non-sticky entry. */
typedef struct {
    ip_addr_t addr;
    uint32_t  seq;
    uint8_t   sticky;
} sTriceUdpDest;

static struct udp_pcb *s_trice_pcb;
static sTriceUdpDest   s_dest[TRICE_UDP_MAX_DEST];
static uint32_t        s_destSeq;
static uint32_t        s_sentCount;
static uint32_t        s_failedCount;

static void Trice_UdpConsumerTask(void *arg);

/* Callers already hold LOCK_TCPIP_CORE. */
static int dest_find(const ip_addr_t *addr)
{
    for (uint32_t i = 0u; i < TRICE_UDP_MAX_DEST; i++) {
        if (s_dest[i].seq != 0u && ip_addr_cmp(&s_dest[i].addr, addr)) {
            return (int)i;
        }
    }
    return -1;
}

static int dest_add(const ip_addr_t *addr, uint8_t sticky)
{
    int slot = dest_find(addr);

    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;

        for (uint32_t i = 0u; i < TRICE_UDP_MAX_DEST; i++) {
            if (s_dest[i].seq == 0u) {
                slot = (int)i;
                break;
            }
            /* Sticky entries are the ones that work without configuration;
             * evicting them to make room for a subscriber would silently undo
             * the property the default exists to provide. */
            if (!s_dest[i].sticky && s_dest[i].seq < oldest) {
                oldest = s_dest[i].seq;
                slot   = (int)i;
            }
        }
        if (slot < 0) {
            return -1;
        }
        ip_addr_copy(s_dest[slot].addr, *addr);
        s_dest[slot].sticky = sticky;
    }

    s_dest[slot].seq = ++s_destSeq;
    return slot;
}

void Trice_UdpWrite(const uint8_t *data, size_t len)
{
    if (s_trice_pcb == NULL || len == 0) {
        return;
    }

    LOCK_TCPIP_CORE();
    /* One FRESH pbuf per destination — never reuse one across the loop.
     *
     * The STM32 ETH driver is zero-copy: low_level_output() hands the pbuf to a
     * DMA descriptor and keeps a reference until the TX-complete interrupt, so
     * the buffer is still in flight when udp_sendto() returns.  Feeding that
     * same pbuf to the next destination corrupts it, and every send that
     * egressed the Ethernet netif failed (the WireGuard netif hid the bug — it
     * encrypts into a pbuf of its own and never retains this one).
     *
     * A failure must never disturb anything — with a tunnel destination this
     * returns ERR_RTE whenever the WireGuard netif is down, and a dropped log
     * line is the correct outcome, not a retry.  It is counted rather than
     * discarded so "the transport is trying but the route is dead" can be told
     * apart from "nothing is being logged at all". */
    for (uint32_t i = 0u; i < TRICE_UDP_MAX_DEST; i++) {
        struct pbuf *p;

        if (s_dest[i].seq == 0u) {
            continue;
        }

        p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (p == NULL) {
            s_failedCount++;
            continue;
        }
        memcpy(p->payload, data, len);

        if (udp_sendto(s_trice_pcb, p, &s_dest[i].addr, TRICE_UDP_PORT) == ERR_OK) {
            s_sentCount++;
        } else {
            s_failedCount++;
        }
        pbuf_free(p);
    }
    UNLOCK_TCPIP_CORE();
}

int Trice_UdpIsReady(void)
{
    return (s_trice_pcb != NULL) ? 1 : 0;
}

void Trice_UdpGetStats(uint32_t *sent, uint32_t *failed)
{
    if (sent != NULL) {
        *sent = s_sentCount;
    }
    if (failed != NULL) {
        *failed = s_failedCount;
    }
}

int Trice_UdpAddDest(const ip_addr_t *addr)
{
    int slot;

    if (addr == NULL || ip_addr_isany(addr)) {
        return -1;
    }

    LOCK_TCPIP_CORE();
    slot = dest_add(addr, 0u);
    UNLOCK_TCPIP_CORE();

    return slot;
}

int Trice_UdpRemoveDest(const ip_addr_t *addr)
{
    int slot;

    if (addr == NULL) {
        return -1;
    }

    LOCK_TCPIP_CORE();
    slot = dest_find(addr);
    if (slot >= 0) {
        s_dest[slot].seq    = 0u;
        s_dest[slot].sticky = 0u;
    }
    UNLOCK_TCPIP_CORE();

    return (slot >= 0) ? 0 : -1;
}

void Trice_UdpResetDests(void)
{
    ip_addr_t bcast;

    IP4_ADDR(&bcast, 255, 255, 255, 255);

    LOCK_TCPIP_CORE();
    memset(s_dest, 0, sizeof(s_dest));
    (void)dest_add(&bcast, 1u);
    UNLOCK_TCPIP_CORE();
}

uint32_t Trice_UdpGetDests(ip_addr_t *out, uint32_t max)
{
    uint32_t n = 0u;

    if (out == NULL) {
        return 0u;
    }

    LOCK_TCPIP_CORE();
    for (uint32_t i = 0u; i < TRICE_UDP_MAX_DEST && n < max; i++) {
        if (s_dest[i].seq != 0u) {
            ip_addr_copy(out[n], s_dest[i].addr);
            n++;
        }
    }
    UNLOCK_TCPIP_CORE();

    return n;
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
    if (s_trice_pcb != NULL) {
        /* IP_SOF_BROADCAST is 0 in this build, so the per-pcb broadcast filter
         * is compiled out — but setting the flag anyway keeps the default slot
         * working if that option is ever turned on. */
        ip_set_option(s_trice_pcb, SOF_BROADCAST);
    }
    UNLOCK_TCPIP_CORE();

    if (s_trice_pcb == NULL) {
        return;
    }

    Trice_UdpResetDests();

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

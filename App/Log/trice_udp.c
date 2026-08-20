#include "App/Log/trice_udp.h"
#include "App/Log/trice_usb.h"
#include "App/Log/trice_consumer.h"
#include "App/Mon/sysmon.h"
#include "App/nv_record.h"
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

/* Persisted destination list.  Its own nvDb user: where the logs go is
 * per-site data, and it must not be lost with the power. */
#define TRICE_DEST_MAGIC   0x54554450u   /* "TUDP" */
#define TRICE_DEST_VERSION 1u

typedef struct {
    sNvRecordHdr hdr;
    uint8_t      count;
    uint8_t      _reserved[3];
    uint32_t     ip4[TRICE_UDP_MAX_DEST];   /* network order, as ip_addr_t   */
    uint8_t      sticky[TRICE_UDP_MAX_DEST];
} sTriceDestRecord;

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

/**
 * @brief Restore persisted destinations on top of the default list.
 * @retval how many were restored
 * @note Additive on purpose: the broadcast default is what makes an
 *       unconfigured board observable, and a saved list must not silently
 *       take that away.
 */
static uint32_t dests_load(void)
{
    sTriceDestRecord rec;
    uint32_t         n = 0u;
    uint32_t         i = 0u;

    if (NvRecord_Load(nvdbUser_triceUdpCfg, TRICE_DEST_MAGIC,
                      TRICE_DEST_VERSION, &rec, sizeof(rec)) != 0) {
        return 0u;
    }
    if (rec.count > TRICE_UDP_MAX_DEST) {
        return 0u;
    }

    LOCK_TCPIP_CORE();
    for (i = 0u; i < rec.count; i++) {
        ip_addr_t addr;
        ip_addr_set_ip4_u32(&addr, rec.ip4[i]);
        if (dest_add(&addr, rec.sticky[i]) >= 0) {
            n++;
        }
    }
    UNLOCK_TCPIP_CORE();
    return n;
}

int Trice_UdpSaveDests(void)
{
    sTriceDestRecord rec;
    uint32_t         i = 0u;

    memset(&rec, 0, sizeof(rec));

    LOCK_TCPIP_CORE();
    for (i = 0u; i < TRICE_UDP_MAX_DEST; i++) {
        if (s_dest[i].seq == 0u) {
            continue;
        }
        rec.ip4[rec.count]    = ip_addr_get_ip4_u32(&s_dest[i].addr);
        rec.sticky[rec.count] = s_dest[i].sticky;
        rec.count++;
    }
    UNLOCK_TCPIP_CORE();

    return NvRecord_Save(nvdbUser_triceUdpCfg, TRICE_DEST_MAGIC,
                         TRICE_DEST_VERSION, &rec, sizeof(rec));
}

int Trice_UdpForgetDests(void)
{
    return NvRecord_Forget(nvdbUser_triceUdpCfg);
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

    /* Purely notification-driven: with nothing being logged it legitimately
     * blocks forever, so no deadline — stack and CPU only. */
    int8_t monId = SysMon_TaskRegister(512U, 0U);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        SysMon_TaskCheckin(monId);
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

    /* Whoever was listening before the reset gets their logs back without
     * anyone having to plug a cable in. */
    {
        uint32_t restored = dests_load();
        if (restored > 0u) {
            TRice("Trice UDP: %u destination(s) restored\n", restored);
        }
    }

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

/**
 * @file    trice_udp.c
 * @brief   Trice UDP transport — broadcasts TCOBS-encoded trice data
 */

#include "App/Log/trice_udp.h"
#include "trice.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include <string.h>

static struct udp_pcb *s_trice_pcb;
static ip_addr_t       s_broadcast_addr;

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

void Trice_UdpInit(void)
{
    LOCK_TCPIP_CORE();
    s_trice_pcb = udp_new();
    UNLOCK_TCPIP_CORE();

    if (s_trice_pcb == NULL) {
        return;
    }

    IP4_ADDR(&s_broadcast_addr, 255, 255, 255, 255);

    /* Register as trice auxiliary output */
    extern Write8AuxiliaryFn_t UserNonBlockingDeferredWrite8AuxiliaryFn;
    UserNonBlockingDeferredWrite8AuxiliaryFn = Trice_UdpWrite;
}

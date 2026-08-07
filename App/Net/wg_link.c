/**
 * @file    wg_link.c
 * @brief   WireGuard tunnel netif — see wg_link.h.
 */

#include "wg_link.h"

#include <string.h>

#include "lwip/netif.h"
#include "lwip/ip.h"
#include "lwip/tcpip.h"
#include "wireguardif.h"
#include "trice.h"

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static struct netif s_wgNetif;
static uint8_t      s_peerIndex = WIREGUARDIF_INVALID_INDEX;
static uint8_t      s_running;

/* wireguardif keeps a pointer to this for the lifetime of the netif, so it
 * must not live on the caller's stack.
 */
static struct wireguardif_init_data s_initData;

/* --------------------------------------------------------------------------
 * Defaults
 * -------------------------------------------------------------------------- */

/* Provisioned key for board #1, issued by WGDashboard on 2026-08-07.
 *
 * !!! BRING-UP ONLY !!!  A private key as a build constant means every unit
 * flashed with this image shares one identity, and the key is recoverable
 * from any .bin.  Production needs a per-device key generated on-device and
 * stored outside the image (EEPROM/ext-flash) — and NOT via the FWU key
 * mechanism, which is bootloader-only by design.
 */
static const sWgLinkCfg s_defaultCfg = {
    .privateKey    = "0DT2RqlrHFSVWcFMeKjRBSPgZkO/hIPe9bds/hTzs0A=",
    .peerPublicKey = "wL7FWpGpruu5iKEErh/UipbW3ooQlz+ZGsw9Bcvw82k=",
    .tunnelIp      = { 10, 77, 0, 64 },
    .tunnelMask    = { 255, 255, 255, 0 },
    .endpointIp    = { 85, 206, 57, 75 },
    .endpointPort  = 51820u,
    .keepAlive     = 21u,
};

const sWgLinkCfg *WgLink_DefaultCfg(void)
{
    return &s_defaultCfg;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int WgLink_Start(const sWgLinkCfg *cfg)
{
    ip4_addr_t tunnelIp;
    ip4_addr_t tunnelMask;
    ip4_addr_t gateway;
    struct wireguardif_peer peer;
    struct netif *added;

    if (s_running) {
        return -1;
    }
    if (cfg == NULL) {
        cfg = &s_defaultCfg;
    }

    IP4_ADDR(&tunnelIp, cfg->tunnelIp[0], cfg->tunnelIp[1],
                        cfg->tunnelIp[2], cfg->tunnelIp[3]);
    IP4_ADDR(&tunnelMask, cfg->tunnelMask[0], cfg->tunnelMask[1],
                          cfg->tunnelMask[2], cfg->tunnelMask[3]);
    ip4_addr_set_zero(&gateway);

    s_initData.private_key = cfg->privateKey;
    s_initData.listen_port = WIREGUARDIF_DEFAULT_PORT;
    s_initData.bind_netif  = NULL;   /* follow the routing table */

    LOCK_TCPIP_CORE();

    added = netif_add(&s_wgNetif, &tunnelIp, &tunnelMask, &gateway,
                      &s_initData, &wireguardif_init, &ip_input);
    if (added == NULL) {
        UNLOCK_TCPIP_CORE();
        TRice("WG: netif_add failed\n");
        return -2;
    }

    /* Deliberately NOT netif_set_default() — the Ethernet netif stays the
     * default route so on-LAN traffic and the encapsulated WireGuard UDP
     * itself keep taking the short path.  Only the tunnel subnet, which this
     * netif's address/netmask covers, routes through here.
     */
    netif_set_up(&s_wgNetif);

    wireguardif_peer_init(&peer);
    peer.public_key   = cfg->peerPublicKey;
    peer.preshared_key = NULL;
    /* Accept/route the whole tunnel subnet via this peer. */
    ip_addr_copy(peer.allowed_ip, *IP_ADDR_ANY);
    IP_ADDR4(&peer.allowed_ip, cfg->tunnelIp[0], cfg->tunnelIp[1],
                               cfg->tunnelIp[2], 0);
    IP_ADDR4(&peer.allowed_mask, cfg->tunnelMask[0], cfg->tunnelMask[1],
                                 cfg->tunnelMask[2], cfg->tunnelMask[3]);
    IP_ADDR4(&peer.endpoint_ip, cfg->endpointIp[0], cfg->endpointIp[1],
                                cfg->endpointIp[2], cfg->endpointIp[3]);
    peer.endport_port = cfg->endpointPort;
    peer.keep_alive   = cfg->keepAlive;

    if (wireguardif_add_peer(&s_wgNetif, &peer, &s_peerIndex) != ERR_OK ||
        s_peerIndex == WIREGUARDIF_INVALID_INDEX) {
        netif_remove(&s_wgNetif);
        UNLOCK_TCPIP_CORE();
        TRice("WG: add_peer failed\n");
        return -3;
    }

    /* Starts the outbound handshake; retries on lwIP timers if the hub is
     * unreachable, so this returns immediately either way.
     */
    (void)wireguardif_connect(&s_wgNetif, s_peerIndex);

    UNLOCK_TCPIP_CORE();

    s_running = 1u;
    TRice("WG: tunnel netif up, %d.%d.%d.%d -> %d.%d.%d.%d:%d\n",
          cfg->tunnelIp[0], cfg->tunnelIp[1], cfg->tunnelIp[2], cfg->tunnelIp[3],
          cfg->endpointIp[0], cfg->endpointIp[1], cfg->endpointIp[2],
          cfg->endpointIp[3], cfg->endpointPort);
    return 0;
}

void WgLink_Stop(void)
{
    if (!s_running) {
        return;
    }

    LOCK_TCPIP_CORE();
    if (s_peerIndex != WIREGUARDIF_INVALID_INDEX) {
        (void)wireguardif_disconnect(&s_wgNetif, s_peerIndex);
        (void)wireguardif_remove_peer(&s_wgNetif, s_peerIndex);
        s_peerIndex = WIREGUARDIF_INVALID_INDEX;
    }
    netif_remove(&s_wgNetif);
    UNLOCK_TCPIP_CORE();

    s_running = 0u;
    TRice("WG: tunnel stopped\n");
}

int WgLink_IsRunning(void)
{
    return (int)s_running;
}

int WgLink_IsUp(void)
{
    err_t err;

    if (!s_running || s_peerIndex == WIREGUARDIF_INVALID_INDEX) {
        return 0;
    }

    LOCK_TCPIP_CORE();
    err = wireguardif_peer_is_up(&s_wgNetif, s_peerIndex, NULL, NULL);
    UNLOCK_TCPIP_CORE();

    return (err == ERR_OK) ? 1 : 0;
}

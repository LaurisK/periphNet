/**
 * @file    wg_link.c
 * @brief   WireGuard tunnel netif — see wg_link.h.
 */

#include "App/Net/wg_link.h"
#include "App/Net/wg_cfg.h"

#include <string.h>

#include "lwip/netif.h"
#include "lwip/ip.h"
#include "lwip/tcpip.h"
#include "wireguardif.h"
#include "wireguard.h"
#include "crypto.h"
#include "wireguard-platform.h"
#include "trice.h"

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static struct netif s_wgNetif;
static uint8_t      s_peerIndex = WIREGUARDIF_INVALID_INDEX;
static uint8_t      s_running;

/* The netif is created once and then reconfigured in place, never removed.
 *
 * netif_remove() would leak: the port allocates its wireguard_device with
 * mem_calloc() and a UDP PCB inside wireguardif_init(), and offers no
 * shutdown entry point to undo either — MEMP_NUM_UDP_PCB is 8, so a handful
 * of stop/start cycles exhaust the pool and the tunnel stops coming back.
 * Freeing them from here is not an option either: wireguardif_init() also
 * arms a self-rearming sys_timeout() holding the device pointer, and that
 * callback is static to the port, so the timer would fire on freed memory.
 * Reconfiguring in place sidesteps both — wireguard_device_init() rewrites
 * only key material, leaving netif, udp_pcb and peers intact. */
static uint8_t      s_netifCreated;

/* wireguardif keeps a pointer to this for the lifetime of the netif, so it
 * must not live on the caller's stack. */
static struct wireguardif_init_data s_initData;

/* The port takes keys as base64 strings; the configuration holds them raw.
 * These are the encoded copies it keeps pointers to. */
static char s_privKeyB64[WG_KEY_B64_SIZE];

static sWgLinkCfg s_cfg;
static uint8_t    s_cfgLoaded;
static uint8_t    s_cfgStored;   /* active cfg came from flash */

/* --------------------------------------------------------------------------
 * Configuration loading
 *
 * There are deliberately NO built-in keys.  A board with no stored identity
 * does not start the tunnel: falling back to a shared key would put two boards
 * on one hub peer entry, where they take turns stealing the endpoint and the
 * hub's greatest-timestamp rule locks the loser out — connected-looking and
 * completely dead, which is the worst kind of failure to diagnose.
 * -------------------------------------------------------------------------- */

static const sWgLinkCfg s_defaultCfg = {
    .endpointPort  = 51820u,   /* the assigned WireGuard port; harmless */
    .keepAlive_sec = 0u,
};

static void cfg_ensure_loaded(void)
{
    if (s_cfgLoaded) {
        return;
    }
    s_cfg       = s_defaultCfg;
    s_cfgLoaded = 1u;

    if (WgCfg_Load(&s_cfg) == 0) {
        s_cfgStored = 1u;
        TRice("WG: stored config v%u, %d.%d.%d.%d key=%u\n",
              (unsigned)WgCfg_StoredVersion(),
              s_cfg.tunnelIp[0], s_cfg.tunnelIp[1],
              s_cfg.tunnelIp[2], s_cfg.tunnelIp[3],
              (unsigned)s_cfg.hasPrivateKey);
    } else {
        s_cfgStored = 0u;
        TRice("WG: no stored config — tunnel stays down until provisioned\n");
    }
}

const sWgLinkCfg *WgLink_ActiveCfg(void)
{
    cfg_ensure_loaded();
    return &s_cfg;
}

int WgLink_HasIdentity(void)
{
    cfg_ensure_loaded();
    return (s_cfg.hasPrivateKey && s_cfg.hasPeerKey) ? 1 : 0;
}

int WgLink_GetPeerStats(sWgPeerStats *out)
{
    struct wireguard_device *dev;
    struct wireguard_peer   *peer;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (!s_running || s_peerIndex == WIREGUARDIF_INVALID_INDEX) {
        return -2;
    }

    LOCK_TCPIP_CORE();
    dev = (struct wireguard_device *)s_wgNetif.state;
    if (dev != NULL && s_peerIndex < WIREGUARD_MAX_PEERS) {
        peer = &dev->peers[s_peerIndex];

        out->sessionValid = peer->curr_keypair.valid ? 1u : 0u;
        out->lastRx_ms    = peer->last_rx;
        out->lastTx_ms    = peer->last_tx;
        out->txPackets    = (uint32_t)peer->curr_keypair.sending_counter;
        out->rxCounter    = (uint32_t)peer->curr_keypair.replay_counter;

        /* peer->ip is the endpoint the port is actually sending to, which is
         * not necessarily the configured one — it follows the source of the
         * last valid handshake, so a roaming or NAT-remapped hub shows up
         * here and nowhere else. */
        out->endpointIp[0] = (uint8_t)(ip4_addr_get_u32(ip_2_ip4(&peer->ip)) >>  0);
        out->endpointIp[1] = (uint8_t)(ip4_addr_get_u32(ip_2_ip4(&peer->ip)) >>  8);
        out->endpointIp[2] = (uint8_t)(ip4_addr_get_u32(ip_2_ip4(&peer->ip)) >> 16);
        out->endpointIp[3] = (uint8_t)(ip4_addr_get_u32(ip_2_ip4(&peer->ip)) >> 24);
        out->endpointPort  = peer->port;
    }
    UNLOCK_TCPIP_CORE();

    return 0;
}

/* --------------------------------------------------------------------------
 * Keys
 * -------------------------------------------------------------------------- */

int WgLink_GetPublicKeyB64(char *out, uint32_t outSz)
{
    static const uint8_t basepoint[WG_CONF_KEY_SIZE] = { 9 };
    uint8_t pub[WG_CONF_KEY_SIZE];

    if (out == NULL || outSz < WG_KEY_B64_SIZE) {
        return -1;
    }
    cfg_ensure_loaded();
    if (!s_cfg.hasPrivateKey) {
        return -2;
    }
    if (wireguard_x25519(pub, s_cfg.privateKey, basepoint) != 0) {
        return -3;
    }
    return WgConf_Base64Encode(pub, sizeof(pub), out, outSz);
}

int WgLink_GetPeerKeyB64(char *out, uint32_t outSz)
{
    if (out == NULL || outSz < WG_KEY_B64_SIZE) {
        return -1;
    }
    cfg_ensure_loaded();
    if (!s_cfg.hasPeerKey) {
        return -2;
    }
    return WgConf_Base64Encode(s_cfg.peerPublicKey,
                               (uint32_t)sizeof(s_cfg.peerPublicKey),
                               out, outSz);
}

/* --------------------------------------------------------------------------
 * Route hook
 *
 * lwIP picks an output netif by matching the destination against each netif's
 * address and mask.  Every .conf gives the tunnel interface a /32, so that
 * match would never fire and nothing would ever route through the tunnel —
 * the routes live in the peer's AllowedIPs instead, exactly as wg-quick
 * installs them separately from the interface address.
 * -------------------------------------------------------------------------- */

struct netif *WgLink_Ip4Route(const struct ip4_addr *dest)
{
    uint32_t d;
    uint8_t  i;

    if (!s_running || dest == NULL) {
        return NULL;
    }

    d = ((const ip4_addr_t *)dest)->addr;

    /* Never swallow traffic to the hub's public address: that is the
     * encapsulated UDP itself, and routing it into the tunnel would be a
     * loop that takes the link down rather than degrading it.  A config with
     * an over-broad AllowedIPs (0.0.0.0/0) would otherwise do exactly that. */
    {
        ip4_addr_t ep;
        IP4_ADDR(&ep, s_cfg.endpointIp[0], s_cfg.endpointIp[1],
                      s_cfg.endpointIp[2], s_cfg.endpointIp[3]);
        if (d == ep.addr) {
            return NULL;
        }
    }

    for (i = 0u; i < s_cfg.allowedCount; i++) {
        ip4_addr_t net;
        ip4_addr_t mask;

        IP4_ADDR(&net, s_cfg.allowed[i].ip[0], s_cfg.allowed[i].ip[1],
                       s_cfg.allowed[i].ip[2], s_cfg.allowed[i].ip[3]);
        IP4_ADDR(&mask, s_cfg.allowed[i].mask[0], s_cfg.allowed[i].mask[1],
                        s_cfg.allowed[i].mask[2], s_cfg.allowed[i].mask[3]);

        if ((d & mask.addr) == (net.addr & mask.addr)) {
            return &s_wgNetif;
        }
    }

    return NULL;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/* Install allowed ranges beyond the first.  wireguardif_add_peer() takes only
 * one, but the port's peer holds WIREGUARD_MAX_SRC_IPS of them and the
 * lookup walks the whole array, so the extra slots are filled directly rather
 * than patching the submodule. */
static void peer_add_extra_ranges(void)
{
    struct wireguard_device *dev = (struct wireguard_device *)s_wgNetif.state;
    struct wireguard_peer   *peer;
    uint8_t                  i;
    int                      slot = 1;

    if (dev == NULL || s_peerIndex >= WIREGUARD_MAX_PEERS) {
        return;
    }
    peer = &dev->peers[s_peerIndex];

    for (i = 1u; i < s_cfg.allowedCount && slot < WIREGUARD_MAX_SRC_IPS; i++) {
        ip_addr_t ip;
        ip_addr_t mask;

        IP_ADDR4(&ip, s_cfg.allowed[i].ip[0], s_cfg.allowed[i].ip[1],
                      s_cfg.allowed[i].ip[2], s_cfg.allowed[i].ip[3]);
        IP_ADDR4(&mask, s_cfg.allowed[i].mask[0], s_cfg.allowed[i].mask[1],
                        s_cfg.allowed[i].mask[2], s_cfg.allowed[i].mask[3]);

        peer->allowed_source_ips[slot].valid = true;
        peer->allowed_source_ips[slot].ip    = ip;
        peer->allowed_source_ips[slot].mask  = mask;
        slot++;
    }
}

int WgLink_Start(const sWgLinkCfg *cfg)
{
    ip4_addr_t tunnelIp;
    ip4_addr_t tunnelMask;
    ip4_addr_t gateway;
    struct wireguardif_peer peer;
    struct netif *added;
    char peerKeyB64[WG_KEY_B64_SIZE];

    if (s_running) {
        return -1;
    }
    if (cfg != NULL) {
        s_cfg       = *cfg;
        s_cfgLoaded = 1u;
    } else {
        cfg_ensure_loaded();
    }

    if (!s_cfg.hasPrivateKey || !s_cfg.hasPeerKey) {
        TRice("WG: not provisioned (upload a .conf) — tunnel not started\n");
        return -4;
    }
    if ((s_cfg.tunnelIp[0] | s_cfg.tunnelIp[1] |
         s_cfg.tunnelIp[2] | s_cfg.tunnelIp[3]) == 0u) {
        TRice("WG: no tunnel address configured\n");
        return -5;
    }
    if (s_cfg.allowedCount == 0u) {
        TRice("WG: no AllowedIPs configured\n");
        return -6;
    }

    if (WgConf_Base64Encode(s_cfg.privateKey, WG_CONF_KEY_SIZE,
                            s_privKeyB64, sizeof(s_privKeyB64)) != 0 ||
        WgConf_Base64Encode(s_cfg.peerPublicKey, WG_CONF_KEY_SIZE,
                            peerKeyB64, sizeof(peerKeyB64)) != 0) {
        return -7;
    }

    IP4_ADDR(&tunnelIp, s_cfg.tunnelIp[0], s_cfg.tunnelIp[1],
                        s_cfg.tunnelIp[2], s_cfg.tunnelIp[3]);
    IP4_ADDR(&tunnelMask, s_cfg.tunnelMask[0], s_cfg.tunnelMask[1],
                          s_cfg.tunnelMask[2], s_cfg.tunnelMask[3]);
    ip4_addr_set_zero(&gateway);

    s_initData.private_key = s_privKeyB64;
    /* Ephemeral source port, NOT WIREGUARDIF_DEFAULT_PORT.  A client has no
     * reason to listen on the server's port, and binding to 51820 breaks any
     * board sharing a site with the hub: the traffic hairpins through the site
     * router, so the endpoint the hub learns is its own public IP:51820 and it
     * sends every reply back into its own listening socket.  udp_bind() with
     * port 0 picks a free port, which is indistinguishable from any other
     * roaming client and keeps the portable public endpoint working. */
    s_initData.listen_port = 0u;
    s_initData.bind_netif  = NULL;   /* follow the routing table */

    LOCK_TCPIP_CORE();

    if (!s_netifCreated) {
        added = netif_add(&s_wgNetif, &tunnelIp, &tunnelMask, &gateway,
                          &s_initData, &wireguardif_init, &ip_input);
        if (added == NULL) {
            UNLOCK_TCPIP_CORE();
            TRice("WG: netif_add failed\n");
            return -2;
        }
        s_netifCreated = 1u;
    } else {
        /* Reconfigure the existing netif rather than recreating it — see the
         * note on s_netifCreated.  The address may have changed, and the
         * identity key may have been replaced by an upload or genkey. */
        struct wireguard_device *dev =
            (struct wireguard_device *)s_wgNetif.state;

        netif_set_addr(&s_wgNetif, &tunnelIp, &tunnelMask, &gateway);
        if (dev == NULL || !wireguard_device_init(dev, s_cfg.privateKey)) {
            UNLOCK_TCPIP_CORE();
            TRice("WG: device re-init failed\n");
            return -8;
        }
    }

    /* Deliberately NOT netif_set_default() — the Ethernet netif stays the
     * default route so on-LAN traffic and the encapsulated WireGuard UDP
     * itself keep taking the short path.  What routes through here is
     * decided by WgLink_Ip4Route() from the peer's allowed ranges. */
    netif_set_up(&s_wgNetif);

    /* A peer left over from a previous run would keep its stale allowed
     * ranges and endpoint. */
    if (s_peerIndex != WIREGUARDIF_INVALID_INDEX) {
        (void)wireguardif_remove_peer(&s_wgNetif, s_peerIndex);
        s_peerIndex = WIREGUARDIF_INVALID_INDEX;
    }

    wireguardif_peer_init(&peer);
    peer.public_key    = peerKeyB64;
    peer.preshared_key = NULL;
    IP_ADDR4(&peer.allowed_ip, s_cfg.allowed[0].ip[0], s_cfg.allowed[0].ip[1],
                               s_cfg.allowed[0].ip[2], s_cfg.allowed[0].ip[3]);
    IP_ADDR4(&peer.allowed_mask,
             s_cfg.allowed[0].mask[0], s_cfg.allowed[0].mask[1],
             s_cfg.allowed[0].mask[2], s_cfg.allowed[0].mask[3]);
    IP_ADDR4(&peer.endpoint_ip, s_cfg.endpointIp[0], s_cfg.endpointIp[1],
                                s_cfg.endpointIp[2], s_cfg.endpointIp[3]);
    peer.endport_port = s_cfg.endpointPort;
    peer.keep_alive   = s_cfg.keepAlive_sec;

    if (wireguardif_add_peer(&s_wgNetif, &peer, &s_peerIndex) != ERR_OK ||
        s_peerIndex == WIREGUARDIF_INVALID_INDEX) {
        netif_set_down(&s_wgNetif);
        UNLOCK_TCPIP_CORE();
        TRice("WG: add_peer failed\n");
        return -3;
    }

    peer_add_extra_ranges();

    /* Starts the outbound handshake; retries on lwIP timers if the hub is
     * unreachable, so this returns immediately either way. */
    (void)wireguardif_connect(&s_wgNetif, s_peerIndex);

    UNLOCK_TCPIP_CORE();

    s_running = 1u;
    TRice("WG: up, %d.%d.%d.%d -> %d.%d.%d.%d:%d, %u range(s)\n",
          s_cfg.tunnelIp[0], s_cfg.tunnelIp[1],
          s_cfg.tunnelIp[2], s_cfg.tunnelIp[3],
          s_cfg.endpointIp[0], s_cfg.endpointIp[1],
          s_cfg.endpointIp[2], s_cfg.endpointIp[3],
          s_cfg.endpointPort, (unsigned)s_cfg.allowedCount);
    return 0;
}

void WgLink_Stop(void)
{
    if (!s_running) {
        return;
    }

    /* Cleared before the netif goes down so the route hook, which may run on
     * tcpip_thread at any moment, stops handing this netif out. */
    s_running = 0u;

    LOCK_TCPIP_CORE();
    if (s_peerIndex != WIREGUARDIF_INVALID_INDEX) {
        (void)wireguardif_disconnect(&s_wgNetif, s_peerIndex);
        (void)wireguardif_remove_peer(&s_wgNetif, s_peerIndex);
        s_peerIndex = WIREGUARDIF_INVALID_INDEX;
    }
    /* Down, not removed — the port's device, UDP PCB and periodic timer
     * outlive this and are reused on the next start. */
    netif_set_down(&s_wgNetif);
    UNLOCK_TCPIP_CORE();

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

/* Re-create the netif so new parameters take effect.  Changing a running
 * netif in place would leave the peer's allowed ranges stale, so a stop/start
 * is both simpler and less error-prone. */
static int cfg_reapply(void)
{
    if (!s_running) {
        return 0;
    }
    WgLink_Stop();
    return WgLink_Start(NULL);
}

/* --------------------------------------------------------------------------
 * Provisioning
 * -------------------------------------------------------------------------- */

int WgLink_ApplyConf(const sWgConf *conf, int save)
{
    uint8_t i;

    if (conf == NULL) {
        return -1;
    }

    cfg_ensure_loaded();

    memcpy(s_cfg.privateKey,    conf->privateKey,    WG_CONF_KEY_SIZE);
    memcpy(s_cfg.peerPublicKey, conf->peerPublicKey, WG_CONF_KEY_SIZE);
    s_cfg.hasPrivateKey = 1u;
    s_cfg.hasPeerKey    = 1u;

    memcpy(s_cfg.tunnelIp,   conf->tunnelIp,   4u);
    memcpy(s_cfg.tunnelMask, conf->tunnelMask, 4u);
    memcpy(s_cfg.endpointIp, conf->endpointIp, 4u);
    s_cfg.endpointPort  = conf->endpointPort;
    s_cfg.keepAlive_sec = conf->keepAlive_sec;

    s_cfg.allowedCount = (conf->allowedCount > (uint8_t)WG_CONF_MAX_ALLOWED)
                             ? (uint8_t)WG_CONF_MAX_ALLOWED
                             : conf->allowedCount;
    for (i = 0u; i < s_cfg.allowedCount; i++) {
        s_cfg.allowed[i] = conf->allowed[i];
    }

    if (save) {
        if (WgCfg_Save(&s_cfg) != 0) {
            return -2;
        }
        s_cfgStored = 1u;
    }

    /* Bring the tunnel up even if it was down: applying a config is exactly
     * the moment an unprovisioned board becomes able to connect. */
    if (s_running) {
        WgLink_Stop();
    }
    return WgLink_Start(NULL);
}

int WgLink_GenerateKey(int save)
{
    uint8_t key[WG_CONF_KEY_SIZE];

    cfg_ensure_loaded();

    wireguard_random_bytes(key, sizeof(key));
    /* Curve25519 scalar clamping, per the WireGuard spec. */
    key[0]  &= 248u;
    key[31] = (uint8_t)((key[31] & 127u) | 64u);

    memcpy(s_cfg.privateKey, key, sizeof(key));
    s_cfg.hasPrivateKey = 1u;

    if (save) {
        if (WgCfg_Save(&s_cfg) != 0) {
            return -2;
        }
        s_cfgStored = 1u;
    }

    return cfg_reapply();
}

/* --------------------------------------------------------------------------
 * Individual settings
 * -------------------------------------------------------------------------- */

int WgLink_SetTunnelIp(const uint8_t ip[4], const uint8_t mask[4])
{
    if (ip == NULL) {
        return -1;
    }
    /* 0.0.0.0 would make netif_add succeed but route nothing. */
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0u) {
        return -1;
    }
    if (mask != NULL && (mask[0] | mask[1] | mask[2] | mask[3]) == 0u) {
        return -1;
    }

    cfg_ensure_loaded();
    memcpy(s_cfg.tunnelIp, ip, 4u);
    if (mask != NULL) {
        memcpy(s_cfg.tunnelMask, mask, 4u);
    }

    return cfg_reapply();
}

int WgLink_SetEndpoint(const uint8_t ip[4], uint16_t port)
{
    ip_addr_t addr;
    err_t     err;

    if (ip == NULL || port == 0u) {
        return -1;
    }

    cfg_ensure_loaded();
    memcpy(s_cfg.endpointIp, ip, 4u);
    s_cfg.endpointPort = port;

    if (!s_running || s_peerIndex == WIREGUARDIF_INVALID_INDEX) {
        return 0;
    }

    IP_ADDR4(&addr, ip[0], ip[1], ip[2], ip[3]);

    LOCK_TCPIP_CORE();
    err = wireguardif_update_endpoint(&s_wgNetif, s_peerIndex, &addr, port);
    UNLOCK_TCPIP_CORE();

    return (err == ERR_OK) ? 0 : -2;
}

int WgLink_SaveCfg(void)
{
    int rc;

    cfg_ensure_loaded();
    rc = WgCfg_Save(&s_cfg);
    if (rc == 0) {
        s_cfgStored = 1u;
    }
    return rc;
}

int WgLink_ResetCfg(void)
{
    if (WgCfg_Clear() != 0) {
        return -1;
    }

    WgLink_Stop();

    s_cfg       = s_defaultCfg;
    s_cfgLoaded = 1u;
    s_cfgStored = 0u;
    memset(s_privKeyB64, 0, sizeof(s_privKeyB64));

    TRice("WG: config erased — board is unprovisioned\n");
    return 0;
}

int WgLink_CfgIsStored(void)
{
    cfg_ensure_loaded();
    return (int)s_cfgStored;
}

uint16_t WgLink_CfgStoredVersion(void)
{
    return WgCfg_StoredVersion();
}

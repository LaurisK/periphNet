/**
 * @file    wg_link.c
 * @brief   WireGuard tunnel netif — see wg_link.h.
 */

#include "App/Net/wg_link.h"
#include "App/Net/wg_cfg.h"
#include "App/Net/wg_ladder.h"

#include <string.h>

#include "lwip/netif.h"
#include "lwip/ip.h"
#include "lwip/tcpip.h"
#include "lwip/sys.h"
#include "lwip/udp.h"
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

/* Liveness (see the note in wg_link.h).  s_lastAlive_ms latches the newest
 * evidence ever sampled so the verdict does not flap in the gap between one
 * keypair expiring and the next completing; s_haveAlive distinguishes "no
 * evidence yet" from "evidence at sys_now() == 0", which matters in the first
 * seconds after boot when a raw age would look plausibly small. */
static uint32_t   s_lastAlive_ms;
static uint8_t    s_haveAlive;
static uint32_t   s_recoveries;
static uint32_t   s_portRotations;

/* INTENT, not state.  s_running is what the tunnel is; s_wantRunning is what
 * was asked for.  They are different exactly when WgLink_Start() failed, and
 * that difference is what used to strand a board forever: both the retry in
 * WgLink_Housekeep() and its caller in app_freertos.c gated on s_running --
 * the flag the failure clears (issue_wg_sodas_offline_2026-09-06.md §5.1).
 * WgLink_Stop() must NOT clear it: Restart() and cfg_reapply() go through
 * Stop(), and a rebuild that disarms the ladder is the same bug wearing a
 * different hat.  Only an explicit stop verb clears it. */
static uint8_t    s_wantRunning;
static uint32_t   s_startFailures;
static sWgLadder  s_ladder;

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

/* Sample the port's peer state and latch anything that proves the hub
 * answered.  Caller must NOT hold the tcpip core lock.
 *
 * Both sources are sys_now() stamps, so sampling is idempotent and a missed
 * poll costs nothing as long as we look more often than a keypair lives
 * (180 s) — the 5 s housekeeping tick and every status request are ample.
 * Unsigned arithmetic keeps this correct across the sys_now() wrap at 49.7
 * days. */
static void liveness_sample(void)
{
    struct wireguard_device *dev;
    struct wireguard_peer   *peer;
    uint32_t                 evidence = 0u;
    uint8_t                  haveEvidence = 0u;

    if (!s_running || s_peerIndex == WIREGUARDIF_INVALID_INDEX) {
        return;
    }

    LOCK_TCPIP_CORE();
    dev = (struct wireguard_device *)s_wgNetif.state;
    if (dev != NULL && s_peerIndex < WIREGUARD_MAX_PEERS) {
        peer = &dev->peers[s_peerIndex];

        /* A valid current keypair means the handshake response authenticated
         * against the hub's public key.  prev_keypair is pointedly not
         * consulted: it is the field that never expires. */
        if (peer->curr_keypair.valid) {
            evidence     = peer->curr_keypair.keypair_millis;
            haveEvidence = 1u;
        }
        /* Data in is proof too, and may be newer than the keypair. */
        if (peer->last_rx != 0u &&
            (!haveEvidence || (uint32_t)(peer->last_rx - evidence) < 0x80000000u)) {
            evidence     = peer->last_rx;
            haveEvidence = 1u;
        }
    }
    UNLOCK_TCPIP_CORE();

    if (!haveEvidence) {
        return;
    }
    if (!s_haveAlive ||
        (uint32_t)(evidence - s_lastAlive_ms) < 0x80000000u) {
        s_lastAlive_ms = evidence;
        s_haveAlive    = 1u;
    }
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
        out->prevValid    = peer->prev_keypair.valid ? 1u : 0u;
        out->lastRx_ms    = peer->last_rx;
        out->lastTx_ms    = peer->last_tx;
        out->keypairAge_ms = peer->curr_keypair.valid
                           ? (uint32_t)(sys_now() - peer->curr_keypair.keypair_millis)
                           : 0u;
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

    liveness_sample();
    out->aliveAge_ms = WgLink_AliveAge();

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

static int wg_start_impl(const sWgLinkCfg *cfg)
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

    /* A rebuilt peer has proved nothing yet, and inheriting the old verdict
     * would either hide a still-dead hub or start the recovery clock from the
     * wrong instant. */
    s_haveAlive    = 0u;
    s_lastAlive_ms = 0u;

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

    s_haveAlive    = 0u;
    s_lastAlive_ms = 0u;

    TRice("WG: tunnel stopped\n");
}

int WgLink_IsRunning(void)
{
    return (int)s_running;
}

uint32_t WgLink_AliveAge(void)
{
    if (!s_running || !s_haveAlive) {
        return WG_LINK_AGE_NEVER;
    }
    return (uint32_t)(sys_now() - s_lastAlive_ms);
}

int WgLink_IsUp(void)
{
    uint32_t age;

    if (!s_running || s_peerIndex == WIREGUARDIF_INVALID_INDEX) {
        return 0;
    }

    liveness_sample();

    age = WgLink_AliveAge();
    return (age != WG_LINK_AGE_NEVER && age <= WG_LINK_STALE_MS) ? 1 : 0;
}

int WgLink_Restart(void)
{
    WgLink_Stop();
    return WgLink_Start(NULL);
}

uint32_t WgLink_RecoveryCount(void)
{
    return s_recoveries;
}

uint16_t WgLink_LocalPort(void)
{
    struct wireguard_device *dev;
    uint16_t                 port = 0u;

    if (!s_netifCreated) {
        return 0u;
    }

    LOCK_TCPIP_CORE();
    dev = (struct wireguard_device *)s_wgNetif.state;
    if (dev != NULL && dev->udp_pcb != NULL) {
        port = dev->udp_pcb->local_port;
    }
    UNLOCK_TCPIP_CORE();

    return port;
}

uint32_t WgLink_PortRotationCount(void)
{
    return s_portRotations;
}

/* Rebind the port's UDP socket so the next handshake leaves from a different
 * source port.
 *
 * The source port is chosen ONCE, by the udp_bind(..., listen_port = 0) inside
 * wireguardif_init(), and nothing in the recovery ladder used to change it.
 * WgLink_Stop() deliberately keeps the netif — "the port's device, UDP PCB and
 * periodic timer outlive this and are reused on the next start" — so every
 * rebuild retried down the very same 5-tuple.  A reboot did not help either:
 * lwIP's ephemeral counter restarts at UDP_LOCAL_PORT_RANGE_START, so the pcb
 * lands on the same port on every boot.
 *
 * That is not academic.  sodas was stranded from 2026-09-06 to 2026-09-09 with
 * a healthy board, a healthy hub and a correct endpoint: its initiations were
 * valid — relaying one to the hub by hand got a response in 200 ms — but every
 * one of them left from :62510, whose NAT mapping on the site's LTE uplink was
 * dead.  325 rebuilds and a reboot all sent from :62510, so the board could not
 * escape on its own and the site needed a visit.
 *
 * Rebinding is the only action available to us that changes the tuple, and it
 * costs nothing: the hub authenticates a peer by its key and follows whatever
 * source the initiation arrives from, so a rotated port is indistinguishable
 * from any other roaming client.  udp_bind() on a pcb already in the list is a
 * documented rebind, it draws a fresh port from udp_new_port() (which skips
 * ports in use, so the new one always differs), and it leaves the udp_recv()
 * registration alone.  A failed bind leaves the old port in place, which is
 * exactly the state we were already in.
 */
static void rotate_source_port(void)
{
    struct wireguard_device *dev;
    uint16_t                 before = 0u;
    uint16_t                 after  = 0u;

    if (!s_netifCreated) {
        return;
    }

    LOCK_TCPIP_CORE();
    dev = (struct wireguard_device *)s_wgNetif.state;
    if (dev != NULL && dev->udp_pcb != NULL) {
        before = dev->udp_pcb->local_port;
        if (udp_bind(dev->udp_pcb, IP_ADDR_ANY, 0) == ERR_OK) {
            after = dev->udp_pcb->local_port;
        }
    }
    UNLOCK_TCPIP_CORE();

    if (after != 0u && after != before) {
        s_portRotations++;
        TRice("WG: source port %u -> %u (rotation #%u)\n",
              (unsigned)before, (unsigned)after, (unsigned)s_portRotations);
    } else {
        TRice("WG: source port rotation failed, still %u\n", (unsigned)before);
    }
}

/* Is the board's OWN network sane?  Gates the terminal rung: with the link
 * down or no default route the fault is demonstrably off-board, a reboot
 * cannot repair it, and firing it would only interrupt local control.  The
 * Ethernet netif is the default one -- WgLink_Start() deliberately never calls
 * netif_set_default() on the tunnel. */
static uint8_t local_net_ok(void)
{
    struct netif *nif;
    uint8_t       ok = 0u;

    LOCK_TCPIP_CORE();
    nif = netif_default;
    if (nif != NULL && netif_is_up(nif) && netif_is_link_up(nif) &&
        !ip4_addr_isany_val(*netif_ip4_gw(nif))) {
        ok = 1u;
    }
    UNLOCK_TCPIP_CORE();

    return ok;
}

/* Rung 2 -- re-read the stored configuration.  The one rung that can repair a
 * corrupted in-RAM s_cfg, which is the only genuinely non-deterministic way
 * WgLink_Start() fails; every other path is config the parser already
 * validated.
 *
 * Refuses when the active config did NOT come from flash: a board provisioned
 * by `wg genkey` without a save holds its identity only in RAM, and reloading
 * would erase the very key it needs.  Restarts only on an actual difference --
 * rung 1 is already restarting every 15 minutes, so a no-op restart here buys
 * nothing. */
static int reload_cfg_from_flash(void)
{
    sWgLinkCfg before;

    if (!s_cfgStored) {
        TRice("WG: config not flash-backed, skipping reload\n");
        return 0;
    }

    before      = s_cfg;
    s_cfgLoaded = 0u;
    cfg_ensure_loaded();

    if (memcmp(&before, &s_cfg, sizeof(before)) == 0) {
        TRice("WG: stored config re-read, unchanged\n");
        return 0;
    }

    TRice("WG: stored config differed from RAM -- restarting on flash copy\n");
    if (s_running) {
        WgLink_Stop();
    }
    return WgLink_Start(NULL);
}

eWgLadderAction WgLink_Housekeep(void)
{
    sWgLadderIn     in;
    eWgLadderAction act;

    (void)WgLink_IsUp();          /* samples the liveness evidence */

    in.now_ms        = sys_now();
    in.aliveAge_ms   = WgLink_AliveAge();
    in.staleAfter_ms = WG_LINK_STALE_MS;
    in.wantRunning   = s_wantRunning;
    in.running       = (uint8_t)(s_running ? 1u : 0u);
    in.hasIdentity   = (uint8_t)(WgLink_HasIdentity() ? 1u : 0u);
    in.localNetOk    = local_net_ok();

    act = WgLadder_Step(&s_ladder, &in);

    switch (act) {
    case wgLadder_start:
        /* The tunnel is not running though it should be: a WgLink_Start() that
         * failed, which before this existed was permanent. */
        TRice("WG: not running, retrying start (failures=%u)\n",
              (unsigned)s_startFailures);
        (void)WgLink_Start(NULL);
        break;

    case wgLadder_rebuildRotate:
        /* Everything the port would have to unstick by itself -- a
         * prev_keypair it can no longer expire, a half-finished handshake, an
         * endpoint it roamed to -- is state only a rebuild clears.  The
         * rotation is what escapes a NAT mapping that has gone dead. */
        TRice("WG: no response for %us, rebuilding peer (recovery #%u)\n",
              (unsigned)(WgLadder_OutageAge(&s_ladder, in.now_ms) / 1000u),
              (unsigned)(s_recoveries + 1u));
        s_recoveries++;
        rotate_source_port();
        if (WgLink_Restart() != 0) {
            TRice("WG: rebuild failed, will retry\n");
        }
        break;

    case wgLadder_reloadConfig:
        TRice("WG: silent %umin, re-reading stored config\n",
              (unsigned)(WgLadder_OutageAge(&s_ladder, in.now_ms) / 60000u));
        (void)reload_cfg_from_flash();
        break;

    case wgLadder_terminalReboot:
        /* Deliberately NOT performed here.  defaultTask owns every reboot on
         * this board and has to stamp the survivor block first. */
        TRice("WG: terminal rung due after %umin silent (reboot #%u)\n",
              (unsigned)(WG_LADDER_TERMINAL_MS / 60000u),
              (unsigned)s_ladder.rebootsUsed);
        break;

    case wgLadder_none:
    case wgLadder_last:
        break;
    }

    return act;
}

/* Public entry point for WgLink_Start(): records intent and counts failures.
 * Intent is recorded even when the attempt fails -- that is the whole point,
 * since it is what lets the ladder retry a start that never succeeded. */
int WgLink_Start(const sWgLinkCfg *cfg)
{
    int rc;

    s_wantRunning = 1u;

    rc = wg_start_impl(cfg);
    if (rc != 0 && rc != -1) {   /* -1 is "already running", not a failure */
        s_startFailures++;
    }
    return rc;
}

void WgLink_StopRequested(void)
{
    /* The only thing that clears intent.  After this the ladder leaves the
     * tunnel alone until someone asks for it again. */
    s_wantRunning = 0u;
    WgLink_Stop();
}

int WgLink_WantRunning(void)
{
    return (int)s_wantRunning;
}

uint32_t WgLink_StartFailures(void)
{
    return s_startFailures;
}

uint8_t WgLink_LadderRung(void)
{
    return s_ladder.rung;
}

uint32_t WgLink_OutageAge(void)
{
    return WgLadder_OutageAge(&s_ladder, sys_now());
}

uint8_t WgLink_TerminalReboots(void)
{
    return s_ladder.rebootsUsed;
}

void WgLink_RestoreTerminalReboots(uint8_t used)
{
    WgLadder_RestoreReboots(&s_ladder, used);
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

    /* Un-provisioning is an explicit stop: without clearing intent the ladder
     * would keep retrying a board that has deliberately been left with no
     * identity. */
    WgLink_StopRequested();

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

/**
 * @file    wg_link.h
 * @brief   WireGuard tunnel netif — hub peer bring-up and configuration.
 *
 * The tunnel is a second lwIP netif.  The Ethernet netif stays the DEFAULT
 * route, so on-LAN traffic and the encapsulated WireGuard UDP itself take the
 * short path; only the peer's allowed ranges are steered into the tunnel, via
 * the lwIP route hook this module installs.
 *
 * ## No tunnel parameters live in the image
 *
 * Keys, address and routes all arrive as configuration — normally by uploading
 * the `.conf` WGDashboard issues (see wg_conf.h), and are persisted by wg_cfg.
 * An unprovisioned board simply does not bring the tunnel up; it does not fall
 * back to a shared identity, because two boards presenting one key to the hub
 * knock each other off the tunnel in a way that is tedious to diagnose.
 *
 * ## Address vs routes
 *
 * `tunnelIp`/`tunnelMask` is this device's own address on the tunnel (the
 * `.conf`'s `[Interface] Address`, conventionally a /32).  `allowed[]` is the
 * peer's `AllowedIPs`: what routes into the tunnel and what the peer may claim
 * as an inner source.  Deriving one from the other — as this module used to —
 * works only when the address mask happens to be wide enough, and otherwise
 * fails silently: the handshake still succeeds while every packet is dropped.
 */

#ifndef WG_LINK_H_
#define WG_LINK_H_

#include <stdint.h>

#include "App/Net/wg_conf.h"

/* Base64 of a 32-byte key: 44 characters + NUL. */
#define WG_KEY_B64_SIZE   45u

/* --------------------------------------------------------------------------
 * Liveness
 *
 * "Is the tunnel up?" cannot be answered by asking the port, and getting that
 * wrong once cost a board 13 hours of silence.  wireguardif_peer_is_up() is
 * `curr_keypair.valid || prev_keypair.valid`, and NOTHING in the port ever
 * invalidates prev_keypair: should_reset_peer(), the only thing that would,
 * is itself gated on curr_keypair.valid, so once the current keypair expires
 * the reset can never fire again.  The predicate is therefore incapable of
 * going false after the first successful handshake, whatever happens on the
 * wire afterwards.  (The same expression drives the port's netif_set_link_down
 * decision, so the netif's link flag is unreliable for the same reason.)
 *
 * What IS evidence, and why:
 *
 *  - A **fresh current keypair**.  A keypair only becomes valid when the
 *    handshake RESPONSE has been received and authenticated with the hub's
 *    public key, so it is proof the hub answered.  It is also a heartbeat on
 *    its own: the port destroys the current keypair at REJECT_AFTER_TIME
 *    (180 s) and immediately re-initiates, so a healthy peer rotates roughly
 *    every three minutes whether or not any data is flowing.  Measured on the
 *    bench: sending_counter restarts from 1 every ~180 s on an idle tunnel.
 *
 *  - **A data packet in** (`last_rx`).  Proof, but not a heartbeat — an idle
 *    tunnel receives nothing for hours, and the hub does not answer our
 *    keepalives.  So it may confirm liveness, never deny it.
 *
 * Deliberately NOT evidence: `last_tx` (keepalives keep advancing it while
 * prev_keypair is valid, i.e. exactly during the failure), and prev_keypair.
 */

/** No authenticated evidence for this long and the tunnel is judged DOWN.
 *  Two keypair rotations plus room for lost initiations — a healthy peer
 *  never gets near it. */
#define WG_LINK_STALE_MS       360000u

/** Judged down for this long and the peer is torn down and rebuilt.  This is
 *  the one action known to clear the stuck state above; it costs no memory
 *  (the netif is reused, see the note in wg_link.c) and no other subsystem
 *  depends on the tunnel. */
#define WG_LINK_RECOVER_MS     900000u

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t    privateKey[WG_CONF_KEY_SIZE];     /* raw, this device's     */
    uint8_t    peerPublicKey[WG_CONF_KEY_SIZE];  /* raw, the hub's         */
    uint8_t    hasPrivateKey;
    uint8_t    hasPeerKey;
    uint8_t    tunnelIp[4];      /* our address inside the tunnel          */
    uint8_t    tunnelMask[4];    /* its prefix, as a mask                  */
    uint8_t    endpointIp[4];    /* hub public IPv4                        */
    uint16_t   endpointPort;     /* hub listen port                        */
    uint16_t   keepAlive_sec;    /* PersistentKeepalive, 0 = off           */
    sWgIpRange allowed[WG_CONF_MAX_ALLOWED];  /* peer AllowedIPs           */
    uint8_t    allowedCount;
} sWgLinkCfg;

/**
 * Live peer state, read straight out of the port's peer struct.
 *
 * This exists because "the tunnel is up" is not observable from either end on
 * its own: the hub reports a peer as active on the strength of handshakes
 * alone, and handshakes succeed regardless of whether a single data packet
 * ever crosses.  @c lastRx_ms is the field that actually settles it — if it
 * never advances while the far end is sending, the loss is before us; if it
 * advances and nothing comes back, the loss is ours.
 */
typedef struct {
    uint8_t  sessionValid;    /* a current keypair exists                   */
    uint8_t  prevValid;       /* ...and a previous one, which never expires */
    uint32_t lastRx_ms;       /* sys_now() of the last DATA packet in, 0=never */
    uint32_t lastTx_ms;       /* sys_now() of the last DATA packet out      */
    uint32_t keypairAge_ms;   /* age of the current keypair, 0 if none      */
    uint32_t aliveAge_ms;     /* since the last authenticated evidence;
                               * WG_LINK_AGE_NEVER if there has been none   */
    uint32_t txPackets;       /* encrypted packets sent on this session     */
    uint32_t rxCounter;       /* highest received counter (replay window)   */
    uint8_t  endpointIp[4];   /* where the port currently thinks the hub is */
    uint16_t endpointPort;
} sWgPeerStats;

/** Reported as aliveAge_ms when the tunnel has never had a live session. */
#define WG_LINK_AGE_NEVER   0xFFFFFFFFu

/* Opaque to callers that do not want lwIP headers. */
struct netif;
struct ip4_addr;

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * @brief  Create the WireGuard netif and start connecting to the hub.
 *         Call after lwIP is initialised and after WgTime_Init(), so the
 *         first handshake carries a timestamp that survived the last reboot.
 *         Safe to call before DHCP has bound — the handshake simply retries
 *         until a route exists.
 * @param  cfg  Configuration to use, or NULL to keep/reuse the current one.
 *              The contents are copied; the caller's buffer need not persist.
 * @return 0 on success, -1 already running, -4 not provisioned (no keys),
 *         other negative values on lwIP errors.
 */
int WgLink_Start(const sWgLinkCfg *cfg);

/** @brief  Tear the tunnel down and remove the netif. */
void WgLink_Stop(void);

/** @brief  1 if the netif exists (not necessarily handshaken). */
int WgLink_IsRunning(void);

/**
 * @brief  1 if the hub has proved itself alive within WG_LINK_STALE_MS.
 *
 * See the Liveness note above for what counts as proof and why the port's own
 * wireguardif_peer_is_up() must not be used for this.  Latching, so it does
 * not flap during the second or two between a keypair expiring and its
 * replacement completing.
 */
int WgLink_IsUp(void);

/**
 * @brief  Age in ms of the newest evidence the hub is alive, or
 *         WG_LINK_AGE_NEVER if there has never been any.
 *
 * The number behind WgLink_IsUp(): on a healthy tunnel it sawtooths up to
 * about REJECT_AFTER_TIME and drops, so a value that only ever grows is the
 * failure this module exists to catch.
 */
uint32_t WgLink_AliveAge(void);

/**
 * @brief  Stop and start the tunnel, rebuilding the peer from configuration.
 *
 * Wipes every scrap of session state the port holds — both keypairs, the
 * in-flight handshake, the cookie, and any endpoint the peer had roamed to —
 * and re-initiates.  Cheap: the netif, its UDP PCB and the port's timer are
 * reused, so this allocates nothing (wg_link.c explains why removal is not an
 * option).
 *
 * @return 0 on success, whatever WgLink_Start() returned otherwise.
 */
int WgLink_Restart(void);

/**
 * @brief  Periodic liveness sampling and self-recovery.  Call about every 5 s
 *         from a task; it takes the tcpip core lock and must not run in lwIP
 *         context.
 *
 * Samples the evidence WgLink_IsUp() reads, and once the tunnel has been down
 * for WG_LINK_RECOVER_MS performs a WgLink_Restart().  A board whose hub is
 * genuinely gone therefore rebuilds its peer every 15 minutes and does
 * nothing else — the tunnel stays a soft dependency.
 *
 * @return 1 if it restarted the tunnel this call, 0 otherwise.
 */
int WgLink_Housekeep(void);

/** @brief  How many times WgLink_Housekeep() has recovered the tunnel since
 *          boot.  Nonzero means the link died and came back by itself. */
uint32_t WgLink_RecoveryCount(void);

/** @brief  1 once both keys are present, i.e. the tunnel can be started. */
int WgLink_HasIdentity(void);

/**
 * @brief  Snapshot the live peer state (see sWgPeerStats).
 * @return 0 on success, negative if the tunnel is not running.
 */
int WgLink_GetPeerStats(sWgPeerStats *out);

/** @brief  The configuration the link is running with (or would start
 *          with).  Never NULL. */
const sWgLinkCfg *WgLink_ActiveCfg(void);

/* --------------------------------------------------------------------------
 * Provisioning
 * -------------------------------------------------------------------------- */

/**
 * @brief  Adopt a parsed `.conf` wholesale: keys, address, endpoint, routes.
 *
 * @param  conf  parsed configuration (see wg_conf.h)
 * @param  save  non-zero to persist it as well
 * @return 0 on success, negative on flash or restart failure.  Applied
 *         immediately: a running link is stopped and restarted, which forces
 *         a fresh handshake under the new identity.
 */
int WgLink_ApplyConf(const sWgConf *conf, int save);

/**
 * @brief  Generate a fresh identity key on-device from the hardware-RNG DRBG.
 *         The private key never leaves the board; read the public half with
 *         WgLink_GetPublicKeyB64() and register that with the hub.
 * @param  save  non-zero to persist immediately
 * @return 0 on success, negative on error.
 */
int WgLink_GenerateKey(int save);

/**
 * @brief  Base64 of the public key derived from the active private key.
 * @return 0 on success, negative if no private key is set or the buffer is
 *         too small (needs WG_KEY_B64_SIZE).
 */
int WgLink_GetPublicKeyB64(char *out, uint32_t outSz);

/**
 * @brief  Base64 of the configured hub public key.
 * @return 0 on success, negative if unset or the buffer is too small.
 */
int WgLink_GetPeerKeyB64(char *out, uint32_t outSz);

/* --------------------------------------------------------------------------
 * Individual settings (all take effect immediately; follow with
 * WgLink_SaveCfg() to make them survive a reset)
 * -------------------------------------------------------------------------- */

/**
 * @brief  Override the hub endpoint.  Applied to the live peer if running.
 * @return 0 on success, negative on error.
 */
int WgLink_SetEndpoint(const uint8_t ip[4], uint16_t port);

/**
 * @brief  Set this device's address inside the tunnel.
 *
 * Must agree with the AllowedIPs the hub holds for this peer — the hub drops
 * packets whose inner source address it has not authorised for the peer's key.
 * A handshake still succeeds when they disagree (it carries no inner
 * addresses), so the symptom is a peer that looks connected while carrying no
 * traffic.
 *
 * @param  ip    tunnel address, must not be 0.0.0.0.
 * @param  mask  tunnel prefix as a mask, or NULL to keep the current one.
 * @return 0 on success, negative on error.  Restarts a running link.
 */
int WgLink_SetTunnelIp(const uint8_t ip[4], const uint8_t mask[4]);

/** @brief  Persist the active configuration so it survives reboot and OTA. */
int WgLink_SaveCfg(void);

/**
 * @brief  Erase the stored configuration, wiping the private key with it.
 *         The tunnel stops: without an identity there is nothing to fall back
 *         to.  Applied immediately.
 */
int WgLink_ResetCfg(void);

/** @brief  1 if the active configuration came from flash. */
int WgLink_CfgIsStored(void);

/** @brief  Version of the stored record (1 legacy, 2 current, 0 none). */
uint16_t WgLink_CfgStoredVersion(void);

/* --------------------------------------------------------------------------
 * lwIP integration
 * -------------------------------------------------------------------------- */

/**
 * @brief  Route hook: returns the tunnel netif for destinations inside a
 *         configured allowed range, NULL for everything else.
 *
 * Wired up as LWIP_HOOK_IP4_ROUTE.  Without it lwIP would pick a netif purely
 * by subnet match against the interface address, so a /32 tunnel address —
 * which is what every `.conf` carries — would route nothing at all.
 */
struct netif *WgLink_Ip4Route(const struct ip4_addr *dest);

#ifdef __cplusplus
}
#endif

#endif /* WG_LINK_H_ */

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

/** @brief  1 if the peer has a valid session key, i.e. the tunnel is
 *          actually carrying traffic. */
int WgLink_IsUp(void);

/** @brief  1 once both keys are present, i.e. the tunnel can be started. */
int WgLink_HasIdentity(void);

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

/**
 * @file    wg_link.h
 * @brief   WireGuard tunnel netif — the board's own VPN link to the hub.
 *
 * Brings up a second lwIP netif alongside the Ethernet one.  The Ethernet
 * netif stays the default route, so on-LAN traffic takes the short path;
 * only the tunnel subnet is routed through WireGuard.
 *
 * The tunnel is a SOFT dependency by design: if the hub is unreachable the
 * handshake retries on lwIP timers and nothing else on the board is affected.
 * No call here blocks a task, the RS485 bus, or the IWDG kick.
 */

#ifndef WG_LINK_H_
#define WG_LINK_H_

#include <stdint.h>

/* Base64 of a 32-byte key: 44 characters + NUL. */
#define WG_KEY_B64_SIZE   45u

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *privateKey;      /* base64, this device's WG private key    */
    const char *peerPublicKey;   /* base64, the hub's public key            */
    uint8_t     tunnelIp[4];     /* our address inside the tunnel           */
    uint8_t     tunnelMask[4];   /* tunnel subnet mask                      */
    uint8_t     endpointIp[4];   /* hub public IPv4                         */
    uint16_t    endpointPort;    /* hub listen port                         */
    uint16_t    keepAlive;       /* PersistentKeepalive seconds, 0 = off    */
} sWgLinkCfg;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief  Create the WireGuard netif and start connecting to the hub.
 *         Call after lwIP is initialised and after WgTime_Init(), so the
 *         first handshake carries a timestamp that survived the last reboot.
 *         Safe to call before DHCP has bound — the handshake simply retries
 *         until a route exists.
 * @param  cfg  Configuration to use, or NULL to keep/reuse the current one.
 *              The contents are copied; the caller's buffer need not persist.
 * @return 0 on success, negative on error.
 */
int WgLink_Start(const sWgLinkCfg *cfg);

/**
 * @brief  Tear the tunnel down and remove the netif.
 */
void WgLink_Stop(void);

/**
 * @brief  Returns 1 if the netif exists (not necessarily handshaken).
 */
int WgLink_IsRunning(void);

/**
 * @brief  Returns 1 if the peer has a valid session key, i.e. the tunnel
 *         is actually carrying traffic.
 */
int WgLink_IsUp(void);

/**
 * @brief  The configuration the link is running with (or would start with).
 *         Never NULL.
 */
const sWgLinkCfg *WgLink_ActiveCfg(void);

/**
 * @brief  Override the hub endpoint. Takes effect on the next WgLink_Start();
 *         if the link is already running it is applied to the live peer.
 * @return 0 on success, negative on error.
 */
int WgLink_SetEndpoint(const uint8_t ip[4], uint16_t port);

/**
 * @brief  Set this device's address inside the tunnel.
 *
 * Must match the AllowedIPs the hub has for this peer — the hub drops packets
 * whose inner source address it has not authorised for the peer's key, and has
 * no route back for any other address.  A handshake still succeeds when they
 * disagree (it does not involve inner addresses), so the symptom is a peer that
 * looks connected while carrying no traffic.
 *
 * @param  ip    tunnel address, must not be 0.0.0.0.
 * @param  mask  tunnel subnet mask, or NULL to keep the current one.
 * @return 0 on success, negative on error.  Applied immediately: if the link
 *         is running it is restarted, which forces a fresh handshake.
 */
int WgLink_SetTunnelIp(const uint8_t ip[4], const uint8_t mask[4]);

/**
 * @brief  Persist the active configuration so it survives reboot and OTA.
 *         Without this, changes last only until the next reset.
 * @return 0 on success, negative on flash error.
 */
int WgLink_SaveCfg(void);

/**
 * @brief  Drop the persisted configuration and revert to the built-in
 *         defaults.  Applied immediately (restarts a running link).
 * @return 0 on success, negative on error.
 */
int WgLink_ResetCfg(void);

/**
 * @brief  1 if the active configuration came from flash rather than the
 *         built-in defaults.
 */
int WgLink_CfgIsStored(void);

/**
 * @brief  Built-in defaults for the Zaliakalnis hub (see
 *         docs/task_board_as_wireguard_peer.md step 0 for provenance).
 */
const sWgLinkCfg *WgLink_DefaultCfg(void);

#endif /* WG_LINK_H_ */

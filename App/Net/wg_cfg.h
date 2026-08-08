/**
 * @file    wg_cfg.h
 * @brief   Persistent WireGuard network configuration (ext flash).
 *
 * The tunnel address is NOT assignable by the network: WireGuard has no
 * address-assignment protocol, because the hub's AllowedIPs is simultaneously
 * the route and the authentication rule for which source addresses a peer may
 * claim.  Both ends must therefore be configured to agree, and the address is
 * deliberately independent of whatever LAN the board lands on (that one comes
 * from DHCP).
 *
 * What this module fixes is a different problem: the address used to live only
 * in the build (App/Net/wg_link.c s_defaultCfg), so every unit flashed with the
 * same image claimed the same tunnel address, and changing it needed a rebuild
 * + OTA.  Here it becomes per-device data, settable over CLI or HTTP.
 *
 * The private key is still a build constant — see wg_link.c.  This record
 * reserves room for it so key provisioning can land without a format change.
 */

#ifndef WG_CFG_H_
#define WG_CFG_H_

#include <stdint.h>
#include "App/Net/wg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Load the stored configuration into @p cfg.
 *
 * Only the network fields are taken from flash; the key pointers in @p cfg are
 * left untouched, so the caller keeps whatever keys it started with.
 *
 * @return 0 if a valid record was applied, negative if none/corrupt (in which
 *         case @p cfg is unmodified and the caller should keep its defaults).
 */
int WgCfg_Load(sWgLinkCfg *cfg);

/**
 * @brief  Persist the network fields of @p cfg (erase + rewrite the sector).
 * @return 0 on success, negative on flash error.
 */
int WgCfg_Save(const sWgLinkCfg *cfg);

/**
 * @brief  Erase the stored record, so the next boot uses the built-in
 *         defaults again.
 * @return 0 on success, negative on flash error.
 */
int WgCfg_Clear(void);

/**
 * @brief  1 if a valid stored record exists, 0 otherwise.
 */
int WgCfg_IsStored(void);

#ifdef __cplusplus
}
#endif

#endif /* WG_CFG_H_ */

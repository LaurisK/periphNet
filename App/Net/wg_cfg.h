/**
 * @file    wg_cfg.h
 * @brief   Persistent WireGuard configuration (ext flash).
 *
 * Everything WireGuard needs is per-device data, not build data: the identity
 * key, the hub's key, the tunnel address and the routes.  Uploading a `.conf`
 * writes this record; the image carries no tunnel parameters at all.
 *
 * The tunnel address is NOT assignable by the network — WireGuard has no
 * address-assignment protocol, because the hub's AllowedIPs is simultaneously
 * the route and the rule for which inner source addresses a peer's key may
 * claim.  Both ends must be configured to agree, which is why it has to arrive
 * as configuration rather than be discovered.
 *
 * ## Record versions
 *
 * v1 (2026-08-08) held only the network fields; keys lived in the image.
 * v2 adds the identity key, the peer key and the peer's allowed ranges.
 * v1 records are still accepted on read so a board provisioned before this
 * change keeps its address — its keys then come from the built-in defaults,
 * and its allowed range is synthesised from the tunnel address the way the
 * old code derived it.
 *
 * ## The private key is stored in plain external flash
 *
 * Anything with SPI access to the W25Q64 can read it.  That is the same
 * exposure as the previous arrangement (the key was recoverable from any
 * `.bin`), and strictly better in that each board now has its own, so one
 * compromised board is not the whole fleet.  Real key protection needs
 * on-device generation plus a secret store the CPU can lock, which this
 * hardware does not have.
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
 * Fields absent from the stored record are left untouched, so the caller's
 * defaults show through.  A v1 record therefore leaves the keys alone.
 *
 * @return 0 if a valid record was applied, negative if none/corrupt (in which
 *         case @p cfg is unmodified and the caller should keep its defaults).
 */
int WgCfg_Load(sWgLinkCfg *cfg);

/**
 * @brief  Persist @p cfg as a v2 record (erase + rewrite the sector).
 * @return 0 on success, negative on flash error.
 */
int WgCfg_Save(const sWgLinkCfg *cfg);

/**
 * @brief  Erase the stored record, wiping the stored private key with it.
 * @return 0 on success, negative on flash error.
 */
int WgCfg_Clear(void);

/**
 * @brief  1 if a valid stored record exists, 0 otherwise.
 */
int WgCfg_IsStored(void);

/**
 * @brief  Version of the stored record (1 or 2), 0 if none/corrupt.
 *         Reported by the status API so a board still running a pre-key
 *         record is identifiable remotely.
 */
uint16_t WgCfg_StoredVersion(void);

#ifdef __cplusplus
}
#endif

#endif /* WG_CFG_H_ */

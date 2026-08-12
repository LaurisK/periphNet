/**
 * @file    lwip_hooks.h
 * @brief   Declarations for the LWIP_HOOK_* macros set in lwipopts.h.
 *
 * lwIP includes this file (via LWIP_HOOK_FILENAME) inside its own core
 * sources, so every hook implementation must be declared here.  Keep it free
 * of application headers: it is pulled into ip4.c, which knows nothing about
 * the rest of the firmware.
 */

#ifndef LWIP_HOOKS_H_
#define LWIP_HOOKS_H_

#include "lwip/arch.h"

struct netif;
struct ip4_addr;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Route hook — App/Net/wg_link.c.
 *
 * Returns the WireGuard netif for destinations inside the tunnel peer's
 * allowed ranges, NULL to let lwIP route normally.  Needed because the tunnel
 * interface carries a /32 address, so lwIP's own subnet match would never
 * select it.
 */
struct netif *WgLink_Ip4Route(const struct ip4_addr *dest);

#ifdef __cplusplus
}
#endif

#endif /* LWIP_HOOKS_H_ */

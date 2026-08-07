/**
 * @file    wg_platform.h
 * @brief   Extras exported by the WireGuard platform layer.
 *
 * The four hooks WireGuard-lwIP itself requires are declared by the port in
 * wireguard-platform.h.  This header only exposes what the rest of the
 * application needs from the same translation unit: entropy contribution and
 * RNG health, so `wg status` can say whether session keys are being drawn
 * from the hardware RNG or from the DRBG fallback.
 */

#ifndef WG_PLATFORM_H_
#define WG_PLATFORM_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief  Stir additional entropy into the DRBG state (never removes any).
 */
void WgPlatform_AddEntropy(const void *data, size_t len);

/**
 * @brief  RNG health.  @p hwSeeded is 1 when the DRBG was seeded entirely
 *         from the hardware RNG; @p failures counts hardware read errors
 *         since boot.  Either pointer may be NULL.
 */
void WgPlatform_GetRngStatus(int *hwSeeded, uint32_t *failures);

/**
 * @brief  No-op stand-in for printf inside the WireGuard port — see the
 *         set_source_files_properties() note in CMakeLists.txt.  Never call
 *         this directly; log with Trice.
 */
int WgPlatform_NullPrintf(const char *fmt, ...);

#endif /* WG_PLATFORM_H_ */

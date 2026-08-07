/**
 * @file    wg_platform.c
 * @brief   Platform integration layer for WireGuard-lwIP.
 *
 * The WireGuard port declares four hooks in wireguard-platform.h and expects
 * the integrator to supply them.  This is that supply.
 *
 * Both security-critical hooks are now backed by real sources: entropy comes
 * from the STM32F407 hardware RNG (whitened through a SHA-256 DRBG), and the
 * handshake timestamp comes from a reboot-surviving monotonic counter
 * (App/Net/wg_time.c).
 *
 * Everything here can run in tcpip_thread context, so no Trice calls — errors
 * are counted and surfaced through WgPlatform_GetRngStatus() for the CLI.
 */

#include "wireguard-platform.h"

#include <string.h>

#include "lwip/sys.h"
#include "crypto.h"
#include "sha256.h"
#include "stm32f4xx_hal.h"
#include "rng.h"
#include "App/Net/wg_platform.h"
#include "App/Net/wg_time.h"

/* --------------------------------------------------------------------------
 * Entropy
 * -------------------------------------------------------------------------- */

/* Two stages:
 *
 *   1. the STM32F407 hardware RNG (analog noise source, 48 MHz PLL clock) is
 *      the entropy input — 32 bytes at seed time, plus one fresh word mixed
 *      into every output block;
 *   2. a SHA-256 DRBG (state = SHA256(state || counter || hw word)) whitens
 *      it and keeps producing unpredictable output even if the RNG stalls or
 *      trips a seed/clock error mid-flight.
 *
 * WireGuard draws ephemeral session keys from this, so stage 1 failing
 * silently is exactly what must not happen: hardware failures are counted and
 * s_hwSeeded stays clear, which the `wg status` CLI reports.
 */
static uint8_t  s_drbgState[SHA256_DIGEST_SIZE];
static uint32_t s_drbgCounter;
static uint8_t  s_drbgSeeded;
static uint8_t  s_hwSeeded;
static uint32_t s_hwFailures;

/* One 32-bit word from the hardware RNG.
 *
 * A seed or clock error latches the peripheral, so on failure the RNG is
 * re-initialised once and retried; a second failure is reported to the caller
 * and the DRBG carries on without fresh hardware entropy.
 */
static int hw_rng_word(uint32_t *out)
{
    if (HAL_RNG_GenerateRandomNumber(&hrng, out) == HAL_OK) {
        return 1;
    }

    s_hwFailures++;

    __HAL_RNG_CLEAR_IT(&hrng, RNG_IT_CEI | RNG_IT_SEI);
    (void)HAL_RNG_DeInit(&hrng);
    if (HAL_RNG_Init(&hrng) != HAL_OK) {
        return 0;
    }

    if (HAL_RNG_GenerateRandomNumber(&hrng, out) == HAL_OK) {
        return 1;
    }

    s_hwFailures++;
    return 0;
}

static void drbg_seed(void)
{
    sSha256Ctx ctx;
    uint32_t   hw[8];
    uint32_t   jitter[4];
    uint32_t   hwWords = 0u;

    /* DWT cycle counter — enable if the debug unit left it off. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    jitter[0] = DWT->CYCCNT;
    jitter[1] = HAL_GetTick();
    jitter[2] = (uint32_t)(uintptr_t)&ctx;      /* stack address jitter */
    jitter[3] = *(volatile uint32_t *)&s_drbgState[0];

    for (uint32_t i = 0u; i < 8u; i++) {
        if (!hw_rng_word(&hw[i])) {
            hw[i] = DWT->CYCCNT;
            continue;
        }
        hwWords++;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)hw, sizeof(hw));
    sha256_update(&ctx, (const uint8_t *)jitter, sizeof(jitter));
    sha256_final(&ctx, s_drbgState);

    s_drbgCounter = 0u;
    s_drbgSeeded  = 1u;
    s_hwSeeded    = (hwWords == 8u) ? 1u : 0u;

    crypto_zero(hw, sizeof(hw));
}

void WgPlatform_AddEntropy(const void *data, size_t len)
{
    sSha256Ctx ctx;

    if (!s_drbgSeeded) {
        drbg_seed();
    }

    sha256_init(&ctx);
    sha256_update(&ctx, s_drbgState, sizeof(s_drbgState));
    sha256_update(&ctx, (const uint8_t *)data, (uint32_t)len);
    sha256_final(&ctx, s_drbgState);
}

void WgPlatform_GetRngStatus(int *hwSeeded, uint32_t *failures)
{
    if (hwSeeded != NULL) {
        *hwSeeded = (int)s_hwSeeded;
    }
    if (failures != NULL) {
        *failures = s_hwFailures;
    }
}

void wireguard_random_bytes(void *bytes, size_t size)
{
    uint8_t   *out = (uint8_t *)bytes;
    sSha256Ctx ctx;
    uint8_t    block[SHA256_DIGEST_SIZE];
    uint32_t   hw;

    if (!s_drbgSeeded) {
        drbg_seed();
    }

    while (size > 0u) {
        size_t chunk = (size < SHA256_DIGEST_SIZE) ? size : SHA256_DIGEST_SIZE;

        s_drbgCounter++;

        if (!hw_rng_word(&hw)) {
            hw = DWT->CYCCNT;
        }

        sha256_init(&ctx);
        sha256_update(&ctx, s_drbgState, sizeof(s_drbgState));
        sha256_update(&ctx, (const uint8_t *)&s_drbgCounter,
                      sizeof(s_drbgCounter));
        sha256_update(&ctx, (const uint8_t *)&hw, sizeof(hw));
        sha256_final(&ctx, block);

        memcpy(out, block, chunk);
        out  += chunk;
        size -= chunk;
    }

    crypto_zero(block, sizeof(block));
}

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

uint32_t wireguard_sys_now(void)
{
    return sys_now();
}

/* TAI64N timestamp used by the remote end for handshake replay detection.
 *
 * The hub keeps the greatest timestamp it has seen per peer and rejects
 * anything older, so this must never go backwards — including across a board
 * reset.  WgTime_Now() is a monotonic seconds counter persisted in external
 * flash; see App/Net/wg_time.h for how it survives reboots and power cuts.
 *
 * Sub-second resolution comes from sys_now(), which only has to break ties
 * within one second — its restart at boot is harmless because the seconds
 * field has already jumped forward by then.
 */
void wireguard_tai64n_now(uint8_t *output)
{
    uint64_t seconds = 0x400000000000000aULL + (uint64_t)WgTime_Now();
    uint32_t nanos   = (uint32_t)((sys_now() % 1000u) * 1000000u);

    U64TO8_BIG(output + 0, seconds);
    U32TO8_BIG(output + 8, nanos);
}

/* --------------------------------------------------------------------------
 * Load shedding
 * -------------------------------------------------------------------------- */

/* Cookie replies exist to shed CPU under a flood of initiation messages.  We
 * only ever talk to one known hub and never accept unsolicited initiations
 * from the internet, so there is nothing to shed.
 */
bool wireguard_is_under_load(void)
{
    return false;
}

/* --------------------------------------------------------------------------
 * printf sink
 * -------------------------------------------------------------------------- */

/* wireguardif.c is compiled with -Dprintf=WgPlatform_NullPrintf so its two
 * leftover debug prints cannot drag newlib stdio (and a ~1 KB malloc from the
 * 1.5 KB newlib heap) into the lwIP core path.  See CMakeLists.txt.
 */
int WgPlatform_NullPrintf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

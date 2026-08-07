/**
 * @file    wg_platform.c
 * @brief   Platform integration layer for WireGuard-lwIP.
 *
 * The WireGuard port declares four hooks in wireguard-platform.h and expects
 * the integrator to supply them.  This is that supply.
 *
 * Two of them carry real security weight and are NOT production-ready yet —
 * see the comments on wireguard_random_bytes() and wireguard_tai64n_now().
 * Both are adequate for the step-1 footprint experiment and for bench
 * bring-up against our own hub; neither should be trusted in the field.
 */

#include "wireguard-platform.h"

#include <string.h>

#include "lwip/sys.h"
#include "crypto.h"
#include "sha256.h"
#include "stm32f4xx_hal.h"

/* --------------------------------------------------------------------------
 * Entropy
 * -------------------------------------------------------------------------- */

/* SHA-256 based DRBG: state = SHA256(state || counter), output taken from the
 * digest.  Reuses the SHA-256 already linked for the FWU path, so it costs
 * essentially nothing in flash.
 *
 * !!! NOT A CSPRNG YET !!!  The seed below is weak — DWT cycle counter plus
 * whatever the uninitialised seed buffer happened to hold.  WireGuard uses
 * this for ephemeral session keys, so a predictable stream is a real break of
 * the tunnel, not a theoretical one.  Before any production use, enable the
 * STM32F407 hardware RNG (HAL_RNG_MODULE_ENABLED is currently commented out in
 * Core/Inc/stm32f4xx_hal_conf.h — toggle it in CubeMX, not by hand) and seed
 * from it.  Tracked in docs/task_board_as_wireguard_peer.md.
 */
static uint8_t  s_drbgState[SHA256_DIGEST_SIZE];
static uint32_t s_drbgCounter;
static uint8_t  s_drbgSeeded;

static void drbg_seed(void)
{
    sSha256Ctx ctx;
    uint32_t   entropy[4];

    /* DWT cycle counter — enable if the debug unit left it off. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    entropy[0] = DWT->CYCCNT;
    entropy[1] = HAL_GetTick();
    entropy[2] = (uint32_t)(uintptr_t)&ctx;   /* stack address jitter */
    entropy[3] = *(volatile uint32_t *)&s_drbgState[0];

    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)entropy, sizeof(entropy));
    sha256_final(&ctx, s_drbgState);

    s_drbgCounter = 0u;
    s_drbgSeeded  = 1u;
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

void wireguard_random_bytes(void *bytes, size_t size)
{
    uint8_t   *out = (uint8_t *)bytes;
    sSha256Ctx ctx;
    uint8_t    block[SHA256_DIGEST_SIZE];

    if (!s_drbgSeeded) {
        drbg_seed();
    }

    while (size > 0u) {
        size_t chunk = (size < SHA256_DIGEST_SIZE) ? size : SHA256_DIGEST_SIZE;

        s_drbgCounter++;

        sha256_init(&ctx);
        sha256_update(&ctx, s_drbgState, sizeof(s_drbgState));
        sha256_update(&ctx, (const uint8_t *)&s_drbgCounter,
                      sizeof(s_drbgCounter));
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
 * !!! REBOOT HAZARD !!!  This is derived from sys_now(), which restarts at 0
 * on every boot.  The hub keeps the greatest timestamp it has seen per peer,
 * so after a board reset our timestamps go backwards and the hub rejects our
 * handshakes until its own state is cleared.  Fixing this needs a monotonic
 * value that survives reset — SNTP once the tunnel is up, or a counter
 * persisted to EEPROM/ext-flash.  Tracked as a step-3 item.
 */
void wireguard_tai64n_now(uint8_t *output)
{
    uint64_t millis  = (uint64_t)sys_now();
    uint64_t seconds = 0x400000000000000aULL + (millis / 1000u);
    uint32_t nanos   = (uint32_t)((millis % 1000u) * 1000000u);

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

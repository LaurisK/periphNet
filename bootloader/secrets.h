#ifndef SECRETS_H
#define SECRETS_H

#include "aes128.h"
#include "hmac_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FWU keys — known only to the build process and the bootloader.
 * Never stored in external flash, never exposed through the BL API,
 * never linked into the application.
 *
 * GLB_blKey   — AES-128 transport key: decrypts .pnfw blobs.
 * GLB_hmacKey — HMAC-SHA256 key: authenticates the plaintext image
 *               (patched into the binary at build time, verified by the
 *               bootloader over internal flash at any boot).
 */

#define HMAC_KEY_SIZE  32u

extern const uint8_t GLB_blKey[AES128_KEY_SIZE];
extern const uint8_t GLB_hmacKey[HMAC_KEY_SIZE];

#ifdef __cplusplus
}
#endif

#endif /* SECRETS_H */

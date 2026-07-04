#ifndef AES_GCM_H
#define AES_GCM_H

#include "aes128.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AES_GCM_IV_SIZE   12u
#define AES_GCM_TAG_SIZE  16u

/* --------------------------------------------------------------------------
 * Streaming AES-128-GCM decryption (NIST SP 800-38D)
 *
 * Usage:
 *   aes_gcm_dec_init(&ctx, key, iv, aad, aad_len);
 *   aes_gcm_dec_update(&ctx, ct, pt, n);   // n % 16 == 0 except last call
 *   ...
 *   if (!aes_gcm_dec_final(&ctx, tag)) -> authentication failed
 *
 * NOTE: plaintext produced by update() is UNAUTHENTICATED until final()
 * returns true — callers must not act on it before the tag check.
 * -------------------------------------------------------------------------- */

typedef struct {
    sAes128Ctx aes;
    uint8_t    H[16];       /* GHASH subkey                   */
    uint8_t    J0[16];      /* pre-counter block (for tag)    */
    uint8_t    ctr[16];     /* running CTR block              */
    uint8_t    S[16];       /* GHASH accumulator              */
    uint32_t   aad_len;
    uint32_t   ct_len;
} sAesGcmCtx;

void aes_gcm_dec_init(sAesGcmCtx *ctx, const uint8_t key[AES128_KEY_SIZE],
                      const uint8_t iv[AES_GCM_IV_SIZE],
                      const uint8_t *aad, uint32_t aad_len);

/**
 * Decrypt a chunk of ciphertext.  ct and pt may alias.
 * len must be a multiple of 16 on all calls except the final one.
 */
void aes_gcm_dec_update(sAesGcmCtx *ctx, const uint8_t *ct,
                        uint8_t *pt, uint32_t len);

/**
 * Verify the authentication tag (constant-time comparison).
 * @return true if the tag is valid.
 */
bool aes_gcm_dec_final(sAesGcmCtx *ctx, const uint8_t tag[AES_GCM_TAG_SIZE]);

/* --------------------------------------------------------------------------
 * One-shot in-place decryption of a small RAM blob.
 *
 * Blob layout: [iv:12][ciphertext:N][tag:16]
 * Total size = 12 + N + 16, so plaintext_size = size - 28.
 * -------------------------------------------------------------------------- */

bool aes_gcm_decrypt(const uint8_t key[AES128_KEY_SIZE],
                     uint8_t *data, uint32_t size,
                     const uint8_t *aad, uint32_t aad_len);

#ifdef __cplusplus
}
#endif

#endif /* AES_GCM_H */

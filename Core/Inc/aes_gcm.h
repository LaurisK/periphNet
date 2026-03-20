#ifndef AES_GCM_H
#define AES_GCM_H

#include "aes128.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AES_GCM_IV_SIZE   12u
#define AES_GCM_TAG_SIZE  16u

/**
 * AES-128-GCM authenticated decryption (in-place).
 *
 * Blob layout: [iv:12][ciphertext:N][tag:16]
 * Total size = 12 + N + 16, so plaintext_size = size - 28.
 *
 * @param key       16-byte AES-128 key.
 * @param data      Input blob (overwritten with plaintext on success).
 * @param size      Total blob size (iv + ciphertext + tag).
 * @param aad       Additional authenticated data (NULL if none).
 * @param aad_len   Length of AAD.
 * @return true if authentication tag is valid and decryption succeeded.
 */
bool aes_gcm_decrypt(const uint8_t key[AES128_KEY_SIZE],
                     uint8_t *data, uint32_t size,
                     const uint8_t *aad, uint32_t aad_len);

#ifdef __cplusplus
}
#endif

#endif /* AES_GCM_H */

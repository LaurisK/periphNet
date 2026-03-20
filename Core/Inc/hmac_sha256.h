#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include "sha256.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HMAC_SHA256_SIZE  SHA256_DIGEST_SIZE   /* 32 bytes */

typedef struct {
    sSha256Ctx inner;
    sSha256Ctx outer;
} sHmacSha256Ctx;

/**
 * Initialize HMAC-SHA256 context with key.
 */
void hmac_sha256_init(sHmacSha256Ctx *ctx, const uint8_t *key, uint32_t key_len);

/**
 * Feed data into HMAC computation (streaming, can be called multiple times).
 */
void hmac_sha256_update(sHmacSha256Ctx *ctx, const uint8_t *data, uint32_t len);

/**
 * Finalize and produce the 32-byte MAC.
 */
void hmac_sha256_final(sHmacSha256Ctx *ctx, uint8_t mac[HMAC_SHA256_SIZE]);

/**
 * One-shot HMAC-SHA256 for small messages.
 */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *data, uint32_t data_len,
                 uint8_t mac[HMAC_SHA256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* HMAC_SHA256_H */

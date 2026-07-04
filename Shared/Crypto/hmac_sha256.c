/*
 * HMAC-SHA256 implementation following RFC 2104.
 */

#include "hmac_sha256.h"
#include <string.h>

void hmac_sha256_init(sHmacSha256Ctx *ctx, const uint8_t *key, uint32_t key_len)
{
    uint8_t k_pad[SHA256_BLOCK_SIZE];

    /* If key is longer than block size, hash it first */
    if (key_len > SHA256_BLOCK_SIZE) {
        sSha256Ctx tmp;
        sha256_init(&tmp);
        sha256_update(&tmp, key, key_len);
        sha256_final(&tmp, k_pad);
        key_len = SHA256_DIGEST_SIZE;
    } else {
        memcpy(k_pad, key, key_len);
    }

    /* Pad key to block size with zeros */
    if (key_len < SHA256_BLOCK_SIZE) {
        memset(&k_pad[key_len], 0, SHA256_BLOCK_SIZE - key_len);
    }

    /* Inner hash: SHA256(K ^ ipad || message) */
    uint8_t ipad[SHA256_BLOCK_SIZE];
    for (uint32_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k_pad[i] ^ 0x36;
    }
    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, ipad, SHA256_BLOCK_SIZE);

    /* Outer hash: SHA256(K ^ opad || inner_hash) — set up for later */
    uint8_t opad[SHA256_BLOCK_SIZE];
    for (uint32_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        opad[i] = k_pad[i] ^ 0x5c;
    }
    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, opad, SHA256_BLOCK_SIZE);
}

void hmac_sha256_update(sHmacSha256Ctx *ctx, const uint8_t *data, uint32_t len)
{
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(sHmacSha256Ctx *ctx, uint8_t mac[HMAC_SHA256_SIZE])
{
    uint8_t inner_hash[SHA256_DIGEST_SIZE];
    sha256_final(&ctx->inner, inner_hash);

    sha256_update(&ctx->outer, inner_hash, SHA256_DIGEST_SIZE);
    sha256_final(&ctx->outer, mac);
}

void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *data, uint32_t data_len,
                 uint8_t mac[HMAC_SHA256_SIZE])
{
    sHmacSha256Ctx ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, data, data_len);
    hmac_sha256_final(&ctx, mac);
}

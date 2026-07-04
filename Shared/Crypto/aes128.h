#ifndef AES128_H
#define AES128_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AES128_KEY_SIZE    16u
#define AES128_BLOCK_SIZE  16u
#define AES128_ROUNDS      10u

typedef struct {
    uint32_t round_key[4 * (AES128_ROUNDS + 1)];
} sAes128Ctx;

/**
 * Expand 128-bit key into round keys.
 */
void aes128_init(sAes128Ctx *ctx, const uint8_t key[AES128_KEY_SIZE]);

/**
 * Encrypt one 16-byte block in-place.
 */
void aes128_encrypt_block(const sAes128Ctx *ctx, uint8_t block[AES128_BLOCK_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* AES128_H */

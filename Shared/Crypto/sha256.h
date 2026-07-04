#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_SIZE  64u
#define SHA256_DIGEST_SIZE 32u

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[SHA256_BLOCK_SIZE];
} sSha256Ctx;

void sha256_init(sSha256Ctx *ctx);
void sha256_update(sSha256Ctx *ctx, const uint8_t *data, uint32_t len);
void sha256_final(sSha256Ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* SHA256_H */

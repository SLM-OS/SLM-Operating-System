#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32u
#define SHA256_HEX_LEN 64u

struct sha256_ctx {
    uint64_t bit_len;
    uint32_t state[8];
    uint8_t block[64];
    size_t block_len;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LEN]);
void sha256_bytes_to_hex(const uint8_t digest[SHA256_DIGEST_LEN],
                         char out[SHA256_HEX_LEN + 1u]);

#endif /* SHA256_H */

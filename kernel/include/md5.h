/*
 * MD5 message-digest algorithm (RFC 1321).
 *
 * Public-domain implementation by Alexander Peslyak (Solar Designer,
 * 2001). Vendored from http://openwall.info/wiki/people/solar/
 * software/public-domain-source-code/md5 via HailoRT's common/include
 * (MIT licensed). No copyright is claimed on the algorithm itself
 * per the original author's declaration.
 *
 * We need MD5 specifically for the Hailo firmware control channel
 * (kernel/ai_accel/hailo/hailo_control.c): every request/response
 * is stamped with an MD5 of the payload which the firmware verifies
 * before processing. No cryptographic security relied on.
 */

#ifndef MD5_H
#define MD5_H

#include <stdint.h>
#include <stddef.h>

#define MD5_DIGEST_LENGTH 16

/*
 * On some platforms `size_t` is narrower than the 32-bit minimum
 * the algorithm expects. SLM-OS is 64-bit on all supported targets
 * so size_t is always >= 32 bits; keeping the upstream typedef
 * preserves drop-in compatibility with the reference.
 */
typedef size_t MD5_u32plus;

typedef struct {
    MD5_u32plus lo, hi;
    MD5_u32plus a, b, c, d;
    unsigned char buffer[64];
    MD5_u32plus block[16];
} MD5_CTX;

void MD5_Init(MD5_CTX *ctx);
void MD5_Update(MD5_CTX *ctx, const void *data, size_t size);
void MD5_Final(unsigned char *result, MD5_CTX *ctx);

/* One-shot helper: compute MD5 of `buf[0..len)` into `out[16]`. */
void md5_compute(const void *buf, size_t len, uint8_t out[MD5_DIGEST_LENGTH]);

#endif /* MD5_H */

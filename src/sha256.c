#include "cutils_sha256.h"

#include <string.h>

/* SHA-256 per FIPS 180-4, section 6.2. A straightforward portable
 * implementation: migration checksums hash a few kilobytes of SQL at
 * startup, so clarity wins over speed here. Words are assembled from
 * bytes explicitly, which keeps it independent of host endianness and
 * alignment. */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void compress(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int t = 0; t < 16; t++) {
        w[t] = (uint32_t)block[t * 4] << 24 | (uint32_t)block[t * 4 + 1] << 16 |
               (uint32_t)block[t * 4 + 2] << 8 | (uint32_t)block[t * 4 + 3];
    }
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], k = h[7];
    for (int t = 0; t < 64; t++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = k + S1 + ch + K[t] + w[t];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void cutils_sha256_init(cutils_sha256_ctx *ctx)
{
    static const uint32_t H0[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(ctx->h, H0, sizeof(H0));
    ctx->total  = 0;
    ctx->buflen = 0;
}

void cutils_sha256_update(cutils_sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;
    ctx->total += len;

    if (ctx->buflen > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p   += take;
        len -= take;
        if (ctx->buflen < 64) return;
        compress(ctx->h, ctx->buf);
        ctx->buflen = 0;
    }
    while (len >= 64) {
        compress(ctx->h, p);
        p   += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void cutils_sha256_final(cutils_sha256_ctx *ctx,
                         uint8_t out[CUTILS_SHA256_DIGEST_LEN])
{
    /* Padding (5.1.1): a 1 bit, zeros to 56 mod 64, then the message
     * length in bits as a 64-bit big-endian integer. */
    uint64_t bits = ctx->total * 8u;
    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        memset(ctx->buf + ctx->buflen, 0, 64 - ctx->buflen);
        compress(ctx->h, ctx->buf);
        ctx->buflen = 0;
    }
    memset(ctx->buf + ctx->buflen, 0, 56 - ctx->buflen);
    for (int i = 0; i < 8; i++)
        ctx->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    compress(ctx->h, ctx->buf);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
    memset(ctx, 0, sizeof(*ctx));
}

void cutils_sha256(const void *data, size_t len,
                   uint8_t out[CUTILS_SHA256_DIGEST_LEN])
{
    cutils_sha256_ctx ctx;
    cutils_sha256_init(&ctx);
    cutils_sha256_update(&ctx, data, len);
    cutils_sha256_final(&ctx, out);
}

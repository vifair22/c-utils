#ifndef CUTILS_SHA256_H
#define CUTILS_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Library-internal SHA-256 (FIPS 180-4), not part of the public API.
 *
 * It exists so the migration runner can checksum SQL without OpenSSL:
 * libcrypto was c-utils' only use of OpenSSL, and statically linking its
 * one SHA256() call pulled in most of libcrypto (1.9.0). Output is
 * byte-identical to OpenSSL's SHA256(), so checksums recorded by earlier
 * versions still match. */

#define CUTILS_SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t total;      /* bytes hashed so far */
    uint8_t  buf[64];
    size_t   buflen;
} cutils_sha256_ctx;

void cutils_sha256_init(cutils_sha256_ctx *ctx);
void cutils_sha256_update(cutils_sha256_ctx *ctx, const void *data, size_t len);
void cutils_sha256_final(cutils_sha256_ctx *ctx,
                         uint8_t out[CUTILS_SHA256_DIGEST_LEN]);

/* One-shot digest of len bytes. */
void cutils_sha256(const void *data, size_t len,
                   uint8_t out[CUTILS_SHA256_DIGEST_LEN]);

#endif

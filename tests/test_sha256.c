#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#include "cutils_sha256.h"

/* The built-in SHA-256 (1.9.0) replaced OpenSSL's SHA256() for migration
 * checksums. These tests pin it to the FIPS 180-4 example vectors and to
 * OpenSSL byte for byte, since a migration checksum recorded by an earlier
 * version must still match. OpenSSL is a test-only dependency. */

static void hex(const uint8_t d[32], char out[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
}

static void assert_digest(const void *data, size_t len, const char *want)
{
    uint8_t d[32];
    char got[65];
    cutils_sha256(data, len, d);
    hex(d, got);
    assert_string_equal(got, want);
}

static void test_nist_vectors(void **state)
{
    (void)state;
    assert_digest("", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert_digest("abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert_digest(two_block, strlen(two_block),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_million_a_streamed(void **state)
{
    (void)state;
    uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    cutils_sha256_ctx ctx;
    cutils_sha256_init(&ctx);
    for (int i = 0; i < 1000; i++)
        cutils_sha256_update(&ctx, chunk, sizeof(chunk));
    uint8_t d[32];
    char got[65];
    cutils_sha256_final(&ctx, d);
    hex(d, got);
    assert_string_equal(got,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* Every length across several block boundaries (55, 56, 63, 64 and the
 * multi-block cases), one-shot and in uneven chunks, against OpenSSL. */
static void test_matches_openssl(void **state)
{
    (void)state;
    enum { MAX = 1100 };
    uint8_t *buf = malloc(MAX);
    assert_non_null(buf);
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < MAX; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (uint8_t)x;
    }

    for (size_t len = 0; len <= MAX; len++) {
        uint8_t want[32], one[32], streamed[32];
        SHA256(buf, len, want);
        cutils_sha256(buf, len, one);
        assert_memory_equal(one, want, 32);

        cutils_sha256_ctx ctx;
        cutils_sha256_init(&ctx);
        size_t off = 0, step = 1;
        while (off < len) {
            size_t n = len - off < step ? len - off : step;
            cutils_sha256_update(&ctx, buf + off, n);
            off += n;
            step = step * 3 % 97 + 1;
        }
        cutils_sha256_final(&ctx, streamed);
        assert_memory_equal(streamed, want, 32);
    }
    free(buf);
}

/* Migration files can run to many kilobytes. */
static void test_matches_openssl_large(void **state)
{
    (void)state;
    static const size_t lens[] = { 4095, 4096, 4097, 65536, 65537, 1048583 };
    uint8_t *buf = malloc(1048583);
    assert_non_null(buf);
    for (size_t i = 0; i < 1048583; i++) buf[i] = (uint8_t)(i * 2654435761u >> 13);
    for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
        uint8_t want[32], got[32];
        SHA256(buf, lens[k], want);
        cutils_sha256(buf, lens[k], got);
        assert_memory_equal(got, want, 32);
    }
    free(buf);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_nist_vectors),
        cmocka_unit_test(test_million_a_streamed),
        cmocka_unit_test(test_matches_openssl),
        cmocka_unit_test(test_matches_openssl_large),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

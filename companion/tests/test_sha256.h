#ifndef COMPANION_TEST_SHA256_H
#define COMPANION_TEST_SHA256_H

/* A plain FIPS 180-4 SHA-256 for host tests, so they need no Mbed TLS build.
 * The journal's self-test checks it against the "abc" vector. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t TestSha256_Rotate(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }

static void TestSha256_Block(uint32_t state[8], const unsigned char block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64], v[8], t1, t2;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = (TestSha256_Rotate(w[i - 2], 17) ^ TestSha256_Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7]
            + (TestSha256_Rotate(w[i - 15], 7) ^ TestSha256_Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
    memcpy(v, state, sizeof(v));
    for (i = 0; i < 64; i++) {
        t1 = v[7] + (TestSha256_Rotate(v[4], 6) ^ TestSha256_Rotate(v[4], 11) ^ TestSha256_Rotate(v[4], 25))
            + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
        t2 = (TestSha256_Rotate(v[0], 2) ^ TestSha256_Rotate(v[0], 13) ^ TestSha256_Rotate(v[0], 22))
            + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof(*v));
        v[4] += t1; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) state[i] += v[i];
}

static int TestSha256(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    unsigned char block[64];
    uint64_t bits = (uint64_t)size * 8;
    size_t rest;
    unsigned i;
    for (; size >= 64; bytes += 64, size -= 64) TestSha256_Block(state, bytes);
    rest = size;
    memset(block, 0, sizeof(block));
    if (rest) memcpy(block, bytes, rest);
    block[rest] = 0x80;
    if (rest >= 56) { TestSha256_Block(state, block); memset(block, 0, sizeof(block)); }
    for (i = 0; i < 8; i++) block[63 - i] = (unsigned char)(bits >> (8 * i));
    TestSha256_Block(state, block);
    for (i = 0; i < 32; i++) digest[i] = (unsigned char)(state[i / 4] >> (24 - 8 * (i % 4)));
    return 0;
}

#endif

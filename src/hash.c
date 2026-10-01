/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * MD5 (RFC 1321) and SHA-256 (FIPS 180-4).
 *
 * Both are Merkle-Damgard constructions over 64-byte blocks with the same
 * padding -- a 0x80 byte, zeros, and the message length in bits -- and differ
 * only in the compression function and in the byte order of the length and the
 * result. So one buffer and one padding routine serve both, and the two
 * compression functions are the only part that is not shared.
 */
#include "hash.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* MD5                                                                       */
/* ------------------------------------------------------------------------- */

static uint32_t rol32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static uint32_t load_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void md5_block(uint32_t st[4], const unsigned char blk[64])
{
    /* The per-round shift amounts and the sine-derived constants, straight
     * from RFC 1321 section 3.4. */
    static const unsigned shift[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    uint32_t m[16];
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    unsigned i;

    for (i = 0; i < 16; i++)
    {
        m[i] = load_le32(blk + (size_t)i * 4);
    }
    for (i = 0; i < 64; i++)
    {
        uint32_t f;
        unsigned g;
        uint32_t tmp;

        if (i < 16)
        {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else
        {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        tmp = d;
        d = c;
        c = b;
        b = b + rol32(a + f + k[i] + m[g], shift[i]);
        a = tmp;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

/* ------------------------------------------------------------------------- */
/* SHA-256                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t ror32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(uint32_t st[8], const unsigned char blk[64])
{
    /* The first 32 bits of the fractional parts of the cube roots of the
     * first 64 primes, FIPS 180-4 section 4.2.2. */
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t w[64];
    uint32_t v[8];
    unsigned i;

    for (i = 0; i < 16; i++)
    {
        w[i] = load_be32(blk + (size_t)i * 4);
    }
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, st, sizeof(v));
    for (i = 0; i < 64; i++)
    {
        uint32_t s1 = ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
        uint32_t s0 = ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22);
        uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = s0 + maj;

        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++)
    {
        st[i] += v[i];
    }
}

/* ------------------------------------------------------------------------- */
/* The shared part                                                           */
/* ------------------------------------------------------------------------- */

static void compress(struct tmd_hash_ctx *ctx, const unsigned char blk[64])
{
    if (ctx->algo == TMD_HASH_MD5)
    {
        md5_block(ctx->state, blk);
    } else
    {
        sha256_block(ctx->state, blk);
    }
}

void tmd_hash_init(struct tmd_hash_ctx *ctx, enum tmd_hash algo)
{
    static const uint32_t md5_iv[4] = {
        0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
    };
    static const uint32_t sha256_iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    memset(ctx, 0, sizeof(*ctx));
    ctx->algo = algo;
    if (algo == TMD_HASH_MD5)
    {
        memcpy(ctx->state, md5_iv, sizeof(md5_iv));
    } else
    {
        memcpy(ctx->state, sha256_iv, sizeof(sha256_iv));
    }
}

void tmd_hash_update(struct tmd_hash_ctx *ctx, const void *data, size_t n)
{
    const unsigned char *p = data;

    ctx->length += n;
    if (ctx->nblock > 0)
    {
        size_t take = sizeof(ctx->block) - ctx->nblock;

        if (take > n)
        {
            take = n;
        }
        memcpy(ctx->block + ctx->nblock, p, take);
        ctx->nblock += take;
        p += take;
        n -= take;
        if (ctx->nblock < sizeof(ctx->block))
        {
            return;
        }
        compress(ctx, ctx->block);
        ctx->nblock = 0;
    }
    while (n >= sizeof(ctx->block))
    {
        compress(ctx, p);
        p += sizeof(ctx->block);
        n -= sizeof(ctx->block);
    }
    if (n > 0)
    {
        memcpy(ctx->block, p, n);
        ctx->nblock = n;
    }
}

void tmd_hash_final(struct tmd_hash_ctx *ctx, char hex[TMD_HASH_HEX_MAX])
{
    uint64_t      bits = ctx->length * 8u;
    unsigned char tail[8];
    /* Zeroed so that every byte is defined whichever digest fills it; MD5
     * writes 16 of the 32. */
    unsigned char out[32] = { 0 };
    size_t        nout;
    size_t        i;

    /* 0x80, then zeros until 8 bytes short of a block boundary. */
    ctx->block[ctx->nblock++] = 0x80;
    if (ctx->nblock > sizeof(ctx->block) - 8)
    {
        memset(ctx->block + ctx->nblock, 0, sizeof(ctx->block) - ctx->nblock);
        compress(ctx, ctx->block);
        ctx->nblock = 0;
    }
    memset(ctx->block + ctx->nblock, 0, sizeof(ctx->block) - 8 - ctx->nblock);

    /* The length in bits: little-endian for MD5, big-endian for SHA-256. */
    for (i = 0; i < 8; i++)
    {
        unsigned shift = (unsigned)(ctx->algo == TMD_HASH_MD5 ? i : 7 - i) * 8u;

        tail[i] = (unsigned char)(bits >> shift);
    }
    memcpy(ctx->block + sizeof(ctx->block) - 8, tail, 8);
    compress(ctx, ctx->block);

    nout = ctx->algo == TMD_HASH_MD5 ? 16 : 32;
    for (i = 0; i < nout; i++)
    {
        uint32_t word = ctx->state[i / 4];
        unsigned shift = ctx->algo == TMD_HASH_MD5 ? (unsigned)(i % 4) * 8u
                                                   : (unsigned)(3 - i % 4) * 8u;

        out[i] = (unsigned char)(word >> shift);
    }
    for (i = 0; i < nout; i++)
    {
        (void)snprintf(hex + i * 2, 3, "%02x", out[i]);
    }
    hex[nout * 2] = '\0';
}

const char *tmd_hash_name(enum tmd_hash algo)
{
    switch (algo)
    {
    case TMD_HASH_MD5:    return "md5";
    case TMD_HASH_SHA256: return "sha256";
    default:              return NULL;
    }
}

size_t tmd_hash_hex_len(enum tmd_hash algo)
{
    switch (algo)
    {
    case TMD_HASH_MD5:    return 32;
    case TMD_HASH_SHA256: return 64;
    default:              return 0;
    }
}

bool tmd_hash_parse(const char *s, enum tmd_hash *out)
{
    static const struct {
        const char   *name;
        enum tmd_hash value;
    } names[] = {
        { "md5",     TMD_HASH_MD5    },
        { "sha256",  TMD_HASH_SHA256 },
        { "sha-256", TMD_HASH_SHA256 },
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        const char *a = s;
        const char *b = names[i].name;

        while (*a && *b && tolower((unsigned char)*a) == *b)
        {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0')
        {
            *out = names[i].value;
            return true;
        }
    }
    return false;
}

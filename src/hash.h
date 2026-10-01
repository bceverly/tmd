/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * Content digests for --hash.
 *
 * Implemented here rather than linked, for the reason everything else in this
 * program is: tmd links libc and nothing else, and a package that needs
 * libcrypto to print an MD5 has traded that away for two short, well-specified
 * algorithms with published test vectors. The unit tests check both against
 * those vectors.
 *
 * MD5 is here because the documents these digests get compared with -- load
 * files, production indexes, older manifests -- record MD5, not because it is
 * a good hash. It identifies content somebody else already fingerprinted; it is
 * not used to resist anybody, and SHA-256 is offered beside it for when the
 * choice is yours.
 */
#ifndef TMD_HASH_H
#define TMD_HASH_H

#include <stddef.h>
#include <stdint.h>

#include "tmd.h"

/* Long enough for the larger digest in hex, plus the terminator. */
#define TMD_HASH_HEX_MAX 65

struct tmd_hash_ctx {
    enum tmd_hash algo;
    uint64_t      length;     /* bytes fed so far */
    unsigned char block[64];  /* both algorithms work in 64-byte blocks */
    size_t        nblock;
    uint32_t      state[8];   /* MD5 uses four of these, SHA-256 all eight */
};

void tmd_hash_init(struct tmd_hash_ctx *ctx, enum tmd_hash algo);
void tmd_hash_update(struct tmd_hash_ctx *ctx, const void *data, size_t n);
/* Writes the digest as lowercase hex. `hex` holds TMD_HASH_HEX_MAX bytes. */
void tmd_hash_final(struct tmd_hash_ctx *ctx, char hex[TMD_HASH_HEX_MAX]);

/* "md5", "sha256"; NULL for TMD_HASH_NONE. */
const char *tmd_hash_name(enum tmd_hash algo);
/* How many hex digits the digest has: 32 or 64, 0 for TMD_HASH_NONE. */
size_t      tmd_hash_hex_len(enum tmd_hash algo);
/* Accepts the names above in any case, and "sha-256". */
bool        tmd_hash_parse(const char *s, enum tmd_hash *out);

#endif /* TMD_HASH_H */

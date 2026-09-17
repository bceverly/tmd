/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * The fuzz target: everything a hostile archive can reach.
 *
 * A tar reader is a parser for a format that anyone can hand you, so it is
 * exactly the kind of code that earns a fuzzer. One entry point does the work —
 * bytes in, the whole reader and every renderer run over them — and it is
 * reached two ways:
 *
 *   libFuzzer (clang)  LLVMFuzzerTestOneInput, coverage-guided, millions of
 *                      cases a second
 *   the built-in loop  a mutation fuzzer over a seed corpus, in-process, which
 *                      needs nothing but the compiler already in use and is
 *                      what `make fuzz` runs by default
 *
 * The built-in loop is not as good as libFuzzer and does not pretend to be.
 * It exists so that fuzzing is something that happens on every machine rather
 * than only where clang is installed — and it has the same job: run under the
 * sanitizers, and let them do the judging.
 *
 * Rendering is included deliberately. A path that reaches the parser and stops
 * would leave the JSON escaper, the CSV quoter and the time formatter — all of
 * which handle attacker-controlled bytes — untested.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "render.h"
#include "source.h"
#include "tar.h"
#include "tmd.h"
#include "util.h"

/* Every archive is read this many ways, because the renderers are part of the
 * attack surface and they do different things with the same entry. */
static void run_one_mode(const uint8_t *data, size_t size,
                         const struct tmd_options *opt, FILE *sink)
{
    struct tmd_source      *src;
    struct tmd_reader      *reader;
    struct tmd_render      *rd;
    const struct tmd_entry *entry;
    unsigned                guard = 0;
    int                     rc;

    src = tmd_source_open_memory(data, size, "(fuzz)");
    reader = tmd_reader_new(src);
    rd = tmd_render_new(sink, opt, 1);
    tmd_render_archive_begin(rd, tmd_reader_archive(reader));

    while ((rc = tmd_reader_next(reader, &entry)) == 1) {
        tmd_render_entry(rd, entry);
        /* A crafted archive can describe a great many members in very few
         * bytes. The fuzzer is looking for memory errors, not for slow inputs,
         * and a case that runs for a minute is a case that is not being run. */
        if (++guard > 20000) {
            break;
        }
    }
    if (rc == 0) {
        tmd_render_archive_end(rd, tmd_reader_archive(reader));
    }
    tmd_render_finish(rd);

    tmd_render_free(rd);
    tmd_reader_free(reader);
    tmd_source_close(src);
}

int tmd_fuzz_one(const uint8_t *data, size_t size);

int tmd_fuzz_one(const uint8_t *data, size_t size)
{
    static FILE       *sink = NULL;
    struct tmd_options opt;
    size_t             i;

    /* /dev/null rather than a buffer: the renderers are being exercised for
     * their memory behavior, and nothing here reads what they wrote. */
    if (!sink) {
        sink = fopen("/dev/null", "w");
        if (!sink) {
            return 0;
        }
    }

    for (i = 0; i < 6; i++) {
        /* A zeroed options struct renders in UTC, so the run does not depend
         * on the machine's zone. */
        memset(&opt, 0, sizeof(opt));
        switch (i) {
        case 0: break;                                  /* the listing      */
        case 1: opt.long_form = true; opt.headers = true; break;
        case 2: opt.output = TMD_OUT_JSON; break;
        case 3: opt.output = TMD_OUT_CSV; break;
        case 4: opt.info = true; opt.human = true; opt.full_time = true; break;
        case 5: opt.local = true; opt.full_time = true; break;
        default: break;
        }
        run_one_mode(data, size, &opt, sink);
    }
    return 0;
}

#ifdef TMD_LIBFUZZER

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return tmd_fuzz_one(data, size);
}

#else /* the built-in mutation loop */

/*
 * xorshift rather than rand(): the point is a reproducible run. A crash found
 * at iteration 4,812,993 with seed 12345 has to be reachable again from the
 * same two numbers, and rand() is neither seedable in a portable way nor the
 * same sequence across libcs.
 */
static uint64_t rng_state = 88172645463325252ULL;

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static size_t rng_below(size_t limit)
{
    return limit ? (size_t)(rng_next() % limit) : 0;
}

/*
 * Mutations chosen for what a tar header is made of.
 *
 * Random byte flips alone almost never produce a valid checksum, so almost
 * every case would be rejected at the first block and the parser past it would
 * never run. These operators aim at the fields that decide control flow — the
 * typeflag, the size, the magic — and re-checksum the block afterwards, so the
 * mutated archive stays parseable and the interesting code is reached.
 */
static void recompute_checksum(uint8_t *block)
{
    unsigned sum = 0;
    size_t   i;

    memset(block + 148, ' ', 8);
    for (i = 0; i < TMD_BLOCK_SIZE; i++) {
        sum += block[i];
    }
    (void)snprintf((char *)block + 148, 8, "%06o", sum & 0777777u);
}

static size_t mutate(uint8_t *buf, size_t size, size_t capacity)
{
    static const char typeflags[] = "012345670gxLKSDMVXEA\0z";
    size_t blocks = size / TMD_BLOCK_SIZE;
    size_t rounds = 1 + rng_below(6);
    size_t r;

    for (r = 0; r < rounds; r++) {
        switch (rng_next() % 8) {
        case 0: /* flip a byte anywhere */
            if (size) {
                buf[rng_below(size)] ^= (uint8_t)(1u << (rng_next() % 8));
            }
            break;
        case 1: /* set a random byte to a random value */
            if (size) {
                buf[rng_below(size)] = (uint8_t)rng_next();
            }
            break;
        case 2: /* change a typeflag and keep the header valid */
            if (blocks) {
                uint8_t *block = buf + rng_below(blocks) * TMD_BLOCK_SIZE;
                block[156] = (uint8_t)typeflags[rng_next() % (sizeof(typeflags) - 1)];
                recompute_checksum(block);
            }
            break;
        case 3: /* rewrite a size field, sometimes to something absurd */
            if (blocks) {
                uint8_t *block = buf + rng_below(blocks) * TMD_BLOCK_SIZE;
                (void)snprintf((char *)block + 124, 12, "%011llo",
                               (unsigned long long)rng_next());
                recompute_checksum(block);
            }
            break;
        case 4: /* switch a header's dialect */
            if (blocks) {
                uint8_t *block = buf + rng_below(blocks) * TMD_BLOCK_SIZE;
                static const char *magics[] = { "ustar\0" "00", "ustar  ",
                                                "\0\0\0\0\0\0\0\0", "ustarXX" };
                memcpy(block + 257, magics[rng_next() % 4], 8);
                recompute_checksum(block);
            }
            break;
        case 5: /* set the high bit, which turns a field into base-256 */
            if (blocks) {
                uint8_t *block = buf + rng_below(blocks) * TMD_BLOCK_SIZE;
                block[124 + rng_below(4)] |= 0x80;
                recompute_checksum(block);
            }
            break;
        case 6: /* truncate, which is the most common real-world damage */
            if (size > TMD_BLOCK_SIZE) {
                size = rng_below(size);
            }
            break;
        case 7: /* grow with junk, as a concatenated archive would */
            if (size + TMD_BLOCK_SIZE <= capacity) {
                size_t i;
                for (i = 0; i < TMD_BLOCK_SIZE; i++) {
                    buf[size + i] = (uint8_t)rng_next();
                }
                size += TMD_BLOCK_SIZE;
            }
            break;
        default:
            break;
        }
        blocks = size / TMD_BLOCK_SIZE;
    }
    return size;
}

struct seed {
    uint8_t *data;
    size_t   size;
};

static struct seed *seeds = NULL;
static size_t       nseeds = 0;

static void load_seed(const char *path)
{
    FILE   *f = fopen(path, "rb");
    long    size;
    uint8_t *data;

    if (!f) {
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return;
    }
    size = ftell(f);
    rewind(f);
    /* A seed larger than this is not a better seed; it is a slower one. */
    if (size <= 0 || size > 4 * 1024 * 1024) {
        (void)fclose(f);
        return;
    }
    data = tmd_xmalloc((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        (void)fclose(f);
        return;
    }
    (void)fclose(f);

    seeds = tmd_xrealloc(seeds, (nseeds + 1) * sizeof(*seeds));
    seeds[nseeds].data = data;
    seeds[nseeds].size = (size_t)size;
    nseeds++;
}

/*
 * Getting the failing input back out.
 *
 * A sanitizer abort does not return, so the obvious approach — parse, and write
 * the input out if it crashed — never writes anything. Writing every input
 * before parsing it does work and costs a file write per case, which at tens of
 * thousands of cases a second is most of the run.
 *
 * The sanitizers provide the hook for exactly this. __sanitizer_set_death_callback
 * runs just before the process is killed, with the failing input still in
 * memory. It is declared weak so that a build without a sanitizer still links,
 * in which case no callback is installed and nothing is lost that was there.
 */
extern void __sanitizer_set_death_callback(void (*callback)(void))
    __attribute__((weak));

static const uint8_t *current_data = NULL;
static size_t         current_size = 0;
static const char    *crash_path = ".fuzz/crash.tar";
static unsigned long  current_iteration = 0;
static uint64_t       run_seed = 0;

static void on_death(void)
{
    FILE *f;

    if (!current_data) {
        return;
    }
    f = fopen(crash_path, "wb");
    if (f) {
        (void)fwrite(current_data, 1, current_size, f);
        (void)fclose(f);
    }
    /* write(2)-free reporting: this runs inside a dying process and fprintf is
     * not guaranteed to be safe here, but it is what every sanitizer's own
     * death path does and the alternative is saying nothing at all. */
    (void)fprintf(stderr,
                  "\n  the failing input is %s (iteration %lu, rng seed %llu)\n"
                  "  reproduce it with: %s --replay=%s\n",
                  crash_path, current_iteration, (unsigned long long)run_seed,
                  "fuzz_tar", crash_path);
}

int main(int argc, char **argv)
{
    const char *replay = NULL;
    unsigned    seconds = 30;
    uint64_t    seed = 0;
    size_t      capacity = 8 * 1024 * 1024;
    uint8_t    *buf;
    time_t      deadline;
    unsigned long iteration = 0;
    int         i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--seconds=", 10) == 0) {
            seconds = (unsigned)strtoul(argv[i] + 10, NULL, 10);
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            seed = strtoull(argv[i] + 7, NULL, 10);
        } else if (strncmp(argv[i], "--crash-file=", 13) == 0) {
            crash_path = argv[i] + 13;
        } else if (strncmp(argv[i], "--replay=", 9) == 0) {
            /* Run one saved input and stop: this is how a crash found here is
             * turned back into a failing case under a debugger. */
            replay = argv[i] + 9;
        } else {
            load_seed(argv[i]);
        }
    }

    if (replay) {
        nseeds = 0;
        load_seed(replay);
        if (nseeds == 0) {
            (void)fprintf(stderr, "fuzz_tar: cannot read %s\n", replay);
            return 2;
        }
        (void)printf("  replaying %s (%zu bytes)\n", replay, seeds[0].size);
        (void)tmd_fuzz_one(seeds[0].data, seeds[0].size);
        (void)printf("  clean\n");
        free(seeds[0].data);
        free(seeds);
        return 0;
    }

    if (nseeds == 0) {
        (void)fprintf(stderr, "fuzz_tar: no seed archives were given\n");
        return 2;
    }
    if (seed) {
        rng_state = seed;
    }
    run_seed = rng_state;
    if (__sanitizer_set_death_callback) {
        __sanitizer_set_death_callback(on_death);
    }

    buf = tmd_xmalloc(capacity);
    deadline = time(NULL) + (time_t)seconds;

    (void)printf("  %zu seeds, %u seconds, rng seed %llu\n", nseeds, seconds,
                 (unsigned long long)run_seed);

    while (time(NULL) < deadline) {
        const struct seed *s = &seeds[rng_below(nseeds)];
        size_t             size = s->size < capacity ? s->size : capacity;

        memcpy(buf, s->data, size);
        size = mutate(buf, size, capacity);

        /* The death callback reads these if this case is the last one. */
        current_data = buf;
        current_size = size;
        current_iteration = iteration;

        (void)tmd_fuzz_one(buf, size);

        iteration++;
        if (iteration % 5000 == 0) {
            (void)printf("  %lu cases\r", iteration);
            (void)fflush(stdout);
        }
    }

    (void)printf("  %lu cases, no crashes\n", iteration);
    for (i = 0; (size_t)i < nseeds; i++) {
        free(seeds[i].data);
    }
    free(seeds);
    free(buf);
    return 0;
}

#endif /* TMD_LIBFUZZER */

/* jdlz.c - EA's JDLZ compressor for the HD UI installer (tools/setup_kit/bun.py loads it with ctypes).
 *
 * Format (the game's decoder 0069c5d0; bun.jdlz_decompress): 16-byte header 'JDLZ' 02 10 00 00, unpacked size,
 * packed size (header included); then tokens steered by two flag bytes read when used up: f1 says literal (0) or
 * match (1); for a match f2 says which kind: 1 = distance 1..16, length 3..4098; 0 = distance 17..2064, length
 * 3..34. The game decodes until the packed size is used up, reading a flag byte right after each token that empties
 * one - so a stream ending on an emptied flag byte carries that byte too. Greedy matching with a hash chain.
 * Every result is decoded again the game's way and compared before it is returned.
 *
 *   cc -O2 -shared -o jdlz.dylib jdlz.c
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW 4096u /* ring of previous positions, >= the 2064 longest distance */
#define HASH_BITS 16
#define CHAIN 48
#define SLACK 4098u /* decode buffers are n + SLACK bytes */

/* The game's decoder (0069c5d0) exactly: driven by the packed size (it stops when the bytes after the first two
 * flag bytes are used up, never by the output size), flag bytes refilled right after the token that used up the
 * last bit - also after the final token. Returns the bytes written, or 0 if the stream would misbehave. */
static size_t decode(const uint8_t *src, size_t n_src, uint8_t *out, size_t n) {
    if (n_src < 18) return 0;
    const uint8_t *p = src + 18, *end = src + n_src;
    unsigned f1 = src[16] | 0x100, f2 = src[17] | 0x100;
    size_t o = 0;
    while (p != end) {
        if (p > end) return 0;
        if (!(f1 & 1)) {
            if (o < n) out[o++] = *p;   /* the game drops literals past the end */
            ++p;
        } else {
            if (p + 2 > end) return 0;
            size_t len, back;
            if (f2 & 1) {
                len = ((size_t)(p[0] & 0xF0) << 4 | p[1]) + 3;
                back = (p[0] & 0x0F) + 1;
            } else {
                len = (p[0] & 0x1F) + 3;
                back = ((size_t)(p[0] & 0xE0) << 3 | p[1]) + 17;
            }
            f2 >>= 1;
            p += 2;
            if (back > o || o + len > n + SLACK) return 0;   /* retail streams may end a match past n */
            for (size_t k = 0; k < len; ++k, ++o) out[o] = out[o - back];
        }
        f1 >>= 1;
        if (f1 == 1) {
            if (p >= end) return 0;
            f1 = *p++ | 0x100;
        }
        if (f2 == 1) {
            if (p >= end) return 0;
            f2 = *p++ | 0x100;
        }
    }
    return o;
}

static inline uint32_t hash3(const uint8_t *p) {
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u >> (32 - HASH_BITS);
}

/* Returns the packed size, or 0 if it would not be smaller than cap (or the check failed). */
size_t jdlz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (cap < 20 || n == 0) return 0;
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * WINDOW);
    if (!head || !prev) {
        free(head);
        free(prev);
        return 0;
    }
    memset(head, 0xff, sizeof(int32_t) << HASH_BITS);
    size_t out = 16, pos = 0, f1_at = 0, f2_at = 0;
    unsigned f1_n = 8, f2_n = 8;
    size_t ok = 1;
#define INSERT(p)                                          \
    do {                                                   \
        if ((p) + 3 <= n) {                                \
            uint32_t h_ = hash3(src + (p));                \
            prev[(p) & (WINDOW - 1)] = head[h_];           \
            head[h_] = (int32_t)(p);                       \
        }                                                  \
    } while (0)
    while (pos < n) {
        if (out + 4 > cap) {
            ok = 0;
            break;
        }
        if (f1_n == 8) {
            f1_at = out++;
            dst[f1_at] = 0;
            f1_n = 0;
        }
        if (f2_n == 8) {
            f2_at = out++;
            dst[f2_at] = 0;
            f2_n = 0;
        }
        size_t left = n - pos, best_len = 0, best_back = 0;
        /* Near: distance 1..16, up to 4098 bytes. */
        for (size_t back = 1; back <= 16 && back <= pos; ++back) {
            size_t max = left < 4098 ? left : 4098, l = 0;
            while (l < max && src[pos + l] == src[pos + l - back]) ++l;
            if (l > best_len) {
                best_len = l;
                best_back = back;
                if (l == max) break;
            }
        }
        /* Far: distance 17..2064, up to 34 bytes. */
        if (best_len < 34 && left >= 3) {
            int32_t cand = head[hash3(src + pos)];
            for (int depth = 0; cand >= 0 && depth < CHAIN; ++depth) {
                size_t back = pos - (size_t)cand;
                if (back > 2064) break;
                if (back >= 17) {
                    size_t max = left < 34 ? left : 34, l = 0;
                    while (l < max && src[cand + l] == src[pos + l]) ++l;
                    if (l > best_len) {
                        best_len = l;
                        best_back = back;
                        if (l == max) break;
                    }
                }
                int32_t next = prev[cand & (WINDOW - 1)];
                if (next >= cand) break;
                cand = next;
            }
        }
        if (best_len >= 3) {
            dst[f1_at] |= (uint8_t)(1u << f1_n);
            size_t l = best_len - 3;
            if (best_back <= 16) {
                dst[f2_at] |= (uint8_t)(1u << f2_n);
                dst[out++] = (uint8_t)((l >> 8) << 4 | (best_back - 1));
                dst[out++] = (uint8_t)l;
            } else {
                size_t b = best_back - 17;
                dst[out++] = (uint8_t)((b >> 8) << 5 | l);
                dst[out++] = (uint8_t)b;
            }
            ++f2_n;
            for (size_t k = 0; k < best_len; ++k) INSERT(pos + k);
            pos += best_len;
        } else {
            INSERT(pos);
            dst[out++] = src[pos++];
        }
        ++f1_n;
    }
#undef INSERT
    /* The game reads the next flag byte right after the token that used up the last bit, even at the end. */
    if (ok && f1_n == 8) dst[out++] = 0;
    if (ok && f2_n == 8) dst[out++] = 0;
    free(head);
    free(prev);
    if (!ok || out >= cap) return 0;
    memcpy(dst, "JDLZ\x02\x10\0\0", 8);
    uint32_t sizes[2] = {(uint32_t)n, (uint32_t)out};
    memcpy(dst + 8, sizes, 8);
    uint8_t *check = malloc(n + SLACK);
    size_t good = check && decode(dst, out, check, n) == n && memcmp(check, src, n) == 0;
    free(check);
    return good ? out : 0;
}

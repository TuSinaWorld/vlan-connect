/*
    Minimal SSE2/SSSE3-to-NEON compatibility helpers for the bundled cm256cc
    sources. This is not the upstream sse2neon project.

    SPDX-License-Identifier: GPL-3.0-only
*/

#ifndef CM256CC_MINIMAL_SSE2NEON_H
#define CM256CC_MINIMAL_SSE2NEON_H

#include <arm_neon.h>
#include <stdint.h>

typedef uint8x16_t __m128i;

static inline __m128i _mm_set_epi8(
    char e15, char e14, char e13, char e12,
    char e11, char e10, char e9, char e8,
    char e7, char e6, char e5, char e4,
    char e3, char e2, char e1, char e0)
{
    const uint8_t values[16] = {
        (uint8_t)e0, (uint8_t)e1, (uint8_t)e2, (uint8_t)e3,
        (uint8_t)e4, (uint8_t)e5, (uint8_t)e6, (uint8_t)e7,
        (uint8_t)e8, (uint8_t)e9, (uint8_t)e10, (uint8_t)e11,
        (uint8_t)e12, (uint8_t)e13, (uint8_t)e14, (uint8_t)e15
    };
    return vld1q_u8(values);
}

static inline __m128i _mm_set1_epi8(char b)
{
    return vdupq_n_u8((uint8_t)b);
}

static inline __m128i _mm_load_si128(const __m128i *p)
{
    return vld1q_u8((const uint8_t *)p);
}

static inline __m128i _mm_loadu_si128(const __m128i *p)
{
    return vld1q_u8((const uint8_t *)p);
}

static inline void _mm_store_si128(__m128i *p, __m128i a)
{
    vst1q_u8((uint8_t *)p, a);
}

static inline void _mm_storeu_si128(__m128i *p, __m128i a)
{
    vst1q_u8((uint8_t *)p, a);
}

static inline __m128i _mm_and_si128(__m128i a, __m128i b)
{
    return vandq_u8(a, b);
}

static inline __m128i _mm_xor_si128(__m128i a, __m128i b)
{
    return veorq_u8(a, b);
}

static inline __m128i _mm_srli_epi64(__m128i a, int imm)
{
    uint64_t lanes[2];
    vst1q_u64(lanes, vreinterpretq_u64_u8(a));

    if (imm >= 64) {
        lanes[0] = 0;
        lanes[1] = 0;
    } else if (imm > 0) {
        lanes[0] >>= imm;
        lanes[1] >>= imm;
    }

    return vreinterpretq_u8_u64(vld1q_u64(lanes));
}

static inline __m128i _mm_shuffle_epi8(__m128i a, __m128i b)
{
    uint8_t av[16];
    uint8_t bv[16];
    uint8_t rv[16];

    vst1q_u8(av, a);
    vst1q_u8(bv, b);

    for (int i = 0; i < 16; ++i) {
        rv[i] = (bv[i] & 0x80) ? 0 : av[bv[i] & 0x0f];
    }

    return vld1q_u8(rv);
}

#endif // CM256CC_MINIMAL_SSE2NEON_H

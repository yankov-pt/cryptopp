// simon_simd.cpp - written and placed in the public domain by Jeffrey Walton
//
//    This source file uses intrinsics and built-ins to gain access to
//    SSSE3, ARM NEON and ARMv8a, and Altivec instructions. A separate
//    source file is needed because additional CXXFLAGS are required to enable
//    the appropriate instructions sets in some build configurations.

#include "pch.h"
#include "config.h"

#include "simon.h"
#include "misc.h"

// Uncomment for benchmarking C++ against SSE or NEON.
// Do so in both simon.cpp and simon_simd.cpp.
// #undef CRYPTOPP_SSSE3_AVAILABLE
// #undef CRYPTOPP_ARM_NEON_AVAILABLE

#if (CRYPTOPP_SSSE3_AVAILABLE)
# include "adv_simd.h"
# include <pmmintrin.h>
# include <tmmintrin.h>
#endif

#if defined(__XOP__)
# if defined(CRYPTOPP_GCC_COMPATIBLE)
#  include <x86intrin.h>
# endif
# include <ammintrin.h>
#endif  // XOP

#if (CRYPTOPP_ARM_NEON_HEADER)
# include "adv_simd.h"
# include <arm_neon.h>
#endif

#if (CRYPTOPP_ARM_ACLE_HEADER)
# include <stdint.h>
# include <arm_acle.h>
#endif

#if defined(_M_ARM64) || defined(_M_ARM64EC)
# include "adv_simd.h"
#endif

#if (CRYPTOPP_ALTIVEC_AVAILABLE)
# include "adv_simd.h"
# include "ppc_simd.h"
#endif

// Squash MS LNK4221 and libtool warnings
extern const char SIMON128_SIMD_FNAME[] = __FILE__;

ANONYMOUS_NAMESPACE_BEGIN

using CryptoPP::byte;
using CryptoPP::word32;
using CryptoPP::word64;
using CryptoPP::vec_swap;  // SunCC

// *************************** ARM NEON ************************** //

#if (CRYPTOPP_ARM_NEON_AVAILABLE)

// Missing from Microsoft's ARM A-32 implementation
#if defined(CRYPTOPP_MSC_VERSION) && !defined(_M_ARM64) && !defined(_M_ARM64EC)
inline uint64x2_t vld1q_dup_u64(const uint64_t* ptr)
{
    return vmovq_n_u64(*ptr);
}
#endif

template <class T>
inline T UnpackHigh64(const T& a, const T& b)
{
    const uint64x1_t x(vget_high_u64((uint64x2_t)a));
    const uint64x1_t y(vget_high_u64((uint64x2_t)b));
    return (T)vcombine_u64(x, y);
}

template <class T>
inline T UnpackLow64(const T& a, const T& b)
{
    const uint64x1_t x(vget_low_u64((uint64x2_t)a));
    const uint64x1_t y(vget_low_u64((uint64x2_t)b));
    return (T)vcombine_u64(x, y);
}

template <unsigned int R>
inline uint64x2_t RotateLeft64(const uint64x2_t& val)
{
    const uint64x2_t a(vshlq_n_u64(val, R));
    const uint64x2_t b(vshrq_n_u64(val, 64 - R));
    return vorrq_u64(a, b);
}

template <unsigned int R>
inline uint64x2_t RotateRight64(const uint64x2_t& val)
{
    const uint64x2_t a(vshlq_n_u64(val, 64 - R));
    const uint64x2_t b(vshrq_n_u64(val, R));
    return vorrq_u64(a, b);
}

#if defined(__aarch32__) || defined(__aarch64__)
// Faster than two Shifts and an Or. Thanks to Louis Wingers and Bryan Weeks.
template <>
inline uint64x2_t RotateLeft64<8>(const uint64x2_t& val)
{
    const uint8_t maskb[16] = { 7,0,1,2, 3,4,5,6, 15,8,9,10, 11,12,13,14 };
    const uint8x16_t mask = vld1q_u8(maskb);

    return vreinterpretq_u64_u8(
        vqtbl1q_u8(vreinterpretq_u8_u64(val), mask));
}

// Faster than two Shifts and an Or. Thanks to Louis Wingers and Bryan Weeks.
template <>
inline uint64x2_t RotateRight64<8>(const uint64x2_t& val)
{
    const uint8_t maskb[16] = { 1,2,3,4, 5,6,7,0, 9,10,11,12, 13,14,15,8 };
    const uint8x16_t mask = vld1q_u8(maskb);

    return vreinterpretq_u64_u8(
        vqtbl1q_u8(vreinterpretq_u8_u64(val), mask));
}
#endif

inline uint64x2_t SIMON128_f(const uint64x2_t& val)
{
    return veorq_u64(RotateLeft64<2>(val),
        vandq_u64(RotateLeft64<1>(val), RotateLeft64<8>(val)));
}

inline void SIMON128_Enc_Block(uint64x2_t &block0, uint64x2_t &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    uint64x2_t x1 = UnpackHigh64(block0, block1);
    uint64x2_t y1 = UnpackLow64(block0, block1);

    for (size_t i = 0; i < static_cast<size_t>(rounds & ~1)-1; i += 2)
    {
        const uint64x2_t rk1 = vld1q_dup_u64(subkeys+i);
        y1 = veorq_u64(veorq_u64(y1, SIMON128_f(x1)), rk1);

        const uint64x2_t rk2 = vld1q_dup_u64(subkeys+i+1);
        x1 = veorq_u64(veorq_u64(x1, SIMON128_f(y1)), rk2);
    }

    if (rounds & 1)
    {
        const uint64x2_t rk = vld1q_dup_u64(subkeys+rounds-1);

        y1 = veorq_u64(veorq_u64(y1, SIMON128_f(x1)), rk);
        std::swap(x1, y1);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = UnpackLow64(y1, x1);
    block1 = UnpackHigh64(y1, x1);
}

inline void SIMON128_Enc_6_Blocks(uint64x2_t &block0, uint64x2_t &block1,
    uint64x2_t &block2, uint64x2_t &block3, uint64x2_t &block4, uint64x2_t &block5,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    uint64x2_t x1 = UnpackHigh64(block0, block1);
    uint64x2_t y1 = UnpackLow64(block0, block1);
    uint64x2_t x2 = UnpackHigh64(block2, block3);
    uint64x2_t y2 = UnpackLow64(block2, block3);
    uint64x2_t x3 = UnpackHigh64(block4, block5);
    uint64x2_t y3 = UnpackLow64(block4, block5);

    for (size_t i = 0; i < static_cast<size_t>(rounds & ~1) - 1; i += 2)
    {
        const uint64x2_t rk1 = vld1q_dup_u64(subkeys+i);
        y1 = veorq_u64(veorq_u64(y1, SIMON128_f(x1)), rk1);
        y2 = veorq_u64(veorq_u64(y2, SIMON128_f(x2)), rk1);
        y3 = veorq_u64(veorq_u64(y3, SIMON128_f(x3)), rk1);

        const uint64x2_t rk2 = vld1q_dup_u64(subkeys+i+1);
        x1 = veorq_u64(veorq_u64(x1, SIMON128_f(y1)), rk2);
        x2 = veorq_u64(veorq_u64(x2, SIMON128_f(y2)), rk2);
        x3 = veorq_u64(veorq_u64(x3, SIMON128_f(y3)), rk2);
    }

    if (rounds & 1)
    {
        const uint64x2_t rk = vld1q_dup_u64(subkeys + rounds - 1);

        y1 = veorq_u64(veorq_u64(y1, SIMON128_f(x1)), rk);
        y2 = veorq_u64(veorq_u64(y2, SIMON128_f(x2)), rk);
        y3 = veorq_u64(veorq_u64(y3, SIMON128_f(x3)), rk);
        std::swap(x1, y1); std::swap(x2, y2); std::swap(x3, y3);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = UnpackLow64(y1, x1);
    block1 = UnpackHigh64(y1, x1);
    block2 = UnpackLow64(y2, x2);
    block3 = UnpackHigh64(y2, x2);
    block4 = UnpackLow64(y3, x3);
    block5 = UnpackHigh64(y3, x3);
}

inline void SIMON128_Dec_Block(uint64x2_t &block0, uint64x2_t &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    uint64x2_t x1 = UnpackHigh64(block0, block1);
    uint64x2_t y1 = UnpackLow64(block0, block1);

    if (rounds & 1)
    {
        std::swap(x1, y1);
        const uint64x2_t rk = vld1q_dup_u64(subkeys + rounds - 1);

        y1 = veorq_u64(veorq_u64(y1, rk), SIMON128_f(x1));
        rounds--;
    }

    for (int i = static_cast<int>(rounds-2); i >= 0; i -= 2)
    {
        const uint64x2_t rk1 = vld1q_dup_u64(subkeys+i+1);
        x1 = veorq_u64(veorq_u64(x1, SIMON128_f(y1)), rk1);

        const uint64x2_t rk2 = vld1q_dup_u64(subkeys+i);
        y1 = veorq_u64(veorq_u64(y1, SIMON128_f(x1)), rk2);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = UnpackLow64(y1, x1);
    block1 = UnpackHigh64(y1, x1);
}

inline void SIMON128_Dec_6_Blocks(uint64x2_t &block0, uint64x2_t &block1,
    uint64x2_t &block2, uint64x2_t &block3, uint64x2_t &block4, uint64x2_t &block5,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    uint64x2_t x1 = UnpackHigh64(block0, block1);
    uint64x2_t y1 = UnpackLow64(block0, block1);
    uint64x2_t x2 = UnpackHigh64(block2, block3);
    uint64x2_t y2 = UnpackLow64(block2, block3);
    uint64x2_t x3 = UnpackHigh64(block4, block5);
    uint64x2_t y3 = UnpackLow64(block4, block5);

    if (rounds & 1)
    {
        std::swap(x1, y1); std::swap(x2, y2); std::swap(x3, y3);
        const uint64x2_t rk = vld1q_dup_u64(subkeys + rounds - 1);

        y1 = veorq_u64(veorq_u64(y1, rk), SIMON128_f(x1));
        y2 = veorq_u64(veorq_u64(y2, rk), SIMON128_f(x2));
        y3 = veorq_u64(veorq_u64(y3, rk), SIMON128_f(x3));
        rounds--;
    }

    for (int i = static_cast<int>(rounds-2); i >= 0; i -= 2)
    {
        const uint64x2_t rk1 = vld1q_dup_u64(subkeys + i + 1);
        x1 = veorq_u64(veorq_u64(x1, SIMON128_f(y1)), rk1);
        x2 = veorq_u64(veorq_u64(x2, SIMON128_f(y2)), rk1);
        x3 = veorq_u64(veorq_u64(x3, SIMON128_f(y3)), rk1);

        const uint64x2_t rk2 = vld1q_dup_u64(subkeys + i);
        y1 = veorq_u64(veorq_u64(y1, SIMON128_f(x1)), rk2);
        y2 = veorq_u64(veorq_u64(y2, SIMON128_f(x2)), rk2);
        y3 = veorq_u64(veorq_u64(y3, SIMON128_f(x3)), rk2);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = UnpackLow64(y1, x1);
    block1 = UnpackHigh64(y1, x1);
    block2 = UnpackLow64(y2, x2);
    block3 = UnpackHigh64(y2, x2);
    block4 = UnpackLow64(y3, x3);
    block5 = UnpackHigh64(y3, x3);
}

#endif  // CRYPTOPP_ARM_NEON_AVAILABLE

// ***************************** IA-32 ***************************** //

#if (CRYPTOPP_SSSE3_AVAILABLE)

// GCC double casts, https://www.spinics.net/lists/gcchelp/msg47735.html
#ifndef DOUBLE_CAST
# define DOUBLE_CAST(x) ((double *)(void *)(x))
#endif
#ifndef CONST_DOUBLE_CAST
# define CONST_DOUBLE_CAST(x) ((const double *)(const void *)(x))
#endif

inline void Swap128(__m128i& a,__m128i& b)
{
#if defined(__SUNPRO_CC) && (__SUNPRO_CC <= 0x5120)
    // __m128i is an unsigned long long[2], and support for swapping it was not added until C++11.
    // SunCC 12.1 - 12.3 fail to consume the swap; while SunCC 12.4 consumes it without -std=c++11.
    vec_swap(a, b);
#else
    std::swap(a, b);
#endif
}

template <unsigned int R>
inline __m128i RotateLeft64(const __m128i& val)
{
#if defined(__XOP__)
    return _mm_roti_epi64(val, R);
#else
    return _mm_or_si128(
        _mm_slli_epi64(val, R), _mm_srli_epi64(val, 64-R));
#endif
}

template <unsigned int R>
inline __m128i RotateRight64(const __m128i& val)
{
#if defined(__XOP__)
    return _mm_roti_epi64(val, 64-R);
#else
    return _mm_or_si128(
        _mm_slli_epi64(val, 64-R), _mm_srli_epi64(val, R));
#endif
}

// Faster than two Shifts and an Or. Thanks to Louis Wingers and Bryan Weeks.
template <>
__m128i RotateLeft64<8>(const __m128i& val)
{
#if defined(__XOP__)
    return _mm_roti_epi64(val, 8);
#else
    const __m128i mask = _mm_set_epi8(14,13,12,11, 10,9,8,15, 6,5,4,3, 2,1,0,7);
    return _mm_shuffle_epi8(val, mask);
#endif
}

// Faster than two Shifts and an Or. Thanks to Louis Wingers and Bryan Weeks.
template <>
__m128i RotateRight64<8>(const __m128i& val)
{
#if defined(__XOP__)
    return _mm_roti_epi64(val, 64-8);
#else
    const __m128i mask = _mm_set_epi8(8,15,14,13, 12,11,10,9, 0,7,6,5, 4,3,2,1);
    return _mm_shuffle_epi8(val, mask);
#endif
}

inline __m128i SIMON128_f(const __m128i& v)
{
    return _mm_xor_si128(RotateLeft64<2>(v),
        _mm_and_si128(RotateLeft64<1>(v), RotateLeft64<8>(v)));
}

inline void SIMON128_Enc_Block(__m128i &block0, __m128i &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    __m128i x1 = _mm_unpackhi_epi64(block0, block1);
    __m128i y1 = _mm_unpacklo_epi64(block0, block1);

    for (size_t i = 0; i < static_cast<size_t>(rounds & ~1)-1; i += 2)
    {
        // Round keys are pre-splated in forward direction
        const __m128i rk1 = _mm_load_si128(CONST_M128_CAST(subkeys+i*2));
        y1 = _mm_xor_si128(_mm_xor_si128(y1, SIMON128_f(x1)), rk1);

        const __m128i rk2 = _mm_load_si128(CONST_M128_CAST(subkeys+(i+1)*2));
        x1 = _mm_xor_si128(_mm_xor_si128(x1, SIMON128_f(y1)), rk2);
    }

    if (rounds & 1)
    {
        // Round keys are pre-splated in forward direction
        const __m128i rk = _mm_load_si128(CONST_M128_CAST(subkeys+(rounds-1)*2));

        y1 = _mm_xor_si128(_mm_xor_si128(y1, SIMON128_f(x1)), rk);
        Swap128(x1, y1);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = _mm_unpacklo_epi64(y1, x1);
    block1 = _mm_unpackhi_epi64(y1, x1);
}

inline void SIMON128_Enc_6_Blocks(__m128i &block0, __m128i &block1,
    __m128i &block2, __m128i &block3, __m128i &block4, __m128i &block5,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    __m128i x1 = _mm_unpackhi_epi64(block0, block1);
    __m128i y1 = _mm_unpacklo_epi64(block0, block1);
    __m128i x2 = _mm_unpackhi_epi64(block2, block3);
    __m128i y2 = _mm_unpacklo_epi64(block2, block3);
    __m128i x3 = _mm_unpackhi_epi64(block4, block5);
    __m128i y3 = _mm_unpacklo_epi64(block4, block5);

    for (size_t i = 0; i < static_cast<size_t>(rounds & ~1) - 1; i += 2)
    {
        // Round keys are pre-splated in forward direction
        const __m128i rk1 = _mm_load_si128(CONST_M128_CAST(subkeys+i*2));
        y1 = _mm_xor_si128(_mm_xor_si128(y1, SIMON128_f(x1)), rk1);
        y2 = _mm_xor_si128(_mm_xor_si128(y2, SIMON128_f(x2)), rk1);
        y3 = _mm_xor_si128(_mm_xor_si128(y3, SIMON128_f(x3)), rk1);

        // Round keys are pre-splated in forward direction
        const __m128i rk2 = _mm_load_si128(CONST_M128_CAST(subkeys+(i+1)*2));
        x1 = _mm_xor_si128(_mm_xor_si128(x1, SIMON128_f(y1)), rk2);
        x2 = _mm_xor_si128(_mm_xor_si128(x2, SIMON128_f(y2)), rk2);
        x3 = _mm_xor_si128(_mm_xor_si128(x3, SIMON128_f(y3)), rk2);
    }

    if (rounds & 1)
    {
        // Round keys are pre-splated in forward direction
        const __m128i rk = _mm_load_si128(CONST_M128_CAST(subkeys+(rounds-1)*2));
        y1 = _mm_xor_si128(_mm_xor_si128(y1, SIMON128_f(x1)), rk);
        y2 = _mm_xor_si128(_mm_xor_si128(y2, SIMON128_f(x2)), rk);
        y3 = _mm_xor_si128(_mm_xor_si128(y3, SIMON128_f(x3)), rk);
        Swap128(x1, y1); Swap128(x2, y2); Swap128(x3, y3);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = _mm_unpacklo_epi64(y1, x1);
    block1 = _mm_unpackhi_epi64(y1, x1);
    block2 = _mm_unpacklo_epi64(y2, x2);
    block3 = _mm_unpackhi_epi64(y2, x2);
    block4 = _mm_unpacklo_epi64(y3, x3);
    block5 = _mm_unpackhi_epi64(y3, x3);
}

inline void SIMON128_Dec_Block(__m128i &block0, __m128i &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    __m128i x1 = _mm_unpackhi_epi64(block0, block1);
    __m128i y1 = _mm_unpacklo_epi64(block0, block1);

    if (rounds & 1)
    {
        const __m128i rk = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys + rounds - 1)));

        Swap128(x1, y1);
        y1 = _mm_xor_si128(_mm_xor_si128(y1, rk), SIMON128_f(x1));
        rounds--;
    }

    for (int i = static_cast<int>(rounds-2); i >= 0; i -= 2)
    {
        const __m128i rk1 = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys+i+1)));
        x1 = _mm_xor_si128(_mm_xor_si128(x1, SIMON128_f(y1)), rk1);

        const __m128i rk2 = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys+i)));
        y1 = _mm_xor_si128(_mm_xor_si128(y1, SIMON128_f(x1)), rk2);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = _mm_unpacklo_epi64(y1, x1);
    block1 = _mm_unpackhi_epi64(y1, x1);
}

inline void SIMON128_Dec_6_Blocks(__m128i &block0, __m128i &block1,
    __m128i &block2, __m128i &block3, __m128i &block4, __m128i &block5,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    __m128i x1 = _mm_unpackhi_epi64(block0, block1);
    __m128i y1 = _mm_unpacklo_epi64(block0, block1);
    __m128i x2 = _mm_unpackhi_epi64(block2, block3);
    __m128i y2 = _mm_unpacklo_epi64(block2, block3);
    __m128i x3 = _mm_unpackhi_epi64(block4, block5);
    __m128i y3 = _mm_unpacklo_epi64(block4, block5);

    if (rounds & 1)
    {
        const __m128i rk = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys + rounds - 1)));

        Swap128(x1, y1); Swap128(x2, y2); Swap128(x3, y3);
        y1 = _mm_xor_si128(_mm_xor_si128(y1, rk), SIMON128_f(x1));
        y2 = _mm_xor_si128(_mm_xor_si128(y2, rk), SIMON128_f(x2));
        y3 = _mm_xor_si128(_mm_xor_si128(y3, rk), SIMON128_f(x3));
        rounds--;
    }

    for (int i = static_cast<int>(rounds-2); i >= 0; i -= 2)
    {
        const __m128i rk1 = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys + i + 1)));
        x1 = _mm_xor_si128(_mm_xor_si128(x1, SIMON128_f(y1)), rk1);
        x2 = _mm_xor_si128(_mm_xor_si128(x2, SIMON128_f(y2)), rk1);
        x3 = _mm_xor_si128(_mm_xor_si128(x3, SIMON128_f(y3)), rk1);

        const __m128i rk2 = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys + i)));
        y1 = _mm_xor_si128(_mm_xor_si128(y1, SIMON128_f(x1)), rk2);
        y2 = _mm_xor_si128(_mm_xor_si128(y2, SIMON128_f(x2)), rk2);
        y3 = _mm_xor_si128(_mm_xor_si128(y3, SIMON128_f(x3)), rk2);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = _mm_unpacklo_epi64(y1, x1);
    block1 = _mm_unpackhi_epi64(y1, x1);
    block2 = _mm_unpacklo_epi64(y2, x2);
    block3 = _mm_unpackhi_epi64(y2, x2);
    block4 = _mm_unpacklo_epi64(y3, x3);
    block5 = _mm_unpackhi_epi64(y3, x3);
}

#endif  // CRYPTOPP_SSSE3_AVAILABLE

// ***************************** Altivec ***************************** //

#if (CRYPTOPP_ALTIVEC_AVAILABLE)

// Altivec uses native 64-bit types on 64-bit environments, or 32-bit types
// in 32-bit environments. Speck128 will use the appropriate type for the
// environment. Functions like VecAdd64 have two overloads, one for each
// environment. The 32-bit overload treats uint32x4_p like a 64-bit type,
// and does things like perform a add with carry or subtract with borrow.

// Speck128 on Power8 performed as expected because of 64-bit environment.
// Performance sucked on old PowerPC machines because of 32-bit environments.
// At Crypto++ 8.3 we added an implementation that operated on 32-bit words.
// Native 64-bit Speck128 performance dropped from about 4.1 to 6.3 cpb, but
// 32-bit Speck128 improved from 66.5 cpb to 10.4 cpb. Overall it was a
// good win even though we lost some performance in 64-bit environments.

using CryptoPP::uint8x16_p;
using CryptoPP::uint32x4_p;
#if defined(_ARCH_PWR8)
using CryptoPP::uint64x2_p;
#endif

using CryptoPP::VecAdd64;
using CryptoPP::VecSub64;
using CryptoPP::VecAnd64;
using CryptoPP::VecOr64;
using CryptoPP::VecXor64;
using CryptoPP::VecRotateLeft64;
using CryptoPP::VecRotateRight64;
using CryptoPP::VecSplatElement64;
using CryptoPP::VecLoad;
using CryptoPP::VecLoadAligned;
using CryptoPP::VecPermute;

#if defined(_ARCH_PWR8)
#define simon128_t uint64x2_p
#else
#define simon128_t uint32x4_p
#endif

inline simon128_t SIMON128_f(const simon128_t val)
{
    return (simon128_t)VecXor64(VecRotateLeft64<2>(val),
        VecAnd64(VecRotateLeft64<1>(val), VecRotateLeft64<8>(val)));
}

inline void SIMON128_Enc_Block(uint32x4_p &block, const word64 *subkeys, unsigned int rounds)
{
#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m1 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m2 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m1 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m2 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    simon128_t x1 = (simon128_t)VecPermute(block, block, m1);
    simon128_t y1 = (simon128_t)VecPermute(block, block, m2);

    for (size_t i = 0; i < static_cast<size_t>(rounds & ~1)-1; i += 2)
    {
        // Round keys are pre-splated in forward direction
        const word32* ptr1 = reinterpret_cast<const word32*>(subkeys+i*2);
        const simon128_t rk1 = (simon128_t)VecLoadAligned(ptr1);
        const word32* ptr2 = reinterpret_cast<const word32*>(subkeys+(i+1)*2);
        const simon128_t rk2 = (simon128_t)VecLoadAligned(ptr2);

        y1 = VecXor64(VecXor64(y1, SIMON128_f(x1)), rk1);
        x1 = VecXor64(VecXor64(x1, SIMON128_f(y1)), rk2);
    }

    if (rounds & 1)
    {
        // Round keys are pre-splated in forward direction
        const word32* ptr = reinterpret_cast<const word32*>(subkeys+(rounds-1)*2);
        const simon128_t rk = (simon128_t)VecLoadAligned(ptr);

        y1 = VecXor64(VecXor64(y1, SIMON128_f(x1)), rk);

        std::swap(x1, y1);
    }

#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m3 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    //const uint8x16_p m4 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m3 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    //const uint8x16_p m4 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block = (uint32x4_p)VecPermute(x1, y1, m3);
}

inline void SIMON128_Dec_Block(uint32x4_p &block, const word64 *subkeys, unsigned int rounds)
{
#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m1 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m2 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m1 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m2 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    simon128_t x1 = (simon128_t)VecPermute(block, block, m1);
    simon128_t y1 = (simon128_t)VecPermute(block, block, m2);

    if (rounds & 1)
    {
        std::swap(x1, y1);

        const word32* ptr = reinterpret_cast<const word32*>(subkeys+rounds-1);
        const simon128_t tk = (simon128_t)VecLoad(ptr);
        const simon128_t rk = (simon128_t)VecSplatElement64<0>(tk);

        y1 = VecXor64(VecXor64(y1, rk), SIMON128_f(x1));
        rounds--;
    }

    for (int i = static_cast<int>(rounds-2); i >= 0; i -= 2)
    {
        const word32* ptr = reinterpret_cast<const word32*>(subkeys+i);
        const simon128_t tk = (simon128_t)VecLoad(ptr);
        const simon128_t rk1 = (simon128_t)VecSplatElement64<1>(tk);
        const simon128_t rk2 = (simon128_t)VecSplatElement64<0>(tk);

        x1 = VecXor64(VecXor64(x1, SIMON128_f(y1)), rk1);
        y1 = VecXor64(VecXor64(y1, SIMON128_f(x1)), rk2);
    }

#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m3 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    //const uint8x16_p m4 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m3 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    //const uint8x16_p m4 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block = (uint32x4_p)VecPermute(x1, y1, m3);
}

inline void SIMON128_Enc_6_Blocks(uint32x4_p &block0, uint32x4_p &block1,
            uint32x4_p &block2, uint32x4_p &block3, uint32x4_p &block4,
            uint32x4_p &block5, const word64 *subkeys, unsigned int rounds)
{
#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m1 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m2 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m1 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m2 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    simon128_t x1 = (simon128_t)VecPermute(block0, block1, m1);
    simon128_t y1 = (simon128_t)VecPermute(block0, block1, m2);
    simon128_t x2 = (simon128_t)VecPermute(block2, block3, m1);
    simon128_t y2 = (simon128_t)VecPermute(block2, block3, m2);
    simon128_t x3 = (simon128_t)VecPermute(block4, block5, m1);
    simon128_t y3 = (simon128_t)VecPermute(block4, block5, m2);

    for (size_t i = 0; i < static_cast<size_t>(rounds & ~1)-1; i += 2)
    {
        // Round keys are pre-splated in forward direction
        const word32* ptr1 = reinterpret_cast<const word32*>(subkeys+i*2);
        const simon128_t rk1 = (simon128_t)VecLoadAligned(ptr1);

        const word32* ptr2 = reinterpret_cast<const word32*>(subkeys+(i+1)*2);
        const simon128_t rk2 = (simon128_t)VecLoadAligned(ptr2);

        y1 = VecXor64(VecXor64(y1, SIMON128_f(x1)), rk1);
        y2 = VecXor64(VecXor64(y2, SIMON128_f(x2)), rk1);
        y3 = VecXor64(VecXor64(y3, SIMON128_f(x3)), rk1);

        x1 = VecXor64(VecXor64(x1, SIMON128_f(y1)), rk2);
        x2 = VecXor64(VecXor64(x2, SIMON128_f(y2)), rk2);
        x3 = VecXor64(VecXor64(x3, SIMON128_f(y3)), rk2);
    }

    if (rounds & 1)
    {
        // Round keys are pre-splated in forward direction
        const word32* ptr = reinterpret_cast<const word32*>(subkeys+(rounds-1)*2);
        const simon128_t rk = (simon128_t)VecLoadAligned(ptr);

        y1 = VecXor64(VecXor64(y1, SIMON128_f(x1)), rk);
        y2 = VecXor64(VecXor64(y2, SIMON128_f(x2)), rk);
        y3 = VecXor64(VecXor64(y3, SIMON128_f(x3)), rk);

        std::swap(x1, y1); std::swap(x2, y2); std::swap(x3, y3);
    }

#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m3 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m4 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m3 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m4 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = (uint32x4_p)VecPermute(x1, y1, m3);
    block1 = (uint32x4_p)VecPermute(x1, y1, m4);
    block2 = (uint32x4_p)VecPermute(x2, y2, m3);
    block3 = (uint32x4_p)VecPermute(x2, y2, m4);
    block4 = (uint32x4_p)VecPermute(x3, y3, m3);
    block5 = (uint32x4_p)VecPermute(x3, y3, m4);
}

inline void SIMON128_Dec_6_Blocks(uint32x4_p &block0, uint32x4_p &block1,
            uint32x4_p &block2, uint32x4_p &block3, uint32x4_p &block4,
            uint32x4_p &block5, const word64 *subkeys, unsigned int rounds)
{
#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m1 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m2 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m1 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m2 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    simon128_t x1 = (simon128_t)VecPermute(block0, block1, m1);
    simon128_t y1 = (simon128_t)VecPermute(block0, block1, m2);
    simon128_t x2 = (simon128_t)VecPermute(block2, block3, m1);
    simon128_t y2 = (simon128_t)VecPermute(block2, block3, m2);
    simon128_t x3 = (simon128_t)VecPermute(block4, block5, m1);
    simon128_t y3 = (simon128_t)VecPermute(block4, block5, m2);

    if (rounds & 1)
    {
        std::swap(x1, y1); std::swap(x2, y2); std::swap(x3, y3);

        const word32* ptr = reinterpret_cast<const word32*>(subkeys+rounds-1);
        const simon128_t tk = (simon128_t)VecLoad(ptr);
        const simon128_t rk = (simon128_t)VecSplatElement64<0>(tk);

        y1 = VecXor64(VecXor64(y1, rk), SIMON128_f(x1));
        y2 = VecXor64(VecXor64(y2, rk), SIMON128_f(x2));
        y3 = VecXor64(VecXor64(y3, rk), SIMON128_f(x3));
        rounds--;
    }

    for (int i = static_cast<int>(rounds-2); i >= 0; i -= 2)
    {
        const word32* ptr = reinterpret_cast<const word32*>(subkeys+i);
        const simon128_t tk = (simon128_t)VecLoad(ptr);
        const simon128_t rk1 = (simon128_t)VecSplatElement64<1>(tk);
        const simon128_t rk2 = (simon128_t)VecSplatElement64<0>(tk);

        x1 = VecXor64(VecXor64(x1, SIMON128_f(y1)), rk1);
        x2 = VecXor64(VecXor64(x2, SIMON128_f(y2)), rk1);
        x3 = VecXor64(VecXor64(x3, SIMON128_f(y3)), rk1);

        y1 = VecXor64(VecXor64(y1, SIMON128_f(x1)), rk2);
        y2 = VecXor64(VecXor64(y2, SIMON128_f(x2)), rk2);
        y3 = VecXor64(VecXor64(y3, SIMON128_f(x3)), rk2);
    }

#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m3 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m4 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m3 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m4 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = (uint32x4_p)VecPermute(x1, y1, m3);
    block1 = (uint32x4_p)VecPermute(x1, y1, m4);
    block2 = (uint32x4_p)VecPermute(x2, y2, m3);
    block3 = (uint32x4_p)VecPermute(x2, y2, m4);
    block4 = (uint32x4_p)VecPermute(x3, y3, m3);
    block5 = (uint32x4_p)VecPermute(x3, y3, m4);
}

#endif  // CRYPTOPP_ALTIVEC_AVAILABLE

ANONYMOUS_NAMESPACE_END

///////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(CryptoPP)

// *************************** ARM NEON **************************** //

#if (CRYPTOPP_ARM_NEON_AVAILABLE)
size_t SIMON128_Enc_AdvancedProcessBlocks_NEON(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_NEON(SIMON128_Enc_Block, SIMON128_Enc_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}

size_t SIMON128_Dec_AdvancedProcessBlocks_NEON(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_NEON(SIMON128_Dec_Block, SIMON128_Dec_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}
#endif  // CRYPTOPP_ARM_NEON_AVAILABLE

// ***************************** IA-32 ***************************** //

#if (CRYPTOPP_SSSE3_AVAILABLE)
size_t SIMON128_Enc_AdvancedProcessBlocks_SSSE3(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_SSE(SIMON128_Enc_Block, SIMON128_Enc_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}

size_t SIMON128_Dec_AdvancedProcessBlocks_SSSE3(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_SSE(SIMON128_Dec_Block, SIMON128_Dec_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}
#endif  // CRYPTOPP_SSSE3_AVAILABLE

// ***************************** Altivec ***************************** //

#if (CRYPTOPP_ALTIVEC_AVAILABLE)
size_t SIMON128_Enc_AdvancedProcessBlocks_ALTIVEC(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x1_ALTIVEC(SIMON128_Enc_Block, SIMON128_Enc_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}

size_t SIMON128_Dec_AdvancedProcessBlocks_ALTIVEC(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x1_ALTIVEC(SIMON128_Dec_Block, SIMON128_Dec_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}
#endif  // CRYPTOPP_ALTIVEC_AVAILABLE

NAMESPACE_END

// speck128_simd.cpp - written and placed in the public domain by Jeffrey Walton
//
//    This source file uses intrinsics and built-ins to gain access to
//    SSSE3, ARM NEON and ARMv8a, and Altivec instructions. A separate
//    source file is needed because additional CXXFLAGS are required to enable
//    the appropriate instructions sets in some build configurations.

#include "pch.h"
#include "config.h"

#include "speck.h"
#include "misc.h"

// Uncomment for benchmarking C++ against SSE or NEON.
// Do so in both speck.cpp and speck_simd.cpp.
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

#if defined(CRYPTOPP_ALTIVEC_AVAILABLE)
# include "adv_simd.h"
# include "ppc_simd.h"
#endif

// Squash MS LNK4221 and libtool warnings
extern const char SPECK128_SIMD_FNAME[] = __FILE__;

ANONYMOUS_NAMESPACE_BEGIN

using CryptoPP::byte;
using CryptoPP::word32;
using CryptoPP::word64;

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

inline void SPECK128_Enc_Block(uint64x2_t &block0, uint64x2_t &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    uint64x2_t x1 = UnpackHigh64(block0, block1);
    uint64x2_t y1 = UnpackLow64(block0, block1);

    for (size_t i=0; i < static_cast<size_t>(rounds); ++i)
    {
        const uint64x2_t rk = vld1q_dup_u64(subkeys+i);

        x1 = RotateRight64<8>(x1);
        x1 = vaddq_u64(x1, y1);
        x1 = veorq_u64(x1, rk);
        y1 = RotateLeft64<3>(y1);
        y1 = veorq_u64(y1, x1);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = UnpackLow64(y1, x1);
    block1 = UnpackHigh64(y1, x1);
}

inline void SPECK128_Enc_6_Blocks(uint64x2_t &block0, uint64x2_t &block1,
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

    for (size_t i=0; i < static_cast<size_t>(rounds); ++i)
    {
        const uint64x2_t rk = vld1q_dup_u64(subkeys+i);

        x1 = RotateRight64<8>(x1);
        x2 = RotateRight64<8>(x2);
        x3 = RotateRight64<8>(x3);
        x1 = vaddq_u64(x1, y1);
        x2 = vaddq_u64(x2, y2);
        x3 = vaddq_u64(x3, y3);
        x1 = veorq_u64(x1, rk);
        x2 = veorq_u64(x2, rk);
        x3 = veorq_u64(x3, rk);
        y1 = RotateLeft64<3>(y1);
        y2 = RotateLeft64<3>(y2);
        y3 = RotateLeft64<3>(y3);
        y1 = veorq_u64(y1, x1);
        y2 = veorq_u64(y2, x2);
        y3 = veorq_u64(y3, x3);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = UnpackLow64(y1, x1);
    block1 = UnpackHigh64(y1, x1);
    block2 = UnpackLow64(y2, x2);
    block3 = UnpackHigh64(y2, x2);
    block4 = UnpackLow64(y3, x3);
    block5 = UnpackHigh64(y3, x3);
}

inline void SPECK128_Dec_Block(uint64x2_t &block0, uint64x2_t &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    uint64x2_t x1 = UnpackHigh64(block0, block1);
    uint64x2_t y1 = UnpackLow64(block0, block1);

    for (int i = static_cast<int>(rounds-1); i >= 0; --i)
    {
        const uint64x2_t rk = vld1q_dup_u64(subkeys+i);

        y1 = veorq_u64(y1, x1);
        y1 = RotateRight64<3>(y1);
        x1 = veorq_u64(x1, rk);
        x1 = vsubq_u64(x1, y1);
        x1 = RotateLeft64<8>(x1);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = UnpackLow64(y1, x1);
    block1 = UnpackHigh64(y1, x1);
}

inline void SPECK128_Dec_6_Blocks(uint64x2_t &block0, uint64x2_t &block1,
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

    for (int i = static_cast<int>(rounds-1); i >= 0; --i)
    {
        const uint64x2_t rk = vld1q_dup_u64(subkeys+i);

        y1 = veorq_u64(y1, x1);
        y2 = veorq_u64(y2, x2);
        y3 = veorq_u64(y3, x3);
        y1 = RotateRight64<3>(y1);
        y2 = RotateRight64<3>(y2);
        y3 = RotateRight64<3>(y3);
        x1 = veorq_u64(x1, rk);
        x2 = veorq_u64(x2, rk);
        x3 = veorq_u64(x3, rk);
        x1 = vsubq_u64(x1, y1);
        x2 = vsubq_u64(x2, y2);
        x3 = vsubq_u64(x3, y3);
        x1 = RotateLeft64<8>(x1);
        x2 = RotateLeft64<8>(x2);
        x3 = RotateLeft64<8>(x3);
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

#if defined(CRYPTOPP_SSSE3_AVAILABLE)

// GCC double casts, https://www.spinics.net/lists/gcchelp/msg47735.html
#ifndef DOUBLE_CAST
# define DOUBLE_CAST(x) ((double *)(void *)(x))
#endif
#ifndef CONST_DOUBLE_CAST
# define CONST_DOUBLE_CAST(x) ((const double *)(const void *)(x))
#endif

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

inline void SPECK128_Enc_Block(__m128i &block0, __m128i &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    __m128i x1 = _mm_unpackhi_epi64(block0, block1);
    __m128i y1 = _mm_unpacklo_epi64(block0, block1);

    for (size_t i=0; i < static_cast<size_t>(rounds); ++i)
    {
        // Round keys are pre-splated in forward direction
        const __m128i rk = _mm_load_si128(CONST_M128_CAST(subkeys+i*2));

        x1 = RotateRight64<8>(x1);
        x1 = _mm_add_epi64(x1, y1);
        x1 = _mm_xor_si128(x1, rk);
        y1 = RotateLeft64<3>(y1);
        y1 = _mm_xor_si128(y1, x1);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = _mm_unpacklo_epi64(y1, x1);
    block1 = _mm_unpackhi_epi64(y1, x1);
}

inline void SPECK128_Enc_6_Blocks(__m128i &block0, __m128i &block1,
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

    for (size_t i=0; i < static_cast<size_t>(rounds); ++i)
    {
        // Round keys are pre-splated in forward direction
        const __m128i rk = _mm_load_si128(CONST_M128_CAST(subkeys+i*2));

        x1 = RotateRight64<8>(x1);
        x2 = RotateRight64<8>(x2);
        x3 = RotateRight64<8>(x3);
        x1 = _mm_add_epi64(x1, y1);
        x2 = _mm_add_epi64(x2, y2);
        x3 = _mm_add_epi64(x3, y3);
        x1 = _mm_xor_si128(x1, rk);
        x2 = _mm_xor_si128(x2, rk);
        x3 = _mm_xor_si128(x3, rk);
        y1 = RotateLeft64<3>(y1);
        y2 = RotateLeft64<3>(y2);
        y3 = RotateLeft64<3>(y3);
        y1 = _mm_xor_si128(y1, x1);
        y2 = _mm_xor_si128(y2, x2);
        y3 = _mm_xor_si128(y3, x3);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = _mm_unpacklo_epi64(y1, x1);
    block1 = _mm_unpackhi_epi64(y1, x1);
    block2 = _mm_unpacklo_epi64(y2, x2);
    block3 = _mm_unpackhi_epi64(y2, x2);
    block4 = _mm_unpacklo_epi64(y3, x3);
    block5 = _mm_unpackhi_epi64(y3, x3);
}

inline void SPECK128_Dec_Block(__m128i &block0, __m128i &block1,
    const word64 *subkeys, unsigned int rounds)
{
    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    __m128i x1 = _mm_unpackhi_epi64(block0, block1);
    __m128i y1 = _mm_unpacklo_epi64(block0, block1);

    for (int i = static_cast<int>(rounds-1); i >= 0; --i)
    {
        const __m128i rk = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys+i)));

        y1 = _mm_xor_si128(y1, x1);
        y1 = RotateRight64<3>(y1);
        x1 = _mm_xor_si128(x1, rk);
        x1 = _mm_sub_epi64(x1, y1);
        x1 = RotateLeft64<8>(x1);
    }

    // [A1 B1][A2 B2] ... => [A1 A2][B1 B2] ...
    block0 = _mm_unpacklo_epi64(y1, x1);
    block1 = _mm_unpackhi_epi64(y1, x1);
}

inline void SPECK128_Dec_6_Blocks(__m128i &block0, __m128i &block1,
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

    for (int i = static_cast<int>(rounds-1); i >= 0; --i)
    {
        const __m128i rk = _mm_castpd_si128(
            _mm_loaddup_pd(CONST_DOUBLE_CAST(subkeys+i)));

        y1 = _mm_xor_si128(y1, x1);
        y2 = _mm_xor_si128(y2, x2);
        y3 = _mm_xor_si128(y3, x3);
        y1 = RotateRight64<3>(y1);
        y2 = RotateRight64<3>(y2);
        y3 = RotateRight64<3>(y3);
        x1 = _mm_xor_si128(x1, rk);
        x2 = _mm_xor_si128(x2, rk);
        x3 = _mm_xor_si128(x3, rk);
        x1 = _mm_sub_epi64(x1, y1);
        x2 = _mm_sub_epi64(x2, y2);
        x3 = _mm_sub_epi64(x3, y3);
        x1 = RotateLeft64<8>(x1);
        x2 = RotateLeft64<8>(x2);
        x3 = RotateLeft64<8>(x3);
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

#if defined(CRYPTOPP_ALTIVEC_AVAILABLE)

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
using CryptoPP::VecSplatWord64;
using CryptoPP::VecRotateLeft64;
using CryptoPP::VecRotateRight64;
using CryptoPP::VecLoad;
using CryptoPP::VecLoadAligned;
using CryptoPP::VecPermute;

#if defined(_ARCH_PWR8)
#define speck128_t uint64x2_p
#else
#define speck128_t uint32x4_p
#endif

void SPECK128_Enc_Block(uint32x4_p &block, const word64 *subkeys, unsigned int rounds)
{
#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m1 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m2 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m1 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m2 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    speck128_t x1 = (speck128_t)VecPermute(block, block, m1);
    speck128_t y1 = (speck128_t)VecPermute(block, block, m2);

    for (size_t i=0; i < static_cast<size_t>(rounds); ++i)
    {
        // Round keys are pre-splated in forward direction
        const word32* ptr = reinterpret_cast<const word32*>(subkeys+i*2);
        const speck128_t rk = (speck128_t)VecLoadAligned(ptr);

        x1 = (speck128_t)VecRotateRight64<8>(x1);
        x1 = (speck128_t)VecAdd64(x1, y1);
        x1 = (speck128_t)VecXor64(x1, rk);

        y1 = (speck128_t)VecRotateLeft64<3>(y1);
        y1 = (speck128_t)VecXor64(y1, x1);
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

void SPECK128_Dec_Block(uint32x4_p &block, const word64 *subkeys, unsigned int rounds)
{
#if (CRYPTOPP_BIG_ENDIAN)
    const uint8x16_p m1 = {31,30,29,28,27,26,25,24, 15,14,13,12,11,10,9,8};
    const uint8x16_p m2 = {23,22,21,20,19,18,17,16, 7,6,5,4,3,2,1,0};
#else
    const uint8x16_p m1 = {7,6,5,4,3,2,1,0, 23,22,21,20,19,18,17,16};
    const uint8x16_p m2 = {15,14,13,12,11,10,9,8, 31,30,29,28,27,26,25,24};
#endif

    // [A1 A2][B1 B2] ... => [A1 B1][A2 B2] ...
    speck128_t x1 = (speck128_t)VecPermute(block, block, m1);
    speck128_t y1 = (speck128_t)VecPermute(block, block, m2);

    for (int i = static_cast<int>(rounds-1); i >= 0; --i)
    {
        const speck128_t rk = (speck128_t)VecSplatWord64(subkeys[i]);

        y1 = (speck128_t)VecXor64(y1, x1);
        y1 = (speck128_t)VecRotateRight64<3>(y1);
        x1 = (speck128_t)VecXor64(x1, rk);
        x1 = (speck128_t)VecSub64(x1, y1);
        x1 = (speck128_t)VecRotateLeft64<8>(x1);
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

void SPECK128_Enc_6_Blocks(uint32x4_p &block0, uint32x4_p &block1,
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
    speck128_t x1 = (speck128_t)VecPermute(block0, block1, m1);
    speck128_t y1 = (speck128_t)VecPermute(block0, block1, m2);
    speck128_t x2 = (speck128_t)VecPermute(block2, block3, m1);
    speck128_t y2 = (speck128_t)VecPermute(block2, block3, m2);
    speck128_t x3 = (speck128_t)VecPermute(block4, block5, m1);
    speck128_t y3 = (speck128_t)VecPermute(block4, block5, m2);

    for (size_t i=0; i < static_cast<size_t>(rounds); ++i)
    {
        // Round keys are pre-splated in forward direction
        const word32* ptr = reinterpret_cast<const word32*>(subkeys+i*2);
        const speck128_t rk = (speck128_t)VecLoadAligned(ptr);

        x1 = (speck128_t)VecRotateRight64<8>(x1);
        x2 = (speck128_t)VecRotateRight64<8>(x2);
        x3 = (speck128_t)VecRotateRight64<8>(x3);
        x1 = (speck128_t)VecAdd64(x1, y1);
        x2 = (speck128_t)VecAdd64(x2, y2);
        x3 = (speck128_t)VecAdd64(x3, y3);
        x1 = (speck128_t)VecXor64(x1, rk);
        x2 = (speck128_t)VecXor64(x2, rk);
        x3 = (speck128_t)VecXor64(x3, rk);

        y1 = (speck128_t)VecRotateLeft64<3>(y1);
        y2 = (speck128_t)VecRotateLeft64<3>(y2);
        y3 = (speck128_t)VecRotateLeft64<3>(y3);
        y1 = (speck128_t)VecXor64(y1, x1);
        y2 = (speck128_t)VecXor64(y2, x2);
        y3 = (speck128_t)VecXor64(y3, x3);
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

void SPECK128_Dec_6_Blocks(uint32x4_p &block0, uint32x4_p &block1,
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
    speck128_t x1 = (speck128_t)VecPermute(block0, block1, m1);
    speck128_t y1 = (speck128_t)VecPermute(block0, block1, m2);
    speck128_t x2 = (speck128_t)VecPermute(block2, block3, m1);
    speck128_t y2 = (speck128_t)VecPermute(block2, block3, m2);
    speck128_t x3 = (speck128_t)VecPermute(block4, block5, m1);
    speck128_t y3 = (speck128_t)VecPermute(block4, block5, m2);

    for (int i = static_cast<int>(rounds-1); i >= 0; --i)
    {
        const speck128_t rk = (speck128_t)VecSplatWord64(subkeys[i]);

        y1 = (speck128_t)VecXor64(y1, x1);
        y2 = (speck128_t)VecXor64(y2, x2);
        y3 = (speck128_t)VecXor64(y3, x3);
        y1 = (speck128_t)VecRotateRight64<3>(y1);
        y2 = (speck128_t)VecRotateRight64<3>(y2);
        y3 = (speck128_t)VecRotateRight64<3>(y3);

        x1 = (speck128_t)VecXor64(x1, rk);
        x2 = (speck128_t)VecXor64(x2, rk);
        x3 = (speck128_t)VecXor64(x3, rk);
        x1 = (speck128_t)VecSub64(x1, y1);
        x2 = (speck128_t)VecSub64(x2, y2);
        x3 = (speck128_t)VecSub64(x3, y3);
        x1 = (speck128_t)VecRotateLeft64<8>(x1);
        x2 = (speck128_t)VecRotateLeft64<8>(x2);
        x3 = (speck128_t)VecRotateLeft64<8>(x3);
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
size_t SPECK128_Enc_AdvancedProcessBlocks_NEON(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_NEON(SPECK128_Enc_Block, SPECK128_Enc_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}

size_t SPECK128_Dec_AdvancedProcessBlocks_NEON(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_NEON(SPECK128_Dec_Block, SPECK128_Dec_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}
#endif  // CRYPTOPP_ARM_NEON_AVAILABLE

// ***************************** IA-32 ***************************** //

#if (CRYPTOPP_SSSE3_AVAILABLE)
size_t SPECK128_Enc_AdvancedProcessBlocks_SSSE3(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_SSE(SPECK128_Enc_Block, SPECK128_Enc_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}

size_t SPECK128_Dec_AdvancedProcessBlocks_SSSE3(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x2_SSE(SPECK128_Dec_Block, SPECK128_Dec_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}
#endif  // CRYPTOPP_SSSE3_AVAILABLE

// ***************************** Altivec ***************************** //

#if (CRYPTOPP_ALTIVEC_AVAILABLE)
size_t SPECK128_Enc_AdvancedProcessBlocks_ALTIVEC(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x1_ALTIVEC(SPECK128_Enc_Block, SPECK128_Enc_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}

size_t SPECK128_Dec_AdvancedProcessBlocks_ALTIVEC(const word64* subKeys, size_t rounds,
    const byte *inBlocks, const byte *xorBlocks, byte *outBlocks, size_t length, word32 flags)
{
    return AdvancedProcessBlocks128_6x1_ALTIVEC(SPECK128_Dec_Block, SPECK128_Dec_6_Blocks,
        subKeys, rounds, inBlocks, xorBlocks, outBlocks, length, flags);
}
#endif  // CRYPTOPP_ALTIVEC_AVAILABLE

NAMESPACE_END

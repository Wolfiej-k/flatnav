#pragma once

#ifndef NO_MANUAL_VECTORIZATION

// _M_AMD64, _M_X64: Code is being compiled for AMD64 or x64 processor.
#if (defined(__SSE__) || defined(_M_AMD64) || defined(_M_X64))
#define USE_SSE

#ifdef __AVX__
#define USE_AVX

#ifdef __AVX512F__
#define USE_AVX512
#endif

#endif
#endif
#endif

#if defined(USE_AVX) || defined(USE_SSE)

// This macro is defined when compiling on MSVC.
// TODO: Further testing on the MSVC compiler is needed to make sure
// this works as expected.
#ifdef _MSC_VER
#include "cpu_x86.h"
#include <intrin.h>
#include <stdexcept>

void cpu_x86::cpuid(int32_t out[4], int32_t eax, int32_t ecx) {
  __cpuidex(out, eax, ecx);
}
__int64 xgetbv(unsigned int x) { return _xgetbv(x); }

#else
#include <cpuid.h>
#include <stdint.h>
#include <x86intrin.h>

void cpuid(int32_t cpu_info[4], int32_t eax, int32_t ecx) {
  __cpuid_count(eax, ecx, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
}

uint64_t xgetbv(unsigned int index) {
  uint32_t eax, edx;
  __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
  return ((uint64_t)edx << 32) | eax;
}
#endif // _MSC_VER

#if defined(USE_AVX512)
#include <immintrin.h>
#endif // USE_AVX512

#if defined(__GNUC__)
// GCC-specific macros that tells the compiler to align variables on a
// 32/64-byte boundary.
#define PORTABLE_ALIGN32 __attribute__((aligned(32)))
#define PORTABLE_ALIGN64 __attribute__((aligned(64)))
#else
#define PORTABLE_ALIGN32 __declspec(align(32))
#define PORTABLE_ALIGN64 __declspec(align(64))
#endif // __GNUC__

// Adapted from https://github.com/Mysticial/FeatureDetector
#define _XCR_XFEATURE_ENABLED_MASK 0

bool platform_supports_avx() {
  int cpu_info[4];

  // CPU support
  cpuid(cpu_info, 0, 0);
  int n_ids = cpu_info[0];

  bool HW_AVX = false;
  if (n_ids >= 0x00000001) {
    cpuid(cpu_info, 0x00000001, 0);
    HW_AVX = (cpu_info[2] & ((int)1 << 28)) != 0;
  }

  // OS support
  cpuid(cpu_info, 1, 0);

  bool osUsesXSAVE_XRSTORE = (cpu_info[2] & (1 << 27)) != 0;
  bool cpuAVXSuport = (cpu_info[2] & (1 << 28)) != 0;

  bool avxSupported = false;
  if (osUsesXSAVE_XRSTORE && cpuAVXSuport) {
    uint64_t xcrFeatureMask = xgetbv(_XCR_XFEATURE_ENABLED_MASK);
    avxSupported = (xcrFeatureMask & 0x6) == 0x6;
  }
  return HW_AVX && avxSupported;
}

bool platform_supports_avx512() {
  if (!platform_supports_avx()) {
    return false;
  }

  int cpu_info[4];

  // CPU support
  cpuid(cpu_info, 0, 0);
  int n_ids = cpu_info[0];

  bool HW_AVX512F = false;
  if (n_ids >= 0x00000007) { //  AVX512 Foundation
    cpuid(cpu_info, 0x00000007, 0);
    HW_AVX512F = (cpu_info[1] & ((int)1 << 16)) != 0;
  }

  // OS support
  cpuid(cpu_info, 1, 0);

  bool osUsesXSAVE_XRSTORE = (cpu_info[2] & (1 << 27)) != 0;
  bool cpuAVXSuport = (cpu_info[2] & (1 << 28)) != 0;

  bool avx512Supported = false;
  if (osUsesXSAVE_XRSTORE && cpuAVXSuport) {
    uint64_t xcrFeatureMask = xgetbv(_XCR_XFEATURE_ENABLED_MASK);
    avx512Supported = (xcrFeatureMask & 0xe6) == 0xe6;
  }
  return HW_AVX512F && avx512Supported;
}
#endif

#if defined(USE_AVX512)
static float distanceImplInnerProductSIMD16ExtAVX512(const void *x,
                                                     const void *y,
                                                     const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);

  float PORTABLE_ALIGN64 temp_res[16];
  size_t dimension_1_16 = dimension >> 4;
  const float *p_end_x = p_x + (dimension_1_16 << 4);
  __m512 sum = _mm512_set1_ps(0.0f);

  while (p_x != p_end_x) {
    __m512 v1 = _mm512_loadu_ps(p_x);
    __m512 v2 = _mm512_loadu_ps(p_y);
    sum = _mm512_add_ps(sum, _mm512_mul_ps(v1, v2));
    p_x += 16;
    p_y += 16;
  }

  _mm512_store_ps(temp_res, sum);
  float total = temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3] +
                temp_res[4] + temp_res[5] + temp_res[6] + temp_res[7] +
                temp_res[8] + temp_res[9] + temp_res[10] + temp_res[11] +
                temp_res[12] + temp_res[13] + temp_res[14] + temp_res[15];
  return 1.0f - total;
}

static float distanceImplSquaredL2SIMD16ExtAVX512(const void *x, const void *y,
                                                  const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);

  float PORTABLE_ALIGN64 temp_res[16];
  size_t dimension_1_16 = dimension >> 4;
  const float *p_end_x = p_x + (dimension_1_16 << 4);

  __m512 diff, v1, v2;
  __m512 sum = _mm512_set1_ps(0.0f);

  while (p_x != p_end_x) {
    v1 = _mm512_loadu_ps(p_x);
    v2 = _mm512_loadu_ps(p_y);
    diff = _mm512_sub_ps(v1, v2);
    sum = _mm512_add_ps(sum, _mm512_mul_ps(diff, diff));
    p_x += 16;
    p_y += 16;
  }

  _mm512_store_ps(temp_res, sum);
  return temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3] + temp_res[4] +
         temp_res[5] + temp_res[6] + temp_res[7] + temp_res[8] + temp_res[9] +
         temp_res[10] + temp_res[11] + temp_res[12] + temp_res[13] +
         temp_res[14] + temp_res[15];
}
#endif

#if defined(USE_AVX)
static float distanceImplInnerProductSIMD4ExtAVX(const void *x, const void *y,
                                                 const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);
  float PORTABLE_ALIGN32 temp_res[8];

  size_t dimension_1_16 = dimension >> 4;
  size_t dimension_1_4 = dimension >> 2;
  const float *p_end_x1 = p_x + (dimension_1_16 << 4);
  const float *p_end_x2 = p_x + (dimension_1_4 << 2);

  __m256 sum256 = _mm256_set1_ps(0.0f);

  while (p_x != p_end_x1) {
    __m256 v1 = _mm256_loadu_ps(p_x);
    __m256 v2 = _mm256_loadu_ps(p_y);
    sum256 = _mm256_add_ps(sum256, _mm256_mul_ps(v1, v2));
    p_x += 8;
    p_y += 8;

    v1 = _mm256_loadu_ps(p_x);
    v2 = _mm256_loadu_ps(p_y);
    sum256 = _mm256_add_ps(sum256, _mm256_mul_ps(v1, v2));
    p_x += 8;
    p_y += 8;
  }

  __m128 v1, v2;
  __m128 sum_prod = _mm_add_ps(_mm256_extractf128_ps(sum256, 0),
                               _mm256_extractf128_ps(sum256, 1));

  while (p_x != p_end_x2) {
    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum_prod = _mm_add_ps(sum_prod, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;
  }

  _mm_store_ps(temp_res, sum_prod);
  float sum = temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3];

  return 1.0f - sum;
}

static float distanceImplInnerProductSIMD16ExtAVX(const void *x, const void *y,
                                                  const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);

  float PORTABLE_ALIGN32 temp_res[8];
  size_t dimension_1_16 = dimension >> 4;
  const float *p_end_x = p_x + (dimension_1_16 << 4);
  __m256 sum = _mm256_set1_ps(0.0f);

  while (p_x != p_end_x) {
    __m256 v1 = _mm256_loadu_ps(p_x);
    __m256 v2 = _mm256_loadu_ps(p_y);
    sum = _mm256_add_ps(sum, _mm256_mul_ps(v1, v2));
    p_x += 8;
    p_y += 8;

    v1 = _mm256_loadu_ps(p_x);
    v2 = _mm256_loadu_ps(p_y);
    sum = _mm256_add_ps(sum, _mm256_mul_ps(v1, v2));
    p_x += 8;
    p_y += 8;
  }

  _mm256_store_ps(temp_res, sum);
  float total = temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3] +
                temp_res[4] + temp_res[5] + temp_res[6] + temp_res[7];
  return 1.0f - total;
}

static float distanceImplSquaredL2SIMD16ExtAVX(const void *x, const void *y,
                                               const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);

  float PORTABLE_ALIGN32 temp_res[8];
  size_t dimension_1_16 = dimension >> 4;
  const float *p_end_x = p_x + (dimension_1_16 << 4);

  __m256 diff, v1, v2;
  __m256 sum = _mm256_set1_ps(0.0f);

  while (p_x != p_end_x) {
    v1 = _mm256_loadu_ps(p_x);
    v2 = _mm256_loadu_ps(p_y);
    diff = _mm256_sub_ps(v1, v2);
    sum = _mm256_add_ps(sum, _mm256_mul_ps(diff, diff));
    p_x += 8;
    p_y += 8;

    v1 = _mm256_loadu_ps(p_x);
    v2 = _mm256_loadu_ps(p_y);
    diff = _mm256_sub_ps(v1, v2);
    sum = _mm256_add_ps(sum, _mm256_mul_ps(diff, diff));
    p_x += 8;
    p_y += 8;
  }

  _mm256_store_ps(temp_res, sum);

  return temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3] + temp_res[4] +
         temp_res[5] + temp_res[6] + temp_res[7];
}
#endif

#if defined(USE_SSE)

static float distanceImplInnerProductSIMD16ExtSSE(const void *x, const void *y,
                                                  const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);

  float PORTABLE_ALIGN32 temp_res[8];
  size_t dimension_1_16 = dimension >> 4;
  const float *p_end_x = p_x + (dimension_1_16 << 4);
  __m128 sum = _mm_set1_ps(0.0f);
  __m128 v1, v2;

  while (p_x != p_end_x) {
    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum = _mm_add_ps(sum, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum = _mm_add_ps(sum, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum = _mm_add_ps(sum, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum = _mm_add_ps(sum, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;
  }

  _mm_store_ps(temp_res, sum);
  float total = temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3];
  return 1.0f - total;
}

static float distanceImplInnerProductSIMD4ExtSSE(const void *x, const void *y,
                                                 const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);
  float PORTABLE_ALIGN32 temp_res[8];
  size_t dimension_1_4 = dimension >> 2;
  size_t dimension_1_16 = dimension >> 4;

  const float *p_end_x1 = p_x + (dimension_1_16 << 4);
  const float *p_end_x2 = p_x + (dimension_1_4 << 2);

  __m128 sum_prod = _mm_set1_ps(0.0f);
  __m128 v1, v2;

  while (p_x != p_end_x1) {
    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum_prod = _mm_add_ps(sum_prod, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum_prod = _mm_add_ps(sum_prod, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum_prod = _mm_add_ps(sum_prod, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum_prod = _mm_add_ps(sum_prod, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;
  }

  while (p_x != p_end_x2) {
    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    sum_prod = _mm_add_ps(sum_prod, _mm_mul_ps(v1, v2));
    p_x += 4;
    p_y += 4;
  }

  _mm_store_ps(temp_res, sum_prod);
  float sum = temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3];
  return 1.0f - sum;
}

static float distanceImplSquaredL2SIMD16ExtSSE(const void *x, const void *y,
                                               const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);

  float PORTABLE_ALIGN32 temp_res[4];
  size_t dimension_1_16 = dimension >> 4;
  const float *p_end_x = p_x + (dimension_1_16 << 4);

  __m128 diff, v1, v2;
  __m128 sum = _mm_set1_ps(0.0f);

  while (p_x != p_end_x) {
    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    diff = _mm_sub_ps(v1, v2);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    diff = _mm_sub_ps(v1, v2);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    diff = _mm_sub_ps(v1, v2);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));
    p_x += 4;
    p_y += 4;

    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    diff = _mm_sub_ps(v1, v2);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));
    p_x += 4;
    p_y += 4;
  }

  _mm_store_ps(temp_res, sum);

  return temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3];
}

static float distanceImplSquaredL2SIMD4Ext(const void *x, const void *y,
                                           const size_t &dimension) {
  float *p_x = (float *)(x);
  float *p_y = (float *)(y);

  float PORTABLE_ALIGN32 temp_res[8];
  size_t dimension_1_4 = dimension >> 2;
  const float *p_end_x = p_x + (dimension_1_4 << 2);

  __m128 diff, v1, v2;
  __m128 sum = _mm_set1_ps(0.0f);

  while (p_x != p_end_x) {
    v1 = _mm_loadu_ps(p_x);
    v2 = _mm_loadu_ps(p_y);
    diff = _mm_sub_ps(v1, v2);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));
    p_x += 4;
    p_y += 4;
  }

  _mm_store_ps(temp_res, sum);

  return temp_res[0] + temp_res[1] + temp_res[2] + temp_res[3];
}

static float distanceImplSquaredL2SIMD4ExtResiduals(const void *x,
                                                    const void *y,
                                                    const size_t &dimension) {
  // The purpose of this is to ensure that dimension is always a multiple of 4.
  size_t dimension4 = dimension >> 2 << 2;
  float res = distanceImplSquaredL2SIMD4Ext(x, y, dimension4);
  size_t residual = dimension - dimension4;

  float *p_x = (float *)(x) + dimension4;
  float *p_y = (float *)(y) + dimension4;
  float squared_dist_res = 0;
  for (size_t i = 0; i < residual; i++) {
    float diff = *p_x - *p_y;
    p_x++;
    p_y++;
    squared_dist_res += diff * diff;
  }
  return res + squared_dist_res;
}

#endif

#if defined(USE_SSE) || defined(USE_AVX) || defined(USE_AVX512)

static float
distanceImplInnerProductSIMD16ExtResiduals(const void *x, const void *y,
                                           const size_t &dimension) {
  size_t dimension16 = dimension >> 4 << 4;
  float res = distanceImplInnerProductSIMD16ExtSSE(x, y, dimension16);
  size_t residual = dimension - dimension16;

  float *p_x = (float *)(x) + dimension16;
  float *p_y = (float *)(y) + dimension16;
  float sum_res = 0;
  for (size_t i = 0; i < residual; i++) {
    sum_res += *p_x * *p_y;
    p_x++;
    p_y++;
  }
  return 1.0f - (res + sum_res);
}

static float
distanceImplInnerProductSIMD4ExtResiduals(const void *x, const void *y,
                                          const size_t &dimension) {
  size_t dimension4 = dimension >> 2 << 2;
  float res = distanceImplInnerProductSIMD4ExtSSE(x, y, dimension4);
  size_t residual = dimension - dimension4;

  float *p_x = (float *)(x) + dimension4;
  float *p_y = (float *)(y) + dimension4;
  float sum_res = 0;
  for (size_t i = 0; i < residual; i++) {
    sum_res += *p_x * *p_y;
    p_x++;
    p_y++;
  }
  return 1.0f - (res + sum_res);
}

static float distanceImplSquaredL2SIMD16ExtResiduals(const void *x,
                                                     const void *y,
                                                     const size_t &dimension) {
  size_t dimension16 = dimension >> 4 << 4;
  float res = distanceImplSquaredL2SIMD16ExtSSE(x, y, dimension16);
  size_t residual = dimension - dimension16;

  float *p_x = (float *)(x) + dimension16;
  float *p_y = (float *)(y) + dimension16;
  float squared_dist_res = 0;
  for (size_t i = 0; i < residual; i++) {
    float diff = *p_x - *p_y;
    p_x++;
    p_y++;
    squared_dist_res += diff * diff;
  }
  return res + squared_dist_res;
}
#endif

// namespace flatnav::util
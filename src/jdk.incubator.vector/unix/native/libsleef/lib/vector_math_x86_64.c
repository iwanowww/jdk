/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifdef __AVX512F__

#include <stdint.h>
#include <immintrin.h>

#include "../generated/misc.h"
#include "../generated/sleefinline_sse2.h"
#include "../generated/sleefinline_sse4.h"
#include "../generated/sleefinline_avx2128.h"
#include "../generated/sleefinline_avx.h"
#include "../generated/sleefinline_avx2.h"
#include "../generated/sleefinline_avx512f.h"

#include <jni.h>

typedef __m128  float32x4_t;
typedef __m128d float64x2_t;

typedef __m256  float32x8_t;
typedef __m256d float64x4_t;

typedef __m512  float32x16_t;
typedef __m512d float64x8_t;

#define DEFINE_UNARY_OP(op, flavor, type) \
JNIEXPORT                                          \
type op##flavor(type input) {                      \
  return Sleef_##op##flavor(input);                \
}

#define DEFINE_BINARY_OP(op, flavor, type) \
JNIEXPORT                                           \
type op##flavor(type input1, type input2) {         \
  return Sleef_##op##flavor(input1, input2);        \
}

#define VECTOR_MATH_OPERATION_DO(do_operation, op, pr) \
  do_operation(op##d2_##pr,  sse2,    float64x2_t) \
  do_operation(op##d2_##pr,  sse4,    float64x2_t) \
  do_operation(op##d2_##pr,  avx2128, float64x2_t) \
  do_operation(op##d4_##pr,  avx,     float64x4_t) \
  do_operation(op##d4_##pr,  avx2,    float64x4_t) \
  do_operation(op##d8_##pr,  avx512f, float64x8_t) \
  do_operation(op##f4_##pr,  sse2,    float32x4_t) \
  do_operation(op##f4_##pr,  sse4,    float32x4_t) \
  do_operation(op##f4_##pr,  avx2128, float32x4_t) \
  do_operation(op##f8_##pr,  avx,     float32x8_t) \
  do_operation(op##f8_##pr,  avx2,    float32x8_t) \
  do_operation(op##f16_##pr, avx512f, float32x16_t)

VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, sin,   u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, cos,   u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, sinh,  u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, cosh,  u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, tan,   u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, tanh,  u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, asin,  u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, acos,  u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, atan,  u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, cbrt,  u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, log,   u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, log10, u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, log1p, u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, exp,   u10)
VECTOR_MATH_OPERATION_DO(DEFINE_UNARY_OP, expm1, u10)

VECTOR_MATH_OPERATION_DO(DEFINE_BINARY_OP, atan2, u10)
VECTOR_MATH_OPERATION_DO(DEFINE_BINARY_OP, pow,   u10)
VECTOR_MATH_OPERATION_DO(DEFINE_BINARY_OP, hypot, u05)

#undef DEFINE_UNARY

#undef DEFINE_BINARY

#undef VECTOR_MATH_OPERATION_DO

#endif // __AVX512F__

/*
 * Copyright (c) 2000, 2022, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2021, Red Hat Inc. All rights reserved.
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

#ifndef CPU_AARCH64_REGISTER_AARCH64_HPP
#define CPU_AARCH64_REGISTER_AARCH64_HPP

#include "asm/register.hpp"
#include "code/vmreg.hpp"
#include "utilities/powerOfTwo.hpp"

class RegisterImpl;

class Register {
  friend class RegisterImpl;

  static constexpr Register first();

  const RegisterImpl* _ptr;

  constexpr Register(const RegisterImpl* ptr) : _ptr(ptr) {}
public:
  inline friend constexpr Register as_Register(int encoding);

  constexpr Register() : _ptr(NULL) {}

  inline static constexpr Register successor(Register r);
  inline static constexpr int encoding(Register r);
  inline static constexpr int encoding_nocheck(Register r);
  inline static constexpr const char* name(Register r);
  inline static constexpr VMReg as_VMReg(Register r);
  inline static constexpr bool is_valid(Register r);

  int operator==(const Register r) const { return _ptr == r._ptr; }
  int operator!=(const Register r) const { return _ptr != r._ptr; }

  int operator<=(const Register r) const { return _ptr <= r._ptr; }
  int operator>=(const Register r) const { return _ptr >= r._ptr; }
  int operator>(const Register r) const { return _ptr > r._ptr; }
  int operator<(const Register r) const { return _ptr < r._ptr; }

//  const RegisterImpl* operator->() const { return _ptr; }
};

class RegisterImpl: public AbstractRegisterImpl {
public:
  static constexpr const RegisterImpl* first();

  enum {
    number_of_registers         =   32,
    number_of_declared_registers  = 34,  // Including SP and ZR.
    max_slots_per_register = 2
  };

  // derived registers, offsets, and addresses
  const Register successor() const { return Register(this + 1); }
//  const RegisterImpl* successor() const { return this + 1; }

  // construction
  inline friend constexpr const RegisterImpl* as_RegisterImpl(int encoding);

  VMReg as_VMReg() const;

  // accessors
  int encoding() const             { assert(is_valid(), "invalid register"); return encoding_nocheck(); }
  bool is_valid() const            { return (unsigned)encoding_nocheck() < number_of_registers; }
  const char* name() const;
  int encoding_nocheck() const     { return this - first(); }
};

inline constexpr const RegisterImpl* as_RegisterImpl(int encoding) {
  return RegisterImpl::first() + encoding;
}
extern RegisterImpl all_Registers[RegisterImpl::number_of_declared_registers + 1] INTERNAL_VISIBILITY;

inline constexpr const RegisterImpl* RegisterImpl::first() { return all_Registers + 1; }

inline constexpr Register as_Register(int encoding) {
  return Register(as_RegisterImpl(encoding));
}

inline constexpr Register Register::first() {
  return Register(RegisterImpl::first());
}

inline constexpr Register Register::successor(Register r) {
  return Register(r._ptr->successor());
}

inline constexpr int Register::encoding(Register r) {
  return r._ptr->encoding();
}

inline constexpr int Register::encoding_nocheck(Register r) {
  return r._ptr->encoding_nocheck();
}

inline constexpr VMReg Register::as_VMReg(Register r) {
  return r._ptr->as_VMReg();
}

inline constexpr const char* Register::name(Register r) {
  return r._ptr->name();
}

inline constexpr bool Register::is_valid(Register r) {
  return r._ptr->is_valid();
}

// The integer registers of the aarch64 architecture

constexpr Register noreg = as_Register(-1);
constexpr Register r0    = as_Register(0);
constexpr Register r1    = as_Register(1);
constexpr Register r2    = as_Register(2);
constexpr Register r3    = as_Register(3);
constexpr Register r4    = as_Register(4);
constexpr Register r5    = as_Register(5);
constexpr Register r6    = as_Register(6);
constexpr Register r7    = as_Register(7);
constexpr Register r8    = as_Register(8);
constexpr Register r9    = as_Register(9);
constexpr Register r10   = as_Register(10);
constexpr Register r11   = as_Register(11);
constexpr Register r12   = as_Register(12);
constexpr Register r13   = as_Register(13);
constexpr Register r14   = as_Register(14);
constexpr Register r15   = as_Register(15);
constexpr Register r16   = as_Register(16);
constexpr Register r17   = as_Register(17);

// In the ABI for Windows+AArch64 the register r18 is used to store the pointer
// to the current thread's TEB (where TLS variables are stored). We could
// carefully save and restore r18 at key places, however Win32 Structured
// Exception Handling (SEH) is using TLS to unwind the stack. If r18 is used
// for any other purpose at the time of an exception happening, SEH would not
// be able to unwind the stack properly and most likely crash.
//
// It's easier to avoid allocating r18 altogether.
//
// See https://docs.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions?view=vs-2019#integer-registers
constexpr Register r18_tls = as_Register(18);
constexpr Register r19     = as_Register(19);
constexpr Register r20     = as_Register(20);
constexpr Register r21     = as_Register(21);
constexpr Register r22     = as_Register(22);
constexpr Register r23     = as_Register(23);
constexpr Register r24     = as_Register(24);
constexpr Register r25     = as_Register(25);
constexpr Register r26     = as_Register(26);
constexpr Register r27     = as_Register(27);
constexpr Register r28     = as_Register(28);
constexpr Register r29     = as_Register(29);
constexpr Register r30     = as_Register(30);

// r31 is not a general purpose register, but represents either the
// stack pointer or the zero/discard register depending on the
// instruction.
constexpr Register r31_sp = as_Register(31);
constexpr Register zr     = as_Register(32);
constexpr Register sp     = as_Register(33);

// Used as a filler in instructions where a register field is unused.
constexpr Register dummy_reg = r31_sp;

/* ============================================================================= */

class FloatRegisterImpl;

class FloatRegister {
  static constexpr FloatRegister first();

  const FloatRegisterImpl* _ptr;

public:
  inline friend constexpr FloatRegister as_FloatRegister(int encoding);

  constexpr FloatRegister() : _ptr(NULL) {}
  constexpr FloatRegister(const FloatRegisterImpl* ptr) : _ptr(ptr) {}

  inline static constexpr FloatRegister successor(FloatRegister fr);
  inline static constexpr int encoding(FloatRegister fr);
  inline static constexpr int encoding_nocheck(FloatRegister fr);
  inline static constexpr const char* name(FloatRegister fr);
  inline static constexpr VMReg as_VMReg(FloatRegister fr);
  inline static constexpr bool is_valid(FloatRegister fr);

  int operator==(const FloatRegister r) const { return _ptr == r._ptr; }
  int operator!=(const FloatRegister r) const { return _ptr != r._ptr; }
  int operator<(const FloatRegister r) const { return _ptr < r._ptr; }

  inline FloatRegister operator+(int off) const;
  inline FloatRegister operator+=(int off) const;

  const FloatRegisterImpl* operator->() const { return _ptr; }
};


inline constexpr FloatRegister as_FloatRegister(int encoding);

// The implementation of floating point registers for the architecture
class FloatRegisterImpl: public AbstractRegisterImpl {
public:
  static constexpr const FloatRegisterImpl* first();

  enum {
    number_of_registers = 32,
    max_slots_per_register = 8,
    save_slots_per_register = 2,
    slots_per_neon_register = 4,
    extra_save_slots_per_neon_register = slots_per_neon_register - save_slots_per_register
  };

  // construction
  inline friend constexpr const FloatRegisterImpl* as_FloatRegisterImpl(int encoding);

  VMReg as_VMReg() const;

  // derived registers, offsets, and addresses
  FloatRegister successor() const {
    return as_FloatRegister((encoding() + 1) % (unsigned)number_of_registers);
  }

  // accessors
  int encoding() const             { assert(is_valid(), "invalid register"); return encoding_nocheck(); }
  bool is_valid() const            { return (unsigned)encoding_nocheck() < number_of_registers; }
  const char* name() const;
  int encoding_nocheck() const     { return this - first(); }
};

inline constexpr const FloatRegisterImpl* as_FloatRegisterImpl(int encoding) {
  return FloatRegisterImpl::first() + encoding;
}
extern FloatRegisterImpl all_FloatRegisters[FloatRegisterImpl::number_of_registers + 1] INTERNAL_VISIBILITY;

inline constexpr const FloatRegisterImpl* FloatRegisterImpl::first() { return all_FloatRegisters + 1; }

inline constexpr FloatRegister as_FloatRegister(int encoding) {
  return FloatRegister(as_FloatRegisterImpl(encoding));
}

inline constexpr FloatRegister FloatRegister::successor(FloatRegister fr) {
  return FloatRegister(fr._ptr->successor());
}

inline constexpr int FloatRegister::encoding(FloatRegister fr) {
  return fr._ptr->encoding();
}

inline constexpr int FloatRegister::encoding_nocheck(FloatRegister fr) {
  return fr._ptr->encoding_nocheck();
}

inline constexpr VMReg FloatRegister::as_VMReg(FloatRegister fr) {
  return fr._ptr->as_VMReg();
}

inline constexpr const char* FloatRegister::name(FloatRegister fr) {
  return fr._ptr->name();
}

inline constexpr bool FloatRegister::is_valid(FloatRegister fr) {
  return fr._ptr->is_valid();
}

inline FloatRegister FloatRegister::operator+(int off) const {
  return as_FloatRegister(_ptr->encoding_nocheck() + off);
}

inline FloatRegister FloatRegister::operator+=(int off) const {
  return as_FloatRegister(_ptr->encoding_nocheck() + off);
}

// The float registers of the AARCH64 architecture

constexpr FloatRegister fnoreg = as_FloatRegister(-1);

constexpr FloatRegister v0  = as_FloatRegister( 0);
constexpr FloatRegister v1  = as_FloatRegister( 1);
constexpr FloatRegister v2  = as_FloatRegister( 2);
constexpr FloatRegister v3  = as_FloatRegister( 3);
constexpr FloatRegister v4  = as_FloatRegister( 4);
constexpr FloatRegister v5  = as_FloatRegister( 5);
constexpr FloatRegister v6  = as_FloatRegister( 6);
constexpr FloatRegister v7  = as_FloatRegister( 7);
constexpr FloatRegister v8  = as_FloatRegister( 8);
constexpr FloatRegister v9  = as_FloatRegister( 9);
constexpr FloatRegister v10 = as_FloatRegister(10);
constexpr FloatRegister v11 = as_FloatRegister(11);
constexpr FloatRegister v12 = as_FloatRegister(12);
constexpr FloatRegister v13 = as_FloatRegister(13);
constexpr FloatRegister v14 = as_FloatRegister(14);
constexpr FloatRegister v15 = as_FloatRegister(15);
constexpr FloatRegister v16 = as_FloatRegister(16);
constexpr FloatRegister v17 = as_FloatRegister(17);
constexpr FloatRegister v18 = as_FloatRegister(18);
constexpr FloatRegister v19 = as_FloatRegister(19);
constexpr FloatRegister v20 = as_FloatRegister(20);
constexpr FloatRegister v21 = as_FloatRegister(21);
constexpr FloatRegister v22 = as_FloatRegister(22);
constexpr FloatRegister v23 = as_FloatRegister(23);
constexpr FloatRegister v24 = as_FloatRegister(24);
constexpr FloatRegister v25 = as_FloatRegister(25);
constexpr FloatRegister v26 = as_FloatRegister(26);
constexpr FloatRegister v27 = as_FloatRegister(27);
constexpr FloatRegister v28 = as_FloatRegister(28);
constexpr FloatRegister v29 = as_FloatRegister(29);
constexpr FloatRegister v30 = as_FloatRegister(30);
constexpr FloatRegister v31 = as_FloatRegister(31);

// SVE vector registers, shared with the SIMD&FP v0-v31. Vn maps to Zn[127:0].
constexpr FloatRegister z0  = as_FloatRegister( 0);
constexpr FloatRegister z1  = as_FloatRegister( 1);
constexpr FloatRegister z2  = as_FloatRegister( 2);
constexpr FloatRegister z3  = as_FloatRegister( 3);
constexpr FloatRegister z4  = as_FloatRegister( 4);
constexpr FloatRegister z5  = as_FloatRegister( 5);
constexpr FloatRegister z6  = as_FloatRegister( 6);
constexpr FloatRegister z7  = as_FloatRegister( 7);
constexpr FloatRegister z8  = as_FloatRegister( 8);
constexpr FloatRegister z9  = as_FloatRegister( 9);
constexpr FloatRegister z10 = as_FloatRegister(10);
constexpr FloatRegister z11 = as_FloatRegister(11);
constexpr FloatRegister z12 = as_FloatRegister(12);
constexpr FloatRegister z13 = as_FloatRegister(13);
constexpr FloatRegister z14 = as_FloatRegister(14);
constexpr FloatRegister z15 = as_FloatRegister(15);
constexpr FloatRegister z16 = as_FloatRegister(16);
constexpr FloatRegister z17 = as_FloatRegister(17);
constexpr FloatRegister z18 = as_FloatRegister(18);
constexpr FloatRegister z19 = as_FloatRegister(19);
constexpr FloatRegister z20 = as_FloatRegister(20);
constexpr FloatRegister z21 = as_FloatRegister(21);
constexpr FloatRegister z22 = as_FloatRegister(22);
constexpr FloatRegister z23 = as_FloatRegister(23);
constexpr FloatRegister z24 = as_FloatRegister(24);
constexpr FloatRegister z25 = as_FloatRegister(25);
constexpr FloatRegister z26 = as_FloatRegister(26);
constexpr FloatRegister z27 = as_FloatRegister(27);
constexpr FloatRegister z28 = as_FloatRegister(28);
constexpr FloatRegister z29 = as_FloatRegister(29);
constexpr FloatRegister z30 = as_FloatRegister(30);
constexpr FloatRegister z31 = as_FloatRegister(31);

/* ============================================================================= */

class PRegisterImpl;

class PRegister {
  static constexpr PRegister first();

  const PRegisterImpl* _ptr;

public:
  inline friend constexpr PRegister as_PRegister(int encoding);

  constexpr PRegister() : _ptr(NULL) {}
  constexpr PRegister(const PRegisterImpl* ptr) : _ptr(ptr) {}

  inline static constexpr PRegister successor(PRegister pr);
  inline static constexpr int encoding(PRegister pr);
  inline static constexpr int encoding_nocheck(PRegister pr);
  inline static constexpr const char* name(PRegister pr);
  inline static constexpr VMReg as_VMReg(PRegister pr);
  inline static constexpr bool is_valid(PRegister pr);

  int operator==(const PRegister r) const { return _ptr == r._ptr; }
  int operator!=(const PRegister r) const { return _ptr != r._ptr; }

  const PRegisterImpl* operator->() const { return _ptr; }
};

inline constexpr PRegister as_PRegister(int encoding);

// The implementation of predicate registers for the architecture
class PRegisterImpl: public AbstractRegisterImpl {
 public:
  static constexpr const PRegisterImpl* first();

  enum {
    number_of_registers = 16,
    number_of_governing_registers = 8,
    // p0-p7 are governing predicates for load/store and arithmetic, but p7 is
    // preserved as an all-true predicate in OpenJDK. And since we don't support
    // non-governing predicate registers allocation for non-temp register, the
    // predicate registers to be saved are p0-p6.
    number_of_saved_registers = number_of_governing_registers - 1,
    max_slots_per_register = 1
  };

  // construction
  inline friend constexpr const PRegisterImpl* as_PRegisterImpl(int encoding);

  VMReg as_VMReg() const;

  // derived registers, offsets, and addresses
  PRegister successor() const     { return this + 1; }

  // accessors
  int encoding() const            { assert(is_valid(), "invalid register"); return encoding_nocheck(); }
  int encoding_nocheck() const    { return this - first(); }
  bool is_valid() const           { return (unsigned)encoding_nocheck() < number_of_registers; }
  bool is_governing() const       { return first() <= this && this - first() < number_of_governing_registers; }
  const char* name() const;
};

inline constexpr const PRegisterImpl* as_PRegisterImpl(int encoding) {
  return PRegisterImpl::first() + encoding;
}
extern PRegisterImpl all_PRegisters[PRegisterImpl::number_of_registers + 1] INTERNAL_VISIBILITY;

inline constexpr const PRegisterImpl* PRegisterImpl::first() { return all_PRegisters + 1; }

inline constexpr PRegister as_PRegister(int encoding) {
  return PRegister(as_PRegisterImpl(encoding));
}

inline constexpr PRegister PRegister::first() {
  return PRegister(PRegisterImpl::first());
}

inline constexpr PRegister PRegister::successor(PRegister pr) {
  return PRegister(pr._ptr->successor());
}

inline constexpr int PRegister::encoding(PRegister pr) {
  return pr._ptr->encoding();
}

inline constexpr int PRegister::encoding_nocheck(PRegister pr) {
  return pr._ptr->encoding_nocheck();
}

inline constexpr VMReg PRegister::as_VMReg(PRegister pr) {
  return pr._ptr->as_VMReg();
}

inline constexpr const char* PRegister::name(PRegister pr) {
  return pr._ptr->name();
}

inline constexpr bool PRegister::is_valid(PRegister pr) {
  return pr._ptr->is_valid();
}

// The predicate registers of SVE.
//
constexpr PRegister pnoreg = as_PRegister(-1);

constexpr PRegister p0  = as_PRegister( 0);
constexpr PRegister p1  = as_PRegister( 1);
constexpr PRegister p2  = as_PRegister( 2);
constexpr PRegister p3  = as_PRegister( 3);
constexpr PRegister p4  = as_PRegister( 4);
constexpr PRegister p5  = as_PRegister( 5);
constexpr PRegister p6  = as_PRegister( 6);
constexpr PRegister p7  = as_PRegister( 7);
constexpr PRegister p8  = as_PRegister( 8);
constexpr PRegister p9  = as_PRegister( 9);
constexpr PRegister p10 = as_PRegister(10);
constexpr PRegister p11 = as_PRegister(11);
constexpr PRegister p12 = as_PRegister(12);
constexpr PRegister p13 = as_PRegister(13);
constexpr PRegister p14 = as_PRegister(14);
constexpr PRegister p15 = as_PRegister(15);

// Need to know the total number of registers of all sorts for SharedInfo.
// Define a class that exports it.
class ConcreteRegisterImpl : public AbstractRegisterImpl {
 public:
  enum {
  // A big enough number for C2: all the registers plus flags
  // This number must be large enough to cover REG_COUNT (defined by c2) registers.
  // There is no requirement that any ordering here matches any ordering c2 gives
  // it's optoregs.

    number_of_registers = (RegisterImpl::max_slots_per_register * RegisterImpl::number_of_registers +
                           FloatRegisterImpl::max_slots_per_register * FloatRegisterImpl::number_of_registers +
                           PRegisterImpl::max_slots_per_register * PRegisterImpl::number_of_registers +
                           1) // flags
  };

  // added to make it compile
  static const int max_gpr;
  static const int max_fpr;
  static const int max_pr;
};

typedef AbstractRegSet<Register> RegSet;
typedef AbstractRegSet<FloatRegister> FloatRegSet;
typedef AbstractRegSet<PRegister> PRegSet;

template <>
inline Register AbstractRegSet<Register>::first() {
  uint32_t first = _bitset & -_bitset;
  return first ? as_Register(exact_log2(first)) : noreg;
}

template <>
inline FloatRegister AbstractRegSet<FloatRegister>::first() {
  uint32_t first = _bitset & -_bitset;
  return first ? as_FloatRegister(exact_log2(first)) : fnoreg;
}

inline Register as_Register(FloatRegister reg) {
  return as_Register(reg->encoding());
}

#endif // CPU_AARCH64_REGISTER_AARCH64_HPP

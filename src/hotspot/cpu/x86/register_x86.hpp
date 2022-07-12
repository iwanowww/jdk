/*
 * Copyright (c) 2000, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef CPU_X86_REGISTER_X86_HPP
#define CPU_X86_REGISTER_X86_HPP

#include "asm/register.hpp"
#include "runtime/globals.hpp"
#include "utilities/count_leading_zeros.hpp"
#include "utilities/powerOfTwo.hpp"

class VMRegImpl;
typedef VMRegImpl* VMReg;

// The implementation of integer registers for the x86/x64 architectures

class Register;

class RegisterImpl: public AbstractRegisterImpl {
  static constexpr RegisterImpl* first();

public:
  enum {
#ifdef _LP64
    number_of_registers      = 16,
    number_of_byte_registers = 16,
    max_slots_per_register   = 2
#else
    number_of_registers      = 8,
    number_of_byte_registers = 4,
    max_slots_per_register   = 1
#endif // _LP64
  };

  // derived registers, offsets, and addresses
  inline Register successor() const;

  // construction
  inline friend constexpr RegisterImpl* as_RegisterImpl(int encoding);

  inline VMReg as_VMReg() const;

  // accessors
  int   raw_encoding() const                     { return this - first(); }
  int   encoding() const                         { assert(is_valid(), "invalid register"); return raw_encoding(); }
  bool  is_valid() const                         { return 0 <= raw_encoding() && raw_encoding() < number_of_registers; }
  bool  has_byte_register() const                { return 0 <= raw_encoding() && raw_encoding() < number_of_byte_registers; }
  const char* name() const;
};

//REGISTER_IMPL_DECLARATION(Register, RegisterImpl, RegisterImpl::number_of_registers);

inline constexpr RegisterImpl* as_RegisterImpl(int encoding) {
  return RegisterImpl::first() + encoding;
}
extern RegisterImpl all_RegisterImpls[RegisterImpl::number_of_registers + 1] INTERNAL_VISIBILITY;
inline constexpr RegisterImpl* RegisterImpl::first() { return all_RegisterImpls + 1; }

class Register {
  friend class RegisterImpl;

  const RegisterImpl* _ptr;

  constexpr Register(const RegisterImpl* ptr, bool unused) : _ptr(ptr) {}

public:
  inline friend constexpr Register as_Register(int encoding);

  constexpr Register() : _ptr(as_RegisterImpl(-1)) {} // noreg

  int operator==(const Register r) const { return _ptr == r._ptr; }
  int operator!=(const Register r) const { return _ptr != r._ptr; }

  const RegisterImpl* operator->() const { return _ptr; }
};

constexpr Register noreg = Register();

inline constexpr Register as_Register(int encoding) {
  if (0 <= encoding && encoding < RegisterImpl::number_of_registers) {
    return Register(as_RegisterImpl(encoding), false);
  }
  return noreg;
}

inline Register RegisterImpl::successor() const {
  return as_Register(encoding() + 1);
}

// The integer registers of the x86/x64 architectures
constexpr Register rax = as_Register(0);
constexpr Register rcx = as_Register(1);
constexpr Register rdx = as_Register(2);
constexpr Register rbx = as_Register(3);
constexpr Register rsp = as_Register(4);
constexpr Register rbp = as_Register(5);
constexpr Register rsi = as_Register(6);
constexpr Register rdi = as_Register(7);
#ifdef _LP64
constexpr Register r8  = as_Register( 8);
constexpr Register r9  = as_Register( 9);
constexpr Register r10 = as_Register(10);
constexpr Register r11 = as_Register(11);
constexpr Register r12 = as_Register(12);
constexpr Register r13 = as_Register(13);
constexpr Register r14 = as_Register(14);
constexpr Register r15 = as_Register(15);
#endif // _LP64


// The implementation of floating point registers for the ia32 architecture
class FloatRegister;

class FloatRegisterImpl: public AbstractRegisterImpl {
  static constexpr FloatRegisterImpl* first();

public:
  enum {
    number_of_registers = 8
  };

  // derived registers, offsets, and addresses
  inline FloatRegister successor() const;

  // construction
  inline friend constexpr FloatRegisterImpl* as_FloatRegisterImpl(int encoding);

  inline VMReg as_VMReg() const;

  // accessors
  int   raw_encoding() const                      { return this - first(); }
  int   encoding() const                          { assert(is_valid(), "invalid register"); return raw_encoding(); }
  bool  is_valid() const                          { return 0 <= raw_encoding() && raw_encoding() < number_of_registers; }
  const char* name() const;
};

// REGISTER_IMPL_DECLARATION(FloatRegister, FloatRegisterImpl, FloatRegisterImpl::number_of_registers);
inline constexpr FloatRegisterImpl* as_FloatRegisterImpl(int encoding) {
  return FloatRegisterImpl::first() + encoding;
}
extern FloatRegisterImpl all_FloatRegisterImpls[FloatRegisterImpl::number_of_registers + 1] INTERNAL_VISIBILITY;
inline constexpr FloatRegisterImpl* FloatRegisterImpl::first() { return all_FloatRegisterImpls + 1; }

class FloatRegister {
  friend class FloatRegisterImpl;

  const FloatRegisterImpl* _ptr;

  constexpr FloatRegister(const FloatRegisterImpl* ptr, bool unused) : _ptr(ptr) {}

public:
  inline friend constexpr FloatRegister as_FloatRegister(int encoding);

  constexpr FloatRegister() : _ptr(as_FloatRegisterImpl(-1)) {} // fnoreg

  int operator==(const FloatRegister r) const { return _ptr == r._ptr; }
  int operator!=(const FloatRegister r) const { return _ptr != r._ptr; }

  const FloatRegisterImpl* operator->() const { return _ptr; }
};

constexpr FloatRegister fnoreg = FloatRegister();

inline constexpr FloatRegister as_FloatRegister(int encoding) {
  if (0 <= encoding && encoding < FloatRegisterImpl::number_of_registers) {
    return FloatRegister(as_FloatRegisterImpl(encoding), false);
  }
  return fnoreg;
}

inline FloatRegister FloatRegisterImpl::successor() const {
  return as_FloatRegister(encoding() + 1);
}


// The implementation of XMM registers.
class XMMRegister;

class XMMRegisterImpl: public AbstractRegisterImpl {
  static constexpr XMMRegisterImpl* first();

 public:
  enum {
#ifdef _LP64
    number_of_registers = 32,
    max_slots_per_register = 16   // 512-bit
#else
    number_of_registers = 8,
    max_slots_per_register = 16   // 512-bit
#endif // _LP64
  };

  // derived registers, offsets, and addresses
  inline XMMRegister successor() const;

  // construction
  friend constexpr XMMRegisterImpl* as_XMMRegisterImpl(int encoding);

  inline VMReg as_VMReg() const;

  // accessors
  int raw_encoding() const                       { return this - first(); }
  int   encoding() const                         { assert(is_valid(), "invalid register"); return raw_encoding(); }
  bool  is_valid() const                         { return 0 <= raw_encoding() && raw_encoding() < number_of_registers; }
  const char* name() const;
  const char* sub_word_name(int offset) const;

  // Actually available XMM registers for use, depending on actual CPU capabilities
  // and flags.
  static int available_xmm_registers() {
    int num_xmm_regs = XMMRegisterImpl::number_of_registers;
#ifdef _LP64
    if (UseAVX < 3) {
      num_xmm_regs /= 2;
    }
#endif
    return num_xmm_regs;
  }
};

// REGISTER_IMPL_DECLARATION(XMMRegister, XMMRegisterImpl, XMMRegisterImpl::number_of_registers);
inline constexpr XMMRegisterImpl* as_XMMRegisterImpl(int encoding) {
  return XMMRegisterImpl::first() + encoding;
}
extern XMMRegisterImpl all_XMMRegisterImpls[XMMRegisterImpl::number_of_registers + 1] INTERNAL_VISIBILITY;
inline constexpr XMMRegisterImpl* XMMRegisterImpl::first() { return all_XMMRegisterImpls + 1; }

class XMMRegister {
  friend class XMMRegisterImpl;

  const XMMRegisterImpl* _ptr;

  constexpr XMMRegister(const XMMRegisterImpl* ptr, bool unused) : _ptr(ptr) {}

public:
  inline friend constexpr XMMRegister as_XMMRegister(int encoding);

  constexpr XMMRegister() : _ptr(as_XMMRegisterImpl(-1)) {} // xnoreg

  int operator==(const XMMRegister r) const { return _ptr == r._ptr; }
  int operator!=(const XMMRegister r) const { return _ptr != r._ptr; }

  const XMMRegisterImpl* operator->() const { return _ptr; }
};

constexpr XMMRegister xnoreg = XMMRegister();

inline constexpr XMMRegister as_XMMRegister(int encoding) {
  if (0 <= encoding && encoding < XMMRegisterImpl::number_of_registers) {
    return XMMRegister(as_XMMRegisterImpl(encoding), false);
  }
  return xnoreg;
}

inline XMMRegister XMMRegisterImpl::successor() const {
  return as_XMMRegister(encoding() + 1);
}

// The XMM registers, for P3 and up chips
constexpr XMMRegister xmm0  = as_XMMRegister( 0);
constexpr XMMRegister xmm1  = as_XMMRegister( 1);
constexpr XMMRegister xmm2  = as_XMMRegister( 2);
constexpr XMMRegister xmm3  = as_XMMRegister( 3);
constexpr XMMRegister xmm4  = as_XMMRegister( 4);
constexpr XMMRegister xmm5  = as_XMMRegister( 5);
constexpr XMMRegister xmm6  = as_XMMRegister( 6);
constexpr XMMRegister xmm7  = as_XMMRegister( 7);
#ifdef _LP64
constexpr XMMRegister xmm8  = as_XMMRegister( 8);
constexpr XMMRegister xmm9  = as_XMMRegister( 9);
constexpr XMMRegister xmm10 = as_XMMRegister(10);
constexpr XMMRegister xmm11 = as_XMMRegister(11);
constexpr XMMRegister xmm12 = as_XMMRegister(12);
constexpr XMMRegister xmm13 = as_XMMRegister(13);
constexpr XMMRegister xmm14 = as_XMMRegister(14);
constexpr XMMRegister xmm15 = as_XMMRegister(15);
constexpr XMMRegister xmm16 = as_XMMRegister(16);
constexpr XMMRegister xmm17 = as_XMMRegister(17);
constexpr XMMRegister xmm18 = as_XMMRegister(18);
constexpr XMMRegister xmm19 = as_XMMRegister(19);
constexpr XMMRegister xmm20 = as_XMMRegister(20);
constexpr XMMRegister xmm21 = as_XMMRegister(21);
constexpr XMMRegister xmm22 = as_XMMRegister(22);
constexpr XMMRegister xmm23 = as_XMMRegister(23);
constexpr XMMRegister xmm24 = as_XMMRegister(24);
constexpr XMMRegister xmm25 = as_XMMRegister(25);
constexpr XMMRegister xmm26 = as_XMMRegister(26);
constexpr XMMRegister xmm27 = as_XMMRegister(27);
constexpr XMMRegister xmm28 = as_XMMRegister(28);
constexpr XMMRegister xmm29 = as_XMMRegister(29);
constexpr XMMRegister xmm30 = as_XMMRegister(30);
constexpr XMMRegister xmm31 = as_XMMRegister(31);
#endif // _LP64


// The implementation of AVX-512 opmask registers.
class KRegister;

class KRegisterImpl: public AbstractRegisterImpl {
  static constexpr KRegisterImpl* first();

 public:
  enum {
    number_of_registers = 8,
    // opmask registers are 64bit wide on both 32 and 64 bit targets.
    // thus two slots are reserved per register.
    max_slots_per_register = 2
  };

  // derived registers, offsets, and addresses
  inline KRegister successor() const;

  // construction
  inline friend constexpr KRegisterImpl* as_KRegisterImpl(int encoding);

  inline VMReg as_VMReg() const;

  // accessors
  int   raw_encoding() const { return this - first(); }
  int   encoding() const     { assert(is_valid(), "invalid register"); return raw_encoding(); }
  bool  is_valid() const     { return 0 <= raw_encoding() && raw_encoding() < number_of_registers; }
  const char* name() const;
};

//REGISTER_IMPL_DECLARATION(KRegister, KRegisterImpl, KRegisterImpl::number_of_registers);
inline constexpr KRegisterImpl* as_KRegisterImpl(int encoding) {
  return KRegisterImpl::first() + encoding;
}
extern KRegisterImpl all_KRegisterImpls[KRegisterImpl::number_of_registers + 1] INTERNAL_VISIBILITY;
inline constexpr KRegisterImpl* KRegisterImpl::first() { return all_KRegisterImpls + 1; }


class KRegister {
  friend class KRegisterImpl;

  const KRegisterImpl* _ptr;

  constexpr KRegister(const KRegisterImpl* ptr, bool unused) : _ptr(ptr) {}

public:
  inline friend constexpr KRegister as_KRegister(int encoding);

  constexpr KRegister() : _ptr(as_KRegisterImpl(-1)) {} // xnoreg

  int operator==(const KRegister r) const { return _ptr == r._ptr; }
  int operator!=(const KRegister r) const { return _ptr != r._ptr; }

  const KRegisterImpl* operator->() const { return _ptr; }
};

constexpr KRegister knoreg = KRegister();

inline constexpr KRegister as_KRegister(int encoding) {
  if (0 <= encoding && encoding < KRegisterImpl::number_of_registers) {
    return KRegister(as_KRegisterImpl(encoding), false);
  }
  return knoreg;
}

inline KRegister KRegisterImpl::successor() const {
  return as_KRegister(encoding() + 1);
}

// The Mask registers, for AVX-512 enabled and up chips
constexpr KRegister k0 = as_KRegister(0);
constexpr KRegister k1 = as_KRegister(1);
constexpr KRegister k2 = as_KRegister(2);
constexpr KRegister k3 = as_KRegister(3);
constexpr KRegister k4 = as_KRegister(4);
constexpr KRegister k5 = as_KRegister(5);
constexpr KRegister k6 = as_KRegister(6);
constexpr KRegister k7 = as_KRegister(7);


// Need to know the total number of registers of all sorts for SharedInfo.
// Define a class that exports it.
class ConcreteRegisterImpl : public AbstractRegisterImpl {
 public:
  enum {
  // A big enough number for C2: all the registers plus flags
  // This number must be large enough to cover REG_COUNT (defined by c2) registers.
  // There is no requirement that any ordering here matches any ordering c2 gives
  // it's optoregs.

  // x86_32.ad defines additional dummy FILL0-FILL7 registers, in order to tally
  // REG_COUNT (computed by ADLC based on the number of reg_defs seen in .ad files)
  // with ConcreteRegisterImpl::number_of_registers additional count of 8 is being
  // added for 32 bit jvm.
    number_of_registers = RegisterImpl::number_of_registers * RegisterImpl::max_slots_per_register +
      2 * FloatRegisterImpl::number_of_registers + NOT_LP64(8) LP64_ONLY(0) +
      XMMRegisterImpl::max_slots_per_register * XMMRegisterImpl::number_of_registers +
      KRegisterImpl::number_of_registers * KRegisterImpl::max_slots_per_register + // mask registers
      1 // eflags
  };

  static const int max_gpr;
  static const int max_fpr;
  static const int max_xmm;
  static const int max_kpr;

};

template <>
inline Register AbstractRegSet<Register>::first() {
  uint32_t first = _bitset & -_bitset;
  return first ? as_Register(exact_log2(first)) : noreg;
}

template <>
inline Register AbstractRegSet<Register>::last() {
  if (_bitset == 0) { return noreg; }
  uint32_t last = 31 - count_leading_zeros(_bitset);
  return as_Register(last);
}

template <>
inline XMMRegister AbstractRegSet<XMMRegister>::first() {
  uint32_t first = _bitset & -_bitset;
  return first ? as_XMMRegister(exact_log2(first)) : xnoreg;
}

template <>
inline XMMRegister AbstractRegSet<XMMRegister>::last() {
  if (_bitset == 0) { return xnoreg; }
  uint32_t last = 31 - count_leading_zeros(_bitset);
  return as_XMMRegister(last);
}

typedef AbstractRegSet<Register> RegSet;
typedef AbstractRegSet<XMMRegister> XMMRegSet;

#endif // CPU_X86_REGISTER_X86_HPP


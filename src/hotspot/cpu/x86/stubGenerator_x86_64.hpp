/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#include "runtime/continuation.hpp"
#include "runtime/stubCodeGenerator.hpp"

// Stub Code definitions

class StubGenerator: public StubCodeGenerator {
 private:

  // Call stubs are used to call Java from C.
  address generate_call_stub(address& return_address);

  // Return point for a Java call if there's an exception thrown in
  // Java code.  The exception is caught and transformed into a
  // pending exception stored in JavaThread that can be tested from
  // within the VM.
  //
  // Note: Usually the parameters are removed by the callee. In case
  // of an exception crossing an activation frame boundary, that is
  // not the case if the callee is compiled code => need to setup the
  // rsp.
  //
  // rax: exception oop

  address generate_catch_exception();

  // Continuation point for runtime calls returning with a pending
  // exception.  The pending exception check happened in the runtime
  // or native call stub.  The pending exception in Thread is
  // converted into a Java-level exception.
  //
  // Contract with Java-level exception handlers:
  // rax: exception
  // rdx: throwing pc
  //
  // NOTE: At entry of this stub, exception-pc must be on stack !!

  address generate_forward_exception();

  // Support for intptr_t OrderAccess::fence()
  address generate_orderaccess_fence();

  // Support for intptr_t get_previous_sp()
  //
  // This routine is used to find the previous stack pointer for the
  // caller.
  address generate_get_previous_sp();

  //----------------------------------------------------------------------------------------------------
  // Support for void verify_mxcsr()
  //
  // This routine is used with -Xcheck:jni to verify that native
  // JNI code does not return to Java code without restoring the
  // MXCSR register to our expected state.

  address generate_verify_mxcsr();

  address generate_f2i_fixup();
  address generate_f2l_fixup();
  address generate_d2i_fixup();
  address generate_d2l_fixup();

  address generate_count_leading_zeros_lut(const char *stub_name);
  address generate_popcount_avx_lut(const char *stub_name);
  address generate_iota_indices(const char *stub_name);
  address generate_vector_reverse_bit_lut(const char *stub_name);

  address generate_vector_reverse_byte_perm_mask_long(const char *stub_name);
  address generate_vector_reverse_byte_perm_mask_int(const char *stub_name);
  address generate_vector_reverse_byte_perm_mask_short(const char *stub_name);
  address generate_vector_byte_shuffle_mask(const char *stub_name);

  address generate_fp_mask(const char *stub_name, int64_t mask);

  address generate_vector_mask(const char *stub_name, int64_t mask);

  address generate_vector_byte_perm_mask(const char *stub_name);

  address generate_vector_fp_mask(const char *stub_name, int64_t mask);

  address generate_vector_custom_i32(const char *stub_name, Assembler::AvxVectorLen len,
                                     int32_t val0, int32_t val1, int32_t val2, int32_t val3,
                                     int32_t val4 = 0, int32_t val5 = 0, int32_t val6 = 0, int32_t val7 = 0,
                                     int32_t val8 = 0, int32_t val9 = 0, int32_t val10 = 0, int32_t val11 = 0,
                                     int32_t val12 = 0, int32_t val13 = 0, int32_t val14 = 0, int32_t val15 = 0);

  // Non-destructive plausibility checks for oops
  //
  // Arguments:
  //    all args on stack!
  //
  // Stack after saving c_rarg3:
  //    [tos + 0]: saved c_rarg3
  //    [tos + 1]: saved c_rarg2
  //    [tos + 2]: saved r12 (several TemplateTable methods use it)
  //    [tos + 3]: saved flags
  //    [tos + 4]: return address
  //  * [tos + 5]: error message (char*)
  //  * [tos + 6]: object to verify (oop)
  //  * [tos + 7]: saved rax - saved by caller and bashed
  //  * [tos + 8]: saved r10 (rscratch1) - saved by caller
  //  * = popped on exit
  address generate_verify_oop();

  // Verify that a register contains clean 32-bits positive value
  // (high 32-bits are 0) so it could be used in 64-bits shifts.
  //
  //  Input:
  //    Rint  -  32-bits value
  //    Rtmp  -  scratch
  //
  void assert_clean_int(Register Rint, Register Rtmp);

  //  Generate overlap test for array copy stubs
  //
  //  Input:
  //     c_rarg0 - from
  //     c_rarg1 - to
  //     c_rarg2 - element count
  //
  //  Output:
  //     rax   - &from[element count - 1]
  //
  void array_overlap_test(address no_overlap_target, Label* NOLp, Address::ScaleFactor sf);

  void array_overlap_test(address no_overlap_target, Address::ScaleFactor sf) {
    assert(no_overlap_target != NULL, "must be generated");
    array_overlap_test(no_overlap_target, NULL, sf);
  }
  void array_overlap_test(Label& L_no_overlap, Address::ScaleFactor sf) {
    array_overlap_test(NULL, &L_no_overlap, sf);
  }

  // Shuffle first three arg regs on Windows into Linux/Solaris locations.
  //
  // Outputs:
  //    rdi - rcx
  //    rsi - rdx
  //    rdx - r8
  //    rcx - r9
  //
  // Registers r9 and r10 are used to save rdi and rsi on Windows, which latter
  // are non-volatile.  r9 and r10 should not be used by the caller.
#ifdef ASSERT
  bool _regs_in_thread;
#endif

  void setup_arg_regs(int nargs = 3);

  void restore_arg_regs();

  // This is used in places where r10 is a scratch register, and can
  // be adapted if r9 is needed also.
  void setup_arg_regs_using_thread();

  void restore_arg_regs_using_thread();

  // Copy big chunks forward
  //
  // Inputs:
  //   end_from     - source arrays end address
  //   end_to       - destination array end address
  //   qword_count  - 64-bits element count, negative
  //   to           - scratch
  //   L_copy_bytes - entry label
  //   L_copy_8_bytes  - exit  label
  //
  void copy_bytes_forward(Register end_from, Register end_to,
                          Register qword_count, Register to,
                          Label& L_copy_bytes, Label& L_copy_8_bytes);

  // Copy big chunks backward
  //
  // Inputs:
  //   from         - source arrays address
  //   dest         - destination array address
  //   qword_count  - 64-bits element count
  //   to           - scratch
  //   L_copy_bytes - entry label
  //   L_copy_8_bytes  - exit  label
  //
  void copy_bytes_backward(Register from, Register dest,
                           Register qword_count, Register to,
                           Label& L_copy_bytes, Label& L_copy_8_bytes);

  void setup_argument_regs(BasicType type);

  void restore_argument_regs(BasicType type);

#if COMPILER2_OR_JVMCI
  // Note: Following rules apply to AVX3 optimized arraycopy stubs:-
  // - If target supports AVX3 features (BW+VL+F) then implementation uses 32 byte vectors (YMMs)
  //   for both special cases (various small block sizes) and aligned copy loop. This is the
  //   default configuration.
  // - If copy length is above AVX3Threshold, then implementation use 64 byte vectors (ZMMs)
  //   for main copy loop (and subsequent tail) since bulk of the cycles will be consumed in it.
  // - If user forces MaxVectorSize=32 then above 4096 bytes its seen that REP MOVs shows a
  //   better performance for disjoint copies. For conjoint/backward copy vector based
  //   copy performs better.
  // - If user sets AVX3Threshold=0, then special cases for small blocks sizes operate over
  //   64 byte vector registers (ZMMs).

  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  //
  // Side Effects:
  //   disjoint_copy_avx3_masked is set to the no-overlap entry point
  //   used by generate_conjoint_[byte/int/short/long]_copy().
  //

  address generate_disjoint_copy_avx3_masked(address* entry, const char *name, int shift,
                                             bool aligned, bool is_oop, bool dest_uninitialized);

  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  //
  address generate_conjoint_copy_avx3_masked(address* entry, const char *name, int shift,
                                             address nooverlap_target, bool aligned, bool is_oop,
                                             bool dest_uninitialized);

#endif // COMPILER2_OR_JVMCI

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-, 2-, or 1-byte boundaries,
  // we let the hardware handle it.  The one to eight bytes within words,
  // dwords or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  // Side Effects:
  //   disjoint_byte_copy_entry is set to the no-overlap entry point
  //   used by generate_conjoint_byte_copy().
  //
  address generate_disjoint_byte_copy(bool aligned, address* entry, const char *name);

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-, 2-, or 1-byte boundaries,
  // we let the hardware handle it.  The one to eight bytes within words,
  // dwords or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  address generate_conjoint_byte_copy(bool aligned, address nooverlap_target,
                                      address* entry, const char *name);

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4- or 2-byte boundaries, we
  // let the hardware handle it.  The two or four words within dwords
  // or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  // Side Effects:
  //   disjoint_short_copy_entry is set to the no-overlap entry point
  //   used by generate_conjoint_short_copy().
  //
  address generate_disjoint_short_copy(bool aligned, address *entry, const char *name);

  address generate_fill(BasicType t, bool aligned, const char *name);

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4- or 2-byte boundaries, we
  // let the hardware handle it.  The two or four words within dwords
  // or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  address generate_conjoint_short_copy(bool aligned, address nooverlap_target,
                                       address *entry, const char *name);

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   is_oop  - true => oop array, so generate store check code
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-byte boundaries, we let
  // the hardware handle it.  The two dwords within qwords that span
  // cache line boundaries will still be loaded and stored atomically.
  //
  // Side Effects:
  //   disjoint_int_copy_entry is set to the no-overlap entry point
  //   used by generate_conjoint_int_oop_copy().
  //
  address generate_disjoint_int_oop_copy(bool aligned, bool is_oop, address* entry,
                                         const char *name, bool dest_uninitialized = false);

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   is_oop  - true => oop array, so generate store check code
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-byte boundaries, we let
  // the hardware handle it.  The two dwords within qwords that span
  // cache line boundaries will still be loaded and stored atomically.
  //
  address generate_conjoint_int_oop_copy(bool aligned, bool is_oop, address nooverlap_target,
                                         address *entry, const char *name,
                                         bool dest_uninitialized = false);

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord boundary == 8 bytes
  //             ignored
  //   is_oop  - true => oop array, so generate store check code
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
 // Side Effects:
  //   disjoint_oop_copy_entry or disjoint_long_copy_entry is set to the
  //   no-overlap entry point used by generate_conjoint_long_oop_copy().
  //
  address generate_disjoint_long_oop_copy(bool aligned, bool is_oop, address *entry,
                                          const char *name, bool dest_uninitialized = false);

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord boundary == 8 bytes
  //             ignored
  //   is_oop  - true => oop array, so generate store check code
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  address generate_conjoint_long_oop_copy(bool aligned, bool is_oop,
                                          address nooverlap_target, address *entry,
                                          const char *name, bool dest_uninitialized = false);

  // Helper for generating a dynamic type check.
  // Smashes no registers.
  void generate_type_check(Register sub_klass,
                           Register super_check_offset,
                           Register super_klass,
                           Label& L_success);

  //
  //  Generate checkcasting array copy stub
  //
  //  Input:
  //    c_rarg0   - source array address
  //    c_rarg1   - destination array address
  //    c_rarg2   - element count, treated as ssize_t, can be zero
  //    c_rarg3   - size_t ckoff (super_check_offset)
  // not Win64
  //    c_rarg4   - oop ckval (super_klass)
  // Win64
  //    rsp+40    - oop ckval (super_klass)
  //
  //  Output:
  //    rax ==  0  -  success
  //    rax == -1^K - failure, where K is partial transfer count
  //
  address generate_checkcast_copy(const char *name, address *entry,
                                  bool dest_uninitialized = false);

  //
  //  Generate 'unsafe' array copy stub
  //  Though just as safe as the other stubs, it takes an unscaled
  //  size_t argument instead of an element count.
  //
  //  Input:
  //    c_rarg0   - source array address
  //    c_rarg1   - destination array address
  //    c_rarg2   - byte count, treated as ssize_t, can be zero
  //
  // Examines the alignment of the operands and dispatches
  // to a long, int, short, or byte copy loop.
  //
  address generate_unsafe_copy(const char *name,
                               address byte_copy_entry, address short_copy_entry,
                               address int_copy_entry, address long_copy_entry);

  // Perform range checks on the proposed arraycopy.
  // Kills temp, but nothing else.
  // Also, clean the sign bits of src_pos and dst_pos.
  void arraycopy_range_checks(Register src,     // source array oop (c_rarg0)
                              Register src_pos, // source position (c_rarg1)
                              Register dst,     // destination array oo (c_rarg2)
                              Register dst_pos, // destination position (c_rarg3)
                              Register length,
                              Register temp,
                              Label& L_failed);

  //
  //  Generate generic array copy stubs
  //
  //  Input:
  //    c_rarg0    -  src oop
  //    c_rarg1    -  src_pos (32-bits)
  //    c_rarg2    -  dst oop
  //    c_rarg3    -  dst_pos (32-bits)
  // not Win64
  //    c_rarg4    -  element count (32-bits)
  // Win64
  //    rsp+40     -  element count (32-bits)
  //
  //  Output:
  //    rax ==  0  -  success
  //    rax == -1^K - failure, where K is partial transfer count
  //
  address generate_generic_copy(const char *name,
                                address byte_copy_entry, address short_copy_entry,
                                address int_copy_entry, address oop_copy_entry,
                                address long_copy_entry, address checkcast_copy_entry);

  address generate_data_cache_writeback();

  address generate_data_cache_writeback_sync();

  void generate_arraycopy_stubs();


  // MD5 stubs

  // ofs and limit are use for multi-block byte array.
  // int com.sun.security.provider.MD5.implCompress(byte[] b, int ofs)
  address generate_md5_implCompress(bool multi_block, const char *name);


  // SHA stubs

  // ofs and limit are use for multi-block byte array.
  // int com.sun.security.provider.DigestBase.implCompressMultiBlock(byte[] b, int ofs, int limit)
  address generate_sha1_implCompress(bool multi_block, const char *name);

  // ofs and limit are use for multi-block byte array.
  // int com.sun.security.provider.DigestBase.implCompressMultiBlock(byte[] b, int ofs, int limit)
  address generate_sha256_implCompress(bool multi_block, const char *name);
  address generate_sha512_implCompress(bool multi_block, const char *name);

  // Mask for byte-swapping a couple of qwords in an XMM register using (v)pshufb.
  address generate_pshuffle_byte_flip_mask_sha512();

  address generate_upper_word_mask();
  address generate_shuffle_byte_flip_mask();
  address generate_pshuffle_byte_flip_mask();


  // AES intrinsic stubs

  enum {
    AESBlockSize = 16
  };

  // Arguments:
  //
  // Inputs:
  //   c_rarg0   - source byte array address
  //   c_rarg1   - destination byte array address
  //   c_rarg2   - K (key) in little endian int array
  //
  address generate_aescrypt_encryptBlock();

  // Arguments:
  //
  // Inputs:
  //   c_rarg0   - source byte array address
  //   c_rarg1   - destination byte array address
  //   c_rarg2   - K (key) in little endian int array
  //
  address generate_aescrypt_decryptBlock();

  // Arguments:
  //
  // Inputs:
  //   c_rarg0   - source byte array address
  //   c_rarg1   - destination byte array address
  //   c_rarg2   - K (key) in little endian int array
  //   c_rarg3   - r vector byte array address
  //   c_rarg4   - input length
  //
  // Output:
  //   rax       - input length
  //
  address generate_cipherBlockChaining_encryptAESCrypt();

  // This is a version of CBC/AES Decrypt which does 4 blocks in a loop at a time
  // to hide instruction latency
  //
  // Arguments:
  //
  // Inputs:
  //   c_rarg0   - source byte array address
  //   c_rarg1   - destination byte array address
  //   c_rarg2   - K (key) in little endian int array
  //   c_rarg3   - r vector byte array address
  //   c_rarg4   - input length
  //
  // Output:
  //   rax       - input length
  //
  address generate_cipherBlockChaining_decryptAESCrypt_Parallel();

  address generate_electronicCodeBook_encryptAESCrypt();

  void aesecb_encrypt(Register source_addr, Register dest_addr, Register key, Register len);

  address generate_electronicCodeBook_decryptAESCrypt();

  void aesecb_decrypt(Register source_addr, Register dest_addr, Register key, Register len);

  // Vector AES Galois Counter Mode implementation. Parameters:
  // Windows regs            |  Linux regs
  // in = c_rarg0 (rcx)      |  c_rarg0 (rsi)
  // len = c_rarg1 (rdx)     |  c_rarg1 (rdi)
  // ct = c_rarg2 (r8)       |  c_rarg2 (rdx)
  // out = c_rarg3 (r9)      |  c_rarg3 (rcx)
  // key = r10               |  c_rarg4 (r8)
  // state = r13             |  c_rarg5 (r9)
  // subkeyHtbl = r14        |  r11
  // counter = rsi           |  r12
  // return - number of processed bytes
  address generate_galoisCounterMode_AESCrypt();
  void aesgcm_encrypt(Register in, Register len, Register ct, Register out, Register key,
                      Register state, Register subkeyHtbl, Register avx512_subkeyHtbl, Register counter);


 // Vector AES Counter implementation
  address generate_counterMode_VectorAESCrypt();
  void aesctr_encrypt(Register src_addr, Register dest_addr, Register key, Register counter,
                      Register len_reg, Register used, Register used_addr, Register saved_encCounter_start);

  // This is a version of CTR/AES crypt which does 6 blocks in a loop at a time
  // to hide instruction latency
  //
  // Arguments:
  //
  // Inputs:
  //   c_rarg0   - source byte array address
  //   c_rarg1   - destination byte array address
  //   c_rarg2   - K (key) in little endian int array
  //   c_rarg3   - counter vector byte array address
  //   Linux
  //     c_rarg4   -          input length
  //     c_rarg5   -          saved encryptedCounter start
  //     rbp + 6 * wordSize - saved used length
  //   Windows
  //     rbp + 6 * wordSize - input length
  //     rbp + 7 * wordSize - saved encryptedCounter start
  //     rbp + 8 * wordSize - saved used length
  //
  // Output:
  //   rax       - input length
  //
  address generate_counterMode_AESCrypt_Parallel();

  address generate_cipherBlockChaining_decryptVectorAESCrypt();

  address generate_key_shuffle_mask();

  address generate_counter_shuffle_mask();

  // This mask is used for incrementing counter value(linc0, linc4, etc.)
  address generate_counter_mask_addr();

  address generate_ghash_polynomial512_addr();

  void roundDec(XMMRegister xmm_reg);
  void roundDeclast(XMMRegister xmm_reg);
  void roundEnc(XMMRegister key, int rnum);
  void lastroundEnc(XMMRegister key, int rnum);
  void roundDec(XMMRegister key, int rnum);
  void lastroundDec(XMMRegister key, int rnum);
  void gfmul_avx512(XMMRegister ghash, XMMRegister hkey);
  void generateHtbl_48_block_zmm(Register htbl, Register avx512_subkeyHtbl);
  void ghash16_encrypt16_parallel(Register key, Register subkeyHtbl, XMMRegister ctr_blockx,
                                  XMMRegister aad_hashx, Register in, Register out, Register data, Register pos, bool reduction,
                                  XMMRegister addmask, bool no_ghash_input, Register rounds, Register ghash_pos,
                                  bool final_reduction, int index, XMMRegister counter_inc_mask);
  // Load key and shuffle operation
  void ev_load_key(XMMRegister xmmdst, Register key, int offset, XMMRegister xmm_shuf_mask = xnoreg);

  // Utility routine for loading a 128-bit key word in little endian format
  // can optionally specify that the shuffle mask is already in an xmmregister
  void load_key(XMMRegister xmmdst, Register key, int offset, XMMRegister xmm_shuf_mask = xnoreg);

  // Utility routine for increase 128bit counter (iv in CTR mode)
  void inc_counter(Register reg, XMMRegister xmmdst, int inc_delta, Label& next_block);

  void generate_aes_stubs();


  // GHASH stubs

  void generate_ghash_stubs();

  void schoolbookAAD(int i, Register subkeyH, XMMRegister data, XMMRegister tmp0,
                     XMMRegister tmp1, XMMRegister tmp2, XMMRegister tmp3);
  void gfmul(XMMRegister tmp0, XMMRegister t);
  void generateHtbl_one_block(Register htbl);
  void generateHtbl_eight_blocks(Register htbl);
  void avx_ghash(Register state, Register htbl, Register data, Register blocks);

  address generate_ghash_long_swap_mask(); // byte swap x86 long

  address generate_ghash_byte_swap_mask(); // byte swap x86 byte array

  // Single and multi-block ghash operations
  address generate_ghash_processBlocks();

  // Ghash single and multi block operations using AVX instructions
  address generate_avx_ghash_processBlocks();


  address base64_shuffle_addr();
  address base64_avx2_shuffle_addr();
  address base64_avx2_input_mask_addr();
  address base64_avx2_lut_addr();
  address base64_encoding_table_addr();

  // Code for generating Base64 encoding.
  // Intrinsic function prototype in Base64.java:
  // private void encodeBlock(byte[] src, int sp, int sl, byte[] dst, int dp, boolean isURL)
  address generate_base64_encodeBlock();

  // base64 AVX512vbmi tables
  address base64_vbmi_lookup_lo_addr();
  address base64_vbmi_lookup_hi_addr();
  address base64_vbmi_lookup_lo_url_addr();
  address base64_vbmi_lookup_hi_url_addr();
  address base64_vbmi_pack_vec_addr();
  address base64_vbmi_join_0_1_addr();
  address base64_vbmi_join_1_2_addr();
  address base64_vbmi_join_2_3_addr();
  address base64_decoding_table_addr();

  // Code for generating Base64 decoding.
  //
  // Based on the article (and associated code) from https://arxiv.org/abs/1910.05109.
  //
  // Intrinsic function prototype in Base64.java:
  // private void decodeBlock(byte[] src, int sp, int sl, byte[] dst, int dp, boolean isURL, isMIME);
  address generate_base64_decodeBlock();

  /**
   *  Arguments:
   *
   * Inputs:
   *   c_rarg0   - int crc
   *   c_rarg1   - byte* buf
   *   c_rarg2   - int length
   *
   * Output:
   *       rax   - int crc result
   */
  address generate_updateBytesCRC32();

  /**
  *  Arguments:
  *
  * Inputs:
  *   c_rarg0   - int crc
  *   c_rarg1   - byte* buf
  *   c_rarg2   - long length
  *   c_rarg3   - table_start - optional (present only when doing a library_call,
  *              not used by x86 algorithm)
  *
  * Output:
  *       rax   - int crc result
  */
  address generate_updateBytesCRC32C(bool is_pclmulqdq_supported);

  /***
   *  Arguments:
   *
   *  Inputs:
   *   c_rarg0   - int   adler
   *   c_rarg1   - byte* buff
   *   c_rarg2   - int   len
   *
   * Output:
   *   rax   - int adler result
   */

  address generate_updateBytesAdler32();

  /**
   *  Arguments:
   *
   *  Input:
   *    c_rarg0   - x address
   *    c_rarg1   - x length
   *    c_rarg2   - y address
   *    c_rarg3   - y length
   * not Win64
   *    c_rarg4   - z address
   *    c_rarg5   - z length
   * Win64
   *    rsp+40    - z address
   *    rsp+48    - z length
   */
  address generate_multiplyToLen();

  /**
  *  Arguments:
  *
  *  Input:
  *    c_rarg0   - obja     address
  *    c_rarg1   - objb     address
  *    c_rarg3   - length   length
  *    c_rarg4   - scale    log2_array_indxscale
  *
  *  Output:
  *        rax   - int >= mismatched index, < 0 bitwise complement of tail
  */
  address generate_vectorizedMismatch();

/**
   *  Arguments:
   *
  //  Input:
  //    c_rarg0   - x address
  //    c_rarg1   - x length
  //    c_rarg2   - z address
  //    c_rarg3   - z length
   *
   */
  address generate_squareToLen();

  address generate_method_entry_barrier();

   /**
   *  Arguments:
   *
   *  Input:
   *    c_rarg0   - out address
   *    c_rarg1   - in address
   *    c_rarg2   - offset
   *    c_rarg3   - len
   * not Win64
   *    c_rarg4   - k
   * Win64
   *    rsp+40    - k
   */
  address generate_mulAdd();

  address generate_bigIntegerRightShift();

   /**
   *  Arguments:
   *
   *  Input:
   *    c_rarg0   - newArr address
   *    c_rarg1   - oldArr address
   *    c_rarg2   - newIdx
   *    c_rarg3   - shiftCount
   * not Win64
   *    c_rarg4   - numIter
   * Win64
   *    rsp40    - numIter
   */
  address generate_bigIntegerLeftShift();

  address generate_libmExp();
  address generate_libmLog();
  address generate_libmLog10();
  address generate_libmPow();
  address generate_libmSin();
  address generate_libmCos();
  address generate_libmTan();

  RuntimeStub* generate_cont_doYield();

  address generate_cont_thaw(const char* label, Continuation::thaw_kind kind);
  address generate_cont_thaw();

  // TODO: will probably need multiple return barriers depending on return type
  address generate_cont_returnBarrier();
  address generate_cont_returnBarrier_exception();

#if INCLUDE_JFR

  // For c2: c_rarg0 is junk, call to runtime to write a checkpoint.
  // It returns a jobject handle to the event writer.
  // The handle is dereferenced and the return value is the event writer oop.
  RuntimeStub* generate_jfr_write_checkpoint();

#endif // INCLUDE_JFR

  // Continuation point for throwing of implicit exceptions that are
  // not handled in the current activation. Fabricates an exception
  // oop and initiates normal exception dispatching in this
  // frame. Since we need to preserve callee-saved values (currently
  // only for C2, but done for C1 as well) we need a callee-saved oop
  // map and therefore have to make these stubs into RuntimeStubs
  // rather than BufferBlobs.  If the compiler needs all registers to
  // be preserved between the fault point and the exception handler
  // then it must assume responsibility for that in
  // AbstractCompiler::continuation_for_implicit_null_exception or
  // continuation_for_implicit_division_by_zero_exception. All other
  // implicit exceptions (e.g., NullPointerException or
  // AbstractMethodError on entry) are either at call sites or
  // otherwise assume that stack unwinding will be initiated, so
  // caller saved registers were assumed volatile in the compiler.
  address generate_throw_exception(const char* name,
                                   address runtime_entry,
                                   Register arg1 = noreg,
                                   Register arg2 = noreg);

  void create_control_words();

  // Initialization
  void generate_initial();
  void generate_phase1();
  void generate_all();

 public:
  StubGenerator(CodeBuffer* code, int phase) : StubCodeGenerator(code) {
    DEBUG_ONLY( _regs_in_thread = false; )
    if (phase == 0) {
      generate_initial();
    } else if (phase == 1) {
      generate_phase1(); // stubs that must be available for the interpreter
    } else {
      generate_all();
    }
  }
};

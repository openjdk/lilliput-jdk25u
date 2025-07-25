/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "asm/assembler.hpp"
#include "asm/assembler.inline.hpp"
#include "asm/codeBuffer.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/os.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/vm_version.hpp"
#include "utilities/checkedCast.hpp"
#include "utilities/macros.hpp"

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#define STOP(error) stop(error)
#else
#define BLOCK_COMMENT(str) block_comment(str)
#define STOP(error) block_comment(error); stop(error)
#endif

#define BIND(label) bind(label); BLOCK_COMMENT(#label ":")
// Implementation of AddressLiteral

// A 2-D table for managing compressed displacement(disp8) on EVEX enabled platforms.
static const unsigned char tuple_table[Assembler::EVEX_ETUP + 1][Assembler::AVX_512bit + 1] = {
  // -----------------Table 4.5 -------------------- //
  16, 32, 64,  // EVEX_FV(0)
  4,  4,  4,   // EVEX_FV(1) - with Evex.b
  16, 32, 64,  // EVEX_FV(2) - with Evex.w
  8,  8,  8,   // EVEX_FV(3) - with Evex.w and Evex.b
  8,  16, 32,  // EVEX_HV(0)
  4,  4,  4,   // EVEX_HV(1) - with Evex.b
  // -----------------Table 4.6 -------------------- //
  16, 32, 64,  // EVEX_FVM(0)
  1,  1,  1,   // EVEX_T1S(0)
  2,  2,  2,   // EVEX_T1S(1)
  4,  4,  4,   // EVEX_T1S(2)
  8,  8,  8,   // EVEX_T1S(3)
  4,  4,  4,   // EVEX_T1F(0)
  8,  8,  8,   // EVEX_T1F(1)
  8,  8,  8,   // EVEX_T2(0)
  0,  16, 16,  // EVEX_T2(1)
  0,  16, 16,  // EVEX_T4(0)
  0,  0,  32,  // EVEX_T4(1)
  0,  0,  32,  // EVEX_T8(0)
  8,  16, 32,  // EVEX_HVM(0)
  4,  8,  16,  // EVEX_QVM(0)
  2,  4,  8,   // EVEX_OVM(0)
  16, 16, 16,  // EVEX_M128(0)
  8,  32, 64,  // EVEX_DUP(0)
  1,   1,  1,  // EVEX_NOSCALE(0)
  0,  0,  0    // EVEX_ETUP
};

AddressLiteral::AddressLiteral(address target, relocInfo::relocType rtype) {
  _is_lval = false;
  _target = target;
  switch (rtype) {
  case relocInfo::oop_type:
  case relocInfo::metadata_type:
    // Oops are a special case. Normally they would be their own section
    // but in cases like icBuffer they are literals in the code stream that
    // we don't have a section for. We use none so that we get a literal address
    // which is always patchable.
    break;
  case relocInfo::external_word_type:
    _rspec = external_word_Relocation::spec(target);
    break;
  case relocInfo::internal_word_type:
    _rspec = internal_word_Relocation::spec(target);
    break;
  case relocInfo::opt_virtual_call_type:
    _rspec = opt_virtual_call_Relocation::spec();
    break;
  case relocInfo::static_call_type:
    _rspec = static_call_Relocation::spec();
    break;
  case relocInfo::runtime_call_type:
    _rspec = runtime_call_Relocation::spec();
    break;
  case relocInfo::poll_type:
  case relocInfo::poll_return_type:
    _rspec = Relocation::spec_simple(rtype);
    break;
  case relocInfo::none:
    break;
  default:
    ShouldNotReachHere();
    break;
  }
}

// Implementation of Address

Address Address::make_array(ArrayAddress adr) {
  // Not implementable on 64bit machines
  // Should have been handled higher up the call chain.
  ShouldNotReachHere();
  return Address();
}

// exceedingly dangerous constructor
Address::Address(int disp, address loc, relocInfo::relocType rtype) {
  _base  = noreg;
  _index = noreg;
  _scale = no_scale;
  _disp  = disp;
  _xmmindex = xnoreg;
  _isxmmindex = false;
  switch (rtype) {
    case relocInfo::external_word_type:
      _rspec = external_word_Relocation::spec(loc);
      break;
    case relocInfo::internal_word_type:
      _rspec = internal_word_Relocation::spec(loc);
      break;
    case relocInfo::runtime_call_type:
      // HMM
      _rspec = runtime_call_Relocation::spec();
      break;
    case relocInfo::poll_type:
    case relocInfo::poll_return_type:
      _rspec = Relocation::spec_simple(rtype);
      break;
    case relocInfo::none:
      break;
    default:
      ShouldNotReachHere();
  }
}


// Convert the raw encoding form into the form expected by the constructor for
// Address.  An index of 4 (rsp) corresponds to having no index, so convert
// that to noreg for the Address constructor.
Address Address::make_raw(int base, int index, int scale, int disp, relocInfo::relocType disp_reloc) {
  RelocationHolder rspec = RelocationHolder::none;
  if (disp_reloc != relocInfo::none) {
    rspec = Relocation::spec_simple(disp_reloc);
  }
  bool valid_index = index != rsp->encoding();
  if (valid_index) {
    Address madr(as_Register(base), as_Register(index), (Address::ScaleFactor)scale, in_ByteSize(disp));
    madr._rspec = rspec;
    return madr;
  } else {
    Address madr(as_Register(base), noreg, Address::no_scale, in_ByteSize(disp));
    madr._rspec = rspec;
    return madr;
  }
}

// Implementation of Assembler

int AbstractAssembler::code_fill_byte() {
  return (u_char)'\xF4'; // hlt
}

void Assembler::init_attributes(void) {
  _legacy_mode_bw = (VM_Version::supports_avx512bw() == false);
  _legacy_mode_dq = (VM_Version::supports_avx512dq() == false);
  _legacy_mode_vl = (VM_Version::supports_avx512vl() == false);
  _legacy_mode_vlbw = (VM_Version::supports_avx512vlbw() == false);
  _attributes = nullptr;
}

void Assembler::set_attributes(InstructionAttr* attributes) {
  // Record the assembler in the attributes, so the attributes destructor can
  // clear the assembler's attributes, cleaning up the otherwise dangling
  // pointer.  gcc13 has a false positive warning, because it doesn't tie that
  // cleanup to the assignment of _attributes here.
  attributes->set_current_assembler(this);
  PRAGMA_DIAG_PUSH
  PRAGMA_DANGLING_POINTER_IGNORED
  _attributes = attributes;
  PRAGMA_DIAG_POP
}

void Assembler::membar(Membar_mask_bits order_constraint) {
  // We only have to handle StoreLoad
  if (order_constraint & StoreLoad) {
    // All usable chips support "locked" instructions which suffice
    // as barriers, and are much faster than the alternative of
    // using cpuid instruction. We use here a locked add [esp-C],0.
    // This is conveniently otherwise a no-op except for blowing
    // flags, and introducing a false dependency on target memory
    // location. We can't do anything with flags, but we can avoid
    // memory dependencies in the current method by locked-adding
    // somewhere else on the stack. Doing [esp+C] will collide with
    // something on stack in current method, hence we go for [esp-C].
    // It is convenient since it is almost always in data cache, for
    // any small C.  We need to step back from SP to avoid data
    // dependencies with other things on below SP (callee-saves, for
    // example). Without a clear way to figure out the minimal safe
    // distance from SP, it makes sense to step back the complete
    // cache line, as this will also avoid possible second-order effects
    // with locked ops against the cache line. Our choice of offset
    // is bounded by x86 operand encoding, which should stay within
    // [-128; +127] to have the 8-byte displacement encoding.
    //
    // Any change to this code may need to revisit other places in
    // the code where this idiom is used, in particular the
    // orderAccess code.

    int offset = -VM_Version::L1_line_size();
    if (offset < -128) {
      offset = -128;
    }

    lock();
    addl(Address(rsp, offset), 0);// Assert the lock# signal here
  }
}

// make this go away someday
void Assembler::emit_data(jint data, relocInfo::relocType rtype, int format) {
  if (rtype == relocInfo::none)
    emit_int32(data);
  else
    emit_data(data, Relocation::spec_simple(rtype), format);
}

void Assembler::emit_data(jint data, RelocationHolder const& rspec, int format) {
  assert(imm_operand == 0, "default format must be immediate in this file");
  assert(inst_mark() != nullptr, "must be inside InstructionMark");
  if (rspec.type() !=  relocInfo::none) {
    #ifdef ASSERT
      check_relocation(rspec, format);
    #endif
    // Do not use AbstractAssembler::relocate, which is not intended for
    // embedded words.  Instead, relocate to the enclosing instruction.

    // hack. call32 is too wide for mask so use disp32
    if (format == call32_operand)
      code_section()->relocate(inst_mark(), rspec, disp32_operand);
    else
      code_section()->relocate(inst_mark(), rspec, format);
  }
  emit_int32(data);
}

static int encode(Register r) {
  return r->encoding() & 7;
}

void Assembler::emit_arith_b(int op1, int op2, Register dst, int imm8) {
  assert(dst->has_byte_register(), "must have byte register");
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  assert(isByte(imm8), "not a byte");
  assert((op1 & 0x01) == 0, "should be 8bit operation");
  emit_int24(op1, (op2 | encode(dst)), imm8);
}

void Assembler::emit_arith(int op1, int op2, Register dst, int32_t imm32, bool optimize_rax_dst) {
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  assert(op1 == 0x81, "Unexpected opcode");
  if (is8bit(imm32)) {
    emit_int24(op1 | 0x02,        // set sign bit
               op2 | encode(dst),
               imm32 & 0xFF);
  } else if (optimize_rax_dst && dst == rax) {
    switch (op2) {
      case 0xD0: emit_int8(0x15); break; // adc
      case 0xC0: emit_int8(0x05); break; // add
      case 0xE0: emit_int8(0x25); break; // and
      case 0xF8: emit_int8(0x3D); break; // cmp
      case 0xC8: emit_int8(0x0D); break; // or
      case 0xD8: emit_int8(0x1D); break; // sbb
      case 0xE8: emit_int8(0x2D); break; // sub
      case 0xF0: emit_int8(0x35); break; // xor
      default: ShouldNotReachHere();
    }
    emit_int32(imm32);
  } else {
    emit_int16(op1, (op2 | encode(dst)));
    emit_int32(imm32);
  }
}

// Force generation of a 4 byte immediate value even if it fits into 8bit
void Assembler::emit_arith_imm32(int op1, int op2, Register dst, int32_t imm32) {
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  assert((op1 & 0x01) == 1, "should be 32bit operation");
  assert((op1 & 0x02) == 0, "sign-extension bit should not be set");
  emit_int16(op1, (op2 | encode(dst)));
  emit_int32(imm32);
}

// immediate-to-memory forms
void Assembler::emit_arith_operand(int op1, Register rm, Address adr, int32_t imm32) {
  assert((op1 & 0x01) == 1, "should be 32bit operation");
  assert((op1 & 0x02) == 0, "sign-extension bit should not be set");
  if (is8bit(imm32)) {
    emit_int8(op1 | 0x02); // set sign bit
    emit_operand(rm, adr, 1);
    emit_int8(imm32 & 0xFF);
  } else {
    emit_int8(op1);
    emit_operand(rm, adr, 4);
    emit_int32(imm32);
  }
}

void Assembler::emit_arith_operand_imm32(int op1, Register rm, Address adr, int32_t imm32) {
  assert(op1 == 0x81, "unexpected opcode");
  emit_int8(op1);
  emit_operand(rm, adr, 4);
  emit_int32(imm32);
}

void Assembler::emit_arith(int op1, int op2, Register dst, Register src) {
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  emit_int16(op1, (op2 | encode(dst) << 3 | encode(src)));
}


bool Assembler::query_compressed_disp_byte(int disp, bool is_evex_inst, int vector_len,
                                           int cur_tuple_type, int in_size_in_bits, int cur_encoding) {
  int mod_idx = 0;
  // We will test if the displacement fits the compressed format and if so
  // apply the compression to the displacement iff the result is8bit.
  if (VM_Version::supports_evex() && is_evex_inst) {
    switch (cur_tuple_type) {
    case EVEX_FV:
      if ((cur_encoding & VEX_W) == VEX_W) {
        mod_idx = ((cur_encoding & EVEX_Rb) == EVEX_Rb) ? 3 : 2;
      } else {
        mod_idx = ((cur_encoding & EVEX_Rb) == EVEX_Rb) ? 1 : 0;
      }
      break;

    case EVEX_HV:
      mod_idx = ((cur_encoding & EVEX_Rb) == EVEX_Rb) ? 1 : 0;
      break;

    case EVEX_FVM:
      break;

    case EVEX_T1S:
      switch (in_size_in_bits) {
      case EVEX_8bit:
        break;

      case EVEX_16bit:
        mod_idx = 1;
        break;

      case EVEX_32bit:
        mod_idx = 2;
        break;

      case EVEX_64bit:
        mod_idx = 3;
        break;
      }
      break;

    case EVEX_T1F:
    case EVEX_T2:
    case EVEX_T4:
      mod_idx = (in_size_in_bits == EVEX_64bit) ? 1 : 0;
      break;

    case EVEX_T8:
      break;

    case EVEX_HVM:
      break;

    case EVEX_QVM:
      break;

    case EVEX_OVM:
      break;

    case EVEX_M128:
      break;

    case EVEX_DUP:
      break;

    case EVEX_NOSCALE:
      break;

    default:
      assert(0, "no valid evex tuple_table entry");
      break;
    }

    if (vector_len >= AVX_128bit && vector_len <= AVX_512bit) {
      int disp_factor = tuple_table[cur_tuple_type + mod_idx][vector_len];
      if ((disp % disp_factor) == 0) {
        int new_disp = disp / disp_factor;
        if ((-0x80 <= new_disp && new_disp < 0x80)) {
          disp = new_disp;
        }
      } else {
        return false;
      }
    }
  }
  return (-0x80 <= disp && disp < 0x80);
}


bool Assembler::emit_compressed_disp_byte(int &disp) {
  int mod_idx = 0;
  // We will test if the displacement fits the compressed format and if so
  // apply the compression to the displacement iff the result is8bit.
  if (VM_Version::supports_evex() && _attributes && _attributes->is_evex_instruction()) {
    int evex_encoding = _attributes->get_evex_encoding();
    int tuple_type = _attributes->get_tuple_type();
    switch (tuple_type) {
    case EVEX_FV:
      if ((evex_encoding & VEX_W) == VEX_W) {
        mod_idx = ((evex_encoding & EVEX_Rb) == EVEX_Rb) ? 3 : 2;
      } else {
        mod_idx = ((evex_encoding & EVEX_Rb) == EVEX_Rb) ? 1 : 0;
      }
      break;

    case EVEX_HV:
      mod_idx = ((evex_encoding & EVEX_Rb) == EVEX_Rb) ? 1 : 0;
      break;

    case EVEX_FVM:
      break;

    case EVEX_T1S:
      switch (_attributes->get_input_size()) {
      case EVEX_8bit:
        break;

      case EVEX_16bit:
        mod_idx = 1;
        break;

      case EVEX_32bit:
        mod_idx = 2;
        break;

      case EVEX_64bit:
        mod_idx = 3;
        break;
      }
      break;

    case EVEX_T1F:
    case EVEX_T2:
    case EVEX_T4:
      mod_idx = (_attributes->get_input_size() == EVEX_64bit) ? 1 : 0;
      break;

    case EVEX_T8:
      break;

    case EVEX_HVM:
      break;

    case EVEX_QVM:
      break;

    case EVEX_OVM:
      break;

    case EVEX_M128:
      break;

    case EVEX_DUP:
      break;

    case EVEX_NOSCALE:
      break;

    default:
      assert(0, "no valid evex tuple_table entry");
      break;
    }

    int vector_len = _attributes->get_vector_len();
    if (vector_len >= AVX_128bit && vector_len <= AVX_512bit) {
      int disp_factor = tuple_table[tuple_type + mod_idx][vector_len];
      if ((disp % disp_factor) == 0) {
        int new_disp = disp / disp_factor;
        if (is8bit(new_disp)) {
          disp = new_disp;
        }
      } else {
        return false;
      }
    }
  }
  return is8bit(disp);
}

bool Assembler::needs_rex2(Register reg1, Register reg2, Register reg3) {
  bool rex2 = (reg1->is_valid() && reg1->encoding() >= 16) ||
              (reg2->is_valid() && reg2->encoding() >= 16) ||
              (reg3->is_valid() && reg3->encoding() >= 16);
  assert(!rex2 || UseAPX, "extended gpr use requires UseAPX");
  return rex2;
}

#ifndef PRODUCT
bool Assembler::needs_evex(XMMRegister reg1, XMMRegister reg2, XMMRegister reg3) {
  return (reg1->is_valid() && reg1->encoding() >= 16) ||
         (reg2->is_valid() && reg2->encoding() >= 16) ||
         (reg3->is_valid() && reg3->encoding() >= 16);
}
#endif

bool Assembler::needs_eevex(Register reg1, Register reg2, Register reg3) {
  return needs_rex2(reg1, reg2, reg3);
}

bool Assembler::needs_eevex(int enc1, int enc2, int enc3) {
  bool eevex = enc1 >= 16 || enc2 >= 16 || enc3 >=16;
  assert(!eevex || UseAPX, "extended gpr use requires UseAPX");
  return eevex;
}

static bool is_valid_encoding(int reg_enc) {
  return reg_enc >= 0;
}

static int raw_encode(Register reg) {
  assert(reg == noreg || reg->is_valid(), "sanity");
  int reg_enc = reg->raw_encoding();
  assert(reg_enc == -1 || is_valid_encoding(reg_enc), "sanity");
  return reg_enc;
}

static int raw_encode(XMMRegister xmmreg) {
  assert(xmmreg == xnoreg || xmmreg->is_valid(), "sanity");
  int xmmreg_enc = xmmreg->raw_encoding();
  assert(xmmreg_enc == -1 || is_valid_encoding(xmmreg_enc), "sanity");
  return xmmreg_enc;
}

static int raw_encode(KRegister kreg) {
  assert(kreg == knoreg || kreg->is_valid(), "sanity");
  int kreg_enc = kreg->raw_encoding();
  assert(kreg_enc == -1 || is_valid_encoding(kreg_enc), "sanity");
  return kreg_enc;
}

static int modrm_encoding(int mod, int dst_enc, int src_enc) {
  return (mod & 3) << 6 | (dst_enc & 7) << 3 | (src_enc & 7);
}

static int sib_encoding(Address::ScaleFactor scale, int index_enc, int base_enc) {
  return (scale & 3) << 6 | (index_enc & 7) << 3 | (base_enc & 7);
}

inline void Assembler::emit_modrm(int mod, int dst_enc, int src_enc) {
  assert((mod & 3) != 0b11, "forbidden");
  int modrm = modrm_encoding(mod, dst_enc, src_enc);
  emit_int8(modrm);
}

inline void Assembler::emit_modrm_disp8(int mod, int dst_enc, int src_enc,
                                        int disp) {
  int modrm = modrm_encoding(mod, dst_enc, src_enc);
  emit_int16(modrm, disp & 0xFF);
}

inline void Assembler::emit_modrm_sib(int mod, int dst_enc, int src_enc,
                                      Address::ScaleFactor scale, int index_enc, int base_enc) {
  int modrm = modrm_encoding(mod, dst_enc, src_enc);
  int sib = sib_encoding(scale, index_enc, base_enc);
  emit_int16(modrm, sib);
}

inline void Assembler::emit_modrm_sib_disp8(int mod, int dst_enc, int src_enc,
                                            Address::ScaleFactor scale, int index_enc, int base_enc,
                                            int disp) {
  int modrm = modrm_encoding(mod, dst_enc, src_enc);
  int sib = sib_encoding(scale, index_enc, base_enc);
  emit_int24(modrm, sib, disp & 0xFF);
}

void Assembler::emit_operand_helper(int reg_enc, int base_enc, int index_enc,
                                    Address::ScaleFactor scale, int disp,
                                    RelocationHolder const& rspec,
                                    int post_addr_length) {
  bool no_relocation = (rspec.type() == relocInfo::none);

  if (is_valid_encoding(base_enc)) {
    if (is_valid_encoding(index_enc)) {
      assert(scale != Address::no_scale, "inconsistent address");
      // [base + index*scale + disp]
      if (disp == 0 && no_relocation && ((base_enc & 0x7) != 5)) {
        // [base + index*scale]
        // !(rbp | r13 | r21 | r29)
        // [00 reg 100][ss index base]
        emit_modrm_sib(0b00, reg_enc, 0b100,
                       scale, index_enc, base_enc);
      } else if (emit_compressed_disp_byte(disp) && no_relocation) {
        // [base + index*scale + imm8]
        // [01 reg 100][ss index base] imm8
        emit_modrm_sib_disp8(0b01, reg_enc, 0b100,
                             scale, index_enc, base_enc,
                             disp);
      } else {
        // [base + index*scale + disp32]
        // [10 reg 100][ss index base] disp32
        emit_modrm_sib(0b10, reg_enc, 0b100,
                       scale, index_enc, base_enc);
        emit_data(disp, rspec, disp32_operand);
      }
    } else if ((base_enc & 0x7) == 4) {
      // rsp | r12 | r20 | r28
      // [rsp + disp]
      if (disp == 0 && no_relocation) {
        // [rsp]
        // [00 reg 100][00 100 100]
        emit_modrm_sib(0b00, reg_enc, 0b100,
                       Address::times_1, 0b100, 0b100);
      } else if (emit_compressed_disp_byte(disp) && no_relocation) {
        // [rsp + imm8]
        // [01 reg 100][00 100 100] disp8
        emit_modrm_sib_disp8(0b01, reg_enc, 0b100,
                             Address::times_1, 0b100, 0b100,
                             disp);
      } else {
        // [rsp + imm32]
        // [10 reg 100][00 100 100] disp32
        emit_modrm_sib(0b10, reg_enc, 0b100,
                       Address::times_1, 0b100, 0b100);
        emit_data(disp, rspec, disp32_operand);
      }
    } else {
      // [base + disp]
      // !(rsp | r12 | r20 | r28) were handled above
      assert(((base_enc & 0x7) != 4), "illegal addressing mode");
      if (disp == 0 && no_relocation &&  ((base_enc & 0x7) != 5)) {
        // [base]
        // !(rbp | r13 | r21 | r29)
        // [00 reg base]
        emit_modrm(0, reg_enc, base_enc);
      } else if (emit_compressed_disp_byte(disp) && no_relocation) {
        // [base + disp8]
        // [01 reg base] disp8
        emit_modrm_disp8(0b01, reg_enc, base_enc,
                         disp);
      } else {
        // [base + disp32]
        // [10 reg base] disp32
        emit_modrm(0b10, reg_enc, base_enc);
        emit_data(disp, rspec, disp32_operand);
      }
    }
  } else {
    if (is_valid_encoding(index_enc)) {
      assert(scale != Address::no_scale, "inconsistent address");
      // base == noreg
      // [index*scale + disp]
      // [00 reg 100][ss index 101] disp32
      emit_modrm_sib(0b00, reg_enc, 0b100,
                     scale, index_enc, 0b101 /* no base */);
      emit_data(disp, rspec, disp32_operand);
    } else if (!no_relocation) {
      // base == noreg, index == noreg
      // [disp] (64bit) RIP-RELATIVE (32bit) abs
      // [00 reg 101] disp32

      emit_modrm(0b00, reg_enc, 0b101 /* no base */);
      // Note that the RIP-rel. correction applies to the generated
      // disp field, but _not_ to the target address in the rspec.

      // disp was created by converting the target address minus the pc
      // at the start of the instruction. That needs more correction here.
      // intptr_t disp = target - next_ip;
      assert(inst_mark() != nullptr, "must be inside InstructionMark");
      address next_ip = pc() + sizeof(int32_t) + post_addr_length;
      int64_t adjusted = disp;
      // Do rip-rel adjustment
      adjusted -=  (next_ip - inst_mark());
      assert(is_simm32(adjusted),
             "must be 32bit offset (RIP relative address)");
      emit_data((int32_t) adjusted, rspec, disp32_operand);

    } else {
      // base == noreg, index == noreg, no_relocation == true
      // 32bit never did this, did everything as the rip-rel/disp code above
      // [disp] ABSOLUTE
      // [00 reg 100][00 100 101] disp32
      emit_modrm_sib(0b00, reg_enc, 0b100 /* no base */,
                     Address::times_1, 0b100, 0b101);
      emit_data(disp, rspec, disp32_operand);
    }
  }
}

void Assembler::emit_operand(Register reg, Register base, Register index,
                             Address::ScaleFactor scale, int disp,
                             RelocationHolder const& rspec,
                             int post_addr_length) {
  assert(!index->is_valid() || index != rsp, "illegal addressing mode");
  emit_operand_helper(raw_encode(reg), raw_encode(base), raw_encode(index),
                      scale, disp, rspec, post_addr_length);

}
void Assembler::emit_operand(XMMRegister xmmreg, Register base, Register index,
                             Address::ScaleFactor scale, int disp,
                             RelocationHolder const& rspec,
                             int post_addr_length) {
  assert(!index->is_valid() || index != rsp, "illegal addressing mode");
  assert(xmmreg->encoding() < 16 || UseAVX > 2, "not supported");
  emit_operand_helper(raw_encode(xmmreg), raw_encode(base), raw_encode(index),
                      scale, disp, rspec, post_addr_length);
}

void Assembler::emit_operand(XMMRegister xmmreg, Register base, XMMRegister xmmindex,
                             Address::ScaleFactor scale, int disp,
                             RelocationHolder const& rspec,
                             int post_addr_length) {
  assert(xmmreg->encoding() < 16 || UseAVX > 2, "not supported");
  assert(xmmindex->encoding() < 16 || UseAVX > 2, "not supported");
  emit_operand_helper(raw_encode(xmmreg), raw_encode(base), raw_encode(xmmindex),
                      scale, disp, rspec, post_addr_length);
}

void Assembler::emit_operand(KRegister kreg, Address adr,
                             int post_addr_length) {
  emit_operand(kreg, adr._base, adr._index, adr._scale, adr._disp,
               adr._rspec,
               post_addr_length);
}

void Assembler::emit_operand(KRegister kreg, Register base, Register index,
                             Address::ScaleFactor scale, int disp,
                             RelocationHolder const& rspec,
                             int post_addr_length) {
  assert(!index->is_valid() || index != rsp, "illegal addressing mode");
  emit_operand_helper(raw_encode(kreg), raw_encode(base), raw_encode(index),
                      scale, disp, rspec, post_addr_length);
}

// Secret local extension to Assembler::WhichOperand:
#define end_pc_operand (_WhichOperand_limit)

address Assembler::locate_operand(address inst, WhichOperand which) {
  // Decode the given instruction, and return the address of
  // an embedded 32-bit operand word.

  // If "which" is disp32_operand, selects the displacement portion
  // of an effective address specifier.
  // If "which" is imm64_operand, selects the trailing immediate constant.
  // If "which" is call32_operand, selects the displacement of a call or jump.
  // Caller is responsible for ensuring that there is such an operand,
  // and that it is 32/64 bits wide.

  // If "which" is end_pc_operand, find the end of the instruction.

  address ip = inst;
  bool is_64bit = false;

  DEBUG_ONLY(bool has_disp32 = false);
  int tail_size = 0; // other random bytes (#32, #16, etc.) at end of insn

  again_after_prefix:
  switch (0xFF & *ip++) {

  // These convenience macros generate groups of "case" labels for the switch.
#define REP4(x) (x)+0: case (x)+1: case (x)+2: case (x)+3
#define REP8(x) (x)+0: case (x)+1: case (x)+2: case (x)+3: \
             case (x)+4: case (x)+5: case (x)+6: case (x)+7
#define REP16(x) REP8((x)+0): \
              case REP8((x)+8)

  case CS_segment:
  case SS_segment:
  case DS_segment:
  case ES_segment:
  case FS_segment:
  case GS_segment:
    // Seems dubious
    assert(false, "shouldn't have that prefix");
    assert(ip == inst+1, "only one prefix allowed");
    goto again_after_prefix;

  case 0x67:
  case REX:
  case REX_B:
  case REX_X:
  case REX_XB:
  case REX_R:
  case REX_RB:
  case REX_RX:
  case REX_RXB:
    goto again_after_prefix;

  case REX2:
    if ((0xFF & *ip++) & REX2BIT_W) {
      is_64bit = true;
    }
    goto again_after_prefix;

  case REX_W:
  case REX_WB:
  case REX_WX:
  case REX_WXB:
  case REX_WR:
  case REX_WRB:
  case REX_WRX:
  case REX_WRXB:
    is_64bit = true;
    goto again_after_prefix;

  case 0xFF: // pushq a; decl a; incl a; call a; jmp a
  case 0x88: // movb a, r
  case 0x89: // movl a, r
  case 0x8A: // movb r, a
  case 0x8B: // movl r, a
  case 0x8F: // popl a
    DEBUG_ONLY(has_disp32 = true);
    break;

  case 0x68: // pushq #32
    if (which == end_pc_operand) {
      return ip + 4;
    }
    assert(which == imm_operand && !is_64bit, "pushl has no disp32 or 64bit immediate");
    return ip;                  // not produced by emit_operand

  case 0x66: // movw ... (size prefix)
    again_after_size_prefix2:
    switch (0xFF & *ip++) {
    case REX:
    case REX_B:
    case REX_X:
    case REX_XB:
    case REX_R:
    case REX_RB:
    case REX_RX:
    case REX_RXB:
    case REX_W:
    case REX_WB:
    case REX_WX:
    case REX_WXB:
    case REX_WR:
    case REX_WRB:
    case REX_WRX:
    case REX_WRXB:
      goto again_after_size_prefix2;

    case REX2:
      if ((0xFF & *ip++) & REX2BIT_W) {
        is_64bit = true;
      }
      goto again_after_size_prefix2;

    case 0x8B: // movw r, a
    case 0x89: // movw a, r
      DEBUG_ONLY(has_disp32 = true);
      break;
    case 0xC7: // movw a, #16
      DEBUG_ONLY(has_disp32 = true);
      tail_size = 2;  // the imm16
      break;
    case 0x0F: // several SSE/SSE2 variants
      ip--;    // reparse the 0x0F
      goto again_after_prefix;
    default:
      ShouldNotReachHere();
    }
    break;

  case REP8(0xB8): // movl/q r, #32/#64(oop?)
    if (which == end_pc_operand)  return ip + (is_64bit ? 8 : 4);
    // these asserts are somewhat nonsensical
    assert(((which == call32_operand || which == imm_operand) && is_64bit) ||
           (which == narrow_oop_operand && !is_64bit),
           "which %d is_64_bit %d ip " INTPTR_FORMAT, which, is_64bit, p2i(ip));
    return ip;

  case 0x69: // imul r, a, #32
  case 0xC7: // movl a, #32(oop?)
    tail_size = 4;
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    break;

  case 0x0F: // movx..., etc.
    switch (0xFF & *ip++) {
    case 0x3A: // pcmpestri
      tail_size = 1;
    case 0x38: // ptest, pmovzxbw
      ip++; // skip opcode
      DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
      break;

    case 0x70: // pshufd r, r/a, #8
      DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    case 0x73: // psrldq r, #8
      tail_size = 1;
      break;

    case 0x10: // movups
    case 0x11: // movups
    case 0x12: // movlps
    case 0x28: // movaps
    case 0x29: // movaps
    case 0x2E: // ucomiss
    case 0x2F: // comiss
    case 0x54: // andps
    case 0x55: // andnps
    case 0x56: // orps
    case 0x57: // xorps
    case 0x58: // addpd
    case 0x59: // mulpd
    case 0x6E: // movd
    case 0x7E: // movd
    case 0x6F: // movdq
    case 0x7F: // movdq
    case 0xAE: // ldmxcsr, stmxcsr, fxrstor, fxsave, clflush
    case 0xD6: // movq
    case 0xFE: // paddd
      DEBUG_ONLY(has_disp32 = true);
      break;

    case 0xAD: // shrd r, a, %cl
    case 0xAF: // imul r, a
    case 0xBE: // movsbl r, a (movsxb)
    case 0xBF: // movswl r, a (movsxw)
    case 0xB6: // movzbl r, a (movzxb)
    case 0xB7: // movzwl r, a (movzxw)
    case REP16(0x40): // cmovl cc, r, a
    case 0xB0: // cmpxchgb
    case 0xB1: // cmpxchg
    case 0xC1: // xaddl
    case 0xC7: // cmpxchg8
    case REP16(0x90): // setcc a
      DEBUG_ONLY(has_disp32 = true);
      // fall out of the switch to decode the address
      break;

    case 0xC4: // pinsrw r, a, #8
      DEBUG_ONLY(has_disp32 = true);
    case 0xC5: // pextrw r, r, #8
      tail_size = 1;  // the imm8
      break;

    case 0xAC: // shrd r, a, #8
      DEBUG_ONLY(has_disp32 = true);
      tail_size = 1;  // the imm8
      break;

    case REP16(0x80): // jcc rdisp32
      if (which == end_pc_operand)  return ip + 4;
      assert(which == call32_operand, "jcc has no disp32 or imm");
      return ip;
    default:
      fatal("not handled: 0x0F%2X", 0xFF & *(ip-1));
    }
    break;

  case 0x81: // addl a, #32; addl r, #32
    // also: orl, adcl, sbbl, andl, subl, xorl, cmpl
    // on 32bit in the case of cmpl, the imm might be an oop
    tail_size = 4;
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    break;

  case 0x83: // addl a, #8; addl r, #8
    // also: orl, adcl, sbbl, andl, subl, xorl, cmpl
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    tail_size = 1;
    break;

  case 0x15: // adc rax, #32
  case 0x05: // add rax, #32
  case 0x25: // and rax, #32
  case 0x3D: // cmp rax, #32
  case 0x0D: // or  rax, #32
  case 0x1D: // sbb rax, #32
  case 0x2D: // sub rax, #32
  case 0x35: // xor rax, #32
    return which == end_pc_operand ? ip + 4 : ip;

  case 0x9B:
    switch (0xFF & *ip++) {
    case 0xD9: // fnstcw a
      DEBUG_ONLY(has_disp32 = true);
      break;
    default:
      ShouldNotReachHere();
    }
    break;

  case REP4(0x00): // addb a, r; addl a, r; addb r, a; addl r, a
  case REP4(0x10): // adc...
  case REP4(0x20): // and...
  case REP4(0x30): // xor...
  case REP4(0x08): // or...
  case REP4(0x18): // sbb...
  case REP4(0x28): // sub...
  case 0xF7: // mull a
  case 0x8D: // lea r, a
  case 0x87: // xchg r, a
  case REP4(0x38): // cmp...
  case 0x85: // test r, a
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    break;

  case 0xA8: // testb rax, #8
    return which == end_pc_operand ? ip + 1 : ip;
  case 0xA9: // testl/testq rax, #32
    return which == end_pc_operand ? ip + 4 : ip;

  case 0xC1: // sal a, #8; sar a, #8; shl a, #8; shr a, #8
  case 0xC6: // movb a, #8
  case 0x80: // cmpb a, #8
  case 0x6B: // imul r, a, #8
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    tail_size = 1; // the imm8
    break;

  case 0xC4: // VEX_3bytes
  case 0xC5: // VEX_2bytes
    assert((UseAVX > 0), "shouldn't have VEX prefix");
    assert(ip == inst+1, "no prefixes allowed");
    // C4 and C5 are also used as opcodes for PINSRW and PEXTRW instructions
    // but they have prefix 0x0F and processed when 0x0F processed above.
    //
    // In 32-bit mode the VEX first byte C4 and C5 alias onto LDS and LES
    // instructions (these instructions are not supported in 64-bit mode).
    // To distinguish them bits [7:6] are set in the VEX second byte since
    // ModRM byte can not be of the form 11xxxxxx in 32-bit mode. To set
    // those VEX bits REX and vvvv bits are inverted.
    //
    // Fortunately C2 doesn't generate these instructions so we don't need
    // to check for them in product version.

    // Check second byte
    int vex_opcode;
    // First byte
    if ((0xFF & *inst) == VEX_3bytes) {
      vex_opcode = VEX_OPCODE_MASK & *ip;
      ip++; // third byte
      is_64bit = ((VEX_W & *ip) == VEX_W);
    } else {
      vex_opcode = VEX_OPCODE_0F;
    }
    ip++; // opcode
    // To find the end of instruction (which == end_pc_operand).
    switch (vex_opcode) {
      case VEX_OPCODE_0F:
        switch (0xFF & *ip) {
        case 0x70: // pshufd r, r/a, #8
        case 0x71: // ps[rl|ra|ll]w r, #8
        case 0x72: // ps[rl|ra|ll]d r, #8
        case 0x73: // ps[rl|ra|ll]q r, #8
        case 0xC2: // cmp[ps|pd|ss|sd] r, r, r/a, #8
        case 0xC4: // pinsrw r, r, r/a, #8
        case 0xC5: // pextrw r/a, r, #8
        case 0xC6: // shufp[s|d] r, r, r/a, #8
          tail_size = 1;  // the imm8
          break;
        }
        break;
      case VEX_OPCODE_0F_3A:
        tail_size = 1;
        break;
    }
    ip++; // skip opcode
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    break;

  case 0x62: // EVEX_4bytes
    assert(VM_Version::cpu_supports_evex(), "shouldn't have EVEX prefix");
    assert(ip == inst+1, "no prefixes allowed");
    // no EVEX collisions, all instructions that have 0x62 opcodes
    // have EVEX versions and are subopcodes of 0x66
    ip++; // skip P0 and examine W in P1
    is_64bit = ((VEX_W & *ip) == VEX_W);
    ip++; // move to P2
    ip++; // skip P2, move to opcode
    // To find the end of instruction (which == end_pc_operand).
    switch (0xFF & *ip) {
    case 0x22: // pinsrd r, r/a, #8
    case 0x61: // pcmpestri r, r/a, #8
    case 0x70: // pshufd r, r/a, #8
    case 0x73: // psrldq r, #8
    case 0x1f: // evpcmpd/evpcmpq
    case 0x3f: // evpcmpb/evpcmpw
      tail_size = 1;  // the imm8
      break;
    default:
      break;
    }
    ip++; // skip opcode
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    break;

  case 0xD1: // sal a, 1; sar a, 1; shl a, 1; shr a, 1
  case 0xD3: // sal a, %cl; sar a, %cl; shl a, %cl; shr a, %cl
  case 0xD9: // fld_s a; fst_s a; fstp_s a; fldcw a
  case 0xDD: // fld_d a; fst_d a; fstp_d a
  case 0xDB: // fild_s a; fistp_s a; fld_x a; fstp_x a
  case 0xDF: // fild_d a; fistp_d a
  case 0xD8: // fadd_s a; fsubr_s a; fmul_s a; fdivr_s a; fcomp_s a
  case 0xDC: // fadd_d a; fsubr_d a; fmul_d a; fdivr_d a; fcomp_d a
  case 0xDE: // faddp_d a; fsubrp_d a; fmulp_d a; fdivrp_d a; fcompp_d a
    DEBUG_ONLY(has_disp32 = true);
    break;

  case 0xE8: // call rdisp32
  case 0xE9: // jmp  rdisp32
    if (which == end_pc_operand)  return ip + 4;
    assert(which == call32_operand, "call has no disp32 or imm");
    return ip;

  case 0xF0:                    // Lock
    goto again_after_prefix;

  case 0xF3:                    // For SSE
  case 0xF2:                    // For SSE2
    switch (0xFF & *ip++) {
    case REX:
    case REX_B:
    case REX_X:
    case REX_XB:
    case REX_R:
    case REX_RB:
    case REX_RX:
    case REX_RXB:
    case REX_W:
    case REX_WB:
    case REX_WX:
    case REX_WXB:
    case REX_WR:
    case REX_WRB:
    case REX_WRX:
    case REX_WRXB:
    case REX2:
      ip++;
      // fall-through
    default:
      ip++;
    }
    DEBUG_ONLY(has_disp32 = true); // has both kinds of operands!
    break;

  default:
    ShouldNotReachHere();

#undef REP8
#undef REP16
  }

  assert(which != call32_operand, "instruction is not a call, jmp, or jcc");
  assert(which != imm_operand, "instruction is not a movq reg, imm64");
  assert(which != disp32_operand || has_disp32, "instruction has no disp32 field");

  // parse the output of emit_operand
  int op2 = 0xFF & *ip++;
  int base = op2 & 0x07;
  int op3 = -1;
  const int b100 = 4;
  const int b101 = 5;
  if (base == b100 && (op2 >> 6) != 3) {
    op3 = 0xFF & *ip++;
    base = op3 & 0x07;   // refetch the base
  }
  // now ip points at the disp (if any)

  switch (op2 >> 6) {
  case 0:
    // [00 reg  100][ss index base]
    // [00 reg  100][00   100  esp]
    // [00 reg base]
    // [00 reg  100][ss index  101][disp32]
    // [00 reg  101]               [disp32]

    if (base == b101) {
      if (which == disp32_operand)
        return ip;              // caller wants the disp32
      ip += 4;                  // skip the disp32
    }
    break;

  case 1:
    // [01 reg  100][ss index base][disp8]
    // [01 reg  100][00   100  esp][disp8]
    // [01 reg base]               [disp8]
    ip += 1;                    // skip the disp8
    break;

  case 2:
    // [10 reg  100][ss index base][disp32]
    // [10 reg  100][00   100  esp][disp32]
    // [10 reg base]               [disp32]
    if (which == disp32_operand)
      return ip;                // caller wants the disp32
    ip += 4;                    // skip the disp32
    break;

  case 3:
    // [11 reg base]  (not a memory addressing mode)
    break;
  }

  if (which == end_pc_operand) {
    return ip + tail_size;
  }

  assert(which == narrow_oop_operand && !is_64bit, "instruction is not a movl adr, imm32");
  return ip;
}

address Assembler::locate_next_instruction(address inst) {
  // Secretly share code with locate_operand:
  return locate_operand(inst, end_pc_operand);
}


#ifdef ASSERT
void Assembler::check_relocation(RelocationHolder const& rspec, int format) {
  address inst = inst_mark();
  assert(inst != nullptr && inst < pc(), "must point to beginning of instruction");
  address opnd;

  Relocation* r = rspec.reloc();
  if (r->type() == relocInfo::none) {
    return;
  } else if (r->is_call() || format == call32_operand) {
    // assert(format == imm32_operand, "cannot specify a nonzero format");
    opnd = locate_operand(inst, call32_operand);
  } else if (r->is_data()) {
    assert(format == imm_operand || format == disp32_operand || format == narrow_oop_operand, "format ok");
    opnd = locate_operand(inst, (WhichOperand)format);
  } else {
    assert(format == imm_operand, "cannot specify a format");
    return;
  }
  assert(opnd == pc(), "must put operand where relocs can find it");
}
#endif // ASSERT

void Assembler::emit_operand(Register reg, Address adr, int post_addr_length) {
  emit_operand(reg, adr._base, adr._index, adr._scale, adr._disp, adr._rspec, post_addr_length);
}

void Assembler::emit_operand(XMMRegister reg, Address adr, int post_addr_length) {
  if (adr.isxmmindex()) {
     emit_operand(reg, adr._base, adr._xmmindex, adr._scale, adr._disp, adr._rspec, post_addr_length);
  } else {
     emit_operand(reg, adr._base, adr._index, adr._scale, adr._disp, adr._rspec, post_addr_length);
  }
}

void Assembler::emit_opcode_prefix_and_encoding(int byte1, int byte2, int ocp_and_encoding, int byte3) {
  int opcode_prefix = (ocp_and_encoding & 0xFF00) >> 8;
  if (opcode_prefix != 0) {
    emit_int32(opcode_prefix, (unsigned char)byte1, byte2 | (ocp_and_encoding & 0xFF), byte3);
  } else {
    emit_int24((unsigned char)byte1, byte2 | (ocp_and_encoding & 0xFF), byte3);
  }
}

void Assembler::emit_opcode_prefix_and_encoding(int byte1, int byte2, int ocp_and_encoding) {
  int opcode_prefix = (ocp_and_encoding & 0xFF00) >> 8;
  if (opcode_prefix != 0) {
    emit_int24(opcode_prefix, (unsigned char)byte1, byte2 | (ocp_and_encoding & 0xFF));
  } else {
    emit_int16((unsigned char)byte1, byte2 | (ocp_and_encoding & 0xFF));
  }
}

void Assembler::emit_opcode_prefix_and_encoding(int byte1, int ocp_and_encoding) {
  int opcode_prefix = (ocp_and_encoding & 0xFF00) >> 8;
  if (opcode_prefix != 0) {
    emit_int16(opcode_prefix, (unsigned char)byte1 | (ocp_and_encoding & 0xFF));
  } else {
    emit_int8((unsigned char)byte1 | (ocp_and_encoding & 0xFF));
  }
}

// Now the Assembler instructions (identical for 32/64 bits)

void Assembler::adcl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, rdx, dst, imm32);
}

void Assembler::adcl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8(0x11);
  emit_operand(src, dst, 0);
}

void Assembler::adcl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xD0, dst, imm32);
}

void Assembler::adcl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x13);
  emit_operand(dst, src, 0);
}

void Assembler::adcl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x13, 0xC0, dst, src);
}

void Assembler::addl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, rax, dst, imm32);
}

void Assembler::eaddl(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, rax, src, imm32);
}

void Assembler::addb(Address dst, int imm8) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0x80);
  emit_operand(rax, dst, 1);
  emit_int8(imm8);
}

void Assembler::addb(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8(0x00);
  emit_operand(src, dst, 0);
}

void Assembler::addb(Register dst, int imm8) {
  (void) prefix_and_encode(dst->encoding(), true);
  emit_arith_b(0x80, 0xC0, dst, imm8);
}

void Assembler::addw(Address dst, int imm16) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(dst);
  emit_int8((unsigned char)0x81);
  emit_operand(rax, dst, 2);
  emit_int16(imm16);
}

void Assembler::addw(Address dst, Register src) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(dst, src);
  emit_int8(0x01);
  emit_operand(src, dst, 0);
}

void Assembler::addl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8(0x01);
  emit_operand(src, dst, 0);
}

void Assembler::eaddl(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x01);
  emit_operand(src2, src1, 0);
}

void Assembler::addl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xC0, dst, imm32);
}

void Assembler::eaddl(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x81, 0xC0, no_flags);
}

void Assembler::addl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x03);
  emit_operand(dst, src, 0);
}

void Assembler::eaddl(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x03, no_flags);
}

void Assembler::addl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x03, 0xC0, dst, src);
}

void Assembler::eaddl(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void)emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith(0x03, 0xC0, src1, src2);
}

void Assembler::addr_nop_4() {
  assert(UseAddressNop, "no CPU support");
  // 4 bytes: NOP DWORD PTR [EAX+0]
  emit_int32(0x0F,
             0x1F,
             0x40, // emit_rm(cbuf, 0x1, EAX_enc, EAX_enc);
             0);   // 8-bits offset (1 byte)
}

void Assembler::addr_nop_5() {
  assert(UseAddressNop, "no CPU support");
  // 5 bytes: NOP DWORD PTR [EAX+EAX*0+0] 8-bits offset
  emit_int32(0x0F,
             0x1F,
             0x44,  // emit_rm(cbuf, 0x1, EAX_enc, 0x4);
             0x00); // emit_rm(cbuf, 0x0, EAX_enc, EAX_enc);
  emit_int8(0);     // 8-bits offset (1 byte)
}

void Assembler::addr_nop_7() {
  assert(UseAddressNop, "no CPU support");
  // 7 bytes: NOP DWORD PTR [EAX+0] 32-bits offset
  emit_int24(0x0F,
             0x1F,
             (unsigned char)0x80);
                   // emit_rm(cbuf, 0x2, EAX_enc, EAX_enc);
  emit_int32(0);   // 32-bits offset (4 bytes)
}

void Assembler::addr_nop_8() {
  assert(UseAddressNop, "no CPU support");
  // 8 bytes: NOP DWORD PTR [EAX+EAX*0+0] 32-bits offset
  emit_int32(0x0F,
             0x1F,
             (unsigned char)0x84,
                    // emit_rm(cbuf, 0x2, EAX_enc, 0x4);
             0x00); // emit_rm(cbuf, 0x0, EAX_enc, EAX_enc);
  emit_int32(0);    // 32-bits offset (4 bytes)
}

void Assembler::addsd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::addsd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::addss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::addss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::aesdec(XMMRegister dst, Address src) {
  assert(VM_Version::supports_aes(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xDE);
  emit_operand(dst, src, 0);
}

void Assembler::aesdec(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_aes(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDE, (0xC0 | encode));
}

void Assembler::vaesdec(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vaes(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDE, (0xC0 | encode));
}


void Assembler::aesdeclast(XMMRegister dst, Address src) {
  assert(VM_Version::supports_aes(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xDF);
  emit_operand(dst, src, 0);
}

void Assembler::aesdeclast(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_aes(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDF, (0xC0 | encode));
}

void Assembler::vaesdeclast(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vaes(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDF, (0xC0 | encode));
}

void Assembler::aesenc(XMMRegister dst, Address src) {
  assert(VM_Version::supports_aes(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xDC);
  emit_operand(dst, src, 0);
}

void Assembler::aesenc(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_aes(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDC, 0xC0 | encode);
}

void Assembler::vaesenc(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vaes(), "requires vaes support/enabling");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDC, (0xC0 | encode));
}

void Assembler::aesenclast(XMMRegister dst, Address src) {
  assert(VM_Version::supports_aes(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xDD);
  emit_operand(dst, src, 0);
}

void Assembler::aesenclast(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_aes(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDD, (0xC0 | encode));
}

void Assembler::vaesenclast(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vaes(), "requires vaes support/enabling");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xDD, (0xC0 | encode));
}

void Assembler::andb(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src, true);
  emit_int8(0x20);
  emit_operand(src, dst, 0);
}

void Assembler::andl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, as_Register(4), dst, imm32);
}

void Assembler::eandl(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, rsp, src, imm32);
}

void Assembler::andl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xE0, dst, imm32);
}

void Assembler::eandl(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x81, 0xE0, no_flags);
}

void Assembler::andl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8(0x21);
  emit_operand(src, dst, 0);
}

void Assembler::andl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x23);
  emit_operand(dst, src, 0);
}

void Assembler::eandl(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x23, no_flags);
}

void Assembler::andl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x23, 0xC0, dst, src);
}

void Assembler::eandl(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith(0x23, 0xC0, src1, src2);
}

void Assembler::andnl(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF2, (0xC0 | encode));
}

void Assembler::andnl(Register dst, Register src1, Address src2) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF2);
  emit_operand(dst, src2, 0);
}

void Assembler::bsfl(Register dst, Register src) {
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBC, 0xC0, encode);
}

void Assembler::bsrl(Register dst, Register src) {
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBD, 0xC0, encode);
}

void Assembler::bswapl(Register reg) { // bswap
  int encode = prefix_and_encode(reg->encoding(), false, true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xC8, encode);
}

void Assembler::blsil(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(rbx->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

void Assembler::blsil(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, dst->encoding(), rbx->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF3);
  emit_operand(rbx, src, 0);
}

void Assembler::blsmskl(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(rdx->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF3,
             0xC0 | encode);
}

void Assembler::blsmskl(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, dst->encoding(), rdx->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF3);
  emit_operand(rdx, src, 0);
}

void Assembler::blsrl(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(rcx->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

void Assembler::blsrl(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, dst->encoding(), rcx->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF3);
  emit_operand(rcx, src, 0);
}

void Assembler::call(Label& L, relocInfo::relocType rtype) {
  if (L.is_bound()) {
    const int long_size = 5;
    int offs = (int)( target(L) - pc() );
    assert(offs <= 0, "assembler error");
    InstructionMark im(this);
    // 1110 1000 #32-bit disp
    emit_int8((unsigned char)0xE8);
    emit_data(offs - long_size, rtype, disp32_operand);
  } else {
    InstructionMark im(this);
    // 1110 1000 #32-bit disp
    L.add_patch_at(code(), locator());

    emit_int8((unsigned char)0xE8);
    emit_data(int(0), rtype, disp32_operand);
  }
}

void Assembler::call(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xFF, (0xD0 | encode));
}


void Assembler::call(Address adr) {
  assert(!adr._rspec.reloc()->is_data(), "should not use ExternalAddress for call");
  InstructionMark im(this);
  prefix(adr);
  emit_int8((unsigned char)0xFF);
  emit_operand(rdx, adr, 0);
}

void Assembler::call_literal(address entry, RelocationHolder const& rspec) {
  InstructionMark im(this);
  emit_int8((unsigned char)0xE8);
  intptr_t disp = entry - (pc() + sizeof(int32_t));
  // Entry is null in case of a scratch emit.
  assert(entry == nullptr || is_simm32(disp), "disp=" INTPTR_FORMAT " must be 32bit offset (call2)", disp);
  // Technically, should use call32_operand, but this format is
  // implied by the fact that we're emitting a call instruction.

  emit_data((int) disp, rspec, disp32_operand);
}

void Assembler::cdql() {
  emit_int8((unsigned char)0x99);
}

void Assembler::cld() {
  emit_int8((unsigned char)0xFC);
}

void Assembler::cmovl(Condition cc, Register dst, Register src) {
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding(0x40 | cc, 0xC0, encode);
}

void Assembler::ecmovl(Condition cc, Register dst, Register src1, Register src2) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x40 | cc, false /* no_flags */, true /* is_map1 */,  true /* swap */);
}

void Assembler::cmovl(Condition cc, Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((0x40 | cc));
  emit_operand(dst, src, 0);
}

void Assembler::ecmovl(Condition cc, Register dst, Register src1, Address src2) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, (0x40 | cc) , false /* no_flags */, true /* is_map1 */);
}

void Assembler::cmpb(Address dst, Register reg) {
  assert(reg->has_byte_register(), "must have byte register");
  InstructionMark im(this);
  prefix(dst, reg, true);
  emit_int8((unsigned char)0x38);
  emit_operand(reg, dst, 0);
}

void Assembler::cmpb(Register reg, Address dst) {
  assert(reg->has_byte_register(), "must have byte register");
  InstructionMark im(this);
  prefix(dst, reg, true);
  emit_int8((unsigned char)0x3a);
  emit_operand(reg, dst, 0);
}

void Assembler::cmpb(Address dst, int imm8) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0x80);
  emit_operand(rdi, dst, 1);
  emit_int8(imm8);
}

void Assembler::cmpb(Register dst, int imm8) {
  prefix(dst);
  emit_arith_b(0x80, 0xF8, dst, imm8);
}

void Assembler::cmpl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, as_Register(7), dst, imm32);
}

void Assembler::cmpl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xF8, dst, imm32);
}

void Assembler::cmpl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x3B, 0xC0, dst, src);
}

void Assembler::cmpl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x3B);
  emit_operand(dst, src, 0);
}

void Assembler::cmpl(Address dst,  Register reg) {
  InstructionMark im(this);
  prefix(dst, reg);
  emit_int8(0x39);
  emit_operand(reg, dst, 0);
}

void Assembler::cmpl_imm32(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand_imm32(0x81, as_Register(7), dst, imm32);
}

void Assembler::cmpw(Address dst, int imm16) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(dst);
  emit_int8((unsigned char)0x81);
  emit_operand(rdi, dst, 2);
  emit_int16(imm16);
}

void Assembler::cmpw(Address dst, Register reg) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(dst, reg);
  emit_int8((unsigned char)0x39);
  emit_operand(reg, dst, 0);
}

// The 32-bit cmpxchg compares the value at adr with the contents of rax,
// and stores reg into adr if so; otherwise, the value at adr is loaded into rax,.
// The ZF is set if the compared values were equal, and cleared otherwise.
void Assembler::cmpxchgl(Register reg, Address adr) { // cmpxchg
  InstructionMark im(this);
  prefix(adr, reg, false, true /* is_map1 */);
  emit_int8((unsigned char)0xB1);
  emit_operand(reg, adr, 0);
}

void Assembler::cmpxchgw(Register reg, Address adr) { // cmpxchg
  InstructionMark im(this);
  size_prefix();
  prefix(adr, reg, false, true /* is_map1 */);
  emit_int8((unsigned char)0xB1);
  emit_operand(reg, adr, 0);
}

// The 8-bit cmpxchg compares the value at adr with the contents of rax,
// and stores reg into adr if so; otherwise, the value at adr is loaded into rax,.
// The ZF is set if the compared values were equal, and cleared otherwise.
void Assembler::cmpxchgb(Register reg, Address adr) { // cmpxchg
  InstructionMark im(this);
  prefix(adr, reg, true, true /* is_map1 */);
  emit_int8((unsigned char)0xB0);
  emit_operand(reg, adr, 0);
}

void Assembler::comisd(XMMRegister dst, Address src) {
  // NOTE: dbx seems to decode this as comiss even though the
  // 0x66 is there. Strangely ucomisd comes out correct
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);;
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2F);
  emit_operand(dst, src, 0);
}

void Assembler::comisd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2F, (0xC0 | encode));
}

void Assembler::comiss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2F);
  emit_operand(dst, src, 0);
}

void Assembler::comiss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2F, (0xC0 | encode));
}

void Assembler::cpuid() {
  emit_int16(0x0F, (unsigned char)0xA2);
}

void Assembler::serialize() {
  assert(VM_Version::supports_serialize(), "");
  emit_int24(0x0F, 0x01, 0xE8);
}

// Opcode / Instruction                      Op /  En  64 - Bit Mode     Compat / Leg Mode Description                  Implemented
// F2 0F 38 F0 / r       CRC32 r32, r / m8   RM        Valid             Valid             Accumulate CRC32 on r / m8.  v
// F2 REX 0F 38 F0 / r   CRC32 r32, r / m8*  RM        Valid             N.E.              Accumulate CRC32 on r / m8.  -
// F2 REX.W 0F 38 F0 / r CRC32 r64, r / m8   RM        Valid             N.E.              Accumulate CRC32 on r / m8.  -
//
// F2 0F 38 F1 / r       CRC32 r32, r / m16  RM        Valid             Valid             Accumulate CRC32 on r / m16. v
//
// F2 0F 38 F1 / r       CRC32 r32, r / m32  RM        Valid             Valid             Accumulate CRC32 on r / m32. v
//
// F2 REX.W 0F 38 F1 / r CRC32 r64, r / m64  RM        Valid             N.E.              Accumulate CRC32 on r / m64. v
void Assembler::crc32(Register crc, Register v, int8_t sizeInBytes) {
  assert(VM_Version::supports_sse4_2(), "");
  if (needs_eevex(crc, v)) {
    InstructionAttr attributes(AVX_128bit, /* rex_w */ sizeInBytes == 8, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
    int encode = vex_prefix_and_encode(crc->encoding(), 0, v->encoding(), sizeInBytes == 2 ? VEX_SIMD_66 : VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, true);
    emit_int16(sizeInBytes == 1 ? (unsigned char)0xF0 : (unsigned char)0xF1, (0xC0 | encode));
  } else {
    int8_t w = 0x01;
    Prefix p = Prefix_EMPTY;

    emit_int8((unsigned char)0xF2);
    switch (sizeInBytes) {
    case 1:
      w = 0;
      break;
    case 2:
    case 4:
      break;
    case 8:
      // Note:
      // http://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-instruction-set-reference-manual-325383.pdf
      //
      // Page B - 72   Vol. 2C says
      // qwreg2 to qwreg            1111 0010 : 0100 1R0B : 0000 1111 : 0011 1000 : 1111 0000 : 11 qwreg1 qwreg2
      // mem64 to qwreg             1111 0010 : 0100 1R0B : 0000 1111 : 0011 1000 : 1111 0000 : mod qwreg r / m
      //                                                                            F0!!!
      // while 3 - 208 Vol. 2A
      // F2 REX.W 0F 38 F1 / r       CRC32 r64, r / m64             RM         Valid      N.E.Accumulate CRC32 on r / m64.
      //
      // the 0 on a last bit is reserved for a different flavor of this instruction :
      // F2 REX.W 0F 38 F0 / r       CRC32 r64, r / m8              RM         Valid      N.E.Accumulate CRC32 on r / m8.
      p = REX_W;
      break;
    default:
      assert(0, "Unsupported value for a sizeInBytes argument");
      break;
    }
    prefix(crc, v, p);
    emit_int32(0x0F,
               0x38,
               0xF0 | w,
               0xC0 | ((crc->encoding() & 0x7) << 3) | (v->encoding() & 7));
  }
}

void Assembler::crc32(Register crc, Address adr, int8_t sizeInBytes) {
  assert(VM_Version::supports_sse4_2(), "");
  InstructionMark im(this);
  if (needs_eevex(crc, adr.base(), adr.index())) {
    InstructionAttr attributes(AVX_128bit, /* vex_w */ sizeInBytes == 8, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
    attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
    vex_prefix(adr, 0, crc->encoding(), sizeInBytes == 2 ? VEX_SIMD_66 : VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes);
    emit_int8(sizeInBytes == 1 ? (unsigned char)0xF0 : (unsigned char)0xF1);
    emit_operand(crc, adr, 0);
  } else {
    int8_t w = 0x01;
    Prefix p = Prefix_EMPTY;

    emit_int8((uint8_t)0xF2);
    switch (sizeInBytes) {
    case 1:
      w = 0;
      break;
    case 2:
    case 4:
      break;
    case 8:
      p = REX_W;
      break;
    default:
      assert(0, "Unsupported value for a sizeInBytes argument");
      break;
    }
    prefix(crc, adr, p);
    emit_int24(0x0F, 0x38, (0xF0 | w));
    emit_operand(crc, adr, 0);
  }
}

void Assembler::cvtdq2pd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE6, (0xC0 | encode));
}

void Assembler::vcvtdq2pd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE6, (0xC0 | encode));
}

void Assembler::vcvtps2ph(XMMRegister dst, XMMRegister src, int imm8, int vector_len) {
  assert(VM_Version::supports_evex() || VM_Version::supports_f16c(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /*uses_vl */ true);
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x1D, (0xC0 | encode), imm8);
}

void Assembler::evcvtps2ph(Address dst, KRegister mask, XMMRegister src, int imm8, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /*uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x1D);
  emit_operand(src, dst, 1);
  emit_int8(imm8);
}

void Assembler::vcvtps2ph(Address dst, XMMRegister src, int imm8, int vector_len) {
  assert(VM_Version::supports_evex() || VM_Version::supports_f16c(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /*uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x1D);
  emit_operand(src, dst, 1);
  emit_int8(imm8);
}

void Assembler::vcvtph2ps(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex() || VM_Version::supports_f16c(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x13, (0xC0 | encode));
}

void Assembler::vcvtph2ps(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_evex() || VM_Version::supports_f16c(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /*uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x13);
  emit_operand(dst, src, 0);
}

void Assembler::cvtdq2ps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5B, (0xC0 | encode));
}

void Assembler::vcvtdq2ps(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5B, (0xC0 | encode));
}

void Assembler::cvtsd2ss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, src, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5A, (0xC0 | encode));
}

void Assembler::cvtsd2ss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5A);
  emit_operand(dst, src, 0);
}

void Assembler::cvtsi2sdl(XMMRegister dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x2A, (0xC0 | encode));
}

void Assembler::cvtsi2sdl(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2A);
  emit_operand(dst, src, 0);
}

void Assembler::cvtsi2ssl(XMMRegister dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x2A, (0xC0 | encode));
}

void Assembler::cvtsi2ssl(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2A);
  emit_operand(dst, src, 0);
}

void Assembler::cvtsi2ssq(XMMRegister dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x2A, (0xC0 | encode));
}

void Assembler::cvtss2sd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, src, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5A, (0xC0 | encode));
}

void Assembler::cvtss2sd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5A);
  emit_operand(dst, src, 0);
}


void Assembler::cvttsd2sil(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2C, (0xC0 | encode));
}

void Assembler::cvtss2sil(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2D, (0xC0 | encode));
}

void Assembler::cvttss2sil(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2C, (0xC0 | encode));
}

void Assembler::cvttpd2dq(XMMRegister dst, XMMRegister src) {
  int vector_len = VM_Version::supports_avx512novl() ? AVX_512bit : AVX_128bit;
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE6, (0xC0 | encode));
}

void Assembler::pabsb(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_ssse3(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1C, (0xC0 | encode));
}

void Assembler::pabsw(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_ssse3(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1D, (0xC0 | encode));
}

void Assembler::pabsd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_ssse3(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1E, (0xC0 | encode));
}

void Assembler::vpabsb(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx()      :
         vector_len == AVX_256bit ? VM_Version::supports_avx2()     :
         vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : false, "not supported");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1C, (0xC0 | encode));
}

void Assembler::vpabsw(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx()      :
         vector_len == AVX_256bit ? VM_Version::supports_avx2()     :
         vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : false, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1D, (0xC0 | encode));
}

void Assembler::vpabsd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
  vector_len == AVX_256bit? VM_Version::supports_avx2() :
  vector_len == AVX_512bit? VM_Version::supports_evex() : 0, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1E, (0xC0 | encode));
}

void Assembler::evpabsq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1F, (0xC0 | encode));
}

void Assembler::vcvtps2pd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5A, (0xC0 | encode));
}

void Assembler::vcvtpd2ps(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  attributes.set_rex_vex_w_reverted();
  emit_int16(0x5A, (0xC0 | encode));
}

void Assembler::vcvttps2dq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5B, (0xC0 | encode));
}

void Assembler::vcvttpd2dq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE6, (0xC0 | encode));
}

void Assembler::vcvtps2dq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5B, (0xC0 | encode));
}

void Assembler::evcvttps2qq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x7A, (0xC0 | encode));
}

void Assembler::evcvtpd2qq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x7B, (0xC0 | encode));
}

void Assembler::evcvtqq2ps(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5B, (0xC0 | encode));
}

void Assembler::evcvttpd2qq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x7A, (0xC0 | encode));
}

void Assembler::evcvtqq2pd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE6, (0xC0 | encode));
}

void Assembler::evpmovwb(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x30, (0xC0 | encode));
}

void Assembler::evpmovdw(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x33, (0xC0 | encode));
}

void Assembler::evpmovdb(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x31, (0xC0 | encode));
}

void Assembler::evpmovqd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x35, (0xC0 | encode));
}

void Assembler::evpmovqb(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x32, (0xC0 | encode));
}

void Assembler::evpmovqw(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x34, (0xC0 | encode));
}

void Assembler::evpmovsqd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x25, (0xC0 | encode));
}

void Assembler::decl(Address dst) {
  // Don't use it directly. Use MacroAssembler::decrement() instead.
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xFF);
  emit_operand(rcx, dst, 0);
}

void Assembler::edecl(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xFF);
  emit_operand(rcx, src, 0);
}

void Assembler::divsd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::divsd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::divss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::divss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::hlt() {
  emit_int8((unsigned char)0xF4);
}

void Assembler::idivl(Register src) {
  int encode = prefix_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xF8 | encode));
}

void Assembler::eidivl(Register src, bool no_flags) { // Signed
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xF8 | encode));
}

void Assembler::divl(Register src) { // Unsigned
  int encode = prefix_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xF0 | encode));
}

void Assembler::edivl(Register src, bool no_flags) { // Unsigned
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xF0 | encode));
}

void Assembler::imull(Register src) {
  int encode = prefix_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xE8 | encode));
}

void Assembler::eimull(Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xE8 | encode));
}

void Assembler::imull(Register dst, Register src) {
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xAF, 0xC0, encode);
}

void Assembler::eimull(Register dst, Register src1, Register src2, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0xAF, no_flags, true /* is_map1 */,  true /* swap */);
}

void Assembler::imull(Register dst, Address src, int32_t value) {
  InstructionMark im(this);
  prefix(src, dst);
  if (is8bit(value)) {
    emit_int8((unsigned char)0x6B);
    emit_operand(dst, src, 1);
    emit_int8(value);
  } else {
    emit_int8((unsigned char)0x69);
    emit_operand(dst, src, 4);
    emit_int32(value);
  }
}

void Assembler::eimull(Register dst, Address src, int32_t value, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (is8bit(value)) {
    emit_int8((unsigned char)0x6B);
    emit_operand(dst, src, 1);
    emit_int8(value);
  } else {
    emit_int8((unsigned char)0x69);
    emit_operand(dst, src, 4);
    emit_int32(value);
  }
}

void Assembler::imull(Register dst, Register src, int value) {
  int encode = prefix_and_encode(dst->encoding(), src->encoding());
  if (is8bit(value)) {
    emit_int24(0x6B, (0xC0 | encode), value & 0xFF);
  } else {
    emit_int16(0x69, (0xC0 | encode));
    emit_int32(value);
  }
}

void Assembler::eimull(Register dst, Register src, int value, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (is8bit(value)) {
    emit_int24(0x6B, (0xC0 | encode), value & 0xFF);
  } else {
    emit_int16(0x69, (0xC0 | encode));
    emit_int32(value);
  }
}

void Assembler::imull(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xAF);
  emit_operand(dst, src, 0);
}

void Assembler::eimull(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, (unsigned char)0xAF, no_flags, true /* is_map1 */);
}

void Assembler::incl(Address dst) {
  // Don't use it directly. Use MacroAssembler::increment() instead.
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xFF);
  emit_operand(rax, dst, 0);
}

void Assembler::eincl(Register dst, Address src, bool no_flags) {
  // Don't use it directly. Use MacroAssembler::increment() instead.
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xFF);
  emit_operand(rax, src, 0);
}

void Assembler::jcc(Condition cc, Label& L, bool maybe_short) {
  InstructionMark im(this);
  assert((0 <= cc) && (cc < 16), "illegal cc");
  if (L.is_bound()) {
    address dst = target(L);
    assert(dst != nullptr, "jcc most probably wrong");

    const int short_size = 2;
    const int long_size = 6;
    int offs = checked_cast<int>((intptr_t)dst - (intptr_t)pc());
    if (maybe_short && is8bit(offs - short_size)) {
      // 0111 tttn #8-bit disp
      emit_int16(0x70 | cc, (offs - short_size) & 0xFF);
    } else {
      // 0000 1111 1000 tttn #32-bit disp
      assert(is_simm32(offs - long_size),
             "must be 32bit offset (call4)");
      emit_int16(0x0F, (0x80 | cc));
      emit_int32(offs - long_size);
    }
  } else {
    // Note: could eliminate cond. jumps to this jump if condition
    //       is the same however, seems to be rather unlikely case.
    // Note: use jccb() if label to be bound is very close to get
    //       an 8-bit displacement
    L.add_patch_at(code(), locator());
    emit_int16(0x0F, (0x80 | cc));
    emit_int32(0);
  }
}

void Assembler::jccb_0(Condition cc, Label& L, const char* file, int line) {
  if (L.is_bound()) {
    const int short_size = 2;
    address entry = target(L);
#ifdef ASSERT
    int dist = checked_cast<int>((intptr_t)entry - (intptr_t)(pc() + short_size));
    int delta = short_branch_delta();
    if (delta != 0) {
      dist += (dist < 0 ? (-delta) :delta);
    }
    assert(is8bit(dist), "Displacement too large for a short jmp at %s:%d", file, line);
#endif
    int offs = checked_cast<int>((intptr_t)entry - (intptr_t)pc());
    // 0111 tttn #8-bit disp
    emit_int16(0x70 | cc, (offs - short_size) & 0xFF);
  } else {
    InstructionMark im(this);
    L.add_patch_at(code(), locator(), file, line);
    emit_int16(0x70 | cc, 0);
  }
}

void Assembler::jmp(Address adr) {
  InstructionMark im(this);
  prefix(adr);
  emit_int8((unsigned char)0xFF);
  emit_operand(rsp, adr, 0);
}

void Assembler::jmp(Label& L, bool maybe_short) {
  if (L.is_bound()) {
    address entry = target(L);
    assert(entry != nullptr, "jmp most probably wrong");
    InstructionMark im(this);
    const int short_size = 2;
    const int long_size = 5;
    int offs = checked_cast<int>(entry - pc());
    if (maybe_short && is8bit(offs - short_size)) {
      emit_int16((unsigned char)0xEB, ((offs - short_size) & 0xFF));
    } else {
      emit_int8((unsigned char)0xE9);
      emit_int32(offs - long_size);
    }
  } else {
    // By default, forward jumps are always 32-bit displacements, since
    // we can't yet know where the label will be bound.  If you're sure that
    // the forward jump will not run beyond 256 bytes, use jmpb to
    // force an 8-bit displacement.
    InstructionMark im(this);
    L.add_patch_at(code(), locator());
    emit_int8((unsigned char)0xE9);
    emit_int32(0);
  }
}

void Assembler::jmp(Register entry) {
  int encode = prefix_and_encode(entry->encoding());
  emit_int16((unsigned char)0xFF, (0xE0 | encode));
}

void Assembler::jmp_literal(address dest, RelocationHolder const& rspec) {
  InstructionMark im(this);
  emit_int8((unsigned char)0xE9);
  assert(dest != nullptr, "must have a target");
  intptr_t disp = dest - (pc() + sizeof(int32_t));
  assert(is_simm32(disp), "must be 32bit offset (jmp)");
  emit_data(checked_cast<int32_t>(disp), rspec, call32_operand);
}

void Assembler::jmpb_0(Label& L, const char* file, int line) {
  if (L.is_bound()) {
    const int short_size = 2;
    address entry = target(L);
    assert(entry != nullptr, "jmp most probably wrong");
#ifdef ASSERT
    int dist = checked_cast<int>((intptr_t)entry - (intptr_t)(pc() + short_size));
    int delta = short_branch_delta();
    if (delta != 0) {
      dist += (dist < 0 ? (-delta) :delta);
    }
    assert(is8bit(dist), "Displacement too large for a short jmp at %s:%d", file, line);
#endif
    intptr_t offs = entry - pc();
    emit_int16((unsigned char)0xEB, (offs - short_size) & 0xFF);
  } else {
    InstructionMark im(this);
    L.add_patch_at(code(), locator(), file, line);
    emit_int16((unsigned char)0xEB, 0);
  }
}

void Assembler::ldmxcsr( Address src) {
  // This instruction should be SSE encoded with the REX2 prefix when an
  // extended GPR is present. To be consistent when UseAPX is enabled, use
  // this encoding even when an extended GPR is not used.
  if (UseAVX > 0 && !UseAPX ) {
    InstructionMark im(this);
    InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
    vex_prefix(src, 0, 0, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
    emit_int8((unsigned char)0xAE);
    emit_operand(as_Register(2), src, 0);
  } else {
    InstructionMark im(this);
    prefix(src, true /* is_map1 */);
    emit_int8((unsigned char)0xAE);
    emit_operand(as_Register(2), src, 0);
  }
}

void Assembler::leal(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8((unsigned char)0x8D);
  emit_operand(dst, src, 0);
}

void Assembler::lea(Register dst, Label& L) {
  emit_prefix_and_int8(get_prefixq(Address(), dst), (unsigned char)0x8D);
  if (!L.is_bound()) {
    // Patch @0x8D opcode
    L.add_patch_at(code(), CodeBuffer::locator(offset() - 1, sect()));
    // Register and [rip+disp] operand
    emit_modrm(0b00, raw_encode(dst), 0b101);
    emit_int32(0);
  } else {
    // Register and [rip+disp] operand
    emit_modrm(0b00, raw_encode(dst), 0b101);
    // Adjust displacement by sizeof lea instruction
    int32_t disp = checked_cast<int32_t>(target(L) - (pc() + sizeof(int32_t)));
    assert(is_simm32(disp), "must be 32bit offset [rip+offset]");
    emit_int32(disp);
  }
}

void Assembler::lfence() {
  emit_int24(0x0F, (unsigned char)0xAE, (unsigned char)0xE8);
}

void Assembler::lock() {
  emit_int8((unsigned char)0xF0);
}

void Assembler::size_prefix() {
  emit_int8(0x66);
}

void Assembler::lzcntl(Register dst, Register src) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  emit_int8((unsigned char)0xF3);
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBD, 0xC0, encode);
}

void Assembler::elzcntl(Register dst, Register src, bool no_flags) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::lzcntl(Register dst, Address src) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  InstructionMark im(this);
  emit_int8((unsigned char)0xF3);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xBD);
  emit_operand(dst, src, 0);
}

void Assembler::elzcntl(Register dst, Address src, bool no_flags) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xF5);
  emit_operand(dst, src, 0);
}

// Emit mfence instruction
void Assembler::mfence() {
  emit_int24(0x0F, (unsigned char)0xAE, (unsigned char)0xF0);
}

// Emit sfence instruction
void Assembler::sfence() {
  emit_int24(0x0F, (unsigned char)0xAE, (unsigned char)0xF8);
}

void Assembler::mov(Register dst, Register src) {
  movq(dst, src);
}

void Assembler::movapd(XMMRegister dst, Address src) {
  NOT_LP64(assert(VM_Version::supports_sse2(), ""));
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x28);
  emit_operand(dst, src, 0);
}

void Assembler::movapd(XMMRegister dst, XMMRegister src) {
  int vector_len = VM_Version::supports_avx512novl() ? AVX_512bit : AVX_128bit;
  InstructionAttr attributes(vector_len, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x28, (0xC0 | encode));
}

void Assembler::movaps(XMMRegister dst, XMMRegister src) {
  int vector_len = VM_Version::supports_avx512novl() ? AVX_512bit : AVX_128bit;
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x28, (0xC0 | encode));
}

void Assembler::movlhps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, src, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x16, (0xC0 | encode));
}

void Assembler::movb(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst, true);
  emit_int8((unsigned char)0x8A);
  emit_operand(dst, src, 0);
}

void Assembler::movddup(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse3(), "");
  int vector_len = VM_Version::supports_avx512novl() ? AVX_512bit : AVX_128bit;
  InstructionAttr attributes(vector_len, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x12, 0xC0 | encode);
}

void Assembler::movddup(XMMRegister dst, Address src) {
  assert(VM_Version::supports_sse3(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_DUP, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x12);
  emit_operand(dst, src, 0);
}

void Assembler::vmovddup(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_DUP, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x12);
  emit_operand(dst, src, 0);
}

void Assembler::kmovbl(KRegister dst, KRegister src) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x90, (0xC0 | encode));
}

void Assembler::kmovbl(KRegister dst, Register src) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int16((unsigned char)0x92, (0xC0 | encode));
}

void Assembler::kmovbl(Register dst, KRegister src) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x93, (0xC0 | encode));
}

void Assembler::kmovwl(KRegister dst, Register src) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes, true);
  emit_int16((unsigned char)0x92, (0xC0 | encode));
}

void Assembler::kmovwl(Register dst, KRegister src) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x93, (0xC0 | encode));
}

void Assembler::kmovwl(KRegister dst, Address src) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0x90);
  emit_operand(dst, src, 0);
}

void Assembler::kmovwl(Address dst, KRegister src) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0x91);
  emit_operand(src, dst, 0);
}

void Assembler::kmovwl(KRegister dst, KRegister src) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x90, (0xC0 | encode));
}

void Assembler::kmovdl(KRegister dst, Register src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes, true);
  emit_int16((unsigned char)0x92, (0xC0 | encode));
}

void Assembler::kmovdl(Register dst, KRegister src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x93, (0xC0 | encode));
}

void Assembler::kmovql(KRegister dst, KRegister src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x90, (0xC0 | encode));
}

void Assembler::kmovql(KRegister dst, Address src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0x90);
  emit_operand(dst, src, 0);
}

void Assembler::kmovql(Address dst, KRegister src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0x91);
  emit_operand(src, dst, 0);
}

void Assembler::kmovql(KRegister dst, Register src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes, true);
  emit_int16((unsigned char)0x92, (0xC0 | encode));
}

void Assembler::kmovql(Register dst, KRegister src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x93, (0xC0 | encode));
}

void Assembler::knotwl(KRegister dst, KRegister src) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x44, (0xC0 | encode));
}

void Assembler::knotbl(KRegister dst, KRegister src) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x44, (0xC0 | encode));
}

void Assembler::korbl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

void Assembler::korwl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

void Assembler::kordl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

void Assembler::korql(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

void Assembler::kxorbl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

void Assembler::kxnorwl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x46, (0xC0 | encode));
}

void Assembler::kxorwl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

void Assembler::kxordl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

void Assembler::kxorql(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

void Assembler::kandbl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x41, (0xC0 | encode));
}

void Assembler::kandwl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x41, (0xC0 | encode));
}

void Assembler::kanddl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x41, (0xC0 | encode));
}

void Assembler::kandql(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x41, (0xC0 | encode));
}

void Assembler::knotdl(KRegister dst, KRegister src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x44, (0xC0 | encode));
}

void Assembler::knotql(KRegister dst, KRegister src) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x44, (0xC0 | encode));
}

// This instruction produces ZF or CF flags
void Assembler::kortestbl(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x98, (0xC0 | encode));
}

// This instruction produces ZF or CF flags
void Assembler::kortestwl(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x98, (0xC0 | encode));
}

// This instruction produces ZF or CF flags
void Assembler::kortestdl(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x98, (0xC0 | encode));
}

// This instruction produces ZF or CF flags
void Assembler::kortestql(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x98, (0xC0 | encode));
}

// This instruction produces ZF or CF flags
void Assembler::ktestql(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x99, (0xC0 | encode));
}

void Assembler::ktestdl(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x99, (0xC0 | encode));
}

void Assembler::ktestwl(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x99, (0xC0 | encode));
}

void Assembler::ktestbl(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x99, (0xC0 | encode));
}

void Assembler::ktestq(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x99, (0xC0 | encode));
}

void Assembler::ktestd(KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src1->encoding(), 0, src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0x99, (0xC0 | encode));
}

void Assembler::kxnorbl(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x46, (0xC0 | encode));
}

void Assembler::kshiftlbl(KRegister dst, KRegister src, int imm8) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0 , src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int16(0x32, (0xC0 | encode));
  emit_int8(imm8);
}

void Assembler::kshiftlql(KRegister dst, KRegister src, int imm8) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0 , src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int16(0x33, (0xC0 | encode));
  emit_int8(imm8);
}


void Assembler::kshiftrbl(KRegister dst, KRegister src, int imm8) {
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0 , src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int16(0x30, (0xC0 | encode));
}

void Assembler::kshiftrwl(KRegister dst, KRegister src, int imm8) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0 , src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int16(0x30, (0xC0 | encode));
  emit_int8(imm8);
}

void Assembler::kshiftrdl(KRegister dst, KRegister src, int imm8) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0 , src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int16(0x31, (0xC0 | encode));
  emit_int8(imm8);
}

void Assembler::kshiftrql(KRegister dst, KRegister src, int imm8) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0 , src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int16(0x31, (0xC0 | encode));
  emit_int8(imm8);
}

void Assembler::kunpckdql(KRegister dst, KRegister src1, KRegister src2) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x4B, (0xC0 | encode));
}

void Assembler::movb(Address dst, int imm8) {
  InstructionMark im(this);
   prefix(dst);
  emit_int8((unsigned char)0xC6);
  emit_operand(rax, dst, 1);
  emit_int8(imm8);
}


void Assembler::movb(Address dst, Register src) {
  assert(src->has_byte_register(), "must have byte register");
  InstructionMark im(this);
  prefix(dst, src, true);
  emit_int8((unsigned char)0x88);
  emit_operand(src, dst, 0);
}

void Assembler::movdl(XMMRegister dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, as_XMMRegister(src->encoding()), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x6E, (0xC0 | encode));
}

void Assembler::movdl(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // swap src/dst to get correct prefix
  int encode = simd_prefix_and_encode(src, xnoreg, as_XMMRegister(dst->encoding()), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x7E, (0xC0 | encode));
}

void Assembler::movdl(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6E);
  emit_operand(dst, src, 0);
}

void Assembler::movdl(Address dst, XMMRegister src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(src, xnoreg, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7E);
  emit_operand(src, dst, 0);
}

void Assembler::movdqa(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6F, (0xC0 | encode));
}

void Assembler::movdqa(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::movdqu(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::movdqu(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6F, (0xC0 | encode));
}

void Assembler::movdqu(Address dst, XMMRegister src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.reset_is_clear_context();
  simd_prefix(src, xnoreg, dst, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7F);
  emit_operand(src, dst, 0);
}

// Move Unaligned 256bit Vector
void Assembler::vmovdqu(XMMRegister dst, XMMRegister src) {
  assert(UseAVX > 0, "");
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6F, (0xC0 | encode));
}

void Assembler::vmovw(XMMRegister dst, Register src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_MAP5, &attributes, true);
  emit_int16(0x6E, (0xC0 | encode));
}

void Assembler::vmovw(Register dst, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_MAP5, &attributes, true);
  emit_int16(0x7E, (0xC0 | encode));
}

void Assembler::vmovdqu(XMMRegister dst, Address src) {
  assert(UseAVX > 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::vmovdqu(Address dst, XMMRegister src) {
  assert(UseAVX > 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.reset_is_clear_context();
  // swap src<->dst for encoding
  assert(src != xnoreg, "sanity");
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7F);
  emit_operand(src, dst, 0);
}

// Move Aligned 256bit Vector
void Assembler::vmovdqa(XMMRegister dst, Address src) {
  assert(UseAVX > 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::vmovdqa(Address dst, XMMRegister src) {
  assert(UseAVX > 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.reset_is_clear_context();
  // swap src<->dst for encoding
  assert(src != xnoreg, "sanity");
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7F);
  emit_operand(src, dst, 0);
}

void Assembler::vpmaskmovd(XMMRegister dst, XMMRegister mask, Address src, int vector_len) {
  assert((VM_Version::supports_avx2() && vector_len == AVX_256bit), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ false, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x8C);
  emit_operand(dst, src, 0);
}

void Assembler::vpmaskmovq(XMMRegister dst, XMMRegister mask, Address src, int vector_len) {
  assert((VM_Version::supports_avx2() && vector_len == AVX_256bit), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ false, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x8C);
  emit_operand(dst, src, 0);
}

void Assembler::vmaskmovps(XMMRegister dst, Address src, XMMRegister mask, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x2C);
  emit_operand(dst, src, 0);
}

void Assembler::vmaskmovpd(XMMRegister dst, Address src, XMMRegister mask, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x2D);
  emit_operand(dst, src, 0);
}

void Assembler::vmaskmovps(Address dst, XMMRegister src, XMMRegister mask, int vector_len) {
  assert(UseAVX > 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(dst, mask->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x2E);
  emit_operand(src, dst, 0);
}

void Assembler::vmaskmovpd(Address dst, XMMRegister src, XMMRegister mask, int vector_len) {
  assert(UseAVX > 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(dst, mask->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x2F);
  emit_operand(src, dst, 0);
}

// Move Unaligned EVEX enabled Vector (programmable : 8,16,32,64)
void Assembler::evmovdqub(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6F, (0xC0 | encode));
}

void Assembler::evmovdqub(XMMRegister dst, XMMRegister src, int vector_len) {
  // Unmasked instruction
  evmovdqub(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdqub(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::evmovdqub(XMMRegister dst, Address src, int vector_len) {
  // Unmasked instruction
  evmovdqub(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdqub(Address dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7F);
  emit_operand(src, dst, 0);
}

void Assembler::evmovdquw(XMMRegister dst, XMMRegister src, int vector_len) {
  // Unmasked instruction
  evmovdquw(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdquw(XMMRegister dst, Address src, int vector_len) {
  // Unmasked instruction
  evmovdquw(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdquw(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::evmovdquw(Address dst, XMMRegister src, int vector_len) {
  // Unmasked instruction
  evmovdquw(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdquw(Address dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7F);
  emit_operand(src, dst, 0);
}

void Assembler::evmovdquw(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6F, (0xC0 | encode));
}


void Assembler::evmovdqul(XMMRegister dst, XMMRegister src, int vector_len) {
  // Unmasked instruction
  evmovdqul(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdqul(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6F, (0xC0 | encode));
}

void Assembler::evmovdqul(XMMRegister dst, Address src, int vector_len) {
  // Unmasked instruction
  evmovdqul(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdqul(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false , /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::evmovdqul(Address dst, XMMRegister src, int vector_len) {
  // Unmasked isntruction
  evmovdqul(dst, k0, src, /*merge*/ true, vector_len);
}

void Assembler::evmovdqul(Address dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7F);
  emit_operand(src, dst, 0);
}

void Assembler::evmovdquq(XMMRegister dst, XMMRegister src, int vector_len) {
  // Unmasked instruction
  evmovdquq(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdquq(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6F, (0xC0 | encode));
}

void Assembler::evmovdquq(XMMRegister dst, Address src, int vector_len) {
  // Unmasked instruction
  evmovdquq(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdquq(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

// Move Aligned 512bit Vector
void Assembler::evmovdqaq(XMMRegister dst, Address src, int vector_len) {
  // Unmasked instruction
  evmovdqaq(dst, k0, src, /*merge*/ false, vector_len);
}

void Assembler::evmovdqaq(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x6F);
  emit_operand(dst, src, 0);
}

void Assembler::evmovntdquq(Address dst, XMMRegister src, int vector_len) {
  // Unmasked instruction
  evmovntdquq(dst, k0, src, /*merge*/ true, vector_len);
}

void Assembler::evmovntdquq(Address dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0xE7);
  emit_operand(src, dst, 0);
}

void Assembler::evmovdquq(Address dst, XMMRegister src, int vector_len) {
  // Unmasked instruction
  evmovdquq(dst, k0, src, /*merge*/ true, vector_len);
}

void Assembler::evmovdquq(Address dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7F);
  emit_operand(src, dst, 0);
}

// Uses zero extension on 64bit

void Assembler::movl(Register dst, int32_t imm32) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int8(0xB8 | encode);
  emit_int32(imm32);
}

void Assembler::movl(Register dst, Register src) {
  int encode = prefix_and_encode(dst->encoding(), src->encoding());
  emit_int16((unsigned char)0x8B, (0xC0 | encode));
}

void Assembler::movl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8((unsigned char)0x8B);
  emit_operand(dst, src, 0);
}

void Assembler::movl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xC7);
  emit_operand(rax, dst, 4);
  emit_int32(imm32);
}

void Assembler::movl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8((unsigned char)0x89);
  emit_operand(src, dst, 0);
}

// New cpus require to use movsd and movss to avoid partial register stall
// when loading from memory. But for old Opteron use movlpd instead of movsd.
// The selection is done in MacroAssembler::movdbl() and movflt().
void Assembler::movlpd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x12);
  emit_operand(dst, src, 0);
}

void Assembler::movq(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x7E);
  emit_operand(dst, src, 0);
}

void Assembler::movq(Address dst, XMMRegister src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(src, xnoreg, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD6);
  emit_operand(src, dst, 0);
}

void Assembler::movq(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(src, xnoreg, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD6, (0xC0 | encode));
}

void Assembler::movq(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // swap src/dst to get correct prefix
  int encode = simd_prefix_and_encode(src, xnoreg, as_XMMRegister(dst->encoding()), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x7E, (0xC0 | encode));
}

void Assembler::movq(XMMRegister dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, as_XMMRegister(src->encoding()), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x6E, (0xC0 | encode));
}

void Assembler::movsbl(Register dst, Address src) { // movsxb
  InstructionMark im(this);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xBE);
  emit_operand(dst, src, 0);
}

void Assembler::movsbl(Register dst, Register src) { // movsxb
  int encode = prefix_and_encode(dst->encoding(), false, src->encoding(), true, true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBE, 0xC0, encode);
}

void Assembler::movsd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x10, (0xC0 | encode));
}

void Assembler::movsd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x10);
  emit_operand(dst, src, 0);
}

void Assembler::movsd(Address dst, XMMRegister src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_rex_vex_w_reverted();
  simd_prefix(src, xnoreg, dst, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x11);
  emit_operand(src, dst, 0);
}

void Assembler::vmovsd(XMMRegister dst, XMMRegister src, XMMRegister src2) {
  assert(UseAVX > 0, "Requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(src2->encoding(), src->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x11, (0xC0 | encode));
}

void Assembler::movss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x10, (0xC0 | encode));
}

void Assembler::movss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x10);
  emit_operand(dst, src, 0);
}

void Assembler::movss(Address dst, XMMRegister src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  simd_prefix(src, xnoreg, dst, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x11);
  emit_operand(src, dst, 0);
}

void Assembler::movswl(Register dst, Address src) { // movsxw
  InstructionMark im(this);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xBF);
  emit_operand(dst, src, 0);
}

void Assembler::movswl(Register dst, Register src) { // movsxw
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBF, 0xC0, encode);
}

void Assembler::movups(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x10);
  emit_operand(dst, src, 0);
}

void Assembler::vmovups(XMMRegister dst, Address src, int vector_len) {
  assert(vector_len == AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x10);
  emit_operand(dst, src, 0);
}

void Assembler::movups(Address dst, XMMRegister src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(src, xnoreg, dst, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x11);
  emit_operand(src, dst, 0);
}

void Assembler::vmovups(Address dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(src, xnoreg, dst, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x11);
  emit_operand(src, dst, 0);
}

void Assembler::movw(Address dst, int imm16) {
  InstructionMark im(this);

  emit_int8(0x66); // switch to 16-bit mode
  prefix(dst);
  emit_int8((unsigned char)0xC7);
  emit_operand(rax, dst, 2);
  emit_int16(imm16);
}

void Assembler::movw(Register dst, Address src) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(src, dst);
  emit_int8((unsigned char)0x8B);
  emit_operand(dst, src, 0);
}

void Assembler::movw(Address dst, Register src) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(dst, src);
  emit_int8((unsigned char)0x89);
  emit_operand(src, dst, 0);
}

void Assembler::movzbl(Register dst, Address src) { // movzxb
  InstructionMark im(this);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xB6);
  emit_operand(dst, src, 0);
}

void Assembler::movzbl(Register dst, Register src) { // movzxb
  int encode = prefix_and_encode(dst->encoding(), false, src->encoding(), true, true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xB6, 0xC0, encode);
}

void Assembler::movzwl(Register dst, Address src) { // movzxw
  InstructionMark im(this);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xB7);
  emit_operand(dst, src, 0);
}

void Assembler::movzwl(Register dst, Register src) { // movzxw
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xB7, 0xC0, encode);
}

void Assembler::mull(Address src) {
  InstructionMark im(this);
  prefix(src);
  emit_int8((unsigned char)0xF7);
  emit_operand(rsp, src, 0);
}

void Assembler::emull(Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_nf(src, 0, 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xF7);
  emit_operand(rsp, src, 0);
}

void Assembler::mull(Register src) {
  int encode = prefix_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xE0 | encode));
}

void Assembler::emull(Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xE0 | encode));
}

void Assembler::mulsd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::mulsd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::mulss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::mulss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::negl(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xF7, (0xD8 | encode));
}

void Assembler::enegl(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xD8 | encode));
}

void Assembler::negl(Address dst) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xF7);
  emit_operand(as_Register(3), dst, 0);
}

void Assembler::enegl(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xF7);
  emit_operand(as_Register(3), src, 0);
}

void Assembler::nop(uint i) {
#ifdef ASSERT
  assert(i > 0, " ");
  // The fancy nops aren't currently recognized by debuggers making it a
  // pain to disassemble code while debugging. If asserts are on clearly
  // speed is not an issue so simply use the single byte traditional nop
  // to do alignment.

  for (; i > 0 ; i--) emit_int8((unsigned char)0x90);
  return;

#endif // ASSERT

  if (UseAddressNop && VM_Version::is_intel()) {
    //
    // Using multi-bytes nops "0x0F 0x1F [address]" for Intel
    //  1: 0x90
    //  2: 0x66 0x90
    //  3: 0x66 0x66 0x90 (don't use "0x0F 0x1F 0x00" - need patching safe padding)
    //  4: 0x0F 0x1F 0x40 0x00
    //  5: 0x0F 0x1F 0x44 0x00 0x00
    //  6: 0x66 0x0F 0x1F 0x44 0x00 0x00
    //  7: 0x0F 0x1F 0x80 0x00 0x00 0x00 0x00
    //  8: 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    //  9: 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    // 10: 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    // 11: 0x66 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00

    // The rest coding is Intel specific - don't use consecutive address nops

    // 12: 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90
    // 13: 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90
    // 14: 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90
    // 15: 0x66 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90

    while(i >= 15) {
      // For Intel don't generate consecutive address nops (mix with regular nops)
      i -= 15;
      emit_int24(0x66, 0x66, 0x66);
      addr_nop_8();
      emit_int32(0x66, 0x66, 0x66, (unsigned char)0x90);
    }
    switch (i) {
      case 14:
        emit_int8(0x66); // size prefix
      case 13:
        emit_int8(0x66); // size prefix
      case 12:
        addr_nop_8();
        emit_int32(0x66, 0x66, 0x66, (unsigned char)0x90);
        break;
      case 11:
        emit_int8(0x66); // size prefix
      case 10:
        emit_int8(0x66); // size prefix
      case 9:
        emit_int8(0x66); // size prefix
      case 8:
        addr_nop_8();
        break;
      case 7:
        addr_nop_7();
        break;
      case 6:
        emit_int8(0x66); // size prefix
      case 5:
        addr_nop_5();
        break;
      case 4:
        addr_nop_4();
        break;
      case 3:
        // Don't use "0x0F 0x1F 0x00" - need patching safe padding
        emit_int8(0x66); // size prefix
      case 2:
        emit_int8(0x66); // size prefix
      case 1:
        emit_int8((unsigned char)0x90);
                         // nop
        break;
      default:
        assert(i == 0, " ");
    }
    return;
  }
  if (UseAddressNop && VM_Version::is_amd_family()) {
    //
    // Using multi-bytes nops "0x0F 0x1F [address]" for AMD.
    //  1: 0x90
    //  2: 0x66 0x90
    //  3: 0x66 0x66 0x90 (don't use "0x0F 0x1F 0x00" - need patching safe padding)
    //  4: 0x0F 0x1F 0x40 0x00
    //  5: 0x0F 0x1F 0x44 0x00 0x00
    //  6: 0x66 0x0F 0x1F 0x44 0x00 0x00
    //  7: 0x0F 0x1F 0x80 0x00 0x00 0x00 0x00
    //  8: 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    //  9: 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    // 10: 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    // 11: 0x66 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00

    // The rest coding is AMD specific - use consecutive address nops

    // 12: 0x66 0x0F 0x1F 0x44 0x00 0x00 0x66 0x0F 0x1F 0x44 0x00 0x00
    // 13: 0x0F 0x1F 0x80 0x00 0x00 0x00 0x00 0x66 0x0F 0x1F 0x44 0x00 0x00
    // 14: 0x0F 0x1F 0x80 0x00 0x00 0x00 0x00 0x0F 0x1F 0x80 0x00 0x00 0x00 0x00
    // 15: 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x0F 0x1F 0x80 0x00 0x00 0x00 0x00
    // 16: 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    //     Size prefixes (0x66) are added for larger sizes

    while(i >= 22) {
      i -= 11;
      emit_int24(0x66, 0x66, 0x66);
      addr_nop_8();
    }
    // Generate first nop for size between 21-12
    switch (i) {
      case 21:
        i -= 1;
        emit_int8(0x66); // size prefix
      case 20:
      case 19:
        i -= 1;
        emit_int8(0x66); // size prefix
      case 18:
      case 17:
        i -= 1;
        emit_int8(0x66); // size prefix
      case 16:
      case 15:
        i -= 8;
        addr_nop_8();
        break;
      case 14:
      case 13:
        i -= 7;
        addr_nop_7();
        break;
      case 12:
        i -= 6;
        emit_int8(0x66); // size prefix
        addr_nop_5();
        break;
      default:
        assert(i < 12, " ");
    }

    // Generate second nop for size between 11-1
    switch (i) {
      case 11:
        emit_int8(0x66); // size prefix
      case 10:
        emit_int8(0x66); // size prefix
      case 9:
        emit_int8(0x66); // size prefix
      case 8:
        addr_nop_8();
        break;
      case 7:
        addr_nop_7();
        break;
      case 6:
        emit_int8(0x66); // size prefix
      case 5:
        addr_nop_5();
        break;
      case 4:
        addr_nop_4();
        break;
      case 3:
        // Don't use "0x0F 0x1F 0x00" - need patching safe padding
        emit_int8(0x66); // size prefix
      case 2:
        emit_int8(0x66); // size prefix
      case 1:
        emit_int8((unsigned char)0x90);
                         // nop
        break;
      default:
        assert(i == 0, " ");
    }
    return;
  }

  if (UseAddressNop && VM_Version::is_zx()) {
    //
    // Using multi-bytes nops "0x0F 0x1F [address]" for ZX
    //  1: 0x90
    //  2: 0x66 0x90
    //  3: 0x66 0x66 0x90 (don't use "0x0F 0x1F 0x00" - need patching safe padding)
    //  4: 0x0F 0x1F 0x40 0x00
    //  5: 0x0F 0x1F 0x44 0x00 0x00
    //  6: 0x66 0x0F 0x1F 0x44 0x00 0x00
    //  7: 0x0F 0x1F 0x80 0x00 0x00 0x00 0x00
    //  8: 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    //  9: 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    // 10: 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00
    // 11: 0x66 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00

    // The rest coding is ZX specific - don't use consecutive address nops

    // 12: 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90
    // 13: 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90
    // 14: 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90
    // 15: 0x66 0x66 0x66 0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00 0x66 0x66 0x66 0x90

    while (i >= 15) {
      // For ZX don't generate consecutive address nops (mix with regular nops)
      i -= 15;
      emit_int24(0x66, 0x66, 0x66);
      addr_nop_8();
      emit_int32(0x66, 0x66, 0x66, (unsigned char)0x90);
    }
    switch (i) {
      case 14:
        emit_int8(0x66); // size prefix
      case 13:
        emit_int8(0x66); // size prefix
      case 12:
        addr_nop_8();
        emit_int32(0x66, 0x66, 0x66, (unsigned char)0x90);
        break;
      case 11:
        emit_int8(0x66); // size prefix
      case 10:
        emit_int8(0x66); // size prefix
      case 9:
        emit_int8(0x66); // size prefix
      case 8:
        addr_nop_8();
        break;
      case 7:
        addr_nop_7();
        break;
      case 6:
        emit_int8(0x66); // size prefix
      case 5:
        addr_nop_5();
        break;
      case 4:
        addr_nop_4();
        break;
      case 3:
        // Don't use "0x0F 0x1F 0x00" - need patching safe padding
        emit_int8(0x66); // size prefix
      case 2:
        emit_int8(0x66); // size prefix
      case 1:
        emit_int8((unsigned char)0x90);
                         // nop
        break;
      default:
        assert(i == 0, " ");
    }
    return;
  }

  // Using nops with size prefixes "0x66 0x90".
  // From AMD Optimization Guide:
  //  1: 0x90
  //  2: 0x66 0x90
  //  3: 0x66 0x66 0x90
  //  4: 0x66 0x66 0x66 0x90
  //  5: 0x66 0x66 0x90 0x66 0x90
  //  6: 0x66 0x66 0x90 0x66 0x66 0x90
  //  7: 0x66 0x66 0x66 0x90 0x66 0x66 0x90
  //  8: 0x66 0x66 0x66 0x90 0x66 0x66 0x66 0x90
  //  9: 0x66 0x66 0x90 0x66 0x66 0x90 0x66 0x66 0x90
  // 10: 0x66 0x66 0x66 0x90 0x66 0x66 0x90 0x66 0x66 0x90
  //
  while (i > 12) {
    i -= 4;
    emit_int32(0x66, 0x66, 0x66, (unsigned char)0x90);
  }
  // 1 - 12 nops
  if (i > 8) {
    if (i > 9) {
      i -= 1;
      emit_int8(0x66);
    }
    i -= 3;
    emit_int24(0x66, 0x66, (unsigned char)0x90);
  }
  // 1 - 8 nops
  if (i > 4) {
    if (i > 6) {
      i -= 1;
      emit_int8(0x66);
    }
    i -= 3;
    emit_int24(0x66, 0x66, (unsigned char)0x90);
  }
  switch (i) {
    case 4:
      emit_int8(0x66);
    case 3:
      emit_int8(0x66);
    case 2:
      emit_int8(0x66);
    case 1:
      emit_int8((unsigned char)0x90);
      break;
    default:
      assert(i == 0, " ");
  }
}

void Assembler::notl(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xF7, (0xD0 | encode));
}

void Assembler::enotl(Register dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes);
  emit_int16((unsigned char)0xF7, (0xD0 | encode));
}

void Assembler::eorw(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith(0x0B, 0xC0, src1, src2);
}

void Assembler::orl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, rcx, dst, imm32);
}

void Assembler::eorl(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, rcx, src, imm32);
}

void Assembler::orl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xC8, dst, imm32);
}

void Assembler::eorl(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x81, 0xC8, no_flags);
}

void Assembler::orl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x0B);
  emit_operand(dst, src, 0);
}

void Assembler::eorl(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x0B, no_flags);
}

void Assembler::orl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x0B, 0xC0, dst, src);
}

void Assembler::eorl(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith(0x0B, 0xC0, src1, src2);
}

void Assembler::orl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8(0x09);
  emit_operand(src, dst, 0);
}

void Assembler::eorl(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x09);
  emit_operand(src2, src1, 0);
}

void Assembler::orb(Address dst, int imm8) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0x80);
  emit_operand(rcx, dst, 1);
  emit_int8(imm8);
}

void Assembler::eorb(Register dst, Address src, int imm8, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_8bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0x80);
  emit_operand(rcx, src, 1);
  emit_int8(imm8);
}

void Assembler::orb(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src, true);
  emit_int8(0x08);
  emit_operand(src, dst, 0);
}

void Assembler::eorb(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_8bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x08);
  emit_operand(src2, src1, 0);
}

void Assembler::packsswb(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x63, (0xC0 | encode));
}

void Assembler::vpacksswb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "some form of AVX must be enabled");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x63, (0xC0 | encode));
}

void Assembler::packssdw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6B, (0xC0 | encode));
}

void Assembler::vpackssdw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "some form of AVX must be enabled");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6B, (0xC0 | encode));
}

void Assembler::packuswb(XMMRegister dst, Address src) {
  assert((UseAVX > 0), "SSE mode requires address alignment 16 bytes");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x67);
  emit_operand(dst, src, 0);
}

void Assembler::packuswb(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x67, (0xC0 | encode));
}

void Assembler::vpackuswb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "some form of AVX must be enabled");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x67, (0xC0 | encode));
}

void Assembler::packusdw(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x2B, (0xC0 | encode));
}

void Assembler::vpackusdw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "some form of AVX must be enabled");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x2B, (0xC0 | encode));
}

void Assembler::vpermq(XMMRegister dst, XMMRegister src, int imm8, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(vector_len != AVX_128bit, "");
  // VEX.256.66.0F3A.W1 00 /r ib
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x00, (0xC0 | encode), imm8);
}

void Assembler::vpermq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_256bit ? VM_Version::supports_avx512vl() :
         vector_len == AVX_512bit ? VM_Version::supports_evex()     : false, "not supported");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x36, (0xC0 | encode));
}

void Assembler::vpermb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8D, (0xC0 | encode));
}

void Assembler::vpermb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x8D);
  emit_operand(dst, src, 0);
}

void Assembler::vpermw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx512vlbw() :
         vector_len == AVX_256bit ? VM_Version::supports_avx512vlbw() :
         vector_len == AVX_512bit ? VM_Version::supports_avx512bw()   : false, "not supported");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8D, (0xC0 | encode));
}

void Assembler::vpermd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert((vector_len == AVX_256bit && VM_Version::supports_avx2()) ||
         (vector_len == AVX_512bit && VM_Version::supports_evex()), "");
  // VEX.NDS.256.66.0F38.W0 36 /r
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x36, (0xC0 | encode));
}

void Assembler::vpermd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert((vector_len == AVX_256bit && VM_Version::supports_avx2()) ||
         (vector_len == AVX_512bit && VM_Version::supports_evex()), "");
  // VEX.NDS.256.66.0F38.W0 36 /r
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x36);
  emit_operand(dst, src, 0);
}

void Assembler::vpermps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert((vector_len == AVX_256bit && VM_Version::supports_avx2()) ||
         (vector_len == AVX_512bit && VM_Version::supports_evex()), "");
  // VEX.NDS.XXX.66.0F38.W0 16 /r
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x16, (0xC0 | encode));
}

void Assembler::vperm2i128(XMMRegister dst,  XMMRegister nds, XMMRegister src, int imm8) {
  assert(VM_Version::supports_avx2(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x46, (0xC0 | encode), imm8);
}

void Assembler::vperm2f128(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x06, (0xC0 | encode), imm8);
}

void Assembler::vpermilps(XMMRegister dst, XMMRegister src, int imm8, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x04, (0xC0 | encode), imm8);
}

void Assembler::vpermilps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x0C, (0xC0 | encode));
}

void Assembler::vpermilpd(XMMRegister dst, XMMRegister src, int imm8, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ VM_Version::supports_evex(),/* legacy_mode */ false,/* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x05, (0xC0 | encode), imm8);
}

void Assembler::vpermpd(XMMRegister dst, XMMRegister src, int imm8, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x01, (0xC0 | encode), imm8);
}

void Assembler::evpmultishiftqb(XMMRegister dst, XMMRegister ctl, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), ctl->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x83, (unsigned char)(0xC0 | encode));
}

void Assembler::pause() {
  emit_int16((unsigned char)0xF3, (unsigned char)0x90);
}

void Assembler::ud2() {
  emit_int16(0x0F, 0x0B);
}

void Assembler::pcmpestri(XMMRegister dst, Address src, int imm8) {
  assert(VM_Version::supports_sse4_2(), "");
  assert(!needs_eevex(src.base(), src.index()), "does not support extended gprs as BASE or INDEX of address operand");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x61);
  emit_operand(dst, src, 1);
  emit_int8(imm8);
}

void Assembler::pcmpestri(XMMRegister dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_2(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x61, (0xC0 | encode), imm8);
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::pcmpeqb(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x74, (0xC0 | encode));
}

void Assembler::vpcmpCCbwd(XMMRegister dst, XMMRegister nds, XMMRegister src, int cond_encoding, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(cond_encoding, (0xC0 | encode));
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::vpcmpeqb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x74, (0xC0 | encode));
}

void Assembler::vpcmpeqb(XMMRegister dst, XMMRegister src1, Address src2, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x74);
  emit_operand(dst, src2, 0);
}

// In this context, kdst is written the mask used to process the equal components
void Assembler::evpcmpeqb(KRegister kdst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x74, (0xC0 | encode));
}

void Assembler::evpcmpgtb(KRegister kdst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x64);
  emit_operand(as_Register(dst_enc), src, 0);
}

void Assembler::evpcmpgtb(KRegister kdst, KRegister mask, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x64);
  emit_operand(as_Register(dst_enc), src, 0);
}

void Assembler::evpcmpub(KRegister kdst, XMMRegister nds, XMMRegister src, ComparisonPredicate vcc, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x3E, (0xC0 | encode), vcc);
}

void Assembler::evpcmpuw(KRegister kdst, XMMRegister nds, XMMRegister src, ComparisonPredicate vcc, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x3E, (0xC0 | encode), vcc);
}

void Assembler::evpcmpud(KRegister kdst, XMMRegister nds, XMMRegister src, ComparisonPredicate vcc, int vector_len) {
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x1E, (0xC0 | encode), vcc);
}

void Assembler::evpcmpuq(KRegister kdst, XMMRegister nds, XMMRegister src, ComparisonPredicate vcc, int vector_len) {
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x1E, (0xC0 | encode), vcc);
}

void Assembler::evpcmpuw(KRegister kdst, XMMRegister nds, Address src, ComparisonPredicate vcc, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), kdst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x3E);
  emit_operand(as_Register(dst_enc), src, 1);
  emit_int8(vcc);
}

void Assembler::evpcmpeqb(KRegister kdst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x74);
  emit_operand(as_Register(dst_enc), src, 0);
}

void Assembler::evpcmpeqb(KRegister kdst, KRegister mask, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(src, nds->encoding(), kdst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x74);
  emit_operand(as_Register(kdst->encoding()), src, 0);
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::pcmpeqw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x75, (0xC0 | encode));
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::vpcmpeqw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x75, (0xC0 | encode));
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::vpcmpeqw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x75);
  emit_operand(dst, src, 0);
}

// In this context, kdst is written the mask used to process the equal components
void Assembler::evpcmpeqw(KRegister kdst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x75, (0xC0 | encode));
}

void Assembler::evpcmpeqw(KRegister kdst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x75);
  emit_operand(as_Register(dst_enc), src, 0);
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::pcmpeqd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x76, (0xC0 | encode));
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::vpcmpeqd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x76, (0xC0 | encode));
}

// In this context, kdst is written the mask used to process the equal components
void Assembler::evpcmpeqd(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x76, (0xC0 | encode));
}

void Assembler::evpcmpeqd(KRegister kdst, KRegister mask, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x76);
  emit_operand(as_Register(dst_enc), src, 0);
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::pcmpeqq(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x29, (0xC0 | encode));
}

void Assembler::evpcmpeqq(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x29, (0xC0 | encode));
}

void Assembler::vpcmpCCq(XMMRegister dst, XMMRegister nds, XMMRegister src, int cond_encoding, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(cond_encoding, (0xC0 | encode));
}

// In this context, the dst vector contains the components that are equal, non equal components are zeroed in dst
void Assembler::vpcmpeqq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x29, (0xC0 | encode));
}

// In this context, kdst is written the mask used to process the equal components
void Assembler::evpcmpeqq(KRegister kdst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.reset_is_clear_context();
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x29, (0xC0 | encode));
}

// In this context, kdst is written the mask used to process the equal components
void Assembler::evpcmpeqq(KRegister kdst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.reset_is_clear_context();
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x29);
  emit_operand(as_Register(dst_enc), src, 0);
}

void Assembler::pcmpgtq(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x37, (0xC0 | encode));
}

void Assembler::pmovmskb(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD7, (0xC0 | encode));
}

void Assembler::vpmovmskb(Register dst, XMMRegister src, int vec_enc) {
  assert((VM_Version::supports_avx() && vec_enc == AVX_128bit) ||
         (VM_Version::supports_avx2() && vec_enc  == AVX_256bit), "");
  InstructionAttr attributes(vec_enc, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD7, (0xC0 | encode));
}

void Assembler::vmovmskps(Register dst, XMMRegister src, int vec_enc) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vec_enc, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x50, (0xC0 | encode));
}

void Assembler::vmovmskpd(Register dst, XMMRegister src, int vec_enc) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vec_enc, /* rex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x50, (0xC0 | encode));
}


void Assembler::pextrd(Register dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(src, xnoreg, as_XMMRegister(dst->encoding()), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x16, (0xC0 | encode), imm8);
}

void Assembler::pextrd(Address dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(src, xnoreg, dst, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x16);
  emit_operand(src, dst, 1);
  emit_int8(imm8);
}

void Assembler::pextrq(Register dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(src, xnoreg, as_XMMRegister(dst->encoding()), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x16, (0xC0 | encode), imm8);
}

void Assembler::pextrq(Address dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  simd_prefix(src, xnoreg, dst, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x16);
  emit_operand(src, dst, 1);
  emit_int8(imm8);
}

void Assembler::pextrw(Register dst, XMMRegister src, int imm8) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC5, (0xC0 | encode), imm8);
}

void Assembler::pextrw(Address dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_16bit);
  simd_prefix(src, xnoreg, dst, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x15);
  emit_operand(src, dst, 1);
  emit_int8(imm8);
}

void Assembler::pextrb(Register dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(src, xnoreg, as_XMMRegister(dst->encoding()), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x14, (0xC0 | encode), imm8);
}

void Assembler::pextrb(Address dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_8bit);
  simd_prefix(src, xnoreg, dst, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x14);
  emit_operand(src, dst, 1);
  emit_int8(imm8);
}

void Assembler::pinsrd(XMMRegister dst, Register src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x22, (0xC0 | encode), imm8);
}

void Assembler::pinsrd(XMMRegister dst, Address src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x22);
  emit_operand(dst, src, 1);
  emit_int8(imm8);
}

void Assembler::vpinsrd(XMMRegister dst, XMMRegister nds, Register src, int imm8) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x22, (0xC0 | encode), imm8);
}

void Assembler::pinsrq(XMMRegister dst, Register src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x22, (0xC0 | encode), imm8);
}

void Assembler::pinsrq(XMMRegister dst, Address src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x22);
  emit_operand(dst, src, 1);
  emit_int8(imm8);
}

void Assembler::vpinsrq(XMMRegister dst, XMMRegister nds, Register src, int imm8) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(),  nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x22, (0xC0 | encode), imm8);
}

void Assembler::pinsrw(XMMRegister dst, Register src, int imm8) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int24((unsigned char)0xC4, (0xC0 | encode), imm8);
}

void Assembler::pinsrw(XMMRegister dst, Address src, int imm8) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_16bit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xC4);
  emit_operand(dst, src, 1);
  emit_int8(imm8);
}

void Assembler::vpinsrw(XMMRegister dst, XMMRegister nds, Register src, int imm8) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int24((unsigned char)0xC4, (0xC0 | encode), imm8);
}

void Assembler::pinsrb(XMMRegister dst, Address src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_8bit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x20);
  emit_operand(dst, src, 1);
  emit_int8(imm8);
}

void Assembler::pinsrb(XMMRegister dst, Register src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x20, (0xC0 | encode), imm8);
}

void Assembler::vpinsrb(XMMRegister dst, XMMRegister nds, Register src, int imm8) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24(0x20, (0xC0 | encode), imm8);
}

void Assembler::insertps(XMMRegister dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x21, (0xC0 | encode), imm8);
}

void Assembler::vinsertps(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x21, (0xC0 | encode), imm8);
}

void Assembler::pmovzxbw(XMMRegister dst, Address src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x30);
  emit_operand(dst, src, 0);
}

void Assembler::pmovzxbw(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x30, (0xC0 | encode));
}

void Assembler::pmovsxbw(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x20, (0xC0 | encode));
}

void Assembler::pmovzxdq(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x35, (0xC0 | encode));
}

void Assembler::pmovsxbd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x21, (0xC0 | encode));
}

void Assembler::pmovzxbd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x31, (0xC0 | encode));
}

void Assembler::pmovsxbq(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x22, (0xC0 | encode));
}

void Assembler::pmovsxwd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x23, (0xC0 | encode));
}

void Assembler::vpmovzxbw(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  assert(dst != xnoreg, "sanity");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x30);
  emit_operand(dst, src, 0);
}

void Assembler::vpmovzxbw(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
  vector_len == AVX_256bit? VM_Version::supports_avx2() :
  vector_len == AVX_512bit? VM_Version::supports_avx512bw() : 0, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x30, (unsigned char) (0xC0 | encode));
}

void Assembler::vpmovsxbw(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
  vector_len == AVX_256bit? VM_Version::supports_avx2() :
  vector_len == AVX_512bit? VM_Version::supports_avx512bw() : 0, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x20, (0xC0 | encode));
}

void Assembler::evpmovzxbw(XMMRegister dst, KRegister mask, Address src, int vector_len) {
  assert(VM_Version::supports_avx512vlbw(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x30);
  emit_operand(dst, src, 0);
}

void Assembler::evpmovzxbd(XMMRegister dst, KRegister mask, Address src, int vector_len) {
  assert(VM_Version::supports_avx512vl(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_QVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x31);
  emit_operand(dst, src, 0);
}

void Assembler::evpmovzxbd(XMMRegister dst, Address src, int vector_len) {
  evpmovzxbd(dst, k0, src, vector_len);
}

void Assembler::evpandd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  // Encoding: EVEX.NDS.XXX.66.0F.W0 DB /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDB, (0xC0 | encode));
}

void Assembler::vpmovzxdq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len > AVX_128bit ? VM_Version::supports_avx2() : VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x35, (0xC0 | encode));
}

void Assembler::vpmovzxbd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len > AVX_128bit ? VM_Version::supports_avx2() : VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x31, (0xC0 | encode));
}

void Assembler::vpmovzxbq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len > AVX_128bit ? VM_Version::supports_avx2() : VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x32, (0xC0 | encode));
}

void Assembler::vpmovsxbd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
             VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x21, (0xC0 | encode));
}

void Assembler::vpmovsxbq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
             VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x22, (0xC0 | encode));
}

void Assembler::vpmovsxwd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
             VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x23, (0xC0 | encode));
}

void Assembler::vpmovsxwq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
             VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x24, (0xC0 | encode));
}

void Assembler::vpmovsxdq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
             VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x25, (0xC0 | encode));
}

void Assembler::evpmovwb(Address dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vlbw(), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x30);
  emit_operand(src, dst, 0);
}

void Assembler::evpmovwb(Address dst, KRegister mask, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vlbw(), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_HVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x30);
  emit_operand(src, dst, 0);
}

void Assembler::evpmovdb(Address dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_QVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x31);
  emit_operand(src, dst, 0);
}

void Assembler::vpmovzxwd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
  vector_len == AVX_256bit? VM_Version::supports_avx2() :
  vector_len == AVX_512bit? VM_Version::supports_evex() : 0, " ");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x33, (0xC0 | encode));
}

void Assembler::vpmovzxwq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
  vector_len == AVX_256bit? VM_Version::supports_avx2() :
  vector_len == AVX_512bit? VM_Version::supports_evex() : 0, " ");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x34, (0xC0 | encode));
}

void Assembler::pmaddwd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::vpmaddwd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
    (vector_len == AVX_256bit ? VM_Version::supports_avx2() :
    (vector_len == AVX_512bit ? VM_Version::supports_evex() : 0)), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, nds, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::vpmaddubsw(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
       vector_len == AVX_256bit? VM_Version::supports_avx2() :
       vector_len == AVX_512bit? VM_Version::supports_avx512bw() : 0, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, src1, src2, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x04, (0xC0 | encode));
}

void Assembler::vpmadd52luq(XMMRegister dst, XMMRegister src1, Address src2, int vector_len) {
  assert ((VM_Version::supports_avxifma() && vector_len <= AVX_256bit) || (VM_Version::supports_avx512ifma() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl())), "");

  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);

  if (VM_Version::supports_avx512ifma()) {
    attributes.set_is_evex_instruction();
    attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  }
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xB4);
  emit_operand(dst, src2, 0);
}

void Assembler::vpmadd52luq(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  assert ((VM_Version::supports_avxifma() && vector_len <= AVX_256bit) || (VM_Version::supports_avx512ifma() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl())), "");

  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);

  if (VM_Version::supports_avx512ifma()) {
    attributes.set_is_evex_instruction();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB4, (0xC0 | encode));
}

void Assembler::evpmadd52luq(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  evpmadd52luq(dst, k0, src1, src2, false, vector_len);
}

void Assembler::evpmadd52luq(XMMRegister dst, KRegister mask, XMMRegister src1, XMMRegister src2, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512ifma(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }

  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB4, (0xC0 | encode));
}

void Assembler::vpmadd52huq(XMMRegister dst, XMMRegister src1, Address src2, int vector_len) {
  assert ((VM_Version::supports_avxifma() && vector_len <= AVX_256bit) || (VM_Version::supports_avx512ifma() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl())), "");

  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);

  if (VM_Version::supports_avx512ifma()) {
    attributes.set_is_evex_instruction();
    attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  }
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xB5);
  emit_operand(dst, src2, 0);
}

void Assembler::vpmadd52huq(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  assert ((VM_Version::supports_avxifma() && vector_len <= AVX_256bit) || (VM_Version::supports_avx512ifma() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl())), "");

  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);

  if (VM_Version::supports_avx512ifma()) {
    attributes.set_is_evex_instruction();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB5, (0xC0 | encode));
}

void Assembler::evpmadd52huq(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  evpmadd52huq(dst, k0, src1, src2, false, vector_len);
}

void Assembler::evpmadd52huq(XMMRegister dst, KRegister mask, XMMRegister src1, XMMRegister src2, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512ifma(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }

  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB5, (0xC0 | encode));
}

void Assembler::evpdpwssd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(VM_Version::supports_avx512_vnni(), "must support vnni");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x52, (0xC0 | encode));
}

// generic
void Assembler::pop(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int8(0x58 | encode);
}

void Assembler::popcntl(Register dst, Address src) {
  assert(VM_Version::supports_popcnt(), "must support");
  InstructionMark im(this);
  emit_int8((unsigned char)0xF3);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xB8);
  emit_operand(dst, src, 0);
}

void Assembler::epopcntl(Register dst, Address src, bool no_flags) {
  assert(VM_Version::supports_popcnt(), "must support");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0x88);
  emit_operand(dst, src, 0);
}

void Assembler::popcntl(Register dst, Register src) {
  assert(VM_Version::supports_popcnt(), "must support");
  emit_int8((unsigned char)0xF3);
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xB8, 0xC0, encode);
}

void Assembler::epopcntl(Register dst, Register src, bool no_flags) {
  assert(VM_Version::supports_popcnt(), "must support");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0x88, (0xC0 | encode));
}

void Assembler::evpopcntb(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_bitalg(), "must support avx512bitalg feature");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x54, (0xC0 | encode));
}

void Assembler::evpopcntw(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_bitalg(), "must support avx512bitalg feature");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x54, (0xC0 | encode));
}

void Assembler::evpopcntd(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vpopcntdq(), "must support vpopcntdq feature");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x55, (0xC0 | encode));
}

void Assembler::evpopcntq(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vpopcntdq(), "must support vpopcntdq feature");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x55, (0xC0 | encode));
}

void Assembler::popf() {
  emit_int8((unsigned char)0x9D);
}

void Assembler::prefetchnta(Address src) {
  InstructionMark im(this);
  prefix(src, true /* is_map1 */);
  emit_int8(0x18);
  emit_operand(rax, src, 0); // 0, src
}

void Assembler::prefetchr(Address src) {
  assert(VM_Version::supports_3dnow_prefetch(), "must support");
  InstructionMark im(this);
  prefix(src, true /* is_map1 */);
  emit_int8(0x0D);
  emit_operand(rax, src, 0); // 0, src
}

void Assembler::prefetcht0(Address src) {
  InstructionMark im(this);
  prefix(src, true /* is_map1 */);
  emit_int8(0x18);
  emit_operand(rcx, src, 0); // 1, src
}

void Assembler::prefetcht1(Address src) {
  InstructionMark im(this);
  prefix(src, true /* is_map1 */);
  emit_int8(0x18);
  emit_operand(rdx, src, 0); // 2, src
}

void Assembler::prefetcht2(Address src) {
  InstructionMark im(this);
  prefix(src, true /* is_map1 */);
  emit_int8(0x18);
  emit_operand(rbx, src, 0); // 3, src
}

void Assembler::prefetchw(Address src) {
  assert(VM_Version::supports_3dnow_prefetch(), "must support");
  InstructionMark im(this);
  prefix(src, true /* is_map1 */);
  emit_int8(0x0D);
  emit_operand(rcx, src, 0); // 1, src
}

void Assembler::prefix(Prefix p) {
  emit_int8(p);
}

void Assembler::prefix16(int prefix) {
  assert(UseAPX, "APX features not enabled");
  emit_int8((prefix & 0xff00) >> 8);
  emit_int8(prefix & 0xff);
}

void Assembler::pshufb(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_ssse3(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x00, (0xC0 | encode));
}

void Assembler::evpshufb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = simd_prefix_and_encode(dst, nds, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x00, (0xC0 | encode));
}

void Assembler::vpshufb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
         vector_len == AVX_256bit? VM_Version::supports_avx2() :
         vector_len == AVX_512bit? VM_Version::supports_avx512bw() : 0, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, nds, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x00, (0xC0 | encode));
}

void Assembler::vpshufb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
         vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, nds, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x00);
  emit_operand(dst, src, 0);
}

void Assembler::pshufb(XMMRegister dst, Address src) {
  assert(VM_Version::supports_ssse3(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x00);
  emit_operand(dst, src, 0);
}

void Assembler::pshufd(XMMRegister dst, XMMRegister src, int mode) {
  assert(isByte(mode), "invalid value");
  int vector_len = VM_Version::supports_avx512novl() ? AVX_512bit : AVX_128bit;
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x70, (0xC0 | encode), mode & 0xFF);
}

void Assembler::vpshufd(XMMRegister dst, XMMRegister src, int mode, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
         (vector_len == AVX_256bit? VM_Version::supports_avx2() :
         (vector_len == AVX_512bit? VM_Version::supports_evex() : 0)), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x70, (0xC0 | encode), mode & 0xFF);
}

void Assembler::pshufd(XMMRegister dst, Address src, int mode) {
  assert(isByte(mode), "invalid value");
  assert((UseAVX > 0), "SSE mode requires address alignment 16 bytes");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x70);
  emit_operand(dst, src, 1);
  emit_int8(mode & 0xFF);
}

void Assembler::pshufhw(XMMRegister dst, XMMRegister src, int mode) {
  assert(isByte(mode), "invalid value");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int24(0x70, (0xC0 | encode), mode & 0xFF);
}

void Assembler::vpshufhw(XMMRegister dst, XMMRegister src, int mode, int vector_len) {
    assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
            (vector_len == AVX_256bit ? VM_Version::supports_avx2() :
            (vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : false)), "");
    InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
    int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
    emit_int24(0x70, (0xC0 | encode), mode & 0xFF);
}

void Assembler::pshuflw(XMMRegister dst, XMMRegister src, int mode) {
  assert(isByte(mode), "invalid value");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int24(0x70, (0xC0 | encode), mode & 0xFF);
}

void Assembler::pshuflw(XMMRegister dst, Address src, int mode) {
  assert(isByte(mode), "invalid value");
  assert((UseAVX > 0), "SSE mode requires address alignment 16 bytes");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x70);
  emit_operand(dst, src, 1);
  emit_int8(mode & 0xFF);
}

void Assembler::vpshuflw(XMMRegister dst, XMMRegister src, int mode, int vector_len) {
    assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
            (vector_len == AVX_256bit ? VM_Version::supports_avx2() :
            (vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : false)), "");
    InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
    int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
    emit_int24(0x70, (0xC0 | encode), mode & 0xFF);
}

void Assembler::evshufi64x2(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_256bit || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x43, (0xC0 | encode), imm8 & 0xFF);
}

void Assembler::shufpd(XMMRegister dst, XMMRegister src, int imm8) {
  assert(isByte(imm8), "invalid value");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC6, (0xC0 | encode), imm8 & 0xFF);
}

void Assembler::vshufpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8, int vector_len) {
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC6, (0xC0 | encode), imm8 & 0xFF);
}

void Assembler::shufps(XMMRegister dst, XMMRegister src, int imm8) {
  assert(isByte(imm8), "invalid value");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC6, (0xC0 | encode), imm8 & 0xFF);
}

void Assembler::vshufps(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8, int vector_len) {
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC6, (0xC0 | encode), imm8 & 0xFF);
}

void Assembler::psrldq(XMMRegister dst, int shift) {
  // Shift left 128 bit value in dst XMMRegister by shift number of bytes.
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(xmm3, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift);
}

void Assembler::vpsrldq(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
         vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : 0, "");
  InstructionAttr attributes(vector_len, /*vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(xmm3->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::pslldq(XMMRegister dst, int shift) {
  // Shift left 128 bit value in dst XMMRegister by shift number of bytes.
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM7 is for /7 encoding: 66 0F 73 /7 ib
  int encode = simd_prefix_and_encode(xmm7, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift);
}

void Assembler::vpslldq(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
         vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : 0, "");
  InstructionAttr attributes(vector_len, /*vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(xmm7->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::ptest(XMMRegister dst, Address src) {
  assert(VM_Version::supports_sse4_1(), "");
  assert((UseAVX > 0), "SSE mode requires address alignment 16 bytes");
  assert(!needs_eevex(src.base(), src.index()), "does not support extended gprs");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x17);
  emit_operand(dst, src, 0);
}

void Assembler::ptest(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1() || VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x17);
  emit_int8((0xC0 | encode));
}

void Assembler::vptest(XMMRegister dst, Address src) {
  assert(VM_Version::supports_avx(), "");
  assert(!needs_eevex(src.base(), src.index()), "does not support extended gprs");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  assert(dst != xnoreg, "sanity");
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x17);
  emit_operand(dst, src, 0);
}

void Assembler::vptest(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_256bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x17, (0xC0 | encode));
}

void Assembler::vptest(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x17, (0xC0 | encode));
}

void Assembler::vtestps(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x0E, (0xC0 | encode));
}

void Assembler::evptestmb(KRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_512bit ? VM_Version::supports_avx512bw() : VM_Version::supports_avx512vlbw(), "");
  // Encoding: EVEX.NDS.XXX.66.0F38.W0 DB /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x26, (0xC0 | encode));
}

void Assembler::evptestmd(KRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx512vl(), "");
  // Encoding: EVEX.NDS.XXX.66.0F38.W0 DB /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x27, (0xC0 | encode));
}

void Assembler::evptestnmd(KRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx512vl(), "");
  // Encoding: EVEX.NDS.XXX.F3.0F38.W0 DB /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x27, (0xC0 | encode));
}

void Assembler::punpcklbw(XMMRegister dst, Address src) {
  assert((UseAVX > 0), "SSE mode requires address alignment 16 bytes");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_vlbw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x60);
  emit_operand(dst, src, 0);
}

void Assembler::punpcklbw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_vlbw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x60, (0xC0 | encode));
}

void Assembler::punpckldq(XMMRegister dst, Address src) {
  assert((UseAVX > 0), "SSE mode requires address alignment 16 bytes");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x62);
  emit_operand(dst, src, 0);
}

void Assembler::punpckldq(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x62, (0xC0 | encode));
}

void Assembler::punpcklqdq(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6C, (0xC0 | encode));
}

void Assembler::evpunpcklqdq(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  evpunpcklqdq(dst, k0, src1, src2, false, vector_len);
}

void Assembler::evpunpcklqdq(XMMRegister dst, KRegister mask, XMMRegister src1, XMMRegister src2, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires AVX512F");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }

  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6C, (0xC0 | encode));
}

void Assembler::evpunpckhqdq(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  evpunpckhqdq(dst, k0, src1, src2, false, vector_len);
}

void Assembler::evpunpckhqdq(XMMRegister dst, KRegister mask, XMMRegister src1, XMMRegister src2, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires AVX512F");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }

  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6D, (0xC0 | encode));
}

void Assembler::push2(Register src1, Register src2, bool with_ppx) {
  assert(VM_Version::supports_apx_f(), "requires APX");
  InstructionAttr attributes(0, /* rex_w */ with_ppx, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  /* EVEX.BASE */
  int src_enc = src1->encoding();
  /* EVEX.VVVV */
  int nds_enc = src2->encoding();

  bool vex_b = (src_enc & 8) == 8;
  bool evex_v = (nds_enc >= 16);
  bool evex_b = (src_enc >= 16);

  // EVEX.ND = 1;
  attributes.set_extended_context();
  attributes.set_is_evex_instruction();
  set_attributes(&attributes);

  evex_prefix(0, vex_b, 0, 0, evex_b, evex_v, false /*eevex_x*/, nds_enc, VEX_SIMD_NONE, /* map4 */ VEX_OPCODE_0F_3C);
  emit_int16(0xFF, (0xC0 | (0x6 << 3) | (src_enc & 7)));
}

void Assembler::pop2(Register src1, Register src2, bool with_ppx) {
  assert(VM_Version::supports_apx_f(), "requires APX");
  InstructionAttr attributes(0, /* rex_w */ with_ppx, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  /* EVEX.BASE */
  int src_enc = src1->encoding();
  /* EVEX.VVVV */
  int nds_enc = src2->encoding();

  bool vex_b = (src_enc & 8) == 8;
  bool evex_v = (nds_enc >= 16);
  bool evex_b = (src_enc >= 16);

  // EVEX.ND = 1;
  attributes.set_extended_context();
  attributes.set_is_evex_instruction();
  set_attributes(&attributes);

  evex_prefix(0, vex_b, 0, 0, evex_b, evex_v, false /*eevex_x*/, nds_enc, VEX_SIMD_NONE, /* map4 */ VEX_OPCODE_0F_3C);
  emit_int16(0x8F, (0xC0 | (src_enc & 7)));
}

void Assembler::push2p(Register src1, Register src2) {
  push2(src1, src2, true);
}

void Assembler::pop2p(Register src1, Register src2) {
  pop2(src1, src2, true);
}

void Assembler::pushp(Register src) {
  assert(VM_Version::supports_apx_f(), "requires APX");
  int encode = prefixq_and_encode_rex2(src->encoding());
  emit_int8(0x50 | encode);
}

void Assembler::popp(Register dst) {
  assert(VM_Version::supports_apx_f(), "requires APX");
  int encode = prefixq_and_encode_rex2(dst->encoding());
  emit_int8((unsigned char)0x58 | encode);
}

void Assembler::push(int32_t imm32) {
  // in 64bits we push 64bits onto the stack but only
  // take a 32bit immediate
  emit_int8(0x68);
  emit_int32(imm32);
}

void Assembler::push(Register src) {
  int encode = prefix_and_encode(src->encoding());
  emit_int8(0x50 | encode);
}

void Assembler::pushf() {
  emit_int8((unsigned char)0x9C);
}

void Assembler::rcll(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  int encode = prefix_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xD0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xD0 | encode), imm8);
  }
}

void Assembler::ercll(Register dst, Register src, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xD0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xD0 | encode), imm8);
  }
}

void Assembler::rcpps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x53, (0xC0 | encode));
}

void Assembler::rcpss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x53, (0xC0 | encode));
}

void Assembler::rdtsc() {
  emit_int16(0x0F, 0x31);
}

// copies data from [esi] to [edi] using rcx pointer sized words
// generic
void Assembler::rep_mov() {
  // REP
  // MOVSQ
  emit_int24((unsigned char)0xF3, REX_W, (unsigned char)0xA5);
}

// sets rcx bytes with rax, value at [edi]
void Assembler::rep_stosb() {
  // REP
  // STOSB
  emit_int24((unsigned char)0xF3, REX_W, (unsigned char)0xAA);
}

// sets rcx pointer sized words with rax, value at [edi]
// generic
void Assembler::rep_stos() {
  // REP
  // STOSQ
  emit_int24((unsigned char)0xF3, REX_W, (unsigned char)0xAB);
}

// scans rcx pointer sized words at [edi] for occurrence of rax,
// generic
void Assembler::repne_scan() { // repne_scan
  // SCASQ
  emit_int24((unsigned char)0xF2, REX_W, (unsigned char)0xAF);
}

// scans rcx 4 byte words at [edi] for occurrence of rax,
// generic
void Assembler::repne_scanl() { // repne_scan
  // SCASL
  emit_int16((unsigned char)0xF2, (unsigned char)0xAF);
}

void Assembler::ret(int imm16) {
  if (imm16 == 0) {
    emit_int8((unsigned char)0xC3);
  } else {
    emit_int8((unsigned char)0xC2);
    emit_int16(imm16);
  }
}

void Assembler::roll(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  int encode = prefix_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xC0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xc0 | encode), imm8);
  }
}

void Assembler::eroll(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
     emit_int16((unsigned char)0xD1, (0xC0 | encode));
   } else {
     emit_int24((unsigned char)0xC1, (0xc0 | encode), imm8);
   }
}

void Assembler::roll(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xC0 | encode));
}

void Assembler::eroll(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xD3, (0xC0 | encode));
}

void Assembler::rorl(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  int encode = prefix_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xC8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xc8 | encode), imm8);
  }
}

void Assembler::erorl(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
     emit_int16((unsigned char)0xD1, (0xC8 | encode));
   } else {
     emit_int24((unsigned char)0xC1, (0xc8 | encode), imm8);
   }
}

void Assembler::rorl(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xC8 | encode));
}

void Assembler::erorl(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xD3, (0xC8 | encode));
}

void Assembler::rorq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xC8 | encode));
}

void Assembler::erorq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xD3, (0xC8 | encode));
}

void Assembler::rorq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xC8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xc8 | encode), imm8);
  }
}

void Assembler::erorq(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  if (imm8 == 1) {
     emit_int16((unsigned char)0xD1, (0xC8 | encode));
   } else {
     emit_int24((unsigned char)0xC1, (0xC8 | encode), imm8);
   }
}

void Assembler::rolq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xC0 | encode));
}

void Assembler::erolq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xD3, (0xC0 | encode));
}

void Assembler::rolq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xC0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xc0 | encode), imm8);
  }
}

void Assembler::erolq(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  if (imm8 == 1) {
     emit_int16((unsigned char)0xD1, (0xC0 | encode));
   } else {
     emit_int24((unsigned char)0xC1, (0xc0 | encode), imm8);
   }
}

void Assembler::sall(Address dst, int imm8) {
  InstructionMark im(this);
  assert(isShiftCount(imm8), "illegal shift count");
  prefix(dst);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(4), dst, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(4), dst, 1);
    emit_int8(imm8);
  }
}

void Assembler::esall(Register dst, Address src, int imm8, bool no_flags) {
  InstructionMark im(this);
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(4), src, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(4), src, 1);
    emit_int8(imm8);
  }
}

void Assembler::sall(Address dst) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(4), dst, 0);
}

void Assembler::esall(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(4), src, 0);
}

void Assembler::sall(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  int encode = prefix_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::esall(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::sall(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::esall(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::sarl(Address dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionMark im(this);
  prefix(dst);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(7), dst, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(7), dst, 1);
    emit_int8(imm8);
  }
}

void Assembler::esarl(Register dst, Address src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(7), src, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(7), src, 1);
    emit_int8(imm8);
  }
}

void Assembler::sarl(Address dst) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(7), dst, 0);
}

void Assembler::esarl(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(7), src, 0);
}

void Assembler::sarl(Register dst, int imm8) {
  int encode = prefix_and_encode(dst->encoding());
  assert(isShiftCount(imm8), "illegal shift count");
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xF8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xF8 | encode), imm8);
  }
}

void Assembler::esarl(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xF8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xF8 | encode), imm8);
  }
}

void Assembler::sarl(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xF8 | encode));
}

void Assembler::esarl(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xD3, (0xF8 | encode));
}

void Assembler::sbbl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, rbx, dst, imm32);
}

void Assembler::sbbl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xD8, dst, imm32);
}

void Assembler::sbbl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x1B);
  emit_operand(dst, src, 0);
}

void Assembler::sbbl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x1B, 0xC0, dst, src);
}

void Assembler::setb(Condition cc, Register dst) {
  assert(0 <= cc && cc < 16, "illegal cc");
  int encode = prefix_and_encode(dst->encoding(), true, true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0x90 | cc, 0xC0, encode);
}

void Assembler::palignr(XMMRegister dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_ssse3(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x0F, (0xC0 | encode), imm8);
}

void Assembler::vpalignr(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8, int vector_len) {
  assert(vector_len == AVX_128bit? VM_Version::supports_avx() :
         vector_len == AVX_256bit? VM_Version::supports_avx2() :
         0, "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, nds, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x0F, (0xC0 | encode), imm8);
}

void Assembler::evalignq(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x3, (0xC0 | encode), imm8);
}

void Assembler::pblendw(XMMRegister dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x0E, (0xC0 | encode), imm8);
}

void Assembler::sha1rnds4(XMMRegister dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_sha(), "");
  int encode = rex_prefix_and_encode(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3A, /* rex_w */ false);
  emit_int24((unsigned char)0xCC, (0xC0 | encode), (unsigned char)imm8);
}

void Assembler::sha1nexte(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha(), "");
  int encode = rex_prefix_and_encode(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, /* rex_w */ false);
  emit_int16((unsigned char)0xC8, (0xC0 | encode));
}

void Assembler::sha1msg1(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha(), "");
  int encode = rex_prefix_and_encode(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, /* rex_w */ false);
  emit_int16((unsigned char)0xC9, (0xC0 | encode));
}

void Assembler::sha1msg2(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha(), "");
  int encode = rex_prefix_and_encode(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, /* rex_w */ false);
  emit_int16((unsigned char)0xCA, (0xC0 | encode));
}

// xmm0 is implicit additional source to this instruction.
void Assembler::sha256rnds2(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha(), "");
  int encode = rex_prefix_and_encode(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, /* rex_w */ false);
  emit_int16((unsigned char)0xCB, (0xC0 | encode));
}

void Assembler::sha256msg1(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha(), "");
  int encode = rex_prefix_and_encode(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, /* rex_w */ false);
  emit_int16((unsigned char)0xCC, (0xC0 | encode));
}

void Assembler::sha256msg2(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha(), "");
  int encode = rex_prefix_and_encode(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, /* rex_w */ false);
  emit_int16((unsigned char)0xCD, (0xC0 | encode));
}

void Assembler::sha512msg1(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha512() && VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xCC, (0xC0 | encode));
}

void Assembler::sha512msg2(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sha512() && VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xCD, (0xC0 | encode));
}

void Assembler::sha512rnds2(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_sha512() && VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xCB, (0xC0 | encode));
}

void Assembler::shll(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  int encode = prefix_and_encode(dst->encoding());
  if (imm8 == 1 ) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::eshll(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1 ) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::shll(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::eshll(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::shrl(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  int encode = prefix_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE8 | encode));
  }
  else {
    emit_int24((unsigned char)0xC1, (0xE8 | encode), imm8);
  }
}

void Assembler::eshrl(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE8 | encode));
  }
  else {
    emit_int24((unsigned char)0xC1, (0xE8 | encode), imm8);
  }
}

void Assembler::shrl(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xE8 | encode));
}

void Assembler::eshrl(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xD3, (0xE8 | encode));
}

void Assembler::shrl(Address dst) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(5), dst, 0);
}

void Assembler::eshrl(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(5), src, 0);
}

void Assembler::shrl(Address dst, int imm8) {
  InstructionMark im(this);
  assert(isShiftCount(imm8), "illegal shift count");
  prefix(dst);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(5), dst, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(5), dst, 1);
    emit_int8(imm8);
  }
}

void Assembler::eshrl(Register dst, Address src, int imm8, bool no_flags) {
  InstructionMark im(this);
  assert(isShiftCount(imm8), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(5), src, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(5), src, 1);
    emit_int8(imm8);
  }
}

void Assembler::shldl(Register dst, Register src) {
  int encode = prefix_and_encode(src->encoding(), dst->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xA5, 0xC0, encode);
}

void Assembler::eshldl(Register dst, Register src1, Register src2, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0xA5, no_flags, true /* is_map1 */);
}

void Assembler::shldl(Register dst, Register src, int8_t imm8) {
  int encode = prefix_and_encode(src->encoding(), dst->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xA4, 0xC0, encode, imm8);
}

void Assembler::eshldl(Register dst, Register src1, Register src2, int8_t imm8, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), imm8, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x24, no_flags, true /* is_map1 */);
}

void Assembler::shrdl(Register dst, Register src) {
  int encode = prefix_and_encode(src->encoding(), dst->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xAD, 0xC0, encode);
}

void Assembler::eshrdl(Register dst, Register src1, Register src2, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0xAD, no_flags, true /* is_map1 */);
}

void Assembler::shrdl(Register dst, Register src, int8_t imm8) {
  int encode = prefix_and_encode(src->encoding(), dst->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xAC, 0xC0, encode, imm8);
}

void Assembler::eshrdl(Register dst, Register src1, Register src2, int8_t imm8, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), imm8, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x2C, no_flags, true /* is_map1 */);
}

void Assembler::shldq(Register dst, Register src, int8_t imm8) {
  int encode = prefixq_and_encode(src->encoding(), dst->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xA4, 0xC0, encode, imm8);
}

void Assembler::eshldq(Register dst, Register src1, Register src2, int8_t imm8, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), imm8, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x24, no_flags, true /* is_map1 */);
}

void Assembler::shrdq(Register dst, Register src, int8_t imm8) {
  int encode = prefixq_and_encode(src->encoding(), dst->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xAC, 0xC0, encode, imm8);
}

void Assembler::eshrdq(Register dst, Register src1, Register src2, int8_t imm8, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), imm8, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x2C, no_flags, true /* is_map1 */);
}

// copies a single word from [esi] to [edi]
void Assembler::smovl() {
  emit_int8((unsigned char)0xA5);
}

void Assembler::roundsd(XMMRegister dst, XMMRegister src, int32_t rmode) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, src, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x0B, (0xC0 | encode), (unsigned char)rmode);
}

void Assembler::roundsd(XMMRegister dst, Address src, int32_t rmode) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x0B);
  emit_operand(dst, src, 1);
  emit_int8((unsigned char)rmode);
}

void Assembler::sqrtsd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::sqrtsd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x51);
  emit_operand(dst, src, 0);
}

void Assembler::sqrtss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::std() {
  emit_int8((unsigned char)0xFD);
}

void Assembler::sqrtss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x51);
  emit_operand(dst, src, 0);
}

void Assembler::stmxcsr(Address dst) {
  // This instruction should be SSE encoded with the REX2 prefix when an
  // extended GPR is present. To be consistent when UseAPX is enabled, use
  // this encoding even when an extended GPR is not used.
  if (UseAVX > 0 && !UseAPX  ) {
    assert(VM_Version::supports_avx(), "");
    InstructionMark im(this);
    InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
    vex_prefix(dst, 0, 0, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
    emit_int8((unsigned char)0xAE);
    emit_operand(as_Register(3), dst, 0);
  } else {
    InstructionMark im(this);
    prefix(dst, true /* is_map1 */);
    emit_int8((unsigned char)0xAE);
    emit_operand(as_Register(3), dst, 0);
  }
}

void Assembler::subl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, rbp, dst, imm32);
}

void Assembler::esubl(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, rbp, src, imm32);
}

void Assembler::subl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8(0x29);
  emit_operand(src, dst, 0);
}

void Assembler::esubl(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x29);
  emit_operand(src2, src1, 0);
}

void Assembler::subl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xE8, dst, imm32);
}

void Assembler::esubl(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x81, 0xE8, no_flags);
}

// Force generation of a 4 byte immediate value even if it fits into 8bit
void Assembler::subl_imm32(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith_imm32(0x81, 0xE8, dst, imm32);
}

void Assembler::esubl_imm32(Register dst, Register src, int32_t imm32, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  (void) emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_imm32(0x81, 0xE8, src, imm32);
}

void Assembler::subl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x2B);
  emit_operand(dst, src, 0);
}

void Assembler::esubl(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x2B, no_flags);
}

void Assembler::subl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x2B, 0xC0, dst, src);
}

void Assembler::esubl(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith(0x2B, 0xC0, src1, src2);
}

void Assembler::subsd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::subsd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::subss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true , /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::subss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::testb(Register dst, int imm8, bool use_ral) {
  if (dst == rax) {
    if (use_ral) {
      emit_int8((unsigned char)0xA8);
      emit_int8(imm8);
    } else {
      emit_int8((unsigned char)0xF6);
      emit_int8((unsigned char)0xC4);
      emit_int8(imm8);
    }
  } else {
    (void) prefix_and_encode(dst->encoding(), true);
    emit_arith_b(0xF6, 0xC0, dst, imm8);
  }
}

void Assembler::testb(Address dst, int imm8) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xF6);
  emit_operand(rax, dst, 1);
  emit_int8(imm8);
}

void Assembler::testl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xF7);
  emit_operand(as_Register(0), dst, 4);
  emit_int32(imm32);
}

void Assembler::testl(Register dst, int32_t imm32) {
  // not using emit_arith because test
  // doesn't support sign-extension of
  // 8bit operands
  if (dst == rax) {
    emit_int8((unsigned char)0xA9);
    emit_int32(imm32);
  } else {
    int encode = dst->encoding();
    encode = prefix_and_encode(encode);
    emit_int16((unsigned char)0xF7, (0xC0 | encode));
    emit_int32(imm32);
  }
}

void Assembler::testl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x85, 0xC0, dst, src);
}

void Assembler::testl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8((unsigned char)0x85);
  emit_operand(dst, src, 0);
}

void Assembler::tzcntl(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  emit_int8((unsigned char)0xF3);
  int encode = prefix_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBC, 0xC0, encode);
}

void Assembler::etzcntl(Register dst, Register src, bool no_flags) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF4, (0xC0 | encode));
}

void Assembler::tzcntl(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  InstructionMark im(this);
  emit_int8((unsigned char)0xF3);
  prefix(src, dst, false, true /* is_map1 */);
  emit_int8((unsigned char)0xBC);
  emit_operand(dst, src, 0);
}

void Assembler::etzcntl(Register dst, Address src, bool no_flags) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xF4);
  emit_operand(dst, src, 0);
}

void Assembler::tzcntq(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  emit_int8((unsigned char)0xF3);
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBC, 0xC0, encode);
}

void Assembler::etzcntq(Register dst, Register src, bool no_flags) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF4, (0xC0 | encode));
}

void Assembler::tzcntq(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  InstructionMark im(this);
  emit_int8((unsigned char)0xF3);
  prefixq(src, dst, true /* is_map1 */);
  emit_int8((unsigned char)0xBC);
  emit_operand(dst, src, 0);
}

void Assembler::etzcntq(Register dst, Address src, bool no_flags) {
  assert(VM_Version::supports_bmi1(), "tzcnt instruction not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xF4);
  emit_operand(dst, src, 0);
}

void Assembler::ucomisd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2E);
  emit_operand(dst, src, 0);
}

void Assembler::ucomisd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2E, (0xC0 | encode));
}

void Assembler::ucomiss(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2E);
  emit_operand(dst, src, 0);
}

void Assembler::ucomiss(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2E, (0xC0 | encode));
}

void Assembler::xabort(int8_t imm8) {
  emit_int24((unsigned char)0xC6, (unsigned char)0xF8, (imm8 & 0xFF));
}

void Assembler::xaddb(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src, true, true /* is_map1 */);
  emit_int8((unsigned char)0xC0);
  emit_operand(src, dst, 0);
}

void Assembler::xaddw(Address dst, Register src) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(dst, src, false, true /* is_map1 */);
  emit_int8((unsigned char)0xC1);
  emit_operand(src, dst, 0);
}

void Assembler::xaddl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src, false, true /* is_map1 */);
  emit_int8((unsigned char)0xC1);
  emit_operand(src, dst, 0);
}

void Assembler::xbegin(Label& abort, relocInfo::relocType rtype) {
  InstructionMark im(this);
  relocate(rtype);
  if (abort.is_bound()) {
    address entry = target(abort);
    assert(entry != nullptr, "abort entry null");
    int offset = checked_cast<int>(entry - pc());
    emit_int16((unsigned char)0xC7, (unsigned char)0xF8);
    emit_int32(offset - 6); // 2 opcode + 4 address
  } else {
    abort.add_patch_at(code(), locator());
    emit_int16((unsigned char)0xC7, (unsigned char)0xF8);
    emit_int32(0);
  }
}

void Assembler::xchgb(Register dst, Address src) { // xchg
  InstructionMark im(this);
  prefix(src, dst, true);
  emit_int8((unsigned char)0x86);
  emit_operand(dst, src, 0);
}

void Assembler::xchgw(Register dst, Address src) { // xchg
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(src, dst);
  emit_int8((unsigned char)0x87);
  emit_operand(dst, src, 0);
}

void Assembler::xchgl(Register dst, Address src) { // xchg
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8((unsigned char)0x87);
  emit_operand(dst, src, 0);
}

void Assembler::xchgl(Register dst, Register src) {
  int encode = prefix_and_encode(dst->encoding(), src->encoding());
  emit_int16((unsigned char)0x87, (0xC0 | encode));
}

void Assembler::xend() {
  emit_int24(0x0F, 0x01, (unsigned char)0xD5);
}

void Assembler::xgetbv() {
  emit_int24(0x0F, 0x01, (unsigned char)0xD0);
}

void Assembler::xorl(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefix(dst);
  emit_arith_operand(0x81, as_Register(6), dst, imm32);
}

void Assembler::exorl(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, as_Register(6), src, imm32);
}

void Assembler::xorl(Register dst, int32_t imm32) {
  prefix(dst);
  emit_arith(0x81, 0xF0, dst, imm32);
}

void Assembler::exorl(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x81, 0xF0, no_flags);
}

void Assembler::xorl(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x33);
  emit_operand(dst, src, 0);
}

void Assembler::exorl(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_32bit, 0x33, no_flags);
}

void Assembler::xorl(Register dst, Register src) {
  (void) prefix_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x33, 0xC0, dst, src);
}

void Assembler::exorl(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith(0x33, 0xC0, src1, src2);
}

void Assembler::xorl(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src);
  emit_int8(0x31);
  emit_operand(src, dst, 0);
}

void Assembler::exorl(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x31);
  emit_operand(src2, src1, 0);
}

void Assembler::xorb(Register dst, Address src) {
  InstructionMark im(this);
  prefix(src, dst);
  emit_int8(0x32);
  emit_operand(dst, src, 0);
}

void Assembler::exorb(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_8bit, 0x32, no_flags);
}

void Assembler::xorb(Address dst, Register src) {
  InstructionMark im(this);
  prefix(dst, src, true);
  emit_int8(0x30);
  emit_operand(src, dst, 0);
}

void Assembler::exorb(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_8bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x30);
  emit_operand(src2, src1, 0);
}

void Assembler::xorw(Register dst, Address src) {
  InstructionMark im(this);
  emit_int8(0x66);
  prefix(src, dst);
  emit_int8(0x33);
  emit_operand(dst, src, 0);
}

void Assembler::exorw(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_66, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_16bit, 0x33, no_flags);
}

// AVX 3-operands scalar float-point arithmetic instructions

void Assembler::vaddsd(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::vaddsd(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::vaddss(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::vaddss(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::vdivsd(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::vdivsd(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::vdivss(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::vdivss(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::vfmadd231sd(XMMRegister dst, XMMRegister src1, XMMRegister src2) {
  assert(VM_Version::supports_fma(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB9, (0xC0 | encode));
}

void Assembler::evfnmadd213sd(XMMRegister dst, XMMRegister src1, XMMRegister src2, EvexRoundPrefix rmode) { // Need to add rmode for rounding mode support
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(rmode, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_extended_context();
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xAD, (0xC0 | encode));
}

void Assembler::vfnmadd213sd(XMMRegister dst, XMMRegister src1, XMMRegister src2) {
  assert(VM_Version::supports_fma(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xAD, (0xC0 | encode));
}

void Assembler::vfnmadd231sd(XMMRegister dst, XMMRegister src1, XMMRegister src2) {
  assert(VM_Version::supports_fma(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xBD, (0xC0 | encode));
}

void Assembler::vfmadd231ss(XMMRegister dst, XMMRegister src1, XMMRegister src2) {
  assert(VM_Version::supports_fma(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB9, (0xC0 | encode));
}

void Assembler::vmulsd(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::vmulsd(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::vmulss(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::vmulss(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::vsubsd(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::vsubsd(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::vsubss(XMMRegister dst, XMMRegister nds, Address src) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::vsubss(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

//====================VECTOR ARITHMETIC=====================================

// Float-point vector arithmetic

void Assembler::addpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::addpd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}


void Assembler::addps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::vaddpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::vaddps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::vaddpd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::vaddps(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::subpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::subps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::vsubpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::vsubps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::vsubpd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::vsubps(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::mulpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::mulpd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::mulps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::vmulpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::vmulps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::vmulpd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::vmulps(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::vfmadd231pd(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  assert(VM_Version::supports_fma(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB8, (0xC0 | encode));
}

void Assembler::vfmadd231ps(XMMRegister dst, XMMRegister src1, XMMRegister src2, int vector_len) {
  assert(VM_Version::supports_fma(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xB8, (0xC0 | encode));
}

void Assembler::vfmadd231pd(XMMRegister dst, XMMRegister src1, Address src2, int vector_len) {
  assert(VM_Version::supports_fma(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xB8);
  emit_operand(dst, src2, 0);
}

void Assembler::vfmadd231ps(XMMRegister dst, XMMRegister src1, Address src2, int vector_len) {
  assert(VM_Version::supports_fma(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xB8);
  emit_operand(dst, src2, 0);
}

void Assembler::divpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::divps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::vdivpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::vdivps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::vdivpd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::vdivps(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::vroundpd(XMMRegister dst, XMMRegister src, int32_t rmode, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x09, (0xC0 | encode), (rmode));
}

void Assembler::vroundpd(XMMRegister dst, Address src, int32_t rmode,  int vector_len) {
  assert(VM_Version::supports_avx(), "");
  assert(!needs_eevex(src.base(), src.index()), "does not support extended gprs as BASE or INDEX of address operand");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x09);
  emit_operand(dst, src, 1);
  emit_int8((rmode));
}

void Assembler::vroundsd(XMMRegister dst, XMMRegister src, XMMRegister src2, int32_t rmode) {
  assert(VM_Version::supports_avx(), "");
  assert(rmode <= 0x0f, "rmode 0x%x", rmode);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x0B, (0xC0 | encode), (rmode));
}

void Assembler::vrndscalesd(XMMRegister dst,  XMMRegister src1, XMMRegister src2, int32_t rmode) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x0B, (0xC0 | encode), (rmode));
}

void Assembler::vrndscalepd(XMMRegister dst,  XMMRegister src,  int32_t rmode, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x09, (0xC0 | encode), (rmode));
}

void Assembler::vrndscalepd(XMMRegister dst, Address src, int32_t rmode, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x09);
  emit_operand(dst, src, 1);
  emit_int8((rmode));
}

void Assembler::vsqrtpd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::vsqrtpd(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x51);
  emit_operand(dst, src, 0);
}

void Assembler::vsqrtps(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::vsqrtps(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x51);
  emit_operand(dst, src, 0);
}

void Assembler::andpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x54, (0xC0 | encode));
}

void Assembler::andnpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x55, (0xC0 | encode));
}

void Assembler::andps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x54, (0xC0 | encode));
}

void Assembler::andps(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x54);
  emit_operand(dst, src, 0);
}

void Assembler::andpd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x54);
  emit_operand(dst, src, 0);
}

void Assembler::vandpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x54, (0xC0 | encode));
}

void Assembler::vandps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x54, (0xC0 | encode));
}

void Assembler::vandpd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x54);
  emit_operand(dst, src, 0);
}

void Assembler::vandps(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x54);
  emit_operand(dst, src, 0);
}

void Assembler::orpd(XMMRegister dst, XMMRegister src) {
  NOT_LP64(assert(VM_Version::supports_sse2(), ""));
  InstructionAttr attributes(AVX_128bit, /* rex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x56, (0xC0 | encode));
}

void Assembler::unpckhpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x15);
  emit_int8((0xC0 | encode));
}

void Assembler::unpcklpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x14, (0xC0 | encode));
}

void Assembler::xorpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x57, (0xC0 | encode));
}

void Assembler::xorps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x57, (0xC0 | encode));
}

void Assembler::xorpd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x57);
  emit_operand(dst, src, 0);
}

void Assembler::xorps(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x57);
  emit_operand(dst, src, 0);
}

void Assembler::vxorpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x57, (0xC0 | encode));
}

void Assembler::vxorps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x57, (0xC0 | encode));
}

void Assembler::vxorpd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ !_legacy_mode_dq, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x57);
  emit_operand(dst, src, 0);
}

void Assembler::vxorps(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x57);
  emit_operand(dst, src, 0);
}

// Integer vector arithmetic
void Assembler::vphaddw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert((VM_Version::supports_avx() && (vector_len == 0)) ||
         VM_Version::supports_avx2(), "256 bit integer vectors requires AVX2");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x01, (0xC0 | encode));
}

void Assembler::vphaddd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert((VM_Version::supports_avx() && (vector_len == 0)) ||
         VM_Version::supports_avx2(), "256 bit integer vectors requires AVX2");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x02, (0xC0 | encode));
}

void Assembler::paddb(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFC, (0xC0 | encode));
}

void Assembler::paddw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFD, (0xC0 | encode));
}

void Assembler::paddd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFE, (0xC0 | encode));
}

void Assembler::paddd(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  simd_prefix(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFE);
  emit_operand(dst, src, 0);
}

void Assembler::paddq(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD4, (0xC0 | encode));
}

void Assembler::phaddw(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse3(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x01, (0xC0 | encode));
}

void Assembler::phaddd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse3(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x02, (0xC0 | encode));
}

void Assembler::vpaddb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFC, (0xC0 | encode));
}

void Assembler::vpaddw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFD, (0xC0 | encode));
}

void Assembler::vpaddd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFE, (0xC0 | encode));
}

void Assembler::vpaddq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD4, (0xC0 | encode));
}

void Assembler::vpaddb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFC);
  emit_operand(dst, src, 0);
}

void Assembler::vpaddw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFD);
  emit_operand(dst, src, 0);
}

void Assembler::vpaddd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFE);
  emit_operand(dst, src, 0);
}

void Assembler::vpaddq(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD4);
  emit_operand(dst, src, 0);
}

void Assembler::vaddsh(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::vsubsh(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::vdivsh(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::vmulsh(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::vmaxsh(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::vminsh(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}

void Assembler::vsqrtsh(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::vfmadd132sh(XMMRegister dst, XMMRegister src1, XMMRegister src2) {
  assert(VM_Version::supports_avx512_fp16(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_MAP6, &attributes);
  emit_int16((unsigned char)0x99, (0xC0 | encode));
}

void Assembler::vpaddsb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEC, (0xC0 | encode));
}

void Assembler::vpaddsb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEC);
  emit_operand(dst, src, 0);
}

void Assembler::vpaddsw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xED, (0xC0 | encode));
}

void Assembler::vpaddsw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xED);
  emit_operand(dst, src, 0);
}

void Assembler::vpaddusb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDC, (0xC0 | encode));
}

void Assembler::vpaddusb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDC);
  emit_operand(dst, src, 0);
}


void Assembler::vpaddusw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDD, (0xC0 | encode));
}

void Assembler::vpaddusw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDD);
  emit_operand(dst, src, 0);
}


void Assembler::vpsubsb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE8, (0xC0 | encode));
}

void Assembler::vpsubsb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xE8);
  emit_operand(dst, src, 0);
}

void Assembler::vpsubsw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE9, (0xC0 | encode));
}

void Assembler::vpsubsw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xE9);
  emit_operand(dst, src, 0);
}

void Assembler::vpsubusb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD8, (0xC0 | encode));
}

void Assembler::vpsubusb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD8);
  emit_operand(dst, src, 0);
}

void Assembler::vpsubusw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD9, (0xC0 | encode));
}

void Assembler::vpsubusw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD9);
  emit_operand(dst, src, 0);
}


void Assembler::psubb(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF8, (0xC0 | encode));
}

void Assembler::psubw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF9, (0xC0 | encode));
}

void Assembler::psubd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFA, (0xC0 | encode));
}

void Assembler::psubq(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFB);
  emit_int8((0xC0 | encode));
}

void Assembler::vpsubb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF8, (0xC0 | encode));
}

void Assembler::vpsubw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF9, (0xC0 | encode));
}

void Assembler::vpsubd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFA, (0xC0 | encode));
}

void Assembler::vpsubq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFB, (0xC0 | encode));
}

void Assembler::vpsubb(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xF8);
  emit_operand(dst, src, 0);
}

void Assembler::vpsubw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xF9);
  emit_operand(dst, src, 0);
}

void Assembler::vpsubd(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFA);
  emit_operand(dst, src, 0);
}

void Assembler::vpsubq(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_rex_vex_w_reverted();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFB);
  emit_operand(dst, src, 0);
}

void Assembler::pmullw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD5, (0xC0 | encode));
}

void Assembler::pmulld(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x40, (0xC0 | encode));
}

void Assembler::pmuludq(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF4, (0xC0 | encode));
}

void Assembler::vpmulhuw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert((vector_len == AVX_128bit && VM_Version::supports_avx()) ||
         (vector_len == AVX_256bit && VM_Version::supports_avx2()) ||
         (vector_len == AVX_512bit && VM_Version::supports_avx512bw()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE4, (0xC0 | encode));
}

void Assembler::vpmullw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD5, (0xC0 | encode));
}

void Assembler::vpmulld(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x40, (0xC0 | encode));
}

void Assembler::evpmullq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "requires some form of EVEX");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x40, (0xC0 | encode));
}

void Assembler::vpmuludq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF4, (0xC0 | encode));
}

void Assembler::vpmuldq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_evex()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x28, (0xC0 | encode));
}

void Assembler::vpmullw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD5);
  emit_operand(dst, src, 0);
}

void Assembler::vpmulld(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x40);
  emit_operand(dst, src, 0);
}

void Assembler::evpmullq(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 2, "requires some form of EVEX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_dq, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x40);
  emit_operand(dst, src, 0);
}

// Min, max
void Assembler::pminsb(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x38, (0xC0 | encode));
}

void Assembler::vpminsb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x38, (0xC0 | encode));
}

void Assembler::pminsw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEA, (0xC0 | encode));
}

void Assembler::vpminsw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEA, (0xC0 | encode));
}

void Assembler::pminsd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x39, (0xC0 | encode));
}

void Assembler::vpminsd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_evex()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x39, (0xC0 | encode));
}

void Assembler::vpminsq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "requires AVX512F");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x39, (0xC0 | encode));
}

void Assembler::minps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}
void Assembler::vminps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len >= AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}

void Assembler::minpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}
void Assembler::vminpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len >= AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}

void Assembler::pmaxsb(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3C, (0xC0 | encode));
}

void Assembler::vpmaxsb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3C, (0xC0 | encode));
}

void Assembler::pmaxsw(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEE, (0xC0 | encode));
}

void Assembler::vpmaxsw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEE, (0xC0 | encode));
}

void Assembler::pmaxsd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3D, (0xC0 | encode));
}

void Assembler::vpmaxsd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_evex()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3D, (0xC0 | encode));
}

void Assembler::vpmaxsq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "requires AVX512F");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3D, (0xC0 | encode));
}

void Assembler::maxps(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::vmaxps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len >= AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::maxpd(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::vmaxpd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len >= AVX_512bit ? VM_Version::supports_evex() : VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::vpminub(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds, src) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDA, (0xC0 | encode));
}

void Assembler::vpminub(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDA);
  emit_operand(dst, src, 0);
}

void Assembler::evpminub(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDA, (0xC0 | encode));
}

void Assembler::evpminub(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDA);
  emit_operand(dst, src, 0);
}

void Assembler::vpminuw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3A, (0xC0 | encode));
}

void Assembler::vpminuw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  assert(!needs_evex(dst, nds) || VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x3A);
  emit_operand(dst, src, 0);
}

void Assembler::evpminuw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3A, (0xC0 | encode));
}

void Assembler::evpminuw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3A);
  emit_operand(dst, src, 0);
}

void Assembler::vpminud(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3B, (0xC0 | encode));
}

void Assembler::vpminud(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x3B);
  emit_operand(dst, src, 0);
}

void Assembler::evpminud(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3B, (0xC0 | encode));
}

void Assembler::evpminud(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3B);
  emit_operand(dst, src, 0);
}

void Assembler::evpminuq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3B, (0xC0 | encode));
}

void Assembler::evpminuq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3B);
  emit_operand(dst, src, 0);
}

void Assembler::vpmaxub(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDE, (0xC0 | encode));
}

void Assembler::vpmaxub(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDE);
  emit_operand(dst, src, 0);
}

void Assembler::evpmaxub(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDE, (0xC0 | encode));
}

void Assembler::evpmaxub(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDE);
  emit_operand(dst, src, 0);
}

void Assembler::vpmaxuw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3E, (0xC0 | encode));
}

void Assembler::vpmaxuw(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
        (vector_len == AVX_256bit ? VM_Version::supports_avx2() : VM_Version::supports_avx512bw()), "");
  assert(UseAVX > 0 && (vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x3E);
  emit_operand(dst, src, 0);
}

void Assembler::evpmaxuw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3E, (0xC0 | encode));
}

void Assembler::evpmaxuw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3E);
  emit_operand(dst, src, 0);
}

void Assembler::vpmaxud(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "");
  assert((vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds, src) || VM_Version::supports_avx512vl())), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3F, (0xC0 | encode));
}

void Assembler::vpmaxud(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "");
  assert((vector_len == Assembler::AVX_512bit || (!needs_evex(dst, nds) || VM_Version::supports_avx512vl())), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x3F);
  emit_operand(dst, src, 0);
}

void Assembler::evpmaxud(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3F, (0xC0 | encode));
}

void Assembler::evpmaxud(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3F);
  emit_operand(dst, src, 0);
}

void Assembler::evpmaxuq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3F, (0xC0 | encode));
}

void Assembler::evpmaxuq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3F);
  emit_operand(dst, src, 0);
}

// Shift packed integers left by specified number of bits.
void Assembler::psllw(XMMRegister dst, int shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM6 is for /6 encoding: 66 0F 71 /6 ib
  int encode = simd_prefix_and_encode(xmm6, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::pslld(XMMRegister dst, int shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM6 is for /6 encoding: 66 0F 72 /6 ib
  int encode = simd_prefix_and_encode(xmm6, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::psllq(XMMRegister dst, int shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  // XMM6 is for /6 encoding: 66 0F 73 /6 ib
  int encode = simd_prefix_and_encode(xmm6, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::psllw(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF1, (0xC0 | encode));
}

void Assembler::pslld(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF2, (0xC0 | encode));
}

void Assembler::psllq(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

void Assembler::vpsllw(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM6 is for /6 encoding: 66 0F 71 /6 ib
  int encode = vex_prefix_and_encode(xmm6->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpslld(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM6 is for /6 encoding: 66 0F 72 /6 ib
  int encode = vex_prefix_and_encode(xmm6->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpsllq(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  // XMM6 is for /6 encoding: 66 0F 73 /6 ib
  int encode = vex_prefix_and_encode(xmm6->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpsllw(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF1, (0xC0 | encode));
}

void Assembler::vpslld(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF2, (0xC0 | encode));
}

void Assembler::vpsllq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

// Shift packed integers logically right by specified number of bits.
void Assembler::psrlw(XMMRegister dst, int shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM2 is for /2 encoding: 66 0F 71 /2 ib
  int encode = simd_prefix_and_encode(xmm2, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::psrld(XMMRegister dst, int shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM2 is for /2 encoding: 66 0F 72 /2 ib
  int encode = simd_prefix_and_encode(xmm2, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::psrlq(XMMRegister dst, int shift) {
  // Do not confuse it with psrldq SSE2 instruction which
  // shifts 128 bit value in xmm register by number of bytes.
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  // XMM2 is for /2 encoding: 66 0F 73 /2 ib
  int encode = simd_prefix_and_encode(xmm2, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::psrlw(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD1, (0xC0 | encode));
}

void Assembler::psrld(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD2, (0xC0 | encode));
}

void Assembler::psrlq(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD3, (0xC0 | encode));
}

void Assembler::vpsrlw(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM2 is for /2 encoding: 66 0F 71 /2 ib
  int encode = vex_prefix_and_encode(xmm2->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpsrld(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM2 is for /2 encoding: 66 0F 72 /2 ib
  int encode = vex_prefix_and_encode(xmm2->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpsrlq(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  // XMM2 is for /2 encoding: 66 0F 73 /2 ib
  int encode = vex_prefix_and_encode(xmm2->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpsrlw(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD1, (0xC0 | encode));
}

void Assembler::vpsrld(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD2, (0xC0 | encode));
}

void Assembler::vpsrlq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD3, (0xC0 | encode));
}

void Assembler::evpsrlvw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x10, (0xC0 | encode));
}

void Assembler::evpsllvw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x12, (0xC0 | encode));
}

// Shift packed integers arithmetically right by specified number of bits.
void Assembler::psraw(XMMRegister dst, int shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM4 is for /4 encoding: 66 0F 71 /4 ib
  int encode = simd_prefix_and_encode(xmm4, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::psrad(XMMRegister dst, int shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM4 is for /4 encoding: 66 0F 72 /4 ib
  int encode = simd_prefix_and_encode(xmm4, dst, dst, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x72);
  emit_int8((0xC0 | encode));
  emit_int8(shift & 0xFF);
}

void Assembler::psraw(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE1, (0xC0 | encode));
}

void Assembler::psrad(XMMRegister dst, XMMRegister shift) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, shift, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE2, (0xC0 | encode));
}

void Assembler::vpsraw(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM4 is for /4 encoding: 66 0F 71 /4 ib
  int encode = vex_prefix_and_encode(xmm4->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpsrad(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  // XMM4 is for /4 encoding: 66 0F 71 /4 ib
  int encode = vex_prefix_and_encode(xmm4->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::vpsraw(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE1, (0xC0 | encode));
}

void Assembler::vpsrad(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE2, (0xC0 | encode));
}

void Assembler::evpsraq(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(UseAVX > 2, "requires AVX512");
  assert ((VM_Version::supports_avx512vl() || vector_len == 2), "requires AVX512vl");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(xmm4->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsraq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 2, "requires AVX512");
  assert ((VM_Version::supports_avx512vl() || vector_len == 2), "requires AVX512vl");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE2, (0xC0 | encode));
}

// logical operations packed integers
void Assembler::pand(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDB, (0xC0 | encode));
}

void Assembler::vpand(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDB, (0xC0 | encode));
}

void Assembler::vpand(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDB);
  emit_operand(dst, src, 0);
}

void Assembler::evpandq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  evpandq(dst, k0, nds, src, false, vector_len);
}

void Assembler::evpandq(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  evpandq(dst, k0, nds, src, false, vector_len);
}

//Variable Shift packed integers logically left.
void Assembler::vpsllvd(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 1, "requires AVX2");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

void Assembler::vpsllvq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 1, "requires AVX2");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

//Variable Shift packed integers logically right.
void Assembler::vpsrlvd(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 1, "requires AVX2");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

void Assembler::vpsrlvq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 1, "requires AVX2");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

//Variable right Shift arithmetic packed integers .
void Assembler::vpsravd(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 1, "requires AVX2");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x46, (0xC0 | encode));
}

void Assembler::evpsravw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x11, (0xC0 | encode));
}

void Assembler::evpsravq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(UseAVX > 2, "requires AVX512");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x46, (0xC0 | encode));
}

void Assembler::vpshldvd(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi2(), "requires vbmi2");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x71, (0xC0 | encode));
}

void Assembler::vpshrdvd(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi2(), "requires vbmi2");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x73, (0xC0 | encode));
}

void Assembler::pandn(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDF, (0xC0 | encode));
}

void Assembler::vpandn(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDF, (0xC0 | encode));
}

void Assembler::por(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEB, (0xC0 | encode));
}

void Assembler::vpor(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEB, (0xC0 | encode));
}

void Assembler::vpor(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEB);
  emit_operand(dst, src, 0);
}

void Assembler::evporq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  evporq(dst, k0, nds, src, false, vector_len);
}

void Assembler::evporq(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  evporq(dst, k0, nds, src, false, vector_len);
}

void Assembler::evpord(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  // Encoding: EVEX.NDS.XXX.66.0F.W0 EB /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEB, (0xC0 | encode));
}

void Assembler::evpord(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  // Encoding: EVEX.NDS.XXX.66.0F.W0 EB /r
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEB);
  emit_operand(dst, src, 0);
}

void Assembler::pxor(XMMRegister dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEF, (0xC0 | encode));
}

void Assembler::vpxor(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
         vector_len == AVX_512bit ? VM_Version::supports_evex() : 0, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEF, (0xC0 | encode));
}

void Assembler::vpxor(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() :
         vector_len == AVX_256bit ? VM_Version::supports_avx2() :
         vector_len == AVX_512bit ? VM_Version::supports_evex() : 0, "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEF);
  emit_operand(dst, src, 0);
}

void Assembler::vpxorq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 2, "requires some form of EVEX");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEF, (0xC0 | encode));
}

void Assembler::evpxord(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  // Encoding: EVEX.NDS.XXX.66.0F.W0 EF /r
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEF, (0xC0 | encode));
}

void Assembler::evpxord(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEF);
  emit_operand(dst, src, 0);
}

void Assembler::evpxorq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  // Encoding: EVEX.NDS.XXX.66.0F.W1 EF /r
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEF, (0xC0 | encode));
}

void Assembler::evpxorq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEF);
  emit_operand(dst, src, 0);
}

void Assembler::evpandd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDB);
  emit_operand(dst, src, 0);
}

void Assembler::evpandq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires AVX512F");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDB, (0xC0 | encode));
}

void Assembler::evpandq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires AVX512F");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDB);
  emit_operand(dst, src, 0);
}

void Assembler::evporq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires AVX512F");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEB, (0xC0 | encode));
}

void Assembler::evporq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires AVX512F");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEB);
  emit_operand(dst, src, 0);
}

void Assembler::evpxorq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEF, (0xC0 | encode));
}

void Assembler::evpxorq(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEF);
  emit_operand(dst, src, 0);
}

void Assembler::evprold(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(xmm1->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprolq(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(xmm1->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprord(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(xmm0->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprorq(XMMRegister dst, XMMRegister src, int shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(xmm0->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprolvd(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x15, (unsigned char)(0xC0 | encode));
}

void Assembler::evprolvq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x15, (unsigned char)(0xC0 | encode));
}

void Assembler::evprorvd(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x14, (unsigned char)(0xC0 | encode));
}

void Assembler::evprorvq(XMMRegister dst, XMMRegister src, XMMRegister shift, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src->encoding(), shift->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x14, (unsigned char)(0xC0 | encode));
}

void Assembler::evplzcntd(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512cd(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x44, (0xC0 | encode));
}

void Assembler::evplzcntq(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512cd(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x44, (0xC0 | encode));
}

void Assembler::vpternlogd(XMMRegister dst, int imm8, XMMRegister src2, XMMRegister src3, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src3->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x25);
  emit_int8((unsigned char)(0xC0 | encode));
  emit_int8(imm8);
}

void Assembler::vpternlogd(XMMRegister dst, int imm8, XMMRegister src2, Address src3, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src3, src2->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x25);
  emit_operand(dst, src3, 1);
  emit_int8(imm8);
}

void Assembler::vpternlogq(XMMRegister dst, int imm8, XMMRegister src2, XMMRegister src3, int vector_len) {
  assert(VM_Version::supports_evex(), "requires AVX512F");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires AVX512VL");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src3->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x25);
  emit_int8((unsigned char)(0xC0 | encode));
  emit_int8(imm8);
}

void Assembler::vpternlogq(XMMRegister dst, int imm8, XMMRegister src2, Address src3, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src3, src2->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x25);
  emit_operand(dst, src3, 1);
  emit_int8(imm8);
}

void Assembler::evexpandps(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x88, (0xC0 | encode));
}

void Assembler::evexpandpd(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x88, (0xC0 | encode));
}

void Assembler::evpexpandb(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi2(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x62, (0xC0 | encode));
}

void Assembler::evpexpandw(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi2(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x62, (0xC0 | encode));
}

void Assembler::evpexpandd(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x89, (0xC0 | encode));
}

void Assembler::evpexpandq(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x89, (0xC0 | encode));
}

// vinserti forms

void Assembler::vinserti128(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx2(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // last byte:
  // 0x00 - insert into lower 128 bits
  // 0x01 - insert into upper 128 bits
  emit_int24(0x38, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::vinserti128(XMMRegister dst, XMMRegister nds, Address src, uint8_t imm8) {
  assert(VM_Version::supports_avx2(), "");
  assert(dst != xnoreg, "sanity");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x38);
  emit_operand(dst, src, 1);
  // 0x00 - insert into lower 128 bits
  // 0x01 - insert into upper 128 bits
  emit_int8(imm8 & 0x01);
}

void Assembler::vinserti32x4(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - insert into q0 128 bits (0..127)
  // 0x01 - insert into q1 128 bits (128..255)
  // 0x02 - insert into q2 128 bits (256..383)
  // 0x03 - insert into q3 128 bits (384..511)
  emit_int24(0x38, (0xC0 | encode), imm8 & 0x03);
}

void Assembler::vinserti32x4(XMMRegister dst, XMMRegister nds, Address src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  attributes.set_is_evex_instruction();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x18);
  emit_operand(dst, src, 1);
  // 0x00 - insert into q0 128 bits (0..127)
  // 0x01 - insert into q1 128 bits (128..255)
  // 0x02 - insert into q2 128 bits (256..383)
  // 0x03 - insert into q3 128 bits (384..511)
  emit_int8(imm8 & 0x03);
}

void Assembler::vinserti64x4(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  //imm8:
  // 0x00 - insert into lower 256 bits
  // 0x01 - insert into upper 256 bits
  emit_int24(0x3A, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::evinserti64x2(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8, int vector_len) {
   assert(VM_Version::supports_avx512dq(), "");
   assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
   InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
   attributes.set_is_evex_instruction();
   int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
   emit_int24(0x38, (0xC0 | encode), imm8 & 0x03);
}


// vinsertf forms

void Assembler::vinsertf128(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - insert into lower 128 bits
  // 0x01 - insert into upper 128 bits
  emit_int24(0x18, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::vinsertf128(XMMRegister dst, XMMRegister nds, Address src, uint8_t imm8) {
  assert(VM_Version::supports_avx(), "");
  assert(dst != xnoreg, "sanity");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x18);
  emit_operand(dst, src, 1);
  // 0x00 - insert into lower 128 bits
  // 0x01 - insert into upper 128 bits
  emit_int8(imm8 & 0x01);
}

void Assembler::vinsertf32x4(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - insert into q0 128 bits (0..127)
  // 0x01 - insert into q1 128 bits (128..255)
  // 0x02 - insert into q0 128 bits (256..383)
  // 0x03 - insert into q1 128 bits (384..512)
  emit_int24(0x18, (0xC0 | encode), imm8 & 0x03);
}

void Assembler::vinsertf32x4(XMMRegister dst, XMMRegister nds, Address src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x18);
  emit_operand(dst, src, 1);
  // 0x00 - insert into q0 128 bits (0..127)
  // 0x01 - insert into q1 128 bits (128..255)
  // 0x02 - insert into q0 128 bits (256..383)
  // 0x03 - insert into q1 128 bits (384..512)
  emit_int8(imm8 & 0x03);
}

void Assembler::vinsertf64x4(XMMRegister dst, XMMRegister nds, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - insert into lower 256 bits
  // 0x01 - insert into upper 256 bits
  emit_int24(0x1A, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::vinsertf64x4(XMMRegister dst, XMMRegister nds, Address src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_is_evex_instruction();
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x1A);
  emit_operand(dst, src, 1);
  // 0x00 - insert into lower 256 bits
  // 0x01 - insert into upper 256 bits
  emit_int8(imm8 & 0x01);
}


// vextracti forms

void Assembler::vextracti128(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx2(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from lower 128 bits
  // 0x01 - extract from upper 128 bits
  emit_int24(0x39, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::vextracti128(Address dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx2(), "");
  assert(src != xnoreg, "sanity");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x39);
  emit_operand(src, dst, 1);
  // 0x00 - extract from lower 128 bits
  // 0x01 - extract from upper 128 bits
  emit_int8(imm8 & 0x01);
}

void Assembler::vextracti32x4(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from bits 127:0
  // 0x01 - extract from bits 255:128
  // 0x02 - extract from bits 383:256
  // 0x03 - extract from bits 511:384
  emit_int24(0x39, (0xC0 | encode), imm8 & 0x03);
}

void Assembler::vextracti32x4(Address dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x39);
  emit_operand(src, dst, 1);
  // 0x00 - extract from bits 127:0
  // 0x01 - extract from bits 255:128
  // 0x02 - extract from bits 383:256
  // 0x03 - extract from bits 511:384
  emit_int8(imm8 & 0x03);
}

void Assembler::vextracti64x2(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx512dq(), "");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from bits 127:0
  // 0x01 - extract from bits 255:128
  // 0x02 - extract from bits 383:256
  // 0x03 - extract from bits 511:384
  emit_int24(0x39, (0xC0 | encode), imm8 & 0x03);
}

void Assembler::vextracti64x4(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from lower 256 bits
  // 0x01 - extract from upper 256 bits
  emit_int24(0x3B, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::vextracti64x4(Address dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x38);
  emit_operand(src, dst, 1);
  // 0x00 - extract from lower 256 bits
  // 0x01 - extract from upper 256 bits
  emit_int8(imm8 & 0x01);
}
// vextractf forms

void Assembler::vextractf128(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from lower 128 bits
  // 0x01 - extract from upper 128 bits
  emit_int24(0x19, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::vextractf128(Address dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx(), "");
  assert(src != xnoreg, "sanity");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x19);
  emit_operand(src, dst, 1);
  // 0x00 - extract from lower 128 bits
  // 0x01 - extract from upper 128 bits
  emit_int8(imm8 & 0x01);
}

void Assembler::vextractf32x4(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from bits 127:0
  // 0x01 - extract from bits 255:128
  // 0x02 - extract from bits 383:256
  // 0x03 - extract from bits 511:384
  emit_int24(0x19, (0xC0 | encode), imm8 & 0x03);
}

void Assembler::vextractf32x4(Address dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x19);
  emit_operand(src, dst, 1);
  // 0x00 - extract from bits 127:0
  // 0x01 - extract from bits 255:128
  // 0x02 - extract from bits 383:256
  // 0x03 - extract from bits 511:384
  emit_int8(imm8 & 0x03);
}

void Assembler::vextractf64x2(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_avx512dq(), "");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from bits 127:0
  // 0x01 - extract from bits 255:128
  // 0x02 - extract from bits 383:256
  // 0x03 - extract from bits 511:384
  emit_int24(0x19, (0xC0 | encode), imm8 & 0x03);
}

void Assembler::vextractf64x4(XMMRegister dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  // imm8:
  // 0x00 - extract from lower 256 bits
  // 0x01 - extract from upper 256 bits
  emit_int24(0x1B, (0xC0 | encode), imm8 & 0x01);
}

void Assembler::vextractf64x4(Address dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_evex(), "");
  assert(src != xnoreg, "sanity");
  assert(imm8 <= 0x01, "imm8: %u", imm8);
  InstructionMark im(this);
  InstructionAttr attributes(AVX_512bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4,/* input_size_in_bits */  EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x1B);
  emit_operand(src, dst, 1);
  // 0x00 - extract from lower 256 bits
  // 0x01 - extract from upper 256 bits
  emit_int8(imm8 & 0x01);
}

void Assembler::extractps(Register dst, XMMRegister src, uint8_t imm8) {
  assert(VM_Version::supports_sse4_1(), "");
  assert(imm8 <= 0x03, "imm8: %u", imm8);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(src, xnoreg, as_XMMRegister(dst->encoding()), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes, true);
  // imm8:
  // 0x00 - extract from bits 31:0
  // 0x01 - extract from bits 63:32
  // 0x02 - extract from bits 95:64
  // 0x03 - extract from bits 127:96
  emit_int24(0x17, (0xC0 | encode), imm8 & 0x03);
}

// duplicate 1-byte integer data from src into programmed locations in dest : requires AVX512BW and AVX512VL
void Assembler::vpbroadcastb(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x78, (0xC0 | encode));
}

void Assembler::vpbroadcastb(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_8bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x78);
  emit_operand(dst, src, 0);
}

// duplicate 2-byte integer data from src into programmed locations in dest : requires AVX512BW and AVX512VL
void Assembler::vpbroadcastw(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x79, (0xC0 | encode));
}

void Assembler::vpbroadcastw(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_16bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x79);
  emit_operand(dst, src, 0);
}

void Assembler::vpsadbw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF6, (0xC0 | encode));
}

void Assembler::vpunpckhwd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x69, (0xC0 | encode));
}

void Assembler::vpunpcklwd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x61, (0xC0 | encode));
}

void Assembler::vpunpckhdq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6A, (0xC0 | encode));
}

void Assembler::vpunpckhqdq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6D, (0xC0 | encode));
}

void Assembler::vpunpckldq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x62, (0xC0 | encode));
}

void Assembler::vpunpcklqdq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(UseAVX > 0, "requires some form of AVX");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x6C, (0xC0 | encode));
}

// xmm/mem sourced byte/word/dword/qword replicate
void Assembler::evpaddb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFC, (0xC0 | encode));
}

void Assembler::evpaddb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFC);
  emit_operand(dst, src, 0);
}

void Assembler::evpaddw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFD, (0xC0 | encode));
}

void Assembler::evpaddw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFD);
  emit_operand(dst, src, 0);
}

void Assembler::evpaddd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFE, (0xC0 | encode));
}

void Assembler::evpaddd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFE);
  emit_operand(dst, src, 0);
}

void Assembler::evpaddq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD4, (0xC0 | encode));
}

void Assembler::evpaddq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD4);
  emit_operand(dst, src, 0);
}

void Assembler::evaddps(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::evaddps(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::evaddpd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::evaddpd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF8, (0xC0 | encode));
}

void Assembler::evpsubb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xF8);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF9, (0xC0 | encode));
}

void Assembler::evpsubw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xF9);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFA, (0xC0 | encode));
}

void Assembler::evpsubd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFA);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xFB, (0xC0 | encode));
}

void Assembler::evpsubq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xFB);
  emit_operand(dst, src, 0);
}

void Assembler::evsubps(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::evsubps(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::evsubpd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::evsubpd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::evpaddsb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEC, (0xC0 | encode));
}

void Assembler::evpaddsb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEC);
  emit_operand(dst, src, 0);
}

void Assembler::evpaddsw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xED, (0xC0 | encode));
}

void Assembler::evpaddsw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xED);
  emit_operand(dst, src, 0);
}

void Assembler::evpaddusb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDC, (0xC0 | encode));
}

void Assembler::evpaddusb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDC);
  emit_operand(dst, src, 0);
}

void Assembler::evpaddusw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xDD, (0xC0 | encode));
}

void Assembler::evpaddusw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xDD);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubsb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE8, (0xC0 | encode));
}

void Assembler::evpsubsb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xE8);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubsw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE9, (0xC0 | encode));
}

void Assembler::evpsubsw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xE9);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubusb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD8, (0xC0 | encode));
}

void Assembler::evpsubusb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD8);
  emit_operand(dst, src, 0);
}

void Assembler::evpsubusw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD9, (0xC0 | encode));
}


void Assembler::evpsubusw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD9);
  emit_operand(dst, src, 0);
}

void Assembler::evpmullw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD5, (0xC0 | encode));
}

void Assembler::evpmullw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xD5);
  emit_operand(dst, src, 0);
}

void Assembler::evpmulld(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x40, (0xC0 | encode));
}

void Assembler::evpmulld(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x40);
  emit_operand(dst, src, 0);
}

void Assembler::evpmullq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512dq() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x40, (0xC0 | encode));
}

void Assembler::evpmullq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512dq() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x40);
  emit_operand(dst, src, 0);
}

void Assembler::evpmulhw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE5, (0xC0 | encode));
}

void Assembler::evmulps(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::evmulps(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::evmulpd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::evmulpd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::evsqrtps(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len,/* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::evsqrtps(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x51);
  emit_operand(dst, src, 0);
}

void Assembler::evsqrtpd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len,/* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::evsqrtpd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x51);
  emit_operand(dst, src, 0);
}


void Assembler::evdivps(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len,/* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::evdivps(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::evdivpd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len,/* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::evdivpd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::evdivsd(XMMRegister dst, XMMRegister nds, XMMRegister src, EvexRoundPrefix rmode) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(rmode, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_extended_context();
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::evpabsb(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1C, (0xC0 | encode));
}


void Assembler::evpabsb(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x1C);
  emit_operand(dst, src, 0);
}

void Assembler::evpabsw(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1D, (0xC0 | encode));
}


void Assembler::evpabsw(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x1D);
  emit_operand(dst, src, 0);
}

void Assembler::evpabsd(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1E, (0xC0 | encode));
}


void Assembler::evpabsd(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x1E);
  emit_operand(dst, src, 0);
}

void Assembler::evpabsq(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x1F, (0xC0 | encode));
}


void Assembler::evpabsq(XMMRegister dst, KRegister mask, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x1F);
  emit_operand(dst, src, 0);
}

void Assembler::evpfma213ps(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xA8, (0xC0 | encode));
}

void Assembler::evpfma213ps(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xA8);
  emit_operand(dst, src, 0);
}

void Assembler::evpfma213pd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0xA8, (0xC0 | encode));
}

void Assembler::evpfma213pd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  InstructionMark im(this);
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true,/* legacy_mode */ false, /* no_mask_reg */ false,/* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV,/* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xA8);
  emit_operand(dst, src, 0);
}

void Assembler::evpermb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* rex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8D, (0xC0 | encode));
}

void Assembler::evpermb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x8D);
  emit_operand(dst, src, 0);
}

void Assembler::evpermw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8D, (0xC0 | encode));
}

void Assembler::evpermw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x8D);
  emit_operand(dst, src, 0);
}

void Assembler::evpermd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex() && vector_len > AVX_128bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x36, (0xC0 | encode));
}

void Assembler::evpermd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex() && vector_len > AVX_128bit, "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x36);
  emit_operand(dst, src, 0);
}

void Assembler::evpermq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex() && vector_len > AVX_128bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x36, (0xC0 | encode));
}

void Assembler::evpermq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex() && vector_len > AVX_128bit, "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x36);
  emit_operand(dst, src, 0);
}

void Assembler::evpsllw(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm6->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpslld(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm6->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsllq(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm6->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsrlw(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm2->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsrld(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm2->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsrlq(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm2->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x73, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsraw(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm4->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x71, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsrad(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm4->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsraq(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm4->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evpsllw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF1, (0xC0 | encode));
}

void Assembler::evpslld(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF2, (0xC0 | encode));
}

void Assembler::evpsllq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

void Assembler::evpsrlw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD1, (0xC0 | encode));
}

void Assembler::evpsrld(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD2, (0xC0 | encode));
}

void Assembler::evpsrlq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xD3, (0xC0 | encode));
}

void Assembler::evpsraw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE1, (0xC0 | encode));
}

void Assembler::evpsrad(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE2, (0xC0 | encode));
}

void Assembler::evpsraq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xE2, (0xC0 | encode));
}

void Assembler::evpsllvw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x12, (0xC0 | encode));
}

void Assembler::evpsllvd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

void Assembler::evpsllvq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x47, (0xC0 | encode));
}

void Assembler::evpsrlvw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x10, (0xC0 | encode));
}

void Assembler::evpsrlvd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

void Assembler::evpsrlvq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x45, (0xC0 | encode));
}

void Assembler::evpsravw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x11, (0xC0 | encode));
}

void Assembler::evpsravd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x46, (0xC0 | encode));
}

void Assembler::evpsravq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x46, (0xC0 | encode));
}

void Assembler::evpminsb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x38, (0xC0 | encode));
}

void Assembler::evpminsb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x38);
  emit_operand(dst, src, 0);
}

void Assembler::evpminsw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEA, (0xC0 | encode));
}

void Assembler::evpminsw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEA);
  emit_operand(dst, src, 0);
}

void Assembler::evpminsd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x39, (0xC0 | encode));
}

void Assembler::evpminsd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x39);
  emit_operand(dst, src, 0);
}

void Assembler::evpminsq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x39, (0xC0 | encode));
}

void Assembler::evpminsq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x39);
  emit_operand(dst, src, 0);
}


void Assembler::evpmaxsb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3C, (0xC0 | encode));
}

void Assembler::evpmaxsb(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3C);
  emit_operand(dst, src, 0);
}

void Assembler::evpmaxsw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16((unsigned char)0xEE, (0xC0 | encode));
}

void Assembler::evpmaxsw(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int8((unsigned char)0xEE);
  emit_operand(dst, src, 0);
}

void Assembler::evpmaxsd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3D, (0xC0 | encode));
}

void Assembler::evpmaxsd(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3D);
  emit_operand(dst, src, 0);
}

void Assembler::evpmaxsq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x3D, (0xC0 | encode));
}

void Assembler::evpmaxsq(XMMRegister dst, KRegister mask, XMMRegister nds, Address src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x3D);
  emit_operand(dst, src, 0);
}

void Assembler::evpternlogd(XMMRegister dst, int imm8, KRegister mask, XMMRegister src2, XMMRegister src3, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src3->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x25, (unsigned char)(0xC0 | encode), imm8);
}

void Assembler::evpternlogd(XMMRegister dst, int imm8, KRegister mask, XMMRegister src2, Address src3, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src3, src2->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x25);
  emit_operand(dst, src3, 1);
  emit_int8(imm8);
}

void Assembler::evpternlogq(XMMRegister dst, int imm8, KRegister mask, XMMRegister src2, XMMRegister src3, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src3->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x25, (unsigned char)(0xC0 | encode), imm8);
}

void Assembler::evpternlogq(XMMRegister dst, int imm8, KRegister mask, XMMRegister src2, Address src3, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "requires EVEX support");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "requires VL support");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  vex_prefix(src3, src2->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int8(0x25);
  emit_operand(dst, src3, 1);
  emit_int8(imm8);
}

void Assembler::gf2p8affineqb(XMMRegister dst, XMMRegister src, int imm8) {
  assert(VM_Version::supports_gfni(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24((unsigned char)0xCE, (unsigned char)(0xC0 | encode), imm8);
}

void Assembler::vgf2p8affineqb(XMMRegister dst, XMMRegister src2, XMMRegister src3, int imm8, int vector_len) {
  assert(VM_Version::supports_gfni(), "requires GFNI support");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src3->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24((unsigned char)0xCE, (unsigned char)(0xC0 | encode), imm8);
}

// duplicate 4-byte integer data from src into programmed locations in dest : requires AVX512VL
void Assembler::vpbroadcastd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(UseAVX >= 2, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::vpbroadcastd(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

// duplicate 8-byte integer data from src into programmed locations in dest : requires AVX512VL
void Assembler::vpbroadcastq(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::vpbroadcastq(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::evbroadcasti32x4(XMMRegister dst, Address src, int vector_len) {
  assert(vector_len != Assembler::AVX_128bit, "");
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x5A);
  emit_operand(dst, src, 0);
}

void Assembler::evbroadcasti64x2(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(vector_len != Assembler::AVX_128bit, "");
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x5A, (0xC0 | encode));
}

void Assembler::evbroadcasti64x2(XMMRegister dst, Address src, int vector_len) {
  assert(vector_len != Assembler::AVX_128bit, "");
  assert(VM_Version::supports_avx512dq(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  attributes.set_address_attributes(/* tuple_type */ EVEX_T2, /* input_size_in_bits */ EVEX_64bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x5A);
  emit_operand(dst, src, 0);
}

void Assembler::vbroadcasti128(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(vector_len == AVX_256bit, "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x5A);
  emit_operand(dst, src, 0);
}

// scalar single/double precision replicate

// duplicate single precision data from src into programmed locations in dest : requires AVX512VL
void Assembler::vbroadcastss(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x18, (0xC0 | encode));
}

void Assembler::vbroadcastss(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x18);
  emit_operand(dst, src, 0);
}

// duplicate double precision data from src into programmed locations in dest : requires AVX512VL
void Assembler::vbroadcastsd(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(vector_len == AVX_256bit || vector_len == AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x19, (0xC0 | encode));
}

void Assembler::vbroadcastsd(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  assert(vector_len == AVX_256bit || vector_len == AVX_512bit, "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_rex_vex_w_reverted();
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x19);
  emit_operand(dst, src, 0);
}

void Assembler::vbroadcastf128(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  assert(vector_len == AVX_256bit, "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T4, /* input_size_in_bits */ EVEX_32bit);
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x1A);
  emit_operand(dst, src, 0);
}

void Assembler::evbroadcastf64x2(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx512dq(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  assert(dst != xnoreg, "sanity");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T2, /* input_size_in_bits */ EVEX_64bit);
  attributes.set_is_evex_instruction();
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8(0x1A);
  emit_operand(dst, src, 0);
}


// gpr source broadcast forms

// duplicate 1-byte integer data from src into programmed locations in dest : requires AVX512BW and AVX512VL
void Assembler::evpbroadcastb(XMMRegister dst, Register src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16(0x7A, (0xC0 | encode));
}

// duplicate 2-byte integer data from src into programmed locations in dest : requires AVX512BW and AVX512VL
void Assembler::evpbroadcastw(XMMRegister dst, Register src, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes , true);
  emit_int16(0x7B, (0xC0 | encode));
}

// duplicate 4-byte integer data from src into programmed locations in dest : requires AVX512VL
void Assembler::evpbroadcastd(XMMRegister dst, Register src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16(0x7C, (0xC0 | encode));
}

// duplicate 8-byte integer data from src into programmed locations in dest : requires AVX512VL
void Assembler::evpbroadcastq(XMMRegister dst, Register src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16(0x7C, (0xC0 | encode));
}

void Assembler::vpgatherdd(XMMRegister dst, Address src, XMMRegister mask, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(!needs_eevex(src.base()), "does not support extended gprs as BASE of address operand");
  assert(vector_len == Assembler::AVX_128bit || vector_len == Assembler::AVX_256bit, "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x90);
  emit_operand(dst, src, 0);
}

void Assembler::vpgatherdq(XMMRegister dst, Address src, XMMRegister mask, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(!needs_eevex(src.base()), "does not support extended gprs as BASE of address operand");
  assert(vector_len == Assembler::AVX_128bit || vector_len == Assembler::AVX_256bit, "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x90);
  emit_operand(dst, src, 0);
}

void Assembler::vgatherdpd(XMMRegister dst, Address src, XMMRegister mask, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(!needs_eevex(src.base()), "does not support extended gprs as BASE of address operand");
  assert(vector_len == Assembler::AVX_128bit || vector_len == Assembler::AVX_256bit, "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x92);
  emit_operand(dst, src, 0);
}

void Assembler::vgatherdps(XMMRegister dst, Address src, XMMRegister mask, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(!needs_eevex(src.base()), "does not support extended gprs as BASE of address operand");
  assert(vector_len == Assembler::AVX_128bit || vector_len == Assembler::AVX_256bit, "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ false, /* uses_vl */ false);
  vex_prefix(src, mask->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x92);
  emit_operand(dst, src, 0);
}
void Assembler::evpgatherdd(XMMRegister dst, KRegister mask, Address src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x90);
  emit_operand(dst, src, 0);
}

void Assembler::evpgatherdq(XMMRegister dst, KRegister mask, Address src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x90);
  emit_operand(dst, src, 0);
}

void Assembler::evgatherdpd(XMMRegister dst, KRegister mask, Address src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x92);
  emit_operand(dst, src, 0);
}

void Assembler::evgatherdps(XMMRegister dst, KRegister mask, Address src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(dst != xnoreg, "sanity");
  assert(src.isxmmindex(),"expected to be xmm index");
  assert(dst != src.xmmindex(), "instruction will #UD if dst and index are the same");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  // swap src<->dst for encoding
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0x92);
  emit_operand(dst, src, 0);
}

void Assembler::evpscatterdd(Address dst, KRegister mask, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xA0);
  emit_operand(src, dst, 0);
}

void Assembler::evpscatterdq(Address dst, KRegister mask, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xA0);
  emit_operand(src, dst, 0);
}

void Assembler::evscatterdps(Address dst, KRegister mask, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xA2);
  emit_operand(src, dst, 0);
}

void Assembler::evscatterdpd(Address dst, KRegister mask, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(mask != k0, "instruction will #UD if mask is in k0");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_64bit);
  attributes.reset_is_clear_context();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  vex_prefix(dst, 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xA2);
  emit_operand(src, dst, 0);
}
// Carry-Less Multiplication Quadword
void Assembler::pclmulqdq(XMMRegister dst, XMMRegister src, int mask) {
  assert(VM_Version::supports_clmul(), "");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, dst, src, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x44, (0xC0 | encode), (unsigned char)mask);
}

// Carry-Less Multiplication Quadword
void Assembler::vpclmulqdq(XMMRegister dst, XMMRegister nds, XMMRegister src, int mask) {
  assert(VM_Version::supports_avx() && VM_Version::supports_clmul(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x44, (0xC0 | encode), (unsigned char)mask);
}

void Assembler::evpclmulqdq(XMMRegister dst, XMMRegister nds, XMMRegister src, int mask, int vector_len) {
  assert(VM_Version::supports_avx512_vpclmulqdq(), "Requires vector carryless multiplication support");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x44, (0xC0 | encode), (unsigned char)mask);
}

void Assembler::vzeroupper_uncached() {
  if (VM_Version::supports_vzeroupper()) {
    InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
    (void)vex_prefix_and_encode(0, 0, 0, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
    emit_int8(0x77);
  }
}

void Assembler::vfpclassss(KRegister kdst, XMMRegister src, uint8_t imm8) {
  // Encoding: EVEX.LIG.66.0F3A.W0 67 /r ib
  assert(VM_Version::supports_evex(), "");
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24((unsigned char)0x67, (unsigned char)(0xC0 | encode), imm8);
}

void Assembler::vfpclasssd(KRegister kdst, XMMRegister src, uint8_t imm8) {
  // Encoding: EVEX.LIG.66.0F3A.W1 67 /r ib
  assert(VM_Version::supports_evex(), "");
  assert(VM_Version::supports_avx512dq(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ false);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(kdst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24((unsigned char)0x67, (unsigned char)(0xC0 | encode), imm8);
}

void Assembler::fld_x(Address adr) {
  InstructionMark im(this);
  emit_int8((unsigned char)0xDB);
  emit_operand32(rbp, adr, 0);
}

void Assembler::fstp_x(Address adr) {
  InstructionMark im(this);
  emit_int8((unsigned char)0xDB);
  emit_operand32(rdi, adr, 0);
}

void Assembler::emit_operand32(Register reg, Address adr, int post_addr_length) {
  assert(reg->encoding() < 8, "no extended registers");
  assert(!adr.base_needs_rex() && !adr.index_needs_rex(), "no extended registers");
  emit_operand(reg, adr._base, adr._index, adr._scale, adr._disp, adr._rspec, post_addr_length);
}

void Assembler::fld_d(Address adr) {
  InstructionMark im(this);
  emit_int8((unsigned char)0xDD);
  emit_operand32(rax, adr, 0);
}

void Assembler::fprem() {
  emit_int16((unsigned char)0xD9, (unsigned char)0xF8);
}

void Assembler::fnstsw_ax() {
  emit_int16((unsigned char)0xDF, (unsigned char)0xE0);
}

void Assembler::fstp_d(Address adr) {
  InstructionMark im(this);
  emit_int8((unsigned char)0xDD);
  emit_operand32(rbx, adr, 0);
}

void Assembler::fstp_d(int index) {
  emit_farith(0xDD, 0xD8, index);
}

void Assembler::emit_farith(int b1, int b2, int i) {
  assert(isByte(b1) && isByte(b2), "wrong opcode");
  assert(0 <= i &&  i < 8, "illegal stack offset");
  emit_int16(b1, b2 + i);
}

// SSE SIMD prefix byte values corresponding to VexSimdPrefix encoding.
static int simd_pre[4] = { 0, 0x66, 0xF3, 0xF2 };
// SSE opcode second byte values (first is 0x0F) corresponding to VexOpcode encoding.
static int simd_opc[4] = { 0,    0, 0x38, 0x3A };

// Generate SSE legacy REX prefix and SIMD opcode based on VEX encoding.
void Assembler::rex_prefix(Address adr, XMMRegister xreg, VexSimdPrefix pre, VexOpcode opc, bool rex_w) {
  if (pre > 0) {
    emit_int8(simd_pre[pre]);
  }
  if (rex_w) {
    prefixq(adr, xreg);
  } else {
    prefix(adr, xreg);
  }
  if (opc > 0) {
    emit_int8(0x0F);
    int opc2 = simd_opc[opc];
    if (opc2 > 0) {
      emit_int8(opc2);
    }
  }
}

int Assembler::rex_prefix_and_encode(int dst_enc, int src_enc, VexSimdPrefix pre, VexOpcode opc, bool rex_w) {
  if (pre > 0) {
    emit_int8(simd_pre[pre]);
  }
  int encode = (rex_w) ? prefixq_and_encode(dst_enc, src_enc) : prefix_and_encode(dst_enc, src_enc);
  if (opc > 0) {
    emit_int8(0x0F);
    int opc2 = simd_opc[opc];
    if (opc2 > 0) {
      emit_int8(opc2);
    }
  }
  return encode;
}


void Assembler::vex_prefix(bool vex_r, bool vex_b, bool vex_x, int nds_enc, VexSimdPrefix pre, VexOpcode opc) {
  int vector_len = _attributes->get_vector_len();
  bool vex_w = _attributes->is_rex_vex_w();
  if (vex_b || vex_x || vex_w || (opc == VEX_OPCODE_0F_38) || (opc == VEX_OPCODE_0F_3A)) {
    int byte1 = (vex_r ? VEX_R : 0) | (vex_x ? VEX_X : 0) | (vex_b ? VEX_B : 0);
    byte1 = (~byte1) & 0xE0;
    byte1 |= opc;

    int byte2 = ((~nds_enc) & 0xf) << 3;
    byte2 |= (vex_w ? VEX_W : 0) | ((vector_len > 0) ? 4 : 0) | pre;

    emit_int24((unsigned char)VEX_3bytes, byte1, byte2);
  } else {
    int byte1 = vex_r ? VEX_R : 0;
    byte1 = (~byte1) & 0x80;
    byte1 |= ((~nds_enc) & 0xf) << 3;
    byte1 |= ((vector_len > 0 ) ? 4 : 0) | pre;
    emit_int16((unsigned char)VEX_2bytes, byte1);
  }
}

// This is a 4 byte encoding
void Assembler::evex_prefix(bool vex_r, bool vex_b, bool vex_x, bool evex_r, bool eevex_b, bool evex_v,
                       bool eevex_x, int nds_enc, VexSimdPrefix pre, VexOpcode opc, bool no_flags) {
  // EVEX 0x62 prefix
  // byte1 = EVEX_4bytes;

  bool vex_w = _attributes->is_rex_vex_w();
  int evex_encoding = (vex_w ? VEX_W : 0);
  // EVEX.b is not currently used for broadcast of single element or data rounding modes
  _attributes->set_evex_encoding(evex_encoding);

  // P0: byte 2, initialized to RXBR'0mmm
  // instead of not'd
  int byte2 = (vex_r ? VEX_R : 0) | (vex_x ? VEX_X : 0) | (vex_b ? VEX_B : 0) | (evex_r ? EVEX_Rb : 0);
  byte2 = (~byte2) & 0xF0;
  byte2 |= eevex_b ? EEVEX_B : 0;
  // confine opc opcode extensions in mm bits to lower two bits
  // of form {0F, 0F_38, 0F_3A, 0F_3C}
  byte2 |= opc;

  // P1: byte 3 as Wvvvv1pp
  int byte3 = ((~nds_enc) & 0xf) << 3;
  byte3 |= (eevex_x ? 0 : EEVEX_X);
  byte3 |= (vex_w & 1) << 7;
  // confine pre opcode extensions in pp bits to lower two bits
  // of form {66, F3, F2}
  byte3 |= pre;

  // P2: byte 4 as zL'Lbv'aaa or 00LXVF00 where V = V4, X(extended context) = ND and F = NF (no flags)
  int byte4 = 0;
  if (no_flags) {
    assert(_attributes->is_no_reg_mask(), "mask register not supported with no_flags");
    byte4 |= 0x4;
  } else {
    // kregs are implemented in the low 3 bits as aaa
    byte4 = (_attributes->is_no_reg_mask()) ?
                0 :
                _attributes->get_embedded_opmask_register_specifier();
  }
  // EVEX.v` for extending EVEX.vvvv or VIDX
  byte4 |= (evex_v ? 0: EVEX_V);
  // third EXEC.b for broadcast actions
  byte4 |= (_attributes->is_extended_context() ? EVEX_Rb : 0);
  // fourth EVEX.L'L for vector length : 0 is 128, 1 is 256, 2 is 512, currently we do not support 1024
  byte4 |= ((_attributes->get_vector_len())& 0x3) << 5;
  // last is EVEX.z for zero/merge actions
  if (_attributes->is_no_reg_mask() == false &&
      _attributes->get_embedded_opmask_register_specifier() != 0) {
    byte4 |= (_attributes->is_clear_context() ? EVEX_Z : 0);
  }
  emit_int32(EVEX_4bytes, byte2, byte3, byte4);
}

void Assembler::vex_prefix(Address adr, int nds_enc, int xreg_enc, VexSimdPrefix pre, VexOpcode opc, InstructionAttr *attributes, bool nds_is_ndd, bool no_flags) {
  if (adr.base_needs_rex2() || adr.index_needs_rex2() || nds_is_ndd || no_flags) {
    assert(UseAPX, "APX features not enabled");
  }
  if (nds_is_ndd) attributes->set_extended_context();
  bool is_extended = adr.base_needs_rex2() || adr.index_needs_rex2() || nds_enc >= 16 || xreg_enc >= 16 || nds_is_ndd;
  bool vex_r = (xreg_enc & 8) == 8;
  bool vex_b = adr.base_needs_rex();
  bool vex_x;
  if (adr.isxmmindex()) {
    vex_x = adr.xmmindex_needs_rex();
  } else {
    vex_x = adr.index_needs_rex();
  }
  set_attributes(attributes);
  // For EVEX instruction (which is not marked as pure EVEX instruction) check and see if this instruction
  // is allowed in legacy mode and has resources which will fit in it.
  // Pure EVEX instructions will have is_evex_instruction set in their definition.
  if (!attributes->is_legacy_mode()) {
    if (UseAVX > 2 && !attributes->is_evex_instruction()) {
      if ((attributes->get_vector_len() != AVX_512bit) && !is_extended) {
          attributes->set_is_legacy_mode();
      }
    }
  }

  if (UseAVX > 2) {
    assert(((!attributes->uses_vl()) ||
            (attributes->get_vector_len() == AVX_512bit) ||
            (!_legacy_mode_vl) ||
            (attributes->is_legacy_mode())),"XMM register should be 0-15");
    assert((!is_extended || (!attributes->is_legacy_mode())),"XMM register should be 0-15");
  }

  if (UseAVX > 2 && !attributes->is_legacy_mode())
  {
    bool evex_r = (xreg_enc >= 16);
    bool evex_v;
    // EVEX.V' is set to true when VSIB is used as we may need to use higher order XMM registers (16-31)
    if (adr.isxmmindex())  {
      evex_v = ((adr._xmmindex->encoding() > 15) ? true : false);
    } else {
      evex_v = (nds_enc >= 16);
    }
    bool eevex_x = adr.index_needs_rex2();
    bool eevex_b = adr.base_needs_rex2();
    attributes->set_is_evex_instruction();
    evex_prefix(vex_r, vex_b, vex_x, evex_r, eevex_b, evex_v, eevex_x, nds_enc, pre, opc, no_flags);
  } else {
    if (UseAVX > 2 && attributes->is_rex_vex_w_reverted()) {
      attributes->set_rex_vex_w(false);
    }
    vex_prefix(vex_r, vex_b, vex_x, nds_enc, pre, opc);
  }
}

void Assembler::eevex_prefix_ndd(Address adr, int ndd_enc, int xreg_enc, VexSimdPrefix pre, VexOpcode opc, InstructionAttr *attributes, bool no_flags) {
  attributes->set_is_evex_instruction();
  vex_prefix(adr, ndd_enc, xreg_enc, pre, opc, attributes, /* nds_is_ndd */ true, no_flags);
}

void Assembler::emit_eevex_or_demote(Register dst, Register src1, Address src2, VexSimdPrefix pre, VexOpcode opc,
                                     int size, int opcode_byte, bool no_flags, bool is_map1) {
  if (is_demotable(no_flags, dst->encoding(), src1->encoding())) {
    if (size == EVEX_64bit) {
      emit_prefix_and_int8(get_prefixq(src2, dst, is_map1), opcode_byte);
    } else {
      // For 32-bit, 16-bit and 8-bit
      if (size == EVEX_16bit) {
        emit_int8(0x66);
      }
      prefix(src2, dst, false, is_map1);
      emit_int8(opcode_byte);
    }
  } else {
    bool vex_w = (size == EVEX_64bit) ? true : false;
    InstructionAttr attributes(AVX_128bit, vex_w, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
    attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, size);
    eevex_prefix_ndd(src2, dst->encoding(), src1->encoding(), pre, opc, &attributes, no_flags);
    emit_int8(opcode_byte);
  }
  emit_operand(src1, src2, 0);
}

void Assembler::eevex_prefix_nf(Address adr, int ndd_enc, int xreg_enc, VexSimdPrefix pre, VexOpcode opc, InstructionAttr *attributes, bool no_flags) {
  attributes->set_is_evex_instruction();
  vex_prefix(adr, ndd_enc, xreg_enc, pre, opc, attributes, /* nds_is_ndd */ false, no_flags);
}

int Assembler::vex_prefix_and_encode(int dst_enc, int nds_enc, int src_enc, VexSimdPrefix pre, VexOpcode opc, InstructionAttr *attributes, bool src_is_gpr, bool nds_is_ndd, bool no_flags) {
  if (nds_is_ndd || no_flags || (src_is_gpr && src_enc >= 16)) {
    assert(UseAPX, "APX features not enabled");
  }
  if (nds_is_ndd) attributes->set_extended_context();
  bool is_extended = dst_enc >= 16 || nds_enc >= 16 || src_enc >=16;
  bool vex_r = (dst_enc & 8) == 8;
  bool vex_b = (src_enc & 8) == 8;
  bool vex_x = false;
  set_attributes(attributes);

  // For EVEX instruction (which is not marked as pure EVEX instruction) check and see if this instruction
  // is allowed in legacy mode and has resources which will fit in it.
  // Pure EVEX instructions will have is_evex_instruction set in their definition.
  if (!attributes->is_legacy_mode()) {
    if (UseAVX > 2 && !attributes->is_evex_instruction()) {
      if ((!attributes->uses_vl() || (attributes->get_vector_len() != AVX_512bit)) &&
           !is_extended) {
          attributes->set_is_legacy_mode();
      }
    }
  }

  if (UseAVX > 2) {
    // All the scalar fp instructions (with uses_vl as false) can have legacy_mode as false
    // Instruction with uses_vl true are vector instructions
    // All the vector instructions with AVX_512bit length can have legacy_mode as false
    // All the vector instructions with < AVX_512bit length can have legacy_mode as false if AVX512vl() is supported
    // Rest all should have legacy_mode set as true
    assert(((!attributes->uses_vl()) ||
            (attributes->get_vector_len() == AVX_512bit) ||
            (!_legacy_mode_vl) ||
            (attributes->is_legacy_mode())),"XMM register should be 0-15");
    // Instruction with legacy_mode true should have dst, nds and src < 15
    assert(((!is_extended) || (!attributes->is_legacy_mode())),"XMM register should be 0-15");
  }

  if (UseAVX > 2 && !attributes->is_legacy_mode())
  {
    bool evex_r = (dst_enc >= 16);
    bool evex_v = (nds_enc >= 16);
    bool evex_b = (src_enc >= 16) && src_is_gpr;
    // can use vex_x as bank extender on rm encoding
    vex_x = (src_enc >= 16) && !src_is_gpr;
    attributes->set_is_evex_instruction();
    evex_prefix(vex_r, vex_b, vex_x, evex_r, evex_b, evex_v, false /*eevex_x*/, nds_enc, pre, opc, no_flags);
  } else {
    if (UseAVX > 2 && attributes->is_rex_vex_w_reverted()) {
      attributes->set_rex_vex_w(false);
    }
    vex_prefix(vex_r, vex_b, vex_x, nds_enc, pre, opc);
  }

  // return modrm byte components for operands
  return (((dst_enc & 7) << 3) | (src_enc & 7));
}

void Assembler::emit_eevex_or_demote(int dst_enc, int nds_enc, int src_enc, int8_t imm8, VexSimdPrefix pre, VexOpcode opc,
                                     int size, int opcode_byte, bool no_flags, bool is_map1) {
  bool is_prefixq = (size == EVEX_64bit) ? true : false;
  if (is_demotable(no_flags, dst_enc, nds_enc)) {
    int encode = is_prefixq ? prefixq_and_encode(src_enc, dst_enc, is_map1) : prefix_and_encode(src_enc, dst_enc, is_map1);
    emit_opcode_prefix_and_encoding((unsigned char)(opcode_byte | 0x80), 0xC0, encode, imm8);
  } else {
    InstructionAttr attributes(AVX_128bit, is_prefixq, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
    attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, size);
    int encode = emit_eevex_prefix_or_demote_ndd(src_enc, dst_enc, nds_enc, pre, opc, &attributes, no_flags);
    emit_int24(opcode_byte, (0xC0 | encode), imm8);
  }
}

void Assembler::emit_eevex_or_demote(int dst_enc, int nds_enc, int src_enc, VexSimdPrefix pre, VexOpcode opc,
                                     int size, int opcode_byte, bool no_flags, bool is_map1, bool swap) {
  int encode;
  bool is_prefixq = (size == EVEX_64bit) ? true : false;
  if (is_demotable(no_flags, dst_enc, nds_enc)) {
    if (size == EVEX_16bit) {
      emit_int8(0x66);
    }

    if (swap) {
      encode = is_prefixq ? prefixq_and_encode(dst_enc, src_enc, is_map1) : prefix_and_encode(dst_enc, src_enc, is_map1);
    } else {
      encode = is_prefixq ? prefixq_and_encode(src_enc, dst_enc, is_map1) : prefix_and_encode(src_enc, dst_enc, is_map1);
    }
    emit_opcode_prefix_and_encoding((unsigned char)opcode_byte, 0xC0, encode);
  } else {
    InstructionAttr attributes(AVX_128bit, is_prefixq, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
    attributes.set_is_evex_instruction();
    if (swap) {
      encode = vex_prefix_and_encode(nds_enc, dst_enc, src_enc, pre, opc, &attributes, /* src_is_gpr */ true, /* nds_is_ndd */ true, no_flags);
    } else {
      encode = vex_prefix_and_encode(src_enc, dst_enc, nds_enc, pre, opc, &attributes, /* src_is_gpr */ true, /* nds_is_ndd */ true, no_flags);
    }
    emit_int16(opcode_byte, (0xC0 | encode));
  }
}

int Assembler::emit_eevex_prefix_or_demote_ndd(int dst_enc, int nds_enc, int src_enc, VexSimdPrefix pre, VexOpcode opc,
                                               InstructionAttr *attributes, bool no_flags, bool use_prefixq) {
  if (is_demotable(no_flags, dst_enc, nds_enc)) {
    if (pre == VEX_SIMD_66) {
      emit_int8(0x66);
    }
    return use_prefixq ? prefixq_and_encode(dst_enc, src_enc) : prefix_and_encode(dst_enc, src_enc);
  }
  attributes->set_is_evex_instruction();
  return vex_prefix_and_encode(dst_enc, nds_enc, src_enc, pre, opc, attributes, /* src_is_gpr */ true, /* nds_is_ndd */ true, no_flags);
}

int Assembler::emit_eevex_prefix_or_demote_ndd(int dst_enc, int nds_enc, VexSimdPrefix pre, VexOpcode opc,
                                               InstructionAttr *attributes, bool no_flags, bool use_prefixq) {
  //Demote RegReg and RegRegImm instructions
  if (is_demotable(no_flags, dst_enc, nds_enc)) {
    return use_prefixq ? prefixq_and_encode(dst_enc) : prefix_and_encode(dst_enc);
  }
  attributes->set_is_evex_instruction();
  return vex_prefix_and_encode(0, dst_enc, nds_enc, pre, opc, attributes, /* src_is_gpr */ true, /* nds_is_ndd */ true, no_flags);
}

int Assembler::emit_eevex_prefix_ndd(int dst_enc, VexSimdPrefix pre, VexOpcode opc, InstructionAttr *attributes, bool no_flags) {
  attributes->set_is_evex_instruction();
  return vex_prefix_and_encode(0, 0, dst_enc, pre, opc, attributes, /* src_is_gpr */ true, /* nds_is_ndd */ true, no_flags);
}

int Assembler::eevex_prefix_and_encode_nf(int dst_enc, int nds_enc, int src_enc, VexSimdPrefix pre, VexOpcode opc,
                                          InstructionAttr *attributes, bool no_flags) {
  attributes->set_is_evex_instruction();
  return vex_prefix_and_encode(dst_enc, nds_enc, src_enc, pre, opc, attributes, /* src_is_gpr */ true, /* nds_is_ndd */ false, no_flags);
}

void Assembler::emit_eevex_prefix_or_demote_arith_ndd(Register dst, Register nds, int32_t imm32, VexSimdPrefix pre, VexOpcode opc,
                                                      int size, int op1, int op2, bool no_flags) {
  int dst_enc = dst->encoding();
  int nds_enc = nds->encoding();
  bool demote = is_demotable(no_flags, dst_enc, nds_enc);
  if (demote) {
    (size == EVEX_64bit) ? (void) prefixq_and_encode(dst_enc) : (void) prefix_and_encode(dst_enc);
  } else {
    bool vex_w = (size == EVEX_64bit) ? true : false;
    InstructionAttr attributes(AVX_128bit, vex_w, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
    //attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, size);
    attributes.set_is_evex_instruction();
    vex_prefix_and_encode(0, dst_enc, nds_enc, pre, opc, &attributes, /* src_is_gpr */ true, /* nds_is_ndd */ true, no_flags);

  }
  emit_arith(op1, op2, nds, imm32, demote);
}

void Assembler::simd_prefix(XMMRegister xreg, XMMRegister nds, Address adr, VexSimdPrefix pre,
                            VexOpcode opc, InstructionAttr *attributes) {
  if (UseAVX > 0) {
    int xreg_enc = xreg->encoding();
    int nds_enc = nds->is_valid() ? nds->encoding() : 0;
    vex_prefix(adr, nds_enc, xreg_enc, pre, opc, attributes);
  } else {
    assert((nds == xreg) || (nds == xnoreg), "wrong sse encoding");
    rex_prefix(adr, xreg, pre, opc, attributes->is_rex_vex_w());
  }
}

int Assembler::simd_prefix_and_encode(XMMRegister dst, XMMRegister nds, XMMRegister src, VexSimdPrefix pre,
                                      VexOpcode opc, InstructionAttr *attributes, bool src_is_gpr) {
  int dst_enc = dst->encoding();
  int src_enc = src->encoding();
  if (UseAVX > 0) {
    int nds_enc = nds->is_valid() ? nds->encoding() : 0;
    return vex_prefix_and_encode(dst_enc, nds_enc, src_enc, pre, opc, attributes, src_is_gpr);
  } else {
    assert((nds == dst) || (nds == src) || (nds == xnoreg), "wrong sse encoding");
    return rex_prefix_and_encode(dst_enc, src_enc, pre, opc, attributes->is_rex_vex_w());
  }
}

bool Assembler::is_demotable(bool no_flags, int dst_enc, int nds_enc) {
  return (!no_flags && dst_enc == nds_enc);
}

void Assembler::vmaxss(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::vmaxsd(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::vminss(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}

void Assembler::vminsd(XMMRegister dst, XMMRegister nds, XMMRegister src) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ VM_Version::supports_evex(), /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_rex_vex_w_reverted();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}

void Assembler::vcmppd(XMMRegister dst, XMMRegister nds, XMMRegister src, int cop, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  assert(vector_len <= AVX_256bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = simd_prefix_and_encode(dst, nds, src, VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC2, (0xC0 | encode), (0xF & cop));
}

void Assembler::blendvpb(XMMRegister dst, XMMRegister nds, XMMRegister src1, XMMRegister src2, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  assert(vector_len <= AVX_256bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src1->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int src2_enc = src2->encoding();
  emit_int24(0x4C, (0xC0 | encode), (0xF0 & src2_enc << 4));
}

void Assembler::vblendvpd(XMMRegister dst, XMMRegister nds, XMMRegister src1, XMMRegister src2, int vector_len) {
  assert(UseAVX > 0 && (vector_len == AVX_128bit || vector_len == AVX_256bit), "");
  assert(vector_len <= AVX_256bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src1->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int src2_enc = src2->encoding();
  emit_int24(0x4B, (0xC0 | encode), (0xF0 & src2_enc << 4));
}

void Assembler::vpblendd(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8, int vector_len) {
  assert(VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x02, (0xC0 | encode), (unsigned char)imm8);
}

void Assembler::vcmpps(XMMRegister dst, XMMRegister nds, XMMRegister src, int comparison, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  assert(vector_len <= AVX_256bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC2, (0xC0 | encode), (unsigned char)comparison);
}

void Assembler::evcmpph(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src,
                        ComparisonPredicateFP comparison, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3A, &attributes);
  emit_int24((unsigned char)0xC2, (0xC0 | encode), comparison);
}

void Assembler::evcmpsh(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src, ComparisonPredicateFP comparison) {
  assert(VM_Version::supports_avx512_fp16(), "");
  InstructionAttr attributes(Assembler::AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_3A, &attributes);
  emit_int24((unsigned char)0xC2, (0xC0 | encode), comparison);
}

void Assembler::evcmpps(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src,
                        ComparisonPredicateFP comparison, int vector_len) {
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  // Encoding: EVEX.NDS.XXX.0F.W0 C2 /r ib
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC2, (0xC0 | encode), comparison);
}

void Assembler::evcmppd(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src,
                        ComparisonPredicateFP comparison, int vector_len) {
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  // Encoding: EVEX.NDS.XXX.66.0F.W1 C2 /r ib
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24((unsigned char)0xC2, (0xC0 | encode), comparison);
}

void Assembler::blendvps(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  assert(UseAVX <= 0, "sse encoding is inconsistent with avx encoding");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x14, (0xC0 | encode));
}

void Assembler::blendvpd(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  assert(UseAVX <= 0, "sse encoding is inconsistent with avx encoding");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x15, (0xC0 | encode));
}

void Assembler::pblendvb(XMMRegister dst, XMMRegister src) {
  assert(VM_Version::supports_sse4_1(), "");
  assert(UseAVX <= 0, "sse encoding is inconsistent with avx encoding");
  InstructionAttr attributes(AVX_128bit, /* rex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, src, VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x10, (0xC0 | encode));
}

void Assembler::vblendvps(XMMRegister dst, XMMRegister nds, XMMRegister src1, XMMRegister src2, int vector_len) {
  assert(UseAVX > 0 && (vector_len == AVX_128bit || vector_len == AVX_256bit), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src1->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int src2_enc = src2->encoding();
  emit_int24(0x4A, (0xC0 | encode), (0xF0 & src2_enc << 4));
}

void Assembler::vblendps(XMMRegister dst, XMMRegister nds, XMMRegister src, int imm8, int vector_len) {
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  emit_int24(0x0C, (0xC0 | encode), imm8);
}

void Assembler::vpcmpgtb(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x64, (0xC0 | encode));
}

void Assembler::vpcmpgtw(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x65, (0xC0 | encode));
}

void Assembler::vpcmpgtd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int16(0x66, (0xC0 | encode));
}

void Assembler::vpcmpgtq(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len == AVX_128bit ? VM_Version::supports_avx() : VM_Version::supports_avx2(), "");
  assert(vector_len <= AVX_256bit, "evex encoding is different - has k register as dest");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x37, (0xC0 | encode));
}

void Assembler::evpcmpd(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W0 1F /r ib
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x1F : 0x1E;
  emit_int24(opcode, (0xC0 | encode), comparison);
}

void Assembler::evpcmpd(KRegister kdst, KRegister mask, XMMRegister nds, Address src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W0 1F /r ib
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x1F : 0x1E;
  emit_int8((unsigned char)opcode);
  emit_operand(as_Register(dst_enc), src, 1);
  emit_int8((unsigned char)comparison);
}

void Assembler::evpcmpq(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W1 1F /r ib
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x1F : 0x1E;
  emit_int24(opcode, (0xC0 | encode), comparison);
}

void Assembler::evpcmpq(KRegister kdst, KRegister mask, XMMRegister nds, Address src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W1 1F /r ib
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x1F : 0x1E;
  emit_int8((unsigned char)opcode);
  emit_operand(as_Register(dst_enc), src, 1);
  emit_int8((unsigned char)comparison);
}

void Assembler::evpcmpb(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(VM_Version::supports_avx512bw(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W0 3F /r ib
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x3F : 0x3E;
  emit_int24(opcode, (0xC0 | encode), comparison);
}

void Assembler::evpcmpb(KRegister kdst, KRegister mask, XMMRegister nds, Address src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(VM_Version::supports_avx512bw(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W0 3F /r ib
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x3F : 0x3E;
  emit_int8((unsigned char)opcode);
  emit_operand(as_Register(dst_enc), src, 1);
  emit_int8((unsigned char)comparison);
}

void Assembler::evpcmpw(KRegister kdst, KRegister mask, XMMRegister nds, XMMRegister src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(VM_Version::supports_avx512bw(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W1 3F /r ib
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int encode = vex_prefix_and_encode(kdst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x3F : 0x3E;
  emit_int24(opcode, (0xC0 | encode), comparison);
}

void Assembler::evpcmpw(KRegister kdst, KRegister mask, XMMRegister nds, Address src,
                        int comparison, bool is_signed, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(VM_Version::supports_avx512bw(), "");
  assert(comparison >= Assembler::eq && comparison <= Assembler::_true, "");
  // Encoding: EVEX.NDS.XXX.66.0F3A.W1 3F /r ib
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_FVM, /* input_size_in_bits */ EVEX_NObit);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.reset_is_clear_context();
  int dst_enc = kdst->encoding();
  vex_prefix(src, nds->encoding(), dst_enc, VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int opcode = is_signed ? 0x3F : 0x3E;
  emit_int8((unsigned char)opcode);
  emit_operand(as_Register(dst_enc), src, 1);
  emit_int8((unsigned char)comparison);
}

void Assembler::evprord(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm0->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprorq(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm0->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprorvd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x14, (0xC0 | encode));
}

void Assembler::evprorvq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x14, (0xC0 | encode));
}

void Assembler::evprold(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm1->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprolq(XMMRegister dst, KRegister mask, XMMRegister src, int shift, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(xmm1->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F, &attributes);
  emit_int24(0x72, (0xC0 | encode), shift & 0xFF);
}

void Assembler::evprolvd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x15, (0xC0 | encode));
}

void Assembler::evprolvq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x15, (0xC0 | encode));
}

void Assembler::vpblendvb(XMMRegister dst, XMMRegister nds, XMMRegister src, XMMRegister mask, int vector_len) {
  assert(VM_Version::supports_avx(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3A, &attributes);
  int mask_enc = mask->encoding();
  emit_int24(0x4C, (0xC0 | encode), 0xF0 & mask_enc << 4);
}

void Assembler::evblendmpd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  // Encoding: EVEX.NDS.XXX.66.0F38.W1 65 /r
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x65, (0xC0 | encode));
}

void Assembler::evblendmps(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  // Encoding: EVEX.NDS.XXX.66.0F38.W0 65 /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x65, (0xC0 | encode));
}

void Assembler::evpblendmb(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  // Encoding: EVEX.NDS.512.66.0F38.W0 66 /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x66, (0xC0 | encode));
}

void Assembler::evpblendmw(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512bw(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  // Encoding: EVEX.NDS.512.66.0F38.W1 66 /r
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ _legacy_mode_bw, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x66, (0xC0 | encode));
}

void Assembler::evpblendmd(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  //Encoding: EVEX.NDS.512.66.0F38.W0 64 /r
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x64, (0xC0 | encode));
}

void Assembler::evpblendmq(XMMRegister dst, KRegister mask, XMMRegister nds, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  //Encoding: EVEX.NDS.512.66.0F38.W1 64 /r
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_embedded_opmask_register_specifier(mask);
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x64, (0xC0 | encode));
}

void Assembler::bzhiq(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::bzhil(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::pextl(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::pdepl(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::pextq(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::pdepq(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::pextl(Register dst, Register src1, Address src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF5);
  emit_operand(dst, src2, 0);
}

void Assembler::pdepl(Register dst, Register src1, Address src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF5);
  emit_operand(dst, src2, 0);
}

void Assembler::pextq(Register dst, Register src1, Address src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF5);
  emit_operand(dst, src2, 0);
}

void Assembler::pdepq(Register dst, Register src1, Address src2) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF5);
  emit_operand(dst, src2, 0);
}

void Assembler::sarxl(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF7, (0xC0 | encode));
}

void Assembler::sarxl(Register dst, Address src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src1, src2->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF7);
  emit_operand(dst, src1, 0);
}

void Assembler::sarxq(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF7, (0xC0 | encode));
}

void Assembler::sarxq(Register dst, Address src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src1, src2->encoding(), dst->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF7);
  emit_operand(dst, src1, 0);
}

void Assembler::shlxl(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF7, (0xC0 | encode));
}

void Assembler::shlxl(Register dst, Address src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src1, src2->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF7);
  emit_operand(dst, src1, 0);
}

void Assembler::shlxq(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF7, (0xC0 | encode));
}

void Assembler::shlxq(Register dst, Address src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src1, src2->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF7);
  emit_operand(dst, src1, 0);
}

void Assembler::shrxl(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF7, (0xC0 | encode));
}

void Assembler::shrxl(Register dst, Address src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src1, src2->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF7);
  emit_operand(dst, src1, 0);
}

void Assembler::shrxq(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  int encode = vex_prefix_and_encode(dst->encoding(), src2->encoding(), src1->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF7, (0xC0 | encode));
}

void Assembler::shrxq(Register dst, Address src1, Register src2) {
  assert(VM_Version::supports_bmi2(), "");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src1, src2->encoding(), dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF7);
  emit_operand(dst, src1, 0);
}

void Assembler::evpmovq2m(KRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vldq(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x39, (0xC0 | encode));
}

void Assembler::evpmovd2m(KRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vldq(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x39, (0xC0 | encode));
}

void Assembler::evpmovw2m(KRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vlbw(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x29, (0xC0 | encode));
}

void Assembler::evpmovb2m(KRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vlbw(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x29, (0xC0 | encode));
}

void Assembler::evpmovm2q(XMMRegister dst, KRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vldq(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x38, (0xC0 | encode));
}

void Assembler::evpmovm2d(XMMRegister dst, KRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vldq(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x38, (0xC0 | encode));
}

void Assembler::evpmovm2w(XMMRegister dst, KRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vlbw(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x28, (0xC0 | encode));
}

void Assembler::evpmovm2b(XMMRegister dst, KRegister src, int vector_len) {
  assert(VM_Version::supports_avx512vlbw(), "");
  assert(VM_Version::supports_avx512vl() || vector_len == Assembler::AVX_512bit, "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x28, (0xC0 | encode));
}

void Assembler::evpcompressb(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi2(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x63, (0xC0 | encode));
}

void Assembler::evpcompressw(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi2(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x63, (0xC0 | encode));
}

void Assembler::evpcompressd(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8B, (0xC0 | encode));
}

void Assembler::evpcompressq(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8B, (0xC0 | encode));
}

void Assembler::evcompressps(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8A, (0xC0 | encode));
}

void Assembler::evcompresspd(XMMRegister dst, KRegister mask, XMMRegister src, bool merge, int vector_len) {
  assert(VM_Version::supports_evex(), "");
  assert(vector_len == AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ false, /* uses_vl */ true);
  attributes.set_embedded_opmask_register_specifier(mask);
  attributes.set_is_evex_instruction();
  if (merge) {
    attributes.reset_is_clear_context();
  }
  int encode = vex_prefix_and_encode(src->encoding(), 0, dst->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16((unsigned char)0x8A, (0xC0 | encode));
}

// This should only be used by 64bit instructions that can use rip-relative
// it cannot be used by instructions that want an immediate value.

// Determine whether an address is always reachable in rip-relative addressing mode
// when accessed from the code cache.
static bool is_always_reachable(address target, relocInfo::relocType reloc_type) {
  switch (reloc_type) {
    // This should be rip-relative and easily reachable.
    case relocInfo::internal_word_type: {
      return true;
    }
    // This should be rip-relative within the code cache and easily
    // reachable until we get huge code caches. (At which point
    // IC code is going to have issues).
    case relocInfo::virtual_call_type:
    case relocInfo::opt_virtual_call_type:
    case relocInfo::static_call_type:
    case relocInfo::static_stub_type: {
      return true;
    }
    case relocInfo::runtime_call_type:
    case relocInfo::external_word_type:
    case relocInfo::poll_return_type: // these are really external_word but need special
    case relocInfo::poll_type: {      // relocs to identify them
      return CodeCache::contains(target);
    }
    default: {
      return false;
    }
  }
}

// Determine whether an address is reachable in rip-relative addressing mode from the code cache.
static bool is_reachable(address target, relocInfo::relocType reloc_type) {
  if (is_always_reachable(target, reloc_type)) {
    return true;
  }
  switch (reloc_type) {
    // None will force a 64bit literal to the code stream. Likely a placeholder
    // for something that will be patched later and we need to certain it will
    // always be reachable.
    case relocInfo::none: {
      return false;
    }
    case relocInfo::runtime_call_type:
    case relocInfo::external_word_type:
    case relocInfo::poll_return_type: // these are really external_word but need special
    case relocInfo::poll_type: {      // relocs to identify them
      assert(!CodeCache::contains(target), "always reachable");
      if (ForceUnreachable) {
        return false; // stress the correction code
      }
      // For external_word_type/runtime_call_type if it is reachable from where we
      // are now (possibly a temp buffer) and where we might end up
      // anywhere in the code cache then we are always reachable.
      // This would have to change if we ever save/restore shared code to be more pessimistic.
      // Code buffer has to be allocated in the code cache, so check against
      // code cache boundaries cover that case.
      //
      // In rip-relative addressing mode, an effective address is formed by adding displacement
      // to the 64-bit RIP of the next instruction which is not known yet. Considering target address
      // is guaranteed to be outside of the code cache, checking against code cache boundaries is enough
      // to account for that.
      return Assembler::is_simm32(target - CodeCache::low_bound()) &&
             Assembler::is_simm32(target - CodeCache::high_bound());
    }
    default: {
      return false;
    }
  }
}

bool Assembler::reachable(AddressLiteral adr) {
  assert(CodeCache::contains(pc()), "required");
  if (adr.is_lval()) {
    return false;
  }
  return is_reachable(adr.target(), adr.reloc());
}

bool Assembler::always_reachable(AddressLiteral adr) {
  assert(CodeCache::contains(pc()), "required");
  if (adr.is_lval()) {
    return false;
  }
  return is_always_reachable(adr.target(), adr.reloc());
}

void Assembler::emit_data64(jlong data,
                            relocInfo::relocType rtype,
                            int format) {
  if (rtype == relocInfo::none) {
    emit_int64(data);
  } else {
    emit_data64(data, Relocation::spec_simple(rtype), format);
  }
}

void Assembler::emit_data64(jlong data,
                            RelocationHolder const& rspec,
                            int format) {
  assert(imm_operand == 0, "default format must be immediate in this file");
  assert(imm_operand == format, "must be immediate");
  assert(inst_mark() != nullptr, "must be inside InstructionMark");
  // Do not use AbstractAssembler::relocate, which is not intended for
  // embedded words.  Instead, relocate to the enclosing instruction.
  code_section()->relocate(inst_mark(), rspec, format);
#ifdef ASSERT
  check_relocation(rspec, format);
#endif
  emit_int64(data);
}

int Assembler::get_base_prefix_bits(int enc) {
  int bits = 0;
  if (enc & 16) bits |= REX2BIT_B4;
  if (enc & 8) bits |= REX2BIT_B;
  return bits;
}

int Assembler::get_index_prefix_bits(int enc) {
  int bits = 0;
  if (enc & 16) bits |= REX2BIT_X4;
  if (enc & 8) bits |= REX2BIT_X;
  return bits;
}

int Assembler::get_base_prefix_bits(Register base) {
  return base->is_valid() ? get_base_prefix_bits(base->encoding()) : 0;
}

int Assembler::get_index_prefix_bits(Register index) {
  return index->is_valid() ? get_index_prefix_bits(index->encoding()) : 0;
}

int Assembler::get_reg_prefix_bits(int enc) {
  int bits = 0;
  if (enc & 16) bits |= REX2BIT_R4;
  if (enc & 8) bits |= REX2BIT_R;
  return bits;
}

void Assembler::prefix(Register reg) {
  if (reg->encoding() >= 16) {
    prefix16(WREX2 | get_base_prefix_bits(reg->encoding()));
  } else if (reg->encoding() >= 8) {
    prefix(REX_B);
  }
}

void Assembler::prefix(Register dst, Register src, Prefix p) {
  if ((p & WREX2) || src->encoding() >= 16 || dst->encoding() >= 16) {
    prefix_rex2(dst, src);
    return;
  }
  if (src->encoding() >= 8) {
    p = (Prefix)(p | REX_B);
  }
  if (dst->encoding() >= 8) {
    p = (Prefix)(p | REX_R);
  }
  if (p != Prefix_EMPTY) {
    // do not generate an empty prefix
    prefix(p);
  }
}

void Assembler::prefix_rex2(Register dst, Register src) {
  int bits = 0;
  bits |= get_base_prefix_bits(src->encoding());
  bits |= get_reg_prefix_bits(dst->encoding());
  prefix16(WREX2 | bits);
}

void Assembler::prefix(Register dst, Address adr, Prefix p) {
  if (adr.base_needs_rex2() || adr.index_needs_rex2() || dst->encoding() >= 16) {
    prefix_rex2(dst, adr);
  }
  if (adr.base_needs_rex()) {
    if (adr.index_needs_rex()) {
      assert(false, "prefix(Register dst, Address adr, Prefix p) does not support handling of an X");
    } else {
      p = (Prefix)(p | REX_B);
    }
  } else {
    if (adr.index_needs_rex()) {
      assert(false, "prefix(Register dst, Address adr, Prefix p) does not support handling of an X");
    }
  }
  if (dst->encoding() >= 8) {
    p = (Prefix)(p | REX_R);
  }
  if (p != Prefix_EMPTY) {
    // do not generate an empty prefix
    prefix(p);
  }
}

void Assembler::prefix_rex2(Register dst, Address adr) {
  assert(!adr.index_needs_rex2(), "prefix(Register dst, Address adr) does not support handling of an X");
  int bits = 0;
  bits |= get_base_prefix_bits(adr.base());
  bits |= get_reg_prefix_bits(dst->encoding());
  prefix16(WREX2 | bits);
}

void Assembler::prefix(Address adr, bool is_map1) {
  if (adr.base_needs_rex2() || adr.index_needs_rex2()) {
    prefix_rex2(adr, is_map1);
    return;
  }
  if (adr.base_needs_rex()) {
    if (adr.index_needs_rex()) {
      prefix(REX_XB);
    } else {
      prefix(REX_B);
    }
  } else {
    if (adr.index_needs_rex()) {
      prefix(REX_X);
    }
  }
  if (is_map1) emit_int8(0x0F);
}

void Assembler::prefix_rex2(Address adr, bool is_map1) {
  int bits = is_map1 ? REX2BIT_M0 : 0;
  bits |= get_base_prefix_bits(adr.base());
  bits |= get_index_prefix_bits(adr.index());
  prefix16(WREX2 | bits);
}

void Assembler::prefix(Address adr, Register reg, bool byteinst, bool is_map1) {
  if (reg->encoding() >= 16 || adr.base_needs_rex2() || adr.index_needs_rex2()) {
    prefix_rex2(adr, reg, byteinst, is_map1);
    return;
  }
  if (reg->encoding() < 8) {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        prefix(REX_XB);
      } else {
        prefix(REX_B);
      }
    } else {
      if (adr.index_needs_rex()) {
        prefix(REX_X);
      } else if (byteinst && reg->encoding() >= 4) {
        prefix(REX);
      }
    }
  } else {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        prefix(REX_RXB);
      } else {
        prefix(REX_RB);
      }
    } else {
      if (adr.index_needs_rex()) {
        prefix(REX_RX);
      } else {
        prefix(REX_R);
      }
    }
  }
  if (is_map1) emit_int8(0x0F);
}

void Assembler::prefix_rex2(Address adr, Register reg, bool byteinst, bool is_map1) {
  int bits = is_map1 ? REX2BIT_M0 : 0;
  bits |= get_base_prefix_bits(adr.base());
  bits |= get_index_prefix_bits(adr.index());
  bits |= get_reg_prefix_bits(reg->encoding());
  prefix16(WREX2 | bits);
}

void Assembler::prefix(Address adr, XMMRegister reg) {
  if (reg->encoding() >= 16 || adr.base_needs_rex2() || adr.index_needs_rex2()) {
    prefixq_rex2(adr, reg);
    return;
  }
  if (reg->encoding() < 8) {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        prefix(REX_XB);
      } else {
        prefix(REX_B);
      }
    } else {
      if (adr.index_needs_rex()) {
        prefix(REX_X);
      }
    }
  } else {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        prefix(REX_RXB);
      } else {
        prefix(REX_RB);
      }
    } else {
      if (adr.index_needs_rex()) {
        prefix(REX_RX);
      } else {
        prefix(REX_R);
      }
    }
  }
}

void Assembler::prefix_rex2(Address adr, XMMRegister src) {
  int bits = 0;
  bits |= get_base_prefix_bits(adr.base());
  bits |= get_index_prefix_bits(adr.index());
  bits |= get_reg_prefix_bits(src->encoding());
  prefix16(WREX2 | bits);
}

int Assembler::prefix_and_encode(int reg_enc, bool byteinst, bool is_map1) {
  if (reg_enc >= 16) {
    return prefix_and_encode_rex2(reg_enc, is_map1);
  }
  if (reg_enc >= 8) {
    prefix(REX_B);
    reg_enc -= 8;
  } else if (byteinst && reg_enc >= 4) {
    prefix(REX);
  }
  int opc_prefix = is_map1 ? 0x0F00 : 0;
  return opc_prefix | reg_enc;
}

int Assembler::prefix_and_encode_rex2(int reg_enc, bool is_map1) {
  prefix16(WREX2 | (is_map1 ? REX2BIT_M0 : 0) | get_base_prefix_bits(reg_enc));
  return reg_enc & 0x7;
}

int Assembler::prefix_and_encode(int dst_enc, bool dst_is_byte, int src_enc, bool src_is_byte, bool is_map1) {
  if (src_enc >= 16 || dst_enc >= 16) {
    return prefix_and_encode_rex2(dst_enc, src_enc, is_map1 ? REX2BIT_M0 : 0);
  }
  if (dst_enc < 8) {
    if (src_enc >= 8) {
      prefix(REX_B);
      src_enc -= 8;
    } else if ((src_is_byte && src_enc >= 4) || (dst_is_byte && dst_enc >= 4)) {
      prefix(REX);
    }
  } else {
    if (src_enc < 8) {
      prefix(REX_R);
    } else {
      prefix(REX_RB);
      src_enc -= 8;
    }
    dst_enc -= 8;
  }
  int opcode_prefix = is_map1 ? 0x0F00 : 0;
  return opcode_prefix | (dst_enc << 3 | src_enc);
}

int Assembler::prefix_and_encode_rex2(int dst_enc, int src_enc, int init_bits) {
  int bits = init_bits;
  bits |= get_reg_prefix_bits(dst_enc);
  bits |= get_base_prefix_bits(src_enc);
  dst_enc &= 0x7;
  src_enc &= 0x7;
  prefix16(WREX2 | bits);
  return dst_enc << 3 | src_enc;
}

bool Assembler::prefix_is_rex2(int prefix) {
  return (prefix & 0xFF00) == WREX2;
}

int Assembler::get_prefixq_rex2(Address adr, bool is_map1) {
  assert(UseAPX, "APX features not enabled");
  int bits = REX2BIT_W;
  if (is_map1) bits |= REX2BIT_M0;
  bits |= get_base_prefix_bits(adr.base());
  bits |= get_index_prefix_bits(adr.index());
  return WREX2 | bits;
}

int Assembler::get_prefixq(Address adr, bool is_map1) {
  if (adr.base_needs_rex2() || adr.index_needs_rex2()) {
    return get_prefixq_rex2(adr, is_map1);
  }
  int8_t prfx = get_prefixq(adr, rax);
  assert(REX_W <= prfx && prfx <= REX_WXB, "must be");
  return is_map1 ? (((int16_t)prfx) << 8) | 0x0F : (int16_t)prfx;
}

int Assembler::get_prefixq(Address adr, Register src, bool is_map1) {
  if (adr.base_needs_rex2() || adr.index_needs_rex2() || src->encoding() >= 16) {
    return get_prefixq_rex2(adr, src, is_map1);
  }
  int8_t prfx = (int8_t)(REX_W +
                         ((int)adr.base_needs_rex()) +
                         ((int)adr.index_needs_rex() << 1) +
                         ((int)(src->encoding() >= 8) << 2));
#ifdef ASSERT
  if (src->encoding() < 8) {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        assert(prfx == REX_WXB, "must be");
      } else {
        assert(prfx == REX_WB, "must be");
      }
    } else {
      if (adr.index_needs_rex()) {
        assert(prfx == REX_WX, "must be");
      } else {
        assert(prfx == REX_W, "must be");
      }
    }
  } else {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        assert(prfx == REX_WRXB, "must be");
      } else {
        assert(prfx == REX_WRB, "must be");
      }
    } else {
      if (adr.index_needs_rex()) {
        assert(prfx == REX_WRX, "must be");
      } else {
        assert(prfx == REX_WR, "must be");
      }
    }
  }
#endif
  return is_map1 ? (((int16_t)prfx) << 8) | 0x0F : (int16_t)prfx;
}

int Assembler::get_prefixq_rex2(Address adr, Register src, bool is_map1) {
  assert(UseAPX, "APX features not enabled");
  int bits = REX2BIT_W;
  if (is_map1) bits |= REX2BIT_M0;
  bits |= get_base_prefix_bits(adr.base());
  bits |= get_index_prefix_bits(adr.index());
  bits |= get_reg_prefix_bits(src->encoding());
  return WREX2 | bits;
}

void Assembler::prefixq(Address adr) {
  if (adr.base_needs_rex2() || adr.index_needs_rex2()) {
    prefix16(get_prefixq_rex2(adr));
  } else {
    emit_int8(get_prefixq(adr));
  }
}

void Assembler::prefixq(Address adr, Register src, bool is_map1) {
  if (adr.base_needs_rex2() || adr.index_needs_rex2() || src->encoding() >= 16) {
    prefix16(get_prefixq_rex2(adr, src, is_map1));
  } else {
    emit_int8(get_prefixq(adr, src));
    if (is_map1) emit_int8(0x0F);
  }
}


void Assembler::prefixq(Address adr, XMMRegister src) {
  if (src->encoding() >= 16 || adr.base_needs_rex2() || adr.index_needs_rex2()) {
    prefixq_rex2(adr, src);
    return;
  }
  if (src->encoding() < 8) {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        prefix(REX_WXB);
      } else {
        prefix(REX_WB);
      }
    } else {
      if (adr.index_needs_rex()) {
        prefix(REX_WX);
      } else {
        prefix(REX_W);
      }
    }
  } else {
    if (adr.base_needs_rex()) {
      if (adr.index_needs_rex()) {
        prefix(REX_WRXB);
      } else {
        prefix(REX_WRB);
      }
    } else {
      if (adr.index_needs_rex()) {
        prefix(REX_WRX);
      } else {
        prefix(REX_WR);
      }
    }
  }
}

void Assembler::prefixq_rex2(Address adr, XMMRegister src) {
  int bits = REX2BIT_W;
  bits |= get_base_prefix_bits(adr.base());
  bits |= get_index_prefix_bits(adr.index());
  bits |= get_reg_prefix_bits(src->encoding());
  prefix16(WREX2 | bits);
}

int Assembler::prefixq_and_encode(int reg_enc, bool is_map1) {
  if (reg_enc >= 16) {
    return prefixq_and_encode_rex2(reg_enc, is_map1);
  }
  if (reg_enc < 8) {
    prefix(REX_W);
  } else {
    prefix(REX_WB);
    reg_enc -= 8;
  }
  int opcode_prefix = is_map1 ? 0x0F00 : 0;
  return opcode_prefix | reg_enc;
}


int Assembler::prefixq_and_encode_rex2(int reg_enc, bool is_map1) {
  prefix16(WREX2 | REX2BIT_W | (is_map1 ? REX2BIT_M0: 0) | get_base_prefix_bits(reg_enc));
  return reg_enc & 0x7;
}

int Assembler::prefixq_and_encode(int dst_enc, int src_enc, bool is_map1) {
  if (dst_enc >= 16 || src_enc >= 16) {
    return prefixq_and_encode_rex2(dst_enc, src_enc, is_map1);
  }
  if (dst_enc < 8) {
    if (src_enc < 8) {
      prefix(REX_W);
    } else {
      prefix(REX_WB);
      src_enc -= 8;
    }
  } else {
    if (src_enc < 8) {
      prefix(REX_WR);
    } else {
      prefix(REX_WRB);
      src_enc -= 8;
    }
    dst_enc -= 8;
  }
  int opcode_prefix = is_map1 ? 0x0F00 : 0;
  return opcode_prefix | (dst_enc << 3 | src_enc);
}

int Assembler::prefixq_and_encode_rex2(int dst_enc, int src_enc, bool is_map1) {
  int init_bits = REX2BIT_W | (is_map1 ? REX2BIT_M0 : 0);
  return prefix_and_encode_rex2(dst_enc, src_enc, init_bits);
}

void Assembler::emit_prefix_and_int8(int prefix, int b1) {
  if ((prefix & 0xFF00) == 0) {
    emit_int16(prefix, b1);
  } else {
    assert((prefix & 0xFF00) != WREX2 || UseAPX, "APX features not enabled");
    emit_int24((prefix & 0xFF00) >> 8, prefix & 0x00FF, b1);
  }
}

void Assembler::adcq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xD0, dst, imm32);
}

void Assembler::adcq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x13);
  emit_operand(dst, src, 0);
}

void Assembler::adcq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x13, 0xC0, dst, src);
}

void Assembler::addq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefixq(dst);
  emit_arith_operand(0x81, rax, dst, imm32);
}

void Assembler::eaddq(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, rax, src, imm32);
}

void Assembler::addq(Address dst, Register src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst, src), 0x01);
  emit_operand(src, dst, 0);
}

void Assembler::eaddq(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x01);
  emit_operand(src2, src1, 0);
}

void Assembler::addq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xC0, dst, imm32);
}

void Assembler::eaddq(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x81, 0xC0, no_flags);
}

void Assembler::addq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x03);
  emit_operand(dst, src, 0);
}

void Assembler::eaddq(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x03, no_flags);
}

void Assembler::addq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x03, 0xC0, dst, src);
}

void Assembler::eaddq(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_arith(0x03, 0xC0, src1, src2);
}

void Assembler::adcxq(Register dst, Register src) {
  //assert(VM_Version::supports_adx(), "adx instructions not supported");
  if (needs_rex2(dst, src)) {
    InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
    int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, true);
    emit_int16((unsigned char)0x66, (0xC0 | encode));
  } else {
    emit_int8(0x66);
    int encode = prefixq_and_encode(dst->encoding(), src->encoding());
    emit_int32(0x0F,
               0x38,
               (unsigned char)0xF6,
               (0xC0 | encode));
  }
}

void Assembler::eadcxq(Register dst, Register src1, Register src2) {
  if (is_demotable(false, dst->encoding(), src1->encoding())) {
    return adcxq(dst, src2);
  }
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, false /* no_flags */, true /* use_prefixq */);
  emit_int16((unsigned char)0x66, (0xC0 | encode));
}

void Assembler::adoxq(Register dst, Register src) {
  //assert(VM_Version::supports_adx(), "adx instructions not supported");
  if (needs_rex2(dst, src)) {
    InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
    int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, true);
    emit_int16((unsigned char)0x66, (0xC0 | encode));
  } else {
    emit_int8((unsigned char)0xF3);
    int encode = prefixq_and_encode(dst->encoding(), src->encoding());
    emit_int32(0x0F,
               0x38,
               (unsigned char)0xF6,
               (0xC0 | encode));
  }
}

void Assembler::eadoxq(Register dst, Register src1, Register src2) {
  if (is_demotable(false, dst->encoding(), src1->encoding())) {
    return adoxq(dst, src2);
  }
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_F3, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, false /* no_flags */, true /* use_prefixq */);
  emit_int16((unsigned char)0x66, (0xC0 | encode));
}

void Assembler::andq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefixq(dst);
  emit_arith_operand(0x81, as_Register(4), dst, imm32);
}

void Assembler::eandq(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, as_Register(4), src, imm32);
}

void Assembler::andq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xE0, dst, imm32);
}

void Assembler::eandq(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x81, 0xE0, no_flags);
}

void Assembler::andq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x23);
  emit_operand(dst, src, 0);
}

void Assembler::eandq(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x23, no_flags);
}

void Assembler::andq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x23, 0xC0, dst, src);
}

void Assembler::eandq(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_arith(0x23, 0xC0, src1, src2);
}

void Assembler::andq(Address dst, Register src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst, src), 0x21);
  emit_operand(src, dst, 0);
}

void Assembler::eandq(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x21);
  emit_operand(src2, src1, 0);
}

void Assembler::andnq(Register dst, Register src1, Register src2) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF2, (0xC0 | encode));
}

void Assembler::andnq(Register dst, Register src1, Address src2) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src2, src1->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF2);
  emit_operand(dst, src2, 0);
}

void Assembler::bsfq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBC, 0xC0, encode);
}

void Assembler::bsrq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBD, 0xC0, encode);
}

void Assembler::bswapq(Register reg) {
  int encode = prefixq_and_encode(reg->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xC8, encode);
}

void Assembler::blsiq(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(rbx->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

void Assembler::blsiq(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src, dst->encoding(), rbx->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF3);
  emit_operand(rbx, src, 0);
}

void Assembler::blsmskq(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(rdx->encoding(),  dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

void Assembler::blsmskq(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src, dst->encoding(), rdx->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF3);
  emit_operand(rdx, src, 0);
}

void Assembler::blsrq(Register dst, Register src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(rcx->encoding(), dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF3, (0xC0 | encode));
}

void Assembler::blsrq(Register dst, Address src) {
  assert(VM_Version::supports_bmi1(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src, dst->encoding(), rcx->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_38, &attributes);
  emit_int8((unsigned char)0xF3);
  emit_operand(rcx, src, 0);
}

void Assembler::cdqq() {
  emit_int16(REX_W, (unsigned char)0x99);
}

void Assembler::cdqe() {
  emit_int16(REX_W, (unsigned char)0x98);
}

void Assembler::clflush(Address adr) {
  assert(VM_Version::supports_clflush(), "should do");
  prefix(adr, true /* is_map1 */);
  emit_int8((unsigned char)0xAE);
  emit_operand(rdi, adr, 0);
}

void Assembler::clflushopt(Address adr) {
  assert(VM_Version::supports_clflushopt(), "should do!");
  // adr should be base reg only with no index or offset
  assert(adr.index() == noreg, "index should be noreg");
  assert(adr.scale() == Address::no_scale, "scale should be no_scale");
  assert(adr.disp() == 0, "displacement should be 0");
  // instruction prefix is 0x66
  emit_int8(0x66);
  prefix(adr, true /* is_map1 */);
  // opcode family is 0x0F 0xAE
  emit_int8((unsigned char)0xAE);
  // extended opcode byte is 7 == rdi
  emit_operand(rdi, adr, 0);
}

void Assembler::clwb(Address adr) {
  assert(VM_Version::supports_clwb(), "should do!");
  // adr should be base reg only with no index or offset
  assert(adr.index() == noreg, "index should be noreg");
  assert(adr.scale() == Address::no_scale, "scale should be no_scale");
  assert(adr.disp() == 0, "displacement should be 0");
  // instruction prefix is 0x66
  emit_int8(0x66);
  prefix(adr, true /* is_map1 */);
  // opcode family is 0x0f 0xAE
  emit_int8((unsigned char)0xAE);
  // extended opcode byte is 6 == rsi
  emit_operand(rsi, adr, 0);
}

void Assembler::cmovq(Condition cc, Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((0x40 | cc), 0xC0, encode);
}

void Assembler::ecmovq(Condition cc, Register dst, Register src1, Register src2) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x40 | cc, false /* no_flags */, true /* is_map1 */, true /* swap */);
}

void Assembler::cmovq(Condition cc, Register dst, Address src) {
  InstructionMark im(this);
  int prefix = get_prefixq(src, dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (0x40 | cc));
  emit_operand(dst, src, 0);
}

void Assembler::ecmovq(Condition cc, Register dst, Register src1, Address src2) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, (0x40 | cc) , false /* no_flags */, true /* is_map1 */);
}

void Assembler::cmpq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefixq(dst);
  emit_arith_operand(0x81, as_Register(7), dst, imm32);
}

void Assembler::cmpq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xF8, dst, imm32);
}

void Assembler::cmpq(Address dst, Register src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst, src), 0x39);
  emit_operand(src, dst, 0);
}

void Assembler::cmpq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x3B, 0xC0, dst, src);
}

void Assembler::cmpq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x3B);
  emit_operand(dst, src, 0);
}

void Assembler::cmpxchgq(Register reg, Address adr) {
  InstructionMark im(this);
  int prefix = get_prefixq(adr, reg, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xB1);
  emit_operand(reg, adr, 0);
}

void Assembler::cvtsi2sdq(XMMRegister dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, dst, as_XMMRegister(src->encoding()), VEX_SIMD_F2, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x2A, (0xC0 | encode));
}

void Assembler::cvtsi2sdq(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2A);
  emit_operand(dst, src, 0);
}

void Assembler::cvtsi2ssq(XMMRegister dst, Address src) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_T1S, /* input_size_in_bits */ EVEX_32bit);
  simd_prefix(dst, dst, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int8(0x2A);
  emit_operand(dst, src, 0);
}

void Assembler::cvttsd2siq(Register dst, Address src) {
  // F2 REX.W 0F 2C /r
  // CVTTSD2SI r64, xmm1/m64
  InstructionMark im(this);
  emit_int8((unsigned char)0xF2);
  prefixq(src, dst, true /* is_map1 */);
  emit_int8((unsigned char)0x2C);
  emit_operand(dst, src, 0);
}

void Assembler::cvttsd2siq(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2C, (0xC0 | encode));
}

void Assembler::cvtsd2siq(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_F2, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2D, (0xC0 | encode));
}

void Assembler::cvttss2siq(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(as_XMMRegister(dst->encoding()), xnoreg, src, VEX_SIMD_F3, VEX_OPCODE_0F, &attributes);
  emit_int16(0x2C, (0xC0 | encode));
}

void Assembler::decl(Register dst) {
  // Don't use it directly. Use MacroAssembler::decrementl() instead.
  // Use two-byte form (one-byte form is a REX prefix in 64-bit mode)
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xFF, (0xC8 | encode));
}

void Assembler::edecl(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xFF, (0xC8 | encode));
}

void Assembler::decq(Register dst) {
  // Don't use it directly. Use MacroAssembler::decrementq() instead.
  // Use two-byte form (one-byte from is a REX prefix in 64-bit mode)
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xFF, 0xC8 | encode);
}

void Assembler::edecq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xFF, (0xC8 | encode));
}

void Assembler::decq(Address dst) {
  // Don't use it directly. Use MacroAssembler::decrementq() instead.
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xFF);
  emit_operand(rcx, dst, 0);
}

void Assembler::edecq(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xFF);
  emit_operand(rcx, src, 0);
}

// can't use REX2
void Assembler::fxrstor(Address src) {
  InstructionMark im(this);
  emit_int24(get_prefixq(src), 0x0F, (unsigned char)0xAE);
  emit_operand(as_Register(1), src, 0);
}

// can't use REX2
void Assembler::xrstor(Address src) {
  InstructionMark im(this);
  emit_int24(get_prefixq(src), 0x0F, (unsigned char)0xAE);
  emit_operand(as_Register(5), src, 0);
}

// can't use REX2
void Assembler::fxsave(Address dst) {
  InstructionMark im(this);
  emit_int24(get_prefixq(dst), 0x0F, (unsigned char)0xAE);
  emit_operand(as_Register(0), dst, 0);
}

// cant use REX2
void Assembler::xsave(Address dst) {
  InstructionMark im(this);
  emit_int24(get_prefixq(dst), 0x0F, (unsigned char)0xAE);
  emit_operand(as_Register(4), dst, 0);
}

void Assembler::idivq(Register src) {
  int encode = prefixq_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xF8 | encode));
}

void Assembler::eidivq(Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xF8 | encode));
}

void Assembler::divq(Register src) {
  int encode = prefixq_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xF0 | encode));
}

void Assembler::edivq(Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xF0 | encode));
}

void Assembler::imulq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xAF, 0xC0, encode);
}

void Assembler::eimulq(Register dst, Register src, bool no_flags) {
  if (is_demotable(no_flags, dst->encoding(), src->encoding())) {
    return imulq(dst);
  }
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xAF, (0xC0 | encode));
}

void Assembler::eimulq(Register dst, Register src1, Register src2, bool no_flags) {
  emit_eevex_or_demote(dst->encoding(), src1->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0xAF, no_flags, true /* is_map1 */,  true /* swap */);
}

void Assembler::imulq(Register src) {
  int encode = prefixq_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xE8 | encode));
}

void Assembler::eimulq(Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xE8 | encode));
}

void Assembler::imulq(Register dst, Address src, int32_t value) {
  InstructionMark im(this);
  prefixq(src, dst);
  if (is8bit(value)) {
    emit_int8((unsigned char)0x6B);
    emit_operand(dst, src, 1);
    emit_int8(value);
  } else {
    emit_int8((unsigned char)0x69);
    emit_operand(dst, src, 4);
    emit_int32(value);
  }
}

void Assembler::eimulq(Register dst, Address src, int32_t value, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (is8bit(value)) {
    emit_int8((unsigned char)0x6B);
    emit_operand(dst, src, 1);
    emit_int8(value);
  } else {
    emit_int8((unsigned char)0x69);
    emit_operand(dst, src, 4);
    emit_int32(value);
  }
}

void Assembler::imulq(Register dst, Register src, int value) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding());
  if (is8bit(value)) {
    emit_int24(0x6B, (0xC0 | encode), (value & 0xFF));
  } else {
    emit_int16(0x69, (0xC0 | encode));
    emit_int32(value);
  }
}

void Assembler::eimulq(Register dst, Register src, int value, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (is8bit(value)) {
    emit_int24(0x6B, (0xC0 | encode), (value & 0xFF));
  } else {
    emit_int16(0x69, (0xC0 | encode));
    emit_int32(value);
  }
}

void Assembler::imulq(Register dst, Address src) {
  InstructionMark im(this);
  int prefix = get_prefixq(src, dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xAF);
  emit_operand(dst, src, 0);
}

void Assembler::eimulq(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);

  emit_int8((unsigned char)0xAF);
  emit_operand(dst, src, 0);
}

void Assembler::eimulq(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, (unsigned char)0xAF, no_flags, true /* is_map1 */);
}

void Assembler::incl(Register dst) {
  // Don't use it directly. Use MacroAssembler::incrementl() instead.
  // Use two-byte form (one-byte from is a REX prefix in 64-bit mode)
  int encode = prefix_and_encode(dst->encoding());
  emit_int16((unsigned char)0xFF, (0xC0 | encode));
}

void Assembler::eincl(Register dst, Register src, bool no_flags) {
  // Don't use it directly. Use MacroAssembler::incrementl() instead.
  // Use two-byte form (one-byte from is a REX prefix in 64-bit mode)
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xFF, (0xC0 | encode));
}

void Assembler::incq(Register dst) {
  // Don't use it directly. Use MacroAssembler::incrementq() instead.
  // Use two-byte form (one-byte from is a REX prefix in 64-bit mode)
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xFF, (0xC0 | encode));
}

void Assembler::eincq(Register dst, Register src, bool no_flags) {
  // Don't use it directly. Use MacroAssembler::incrementq() instead.
  // Use two-byte form (one-byte from is a REX prefix in 64-bit mode)
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xFF, (0xC0 | encode));
}

void Assembler::incq(Address dst) {
  // Don't use it directly. Use MacroAssembler::incrementq() instead.
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xFF);
  emit_operand(rax, dst, 0);
}

void Assembler::eincq(Register dst, Address src, bool no_flags) {
  // Don't use it directly. Use MacroAssembler::incrementq() instead.
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char) 0xFF);
  emit_operand(rax, src, 0);
}

void Assembler::lea(Register dst, Address src) {
  leaq(dst, src);
}

void Assembler::leaq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), (unsigned char)0x8D);
  emit_operand(dst, src, 0);
}

void Assembler::mov64(Register dst, int64_t imm64) {
  InstructionMark im(this);
  int encode = prefixq_and_encode(dst->encoding());
  emit_int8(0xB8 | encode);
  emit_int64(imm64);
}

void Assembler::mov64(Register dst, int64_t imm64, relocInfo::relocType rtype, int format) {
  InstructionMark im(this);
  int encode = prefixq_and_encode(dst->encoding());
  emit_int8(0xB8 | encode);
  emit_data64(imm64, rtype, format);
}

void Assembler::mov_literal64(Register dst, intptr_t imm64, RelocationHolder const& rspec) {
  InstructionMark im(this);
  int encode = prefixq_and_encode(dst->encoding());
  emit_int8(0xB8 | encode);
  emit_data64(imm64, rspec);
}

void Assembler::mov_narrow_oop(Register dst, int32_t imm32, RelocationHolder const& rspec) {
  InstructionMark im(this);
  int encode = prefix_and_encode(dst->encoding());
  emit_int8(0xB8 | encode);
  emit_data((int)imm32, rspec, narrow_oop_operand);
}

void Assembler::mov_narrow_oop(Address dst, int32_t imm32,  RelocationHolder const& rspec) {
  InstructionMark im(this);
  prefix(dst);
  emit_int8((unsigned char)0xC7);
  emit_operand(rax, dst, 4);
  emit_data((int)imm32, rspec, narrow_oop_operand);
}

void Assembler::cmp_narrow_oop(Register src1, int32_t imm32, RelocationHolder const& rspec) {
  InstructionMark im(this);
  int encode = prefix_and_encode(src1->encoding());
  emit_int16((unsigned char)0x81, (0xF8 | encode));
  emit_data((int)imm32, rspec, narrow_oop_operand);
}

void Assembler::cmp_narrow_oop(Address src1, int32_t imm32, RelocationHolder const& rspec) {
  InstructionMark im(this);
  prefix(src1);
  emit_int8((unsigned char)0x81);
  emit_operand(rax, src1, 4);
  emit_data((int)imm32, rspec, narrow_oop_operand);
}

void Assembler::lzcntq(Register dst, Register src) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  emit_int8((unsigned char)0xF3);
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBD, 0xC0, encode);
}

void Assembler::elzcntq(Register dst, Register src, bool no_flags) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF5, (0xC0 | encode));
}

void Assembler::lzcntq(Register dst, Address src) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  InstructionMark im(this);
  emit_int8((unsigned char)0xF3);
  prefixq(src, dst, true /* is_map1 */);
  emit_int8((unsigned char)0xBD);
  emit_operand(dst, src, 0);
}

void Assembler::elzcntq(Register dst, Address src, bool no_flags) {
  assert(VM_Version::supports_lzcnt(), "encoding is treated as BSR");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xF5);
  emit_operand(dst, src, 0);
}

void Assembler::movdq(XMMRegister dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = simd_prefix_and_encode(dst, xnoreg, as_XMMRegister(src->encoding()), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x6E, (0xC0 | encode));
}

void Assembler::movdq(Register dst, XMMRegister src) {
  InstructionAttr attributes(AVX_128bit, /* rex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // swap src/dst to get correct prefix
  int encode = simd_prefix_and_encode(src, xnoreg, as_XMMRegister(dst->encoding()), VEX_SIMD_66, VEX_OPCODE_0F, &attributes, true);
  emit_int16(0x7E,
             (0xC0 | encode));
}

void Assembler::movq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding());
  emit_int16((unsigned char)0x8B,
             (0xC0 | encode));
}

void Assembler::movq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), (unsigned char)0x8B);
  emit_operand(dst, src, 0);
}

void Assembler::movq(Address dst, Register src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst, src), (unsigned char)0x89);
  emit_operand(src, dst, 0);
}

void Assembler::movq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xC7);
  emit_operand(as_Register(0), dst, 4);
  emit_int32(imm32);
}

void Assembler::movq(Register dst, int32_t imm32) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xC7, (0xC0 | encode));
  emit_int32(imm32);
}

void Assembler::movsbq(Register dst, Address src) {
  InstructionMark im(this);
  int prefix = get_prefixq(src, dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xBE);
  emit_operand(dst, src, 0);
}

void Assembler::movsbq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBE, 0xC0, encode);
}

void Assembler::movslq(Address dst, int32_t imm32) {
  assert(is_simm32(imm32), "lost bits");
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xC7);
  emit_operand(rax, dst, 4);
  emit_int32(imm32);
}

void Assembler::movslq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x63);
  emit_operand(dst, src, 0);
}

void Assembler::movslq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding());
  emit_int16(0x63, (0xC0 | encode));
}

void Assembler::movswq(Register dst, Address src) {
  InstructionMark im(this);
  int prefix = get_prefixq(src, dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xBF);
  emit_operand(dst, src, 0);
}

void Assembler::movswq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBF, 0xC0, encode);
}

void Assembler::movzbq(Register dst, Address src) {
  InstructionMark im(this);
  int prefix = get_prefixq(src, dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xB6);
  emit_operand(dst, src, 0);
}

void Assembler::movzbq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xB6, 0xC0, encode);
}

void Assembler::movzwq(Register dst, Address src) {
  InstructionMark im(this);
  int prefix = get_prefixq(src, dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xB7);
  emit_operand(dst, src, 0);
}

void Assembler::movzwq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xB7, 0xC0, encode);
}

void Assembler::mulq(Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src), (unsigned char)0xF7);
  emit_operand(rsp, src, 0);
}

void Assembler::emulq(Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_nf(src, 0, 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0xF7);
  emit_operand(rsp, src, 0);
}

void Assembler::mulq(Register src) {
  int encode = prefixq_and_encode(src->encoding());
  emit_int16((unsigned char)0xF7, (0xE0 | encode));
}

void Assembler::emulq(Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(0, 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0xF7, (0xE0 | encode));
}

void Assembler::mulxq(Register dst1, Register dst2, Register src) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst1->encoding(), dst2->encoding(), src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_38, &attributes, true);
  emit_int16((unsigned char)0xF6, (0xC0 | encode));
}

void Assembler::negq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xF7, (0xD8 | encode));
}

void Assembler::enegq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xF7, (0xD8 | encode));
}

void Assembler::negq(Address dst) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xF7);
  emit_operand(as_Register(3), dst, 0);
}

void Assembler::enegq(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xF7);
  emit_operand(as_Register(3), src, 0);
}

void Assembler::notq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xF7, (0xD0 | encode));
}

void Assembler::enotq(Register dst, Register src) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, false /* no_flags */, true /* use_prefixq */);
  emit_int16((unsigned char)0xF7, (0xD0 | encode));
}

void Assembler::btq(Register dst, Register src) {
  int encode = prefixq_and_encode(src->encoding(), dst->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xA3, 0xC0, encode);
}

void Assembler::btq(Register src, int imm8) {
  assert(isByte(imm8), "not a byte");
  int encode = prefixq_and_encode(src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xBA, 0xE0, encode);
  emit_int8(imm8);
}

void Assembler::btsq(Address dst, int imm8) {
  assert(isByte(imm8), "not a byte");
  InstructionMark im(this);
  int prefix = get_prefixq(dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xBA);
  emit_operand(rbp /* 5 */, dst, 1);
  emit_int8(imm8);
}

void Assembler::btrq(Address dst, int imm8) {
  assert(isByte(imm8), "not a byte");
  InstructionMark im(this);
  int prefix = get_prefixq(dst, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xBA);
  emit_operand(rsi /* 6 */, dst, 1);
  emit_int8(imm8);
}

void Assembler::orq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefixq(dst);
  emit_arith_operand(0x81, as_Register(1), dst, imm32);
}

void Assembler::eorq(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, as_Register(1), src, imm32);
}

void Assembler::orq(Address dst, Register src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst, src), (unsigned char)0x09);
  emit_operand(src, dst, 0);
}

void Assembler::eorq(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x09);
  emit_operand(src2, src1, 0);
}

void Assembler::orq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xC8, dst, imm32);
}

void Assembler::eorq(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x81, 0xC8, no_flags);
}

void Assembler::orq_imm32(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith_imm32(0x81, 0xC8, dst, imm32);
}

void Assembler::eorq_imm32(Register dst, Register src, int32_t imm32, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  (void) emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_arith_imm32(0x81, 0xC8, src, imm32);
}

void Assembler::orq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x0B);
  emit_operand(dst, src, 0);
}

void Assembler::eorq(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x0B, no_flags);
}

void Assembler::orq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x0B, 0xC0, dst, src);
}

void Assembler::eorq(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_arith(0x0B, 0xC0, src1, src2);
}

void Assembler::popcntq(Register dst, Address src) {
  assert(VM_Version::supports_popcnt(), "must support");
  InstructionMark im(this);
  emit_int8((unsigned char)0xF3);
  emit_prefix_and_int8(get_prefixq(src, dst, true /* is_map1 */), (unsigned char) 0xB8);
  emit_operand(dst, src, 0);
}

void Assembler::epopcntq(Register dst, Address src, bool no_flags) {
  assert(VM_Version::supports_popcnt(), "must support");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_nf(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char) 0x88);
  emit_operand(dst, src, 0);
}

void Assembler::popcntq(Register dst, Register src) {
  assert(VM_Version::supports_popcnt(), "must support");
  emit_int8((unsigned char)0xF3);
  int encode = prefixq_and_encode(dst->encoding(), src->encoding(), true /* is_map1 */);
  emit_opcode_prefix_and_encoding((unsigned char)0xB8, 0xC0, encode);
}

void Assembler::epopcntq(Register dst, Register src, bool no_flags) {
  assert(VM_Version::supports_popcnt(), "must support");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = eevex_prefix_and_encode_nf(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int16((unsigned char)0x88, (0xC0 | encode));
}

void Assembler::popq(Address dst) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0x8F);
  emit_operand(rax, dst, 0);
}

void Assembler::popq(Register dst) {
  int encode = prefix_and_encode(dst->encoding());
  emit_int8((unsigned char)0x58 | encode);
}

// Precomputable: popa, pusha, vzeroupper

// The result of these routines are invariant from one invocation to another
// invocation for the duration of a run. Caching the result on bootstrap
// and copying it out on subsequent invocations can thus be beneficial
static bool     precomputed = false;

static u_char* popa_code  = nullptr;
static int     popa_len   = 0;

static u_char* pusha_code = nullptr;
static int     pusha_len  = 0;

static u_char* vzup_code  = nullptr;
static int     vzup_len   = 0;

void Assembler::precompute_instructions() {
  assert(!Universe::is_fully_initialized(), "must still be single threaded");
  guarantee(!precomputed, "only once");
  precomputed = true;
  ResourceMark rm;

  // Make a temporary buffer big enough for the routines we're capturing
  int size = UseAPX ? 512 : 256;
  char* tmp_code = NEW_RESOURCE_ARRAY(char, size);
  CodeBuffer buffer((address)tmp_code, size);
  MacroAssembler masm(&buffer);

  address begin_popa  = masm.code_section()->end();
  masm.popa_uncached();
  address end_popa    = masm.code_section()->end();
  masm.pusha_uncached();
  address end_pusha   = masm.code_section()->end();
  masm.vzeroupper_uncached();
  address end_vzup    = masm.code_section()->end();

  // Save the instructions to permanent buffers.
  popa_len = (int)(end_popa - begin_popa);
  popa_code = NEW_C_HEAP_ARRAY(u_char, popa_len, mtInternal);
  memcpy(popa_code, begin_popa, popa_len);

  pusha_len = (int)(end_pusha - end_popa);
  pusha_code = NEW_C_HEAP_ARRAY(u_char, pusha_len, mtInternal);
  memcpy(pusha_code, end_popa, pusha_len);

  vzup_len = (int)(end_vzup - end_pusha);
  if (vzup_len > 0) {
    vzup_code = NEW_C_HEAP_ARRAY(u_char, vzup_len, mtInternal);
    memcpy(vzup_code, end_pusha, vzup_len);
  } else {
    vzup_code = pusha_code; // dummy
  }

  assert(masm.code()->total_oop_size() == 0 &&
         masm.code()->total_metadata_size() == 0 &&
         masm.code()->total_relocation_size() == 0,
         "pre-computed code can't reference oops, metadata or contain relocations");
}

static void emit_copy(CodeSection* code_section, u_char* src, int src_len) {
  assert(src != nullptr, "code to copy must have been pre-computed");
  assert(code_section->limit() - code_section->end() > src_len, "code buffer not large enough");
  address end = code_section->end();
  memcpy(end, src, src_len);
  code_section->set_end(end + src_len);
}


// Does not actually store the value of rsp on the stack.
// The slot for rsp just contains an arbitrary value.
void Assembler::pusha() { // 64bit
  emit_copy(code_section(), pusha_code, pusha_len);
}

// Does not actually store the value of rsp on the stack.
// The slot for rsp just contains an arbitrary value.
void Assembler::pusha_uncached() { // 64bit
  if (UseAPX) {
    // Data being pushed by PUSH2 must be 16B-aligned on the stack, for this push rax upfront
    // and use it as a temporary register for stack alignment.
    pushp(rax);
    // Move original stack pointer to RAX and align stack pointer to 16B boundary.
    movq(rax, rsp);
    andq(rsp, -(StackAlignmentInBytes));
    // Push pair of original stack pointer along with remaining registers
    // at 16B aligned boundary.
    push2p(rax, r31);
    // Restore the original contents of RAX register.
    movq(rax, Address(rax));
    push2p(r30, r29);
    push2p(r28, r27);
    push2p(r26, r25);
    push2p(r24, r23);
    push2p(r22, r21);
    push2p(r20, r19);
    push2p(r18, r17);
    push2p(r16, r15);
    push2p(r14, r13);
    push2p(r12, r11);
    push2p(r10, r9);
    push2p(r8, rdi);
    push2p(rsi, rbp);
    push2p(rbx, rdx);
    // To maintain 16 byte alignment after rcx is pushed.
    subq(rsp, 8);
    pushp(rcx);
  } else {
    subq(rsp, 16 * wordSize);
    movq(Address(rsp, 15 * wordSize), rax);
    movq(Address(rsp, 14 * wordSize), rcx);
    movq(Address(rsp, 13 * wordSize), rdx);
    movq(Address(rsp, 12 * wordSize), rbx);
    // Skip rsp as the value is normally not used. There are a few places where
    // the original value of rsp needs to be known but that can be computed
    // from the value of rsp immediately after pusha (rsp + 16 * wordSize).
    // FIXME: For APX any such direct access should also consider EGPR size
    // during address compution.
    movq(Address(rsp, 10 * wordSize), rbp);
    movq(Address(rsp, 9 * wordSize), rsi);
    movq(Address(rsp, 8 * wordSize), rdi);
    movq(Address(rsp, 7 * wordSize), r8);
    movq(Address(rsp, 6 * wordSize), r9);
    movq(Address(rsp, 5 * wordSize), r10);
    movq(Address(rsp, 4 * wordSize), r11);
    movq(Address(rsp, 3 * wordSize), r12);
    movq(Address(rsp, 2 * wordSize), r13);
    movq(Address(rsp, wordSize), r14);
    movq(Address(rsp, 0), r15);
  }
}

void Assembler::popa() { // 64bit
  emit_copy(code_section(), popa_code, popa_len);
}

void Assembler::popa_uncached() { // 64bit
  if (UseAPX) {
    popp(rcx);
    addq(rsp, 8);
    // Data being popped by POP2 must be 16B-aligned on the stack.
    pop2p(rdx, rbx);
    pop2p(rbp, rsi);
    pop2p(rdi, r8);
    pop2p(r9, r10);
    pop2p(r11, r12);
    pop2p(r13, r14);
    pop2p(r15, r16);
    pop2p(r17, r18);
    pop2p(r19, r20);
    pop2p(r21, r22);
    pop2p(r23, r24);
    pop2p(r25, r26);
    pop2p(r27, r28);
    pop2p(r29, r30);
    // Popped value in RAX holds original unaligned stack pointer.
    pop2p(r31, rax);
    // Reinstantiate original stack pointer.
    movq(rsp, rax);
    popp(rax);
  } else {
    movq(r15, Address(rsp, 0));
    movq(r14, Address(rsp, wordSize));
    movq(r13, Address(rsp, 2 * wordSize));
    movq(r12, Address(rsp, 3 * wordSize));
    movq(r11, Address(rsp, 4 * wordSize));
    movq(r10, Address(rsp, 5 * wordSize));
    movq(r9,  Address(rsp, 6 * wordSize));
    movq(r8,  Address(rsp, 7 * wordSize));
    movq(rdi, Address(rsp, 8 * wordSize));
    movq(rsi, Address(rsp, 9 * wordSize));
    movq(rbp, Address(rsp, 10 * wordSize));
    // Skip rsp as it is restored automatically to the value
    // before the corresponding pusha when popa is done.
    movq(rbx, Address(rsp, 12 * wordSize));
    movq(rdx, Address(rsp, 13 * wordSize));
    movq(rcx, Address(rsp, 14 * wordSize));
    movq(rax, Address(rsp, 15 * wordSize));

    addq(rsp, 16 * wordSize);
  }
}

void Assembler::vzeroupper() {
  emit_copy(code_section(), vzup_code, vzup_len);
}

void Assembler::vzeroall() {
  assert(VM_Version::supports_avx(), "requires AVX");
  InstructionAttr attributes(AVX_256bit, /* vex_w */ false, /* legacy_mode */ true, /* no_mask_reg */ true, /* uses_vl */ false);
  (void)vex_prefix_and_encode(0, 0, 0, VEX_SIMD_NONE, VEX_OPCODE_0F, &attributes);
  emit_int8(0x77);
}

void Assembler::pushq(Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src), (unsigned char)0xFF);
  emit_operand(rsi, src, 0);
}

void Assembler::rclq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xD0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xD0 | encode), imm8);
  }
}

void Assembler::erclq(Register dst, Register src, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode =  emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, false /* no_flags */, true /* use_prefixq */);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xD0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xD0 | encode), imm8);
  }
}

void Assembler::rcrq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xD8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xD8 | encode), imm8);
  }
}

void Assembler::ercrq(Register dst, Register src, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode =  emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, false /* no_flags */, true /* use_prefixq */);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xD8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xD8 | encode), imm8);
  }
}

void Assembler::rorxl(Register dst, Register src, int imm8) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24((unsigned char)0xF0, (0xC0 | encode), imm8);
}

void Assembler::rorxl(Register dst, Address src, int imm8) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_32bit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_3A, &attributes);
  emit_int8((unsigned char)0xF0);
  emit_operand(dst, src, 1);
  emit_int8(imm8);
}

void Assembler::rorxq(Register dst, Register src, int imm8) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = vex_prefix_and_encode(dst->encoding(), 0,  src->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_3A, &attributes, true);
  emit_int24((unsigned char)0xF0, (0xC0 | encode), imm8);
}

void Assembler::rorxq(Register dst, Address src, int imm8) {
  assert(VM_Version::supports_bmi2(), "bit manipulation instructions not supported");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_3A, &attributes);
  emit_int8((unsigned char)0xF0);
  emit_operand(dst, src, 1);
  emit_int8(imm8);
}

void Assembler::salq(Address dst, int imm8) {
  InstructionMark im(this);
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  if (imm8 == 1) {
    emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xD1);
    emit_operand(as_Register(4), dst, 0);
  }
  else {
    emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xC1);
    emit_operand(as_Register(4), dst, 1);
    emit_int8(imm8);
  }
}

void Assembler::esalq(Register dst, Address src, int imm8, bool no_flags) {
  InstructionMark im(this);
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(4), src, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(4), src, 1);
    emit_int8(imm8);
  }
}

void Assembler::salq(Address dst) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xD3);
  emit_operand(as_Register(4), dst, 0);
}

void Assembler::esalq(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(4), src, 0);
}

void Assembler::salq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::esalq(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::salq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::esalq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::sarq(Address dst, int imm8) {
  InstructionMark im(this);
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  if (imm8 == 1) {
    emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xD1);
    emit_operand(as_Register(7), dst, 0);
  }
  else {
    emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xC1);
    emit_operand(as_Register(7), dst, 1);
    emit_int8(imm8);
  }
}

void Assembler::esarq(Register dst, Address src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(7), src, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(7), src, 1);
    emit_int8(imm8);
  }
}

void Assembler::sarq(Address dst) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xD3);
  emit_operand(as_Register(7), dst, 0);
}

void Assembler::esarq(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(7), src, 0);
}

void Assembler::sarq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xF8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xF8 | encode), imm8);
  }
}

void Assembler::esarq(Register dst, Register src, int imm8, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xF8 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xF8 | encode), imm8);
  }
}

void Assembler::sarq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xF8 | encode));
}

void Assembler::esarq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xD3, (0xF8 | encode));
}

void Assembler::sbbq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefixq(dst);
  emit_arith_operand(0x81, rbx, dst, imm32);
}

void Assembler::sbbq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xD8, dst, imm32);
}

void Assembler::sbbq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x1B);
  emit_operand(dst, src, 0);
}

void Assembler::sbbq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x1B, 0xC0, dst, src);
}

void Assembler::shlq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::eshlq(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  if (imm8 == 1 ) {
    emit_int16((unsigned char)0xD1, (0xE0 | encode));
  } else {
    emit_int24((unsigned char)0xC1, (0xE0 | encode), imm8);
  }
}

void Assembler::shlq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::eshlq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xD3, (0xE0 | encode));
}

void Assembler::shrq(Register dst, int imm8) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  int encode = prefixq_and_encode(dst->encoding());
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE8 | encode));
  }
  else {
    emit_int24((unsigned char)0xC1, (0xE8 | encode), imm8);
  }
}

void Assembler::eshrq(Register dst, Register src, int imm8, bool no_flags) {
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  if (imm8 == 1) {
    emit_int16((unsigned char)0xD1, (0xE8 | encode));
  }
  else {
    emit_int24((unsigned char)0xC1, (0xE8 | encode), imm8);
  }
}

void Assembler::shrq(Register dst) {
  int encode = prefixq_and_encode(dst->encoding());
  emit_int16((unsigned char)0xD3, 0xE8 | encode);
}

void Assembler::eshrq(Register dst, Register src, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  int encode = emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_int16((unsigned char)0xD3, (0xE8 | encode));
}

void Assembler::shrq(Address dst) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xD3);
  emit_operand(as_Register(5), dst, 0);
}

void Assembler::eshrq(Register dst, Address src, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8((unsigned char)0xD3);
  emit_operand(as_Register(5), src, 0);
}

void Assembler::shrq(Address dst, int imm8) {
  InstructionMark im(this);
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  if (imm8 == 1) {
    emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xD1);
    emit_operand(as_Register(5), dst, 0);
  }
  else {
    emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xC1);
    emit_operand(as_Register(5), dst, 1);
    emit_int8(imm8);
  }
}

void Assembler::eshrq(Register dst, Address src, int imm8, bool no_flags) {
  InstructionMark im(this);
  assert(isShiftCount(imm8 >> 1), "illegal shift count");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  if (imm8 == 1) {
    emit_int8((unsigned char)0xD1);
    emit_operand(as_Register(5), src, 0);
  }
  else {
    emit_int8((unsigned char)0xC1);
    emit_operand(as_Register(5), src, 1);
    emit_int8(imm8);
  }
}

void Assembler::subq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefixq(dst);
  emit_arith_operand(0x81, rbp, dst, imm32);
}

void Assembler::esubq(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, rbp, src, imm32);
}

void Assembler::subq(Address dst, Register src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst, src), 0x29);
  emit_operand(src, dst, 0);
}

void Assembler::esubq(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x29);
  emit_operand(src2, src1, 0);
}

void Assembler::subq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xE8, dst, imm32);
}

void Assembler::esubq(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x81, 0xE8, no_flags);
}

// Force generation of a 4 byte immediate value even if it fits into 8bit
void Assembler::subq_imm32(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith_imm32(0x81, 0xE8, dst, imm32);
}

void Assembler::esubq_imm32(Register dst, Register src, int32_t imm32, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  (void) emit_eevex_prefix_or_demote_ndd(dst->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_arith_imm32(0x81, 0xE8, src, imm32);
}

void Assembler::subq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x2B);
  emit_operand(dst, src, 0);
}

void Assembler::esubq(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x2B, no_flags);
}

void Assembler::subq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x2B, 0xC0, dst, src);
}

void Assembler::esubq(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_arith(0x2B, 0xC0, src1, src2);
}

void Assembler::testq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst), (unsigned char)0xF7);
  emit_operand(as_Register(0), dst, 4);
  emit_int32(imm32);
}

void Assembler::testq(Register dst, int32_t imm32) {
  // not using emit_arith because test
  // doesn't support sign-extension of
  // 8bit operands
  if (dst == rax) {
    prefix(REX_W);
    emit_int8((unsigned char)0xA9);
    emit_int32(imm32);
  } else {
    int encode = dst->encoding();
    encode = prefixq_and_encode(encode);
    emit_int16((unsigned char)0xF7, (0xC0 | encode));
    emit_int32(imm32);
  }
}

void Assembler::testq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x85, 0xC0, dst, src);
}

void Assembler::testq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), (unsigned char)0x85);
  emit_operand(dst, src, 0);
}

void Assembler::xaddq(Address dst, Register src) {
  InstructionMark im(this);
  int prefix = get_prefixq(dst, src, true /* is_map1 */);
  emit_prefix_and_int8(prefix, (unsigned char)0xC1);
  emit_operand(src, dst, 0);
}

void Assembler::xchgq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), (unsigned char)0x87);
  emit_operand(dst, src, 0);
}

void Assembler::xchgq(Register dst, Register src) {
  int encode = prefixq_and_encode(dst->encoding(), src->encoding());
  emit_int16((unsigned char)0x87, (0xc0 | encode));
}

void Assembler::xorq(Register dst, Register src) {
  (void) prefixq_and_encode(dst->encoding(), src->encoding());
  emit_arith(0x33, 0xC0, dst, src);
}

void Assembler::exorq(Register dst, Register src1, Register src2, bool no_flags) {
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // NDD shares its encoding bits with NDS bits for regular EVEX instruction.
  // Therefore, DST is passed as the second argument to minimize changes in the leaf level routine.
  (void) emit_eevex_prefix_or_demote_ndd(src1->encoding(), dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags, true /* use_prefixq */);
  emit_arith(0x33, 0xC0, src1, src2);
}

void Assembler::xorq(Register dst, Address src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(src, dst), 0x33);
  emit_operand(dst, src, 0);
}

void Assembler::exorq(Register dst, Register src1, Address src2, bool no_flags) {
  InstructionMark im(this);
  emit_eevex_or_demote(dst, src1, src2, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x33, no_flags);
}

void Assembler::xorq(Register dst, int32_t imm32) {
  (void) prefixq_and_encode(dst->encoding());
  emit_arith(0x81, 0xF0, dst, imm32);
}

void Assembler::exorq(Register dst, Register src, int32_t imm32, bool no_flags) {
  emit_eevex_prefix_or_demote_arith_ndd(dst, src, imm32, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, EVEX_64bit, 0x81, 0xF0, no_flags);
}

void Assembler::xorq(Address dst, int32_t imm32) {
  InstructionMark im(this);
  prefixq(dst);
  emit_arith_operand(0x81, as_Register(6), dst, imm32);
}

void Assembler::exorq(Register dst, Address src, int32_t imm32, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src, dst->encoding(), 0, VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_arith_operand(0x81, as_Register(6), src, imm32);
}

void Assembler::xorq(Address dst, Register src) {
  InstructionMark im(this);
  emit_prefix_and_int8(get_prefixq(dst, src), 0x31);
  emit_operand(src, dst, 0);
}

void Assembler::esetzucc(Condition cc, Register dst) {
  assert(VM_Version::supports_apx_f(), "");
  assert(0 <= cc && cc < 16, "illegal cc");
  InstructionAttr attributes(AVX_128bit, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  // Encoding Format : eevex_prefix (4 bytes) | opcode_cc | modrm
  int encode =  emit_eevex_prefix_ndd(dst->encoding(), VEX_SIMD_F2, VEX_OPCODE_0F_3C /* MAP4 */, &attributes); // demotion disabled
  emit_opcode_prefix_and_encoding((0x40 | cc), 0xC0, encode);
}

void Assembler::exorq(Register dst, Address src1, Register src2, bool no_flags) {
  InstructionMark im(this);
  InstructionAttr attributes(AVX_128bit, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ false);
  attributes.set_address_attributes(/* tuple_type */ EVEX_NOSCALE, /* input_size_in_bits */ EVEX_64bit);
  eevex_prefix_ndd(src1, dst->encoding(), src2->encoding(), VEX_SIMD_NONE, VEX_OPCODE_0F_3C /* MAP4 */, &attributes, no_flags);
  emit_int8(0x31);
  emit_operand(src2, src1, 0);
}

void InstructionAttr::set_address_attributes(int tuple_type, int input_size_in_bits) {
  if (VM_Version::supports_evex()) {
    _tuple_type = tuple_type;
    _input_size_in_bits = input_size_in_bits;
  }
}

void Assembler::evpermi2b(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x75, (0xC0 | encode));
}

void Assembler::evpermi2w(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512bw() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x75, (0xC0 | encode));
}

void Assembler::evpermi2d(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x76, (0xC0 | encode));
}

void Assembler::evpermi2q(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x76, (0xC0 | encode));
}

void Assembler::evpermi2ps(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x77, (0xC0 | encode));
}

void Assembler::evpermi2pd(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x77, (0xC0 | encode));
}

void Assembler::evpermt2b(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_vbmi(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x7D, (0xC0 | encode));
}

void Assembler::evpermt2w(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(vector_len <= AVX_256bit ? VM_Version::supports_avx512vlbw() : VM_Version::supports_avx512bw(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x7D, (0xC0 | encode));
}

void Assembler::evpermt2d(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x7E, (0xC0 | encode));
}

void Assembler::evpermt2q(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_evex() && (vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl()), "");
  InstructionAttr attributes(vector_len, /* vex_w */ true, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_0F_38, &attributes);
  emit_int16(0x7E, (0xC0 | encode));
}

void Assembler::evaddph(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x58, (0xC0 | encode));
}

void Assembler::evaddph(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int8(0x58);
  emit_operand(dst, src, 0);
}

void Assembler::evsubph(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5C, (0xC0 | encode));
}

void Assembler::evsubph(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int8(0x5C);
  emit_operand(dst, src, 0);
}

void Assembler::evmulph(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x59, (0xC0 | encode));
}

void Assembler::evmulph(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int8(0x59);
  emit_operand(dst, src, 0);
}

void Assembler::evminph(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5D, (0xC0 | encode));
}

void Assembler::evminph(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int8(0x5D);
  emit_operand(dst, src, 0);
}

void Assembler::evmaxph(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5F, (0xC0 | encode));
}

void Assembler::evmaxph(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int8(0x5F);
  emit_operand(dst, src, 0);
}

void Assembler::evdivph(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x5E, (0xC0 | encode));
}

void Assembler::evdivph(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "requires AVX512-FP16");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int8(0x5E);
  emit_operand(dst, src, 0);
}

void Assembler::evsqrtph(XMMRegister dst, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), 0, src->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int16(0x51, (0xC0 | encode));
}

void Assembler::evsqrtph(XMMRegister dst, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, 0, dst->encoding(), VEX_SIMD_NONE, VEX_OPCODE_MAP5, &attributes);
  emit_int8(0x51);
  emit_operand(dst, src, 0);
}

void Assembler::evfmadd132ph(XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  int encode = vex_prefix_and_encode(dst->encoding(), nds->encoding(), src->encoding(), VEX_SIMD_66, VEX_OPCODE_MAP6, &attributes);
  emit_int16(0x98, (0xC0 | encode));
}

void Assembler::evfmadd132ph(XMMRegister dst, XMMRegister nds, Address src, int vector_len) {
  assert(VM_Version::supports_avx512_fp16(), "");
  assert(vector_len == Assembler::AVX_512bit || VM_Version::supports_avx512vl(), "");
  InstructionMark im(this);
  InstructionAttr attributes(vector_len, /* vex_w */ false, /* legacy_mode */ false, /* no_mask_reg */ true, /* uses_vl */ true);
  attributes.set_is_evex_instruction();
  attributes.set_address_attributes(/* tuple_type */ EVEX_FV, /* input_size_in_bits */ EVEX_NObit);
  vex_prefix(src, nds->encoding(), dst->encoding(), VEX_SIMD_66, VEX_OPCODE_MAP6, &attributes);
  emit_int8(0x98);
  emit_operand(dst, src, 0);
}


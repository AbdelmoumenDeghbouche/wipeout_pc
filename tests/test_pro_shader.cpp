// SPDX-License-Identifier: GPL-2.0-or-later
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include "common/object_pool.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/profile.h"

int main() {
    using namespace Shader;
    // Pro FP16 operations: unary v7 <- v6; MAD v7 <- v6 * v7 + v8.
    const std::array<u32, 13> opcodes{84, 85, 86, 87, 88, 833, 86, 86, 91, 92, 93, 94, 95};
    const std::array<IR::Opcode, 13> operations{IR::Opcode::FPRecip32, IR::Opcode::FPSqrt,
        IR::Opcode::FPRecipSqrt32, IR::Opcode::FPLog2, IR::Opcode::FPExp2, IR::Opcode::FPAdd32,
        IR::Opcode::FPRecipSqrt32, IR::Opcode::FPRecipSqrt32, IR::Opcode::FPFloor32,
        IR::Opcode::FPCeil32, IR::Opcode::FPTrunc32, IR::Opcode::FPRoundEven32,
        IR::Opcode::FPFract32};
    for (u32 operation = 0; operation < operations.size(); ++operation) {
        const bool mad = operation == 5;
        const bool vcc = operation == 6 || operation == 7;
        const u32 source = vcc ? 106u + operation - 6u : 262u;
        const u32 opcode = opcodes[operation];
        const u32 vop3 = mad ? opcode : opcode + 384u;
        const std::array<u32, 1> e32{0x7e000000u | (7u << 17) | (opcode << 9) | source};
        const std::array<u32, 2> e64{0xd0000000u | ((vop3 & 511u) << 17) |
            ((vop3 >> 9) << 16) | 7u, mad ? (source | (263u << 9) | (264u << 18)) : source};
        for (const std::span<const u32> code : {std::span<const u32>{e32},
                                              std::span<const u32>{e64}}) {
            if (mad && code.size() == 1) { continue; } // MAD has only a VOP3 encoding.
            Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
            Gcn::GcnDecodeContext decoder;
            const auto inst = decoder.decodeInstruction(slice);
            assert(slice.atEnd());
            if (inst.inst_class != (mad ? Gcn::InstClass::VectorFpArith16
                                       : Gcn::InstClass::VectorFpTran16)) {
                std::fprintf(stderr, "FAIL: Pro opcode %u does not decode as FP16 arithmetic\n", opcode);
                return 1;
            }
            assert(inst.src_count == (mad ? 3u : 1u) && inst.dst_count == 1);
            assert(inst.src[0].type == Gcn::ScalarType::Float16);
            assert(inst.dst[0].type == Gcn::ScalarType::Float16);
            assert(inst.src[0].code == (vcc ? source : 6u) && inst.dst[0].code == 7);
            if (mad) {
                assert(inst.src[1].code == 7 && inst.src[2].code == 8);
                assert(inst.src[1].type == Gcn::ScalarType::Float16 &&
                       inst.src[2].type == Gcn::ScalarType::Float16);
            }

            Info info{};
            info.hw_stage = HwStage::Fragment;
            info.sw_stage = SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            Profile profile{};
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            Gcn::Translator translator(info, runtime, profile);
            translator.Translate(&block, 0, IR::Condition::True, {&inst, 1});
            if (info.translation_failed) {
                std::fprintf(stderr, "FAIL: Pro opcode %u is not translated\n", opcode);
                return 1;
            }
            bool expected_operation = false, half_result = false, multiply = false, read_vcc = false;
            for (const auto& op : block) {
                expected_operation |= op.GetOpcode() == operations[operation];
                half_result |= op.GetOpcode() == IR::Opcode::ConvertF16F32;
                multiply |= op.GetOpcode() == IR::Opcode::FPMul32;
                read_vcc |= op.GetOpcode() == (source == 107u ? IR::Opcode::GetVccHi
                                                             : IR::Opcode::GetVccLo);
            }
            assert(expected_operation && half_result);
            assert(!mad || multiply);
            assert(!vcc || read_vcc);
        }
    }
    for (u32 opcode : {46u, 86u}) {
        for (u32 permutation : {0u, 0x55u, 0xaau, 0xffu, 0xe4u}) {
            const std::array<u32, 2> code{0x7e000000u | (7u << 17) | (opcode << 9) | 250u,
                6u | (permutation << 8) | (1u << 19) | 0xff000000u};
            Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
            Gcn::GcnDecodeContext decoder;
            const auto inst = decoder.decodeInstruction(slice);
            assert(slice.atEnd() && inst.src[0].dpp);
            Info info{};
            info.hw_stage = HwStage::Fragment;
            info.sw_stage = SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            Profile profile{};
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            Gcn::Translator translator(info, runtime, profile);
            translator.Translate(&block, 0, IR::Condition::True, {&inst, 1});
            assert(!info.translation_failed);
            bool shuffle = false;
            for (const auto& op : block) { shuffle |= op.GetOpcode() == IR::Opcode::Shuffle; }
            if (!shuffle) {
                std::fputs("FAIL: DPP quad permutation does not exchange lane values\n", stderr);
                return 1;
            }
        }
    }
    for (u32 opcode : {55u, 56u}) {
        // K=2.0 in the low half, NaN in the high half; ENDPGM must remain a separate opcode.
        const std::array<u32, 3> code{(opcode << 25) | (9u << 17) | (7u << 9) | 262u,
                                      0x7e004000u, 0xbf810000u};
        Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
        Gcn::GcnDecodeContext decoder;
        const auto inst = decoder.decodeInstruction(slice);
        assert(inst.length == 8 && inst.src_count == 3);
        assert(inst.src[2].field == Gcn::OperandField::LiteralConst);
        assert(inst.src[2].type == Gcn::ScalarType::Float16 && inst.src[2].code == code[1]);
        assert(decoder.decodeInstruction(slice).opcode == Gcn::Opcode::S_ENDPGM);
        assert(slice.atEnd());
        Info info{};
        info.hw_stage = HwStage::Fragment;
        info.sw_stage = SwStage::Fragment;
        RuntimeInfo runtime{};
        runtime.Initialize(info.hw_stage, info.sw_stage);
        Profile profile{};
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        Gcn::Translator translator(info, runtime, profile);
        translator.Translate(&block, 0, IR::Condition::True, {&inst, 1});
        assert(!info.translation_failed);
        const IR::Inst* fma = nullptr;
        for (const auto& op : block) {
            if (op.GetOpcode() == IR::Opcode::FPFma32) { fma = &op; }
        }
        assert(fma);
        const auto* constant = fma->Arg(opcode == 55 ? 1 : 2).Inst();
        assert(constant->GetOpcode() == IR::Opcode::CompositeExtractF32x2);
        assert(constant->Arg(1).U32() == 0);
        const auto* unpack = constant->Arg(0).Inst();
        assert(unpack->GetOpcode() == IR::Opcode::UnpackHalf2x16);
        assert(unpack->Arg(0).IsImmediate() && unpack->Arg(0).U32() == code[1]);
    }
    const std::array<IR::Opcode, 9> predicates{IR::Opcode::Ballot, IR::Opcode::FPOrdLessThan32,
        IR::Opcode::FPOrdEqual32, IR::Opcode::FPOrdLessThanEqual32, IR::Opcode::FPOrdGreaterThan32,
        IR::Opcode::FPOrdNotEqual32, IR::Opcode::FPOrdGreaterThanEqual32, IR::Opcode::LogicalNot,
        IR::Opcode::FPOrdLessThanEqual32};
    for (u32 condition = 0; condition < predicates.size(); ++condition) {
        const bool fp16 = condition != 8;
        const u32 opcode = fp16 ? 200u + condition : 3u; // LE_F32 guards the shared comparator.
        const std::array<u32, 1> e32{0x7c000000u | (opcode << 17) | (7u << 9) | 262u};
        const std::array<u32, 2> e64{0xd0000000u | (opcode << 17) | 8u, 262u | (263u << 9)};
        for (const std::span<const u32> code : {std::span<const u32>{e32},
                                              std::span<const u32>{e64}}) {
            Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
            Gcn::GcnDecodeContext decoder;
            const auto inst = decoder.decodeInstruction(slice);
            assert(slice.atEnd());
            const auto type = fp16 ? Gcn::ScalarType::Float16 : Gcn::ScalarType::Float32;
            assert(inst.src[0].type == type && inst.src[1].type == type);
            assert(inst.dst[1].type == Gcn::ScalarType::Uint64);
            Info info{};
            info.hw_stage = HwStage::Fragment;
            info.sw_stage = SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            Profile profile{};
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            Gcn::Translator translator(info, runtime, profile);
            translator.Translate(&block, 0, IR::Condition::True, {&inst, 1});
            if (info.translation_failed) {
                std::fprintf(stderr, "FAIL: Pro comparison opcode %u is not translated\n", opcode);
                return 1;
            }
            unsigned halves = 0, writes = 0;
            bool predicate = false;
            for (const auto& op : block) {
                halves += op.GetOpcode() == IR::Opcode::UnpackHalf2x16;
                predicate |= op.GetOpcode() == predicates[condition];
                writes += code.size() == 1 ? (op.GetOpcode() == IR::Opcode::SetVccLo ||
                                              op.GetOpcode() == IR::Opcode::SetVccHi)
                                         : op.GetOpcode() == IR::Opcode::SetScalarRegister;
                if (condition == 0 && op.GetOpcode() == IR::Opcode::Ballot) {
                    assert(op.Arg(0).IsImmediate() && !op.Arg(0).U1());
                }
            }
            assert(halves == (fp16 ? 2u : 0u) && writes == 2 && predicate);
        }
    }
    for (u32 source : {106u, 107u}) {
        for (u32 opcode : {10u, 15u, 32u}) {
            const unsigned modes = opcode == 32 ? 3 : 1;
            for (unsigned mode = 0; mode < modes; ++mode) {
                const u32 select_lo = opcode == 32 && mode == 2 ? 1u << 11 : 0;
                const u32 select_hi = opcode == 32 ? (mode ? 1u << 27 : 0) : 3u << 27;
                const std::array<u32, 2> code{0xcc000000u | (opcode << 16) | select_lo | 7u,
                    source | (263u << 9) | (264u << 18) | select_hi};
                Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
                Gcn::GcnDecodeContext decoder;
                const auto inst = decoder.decodeInstruction(slice);
                assert(slice.atEnd() && inst.src[0].code == source);
                Info info{};
                info.hw_stage = HwStage::Fragment;
                info.sw_stage = SwStage::Fragment;
                RuntimeInfo runtime{};
                runtime.Initialize(info.hw_stage, info.sw_stage);
                Profile profile{};
                Common::ObjectPool<IR::Inst> pool;
                IR::Block block(pool);
                Gcn::Translator translator(info, runtime, profile);
                translator.Translate(&block, 0, IR::Condition::True, {&inst, 1});
                assert(!info.translation_failed);
                bool read_vcc = false;
                unsigned half_unpacks = 0, writes = 0;
                for (const auto& op : block) {
                    read_vcc |= op.GetOpcode() == (source == 107 ? IR::Opcode::GetVccHi
                                                                : IR::Opcode::GetVccLo);
                    half_unpacks += op.GetOpcode() == IR::Opcode::UnpackHalf2x16;
                    writes += op.GetOpcode() == IR::Opcode::SetVectorRegister;
                }
                assert(read_vcc && writes == 1);
                if (opcode == 32) { assert(half_unpacks == (mode ? 1u : 0u)); }
            }
        }
    }
    for (bool sd : {false, true}) {
        // Pro SDWA compare v_cmp v242, v11 (issue #55): SD=1 writes the mask to SDST = s[34:35].
        const std::array<u32, 2> code{0x7c0616f9u, sd ? 0x0686a2f2u : 0x068600f2u};
        Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
        Gcn::GcnDecodeContext decoder;
        const auto inst = decoder.decodeInstruction(slice);
        assert(slice.atEnd());
        if (inst.dst[1].field != (sd ? Gcn::OperandField::ScalarGPR : Gcn::OperandField::VccLo) ||
            (sd && inst.dst[1].code != 34)) {
            std::fprintf(stderr, "FAIL: SDWA compare with SD=%d decodes the wrong mask\n", sd);
            return 1;
        }
        Info info{};
        info.hw_stage = HwStage::Fragment;
        info.sw_stage = SwStage::Fragment;
        RuntimeInfo runtime{};
        runtime.Initialize(info.hw_stage, info.sw_stage);
        Profile profile{};
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        Gcn::Translator translator(info, runtime, profile);
        translator.Translate(&block, 0, IR::Condition::True, {&inst, 1});
        assert(!info.translation_failed);
        unsigned vcc_writes = 0, sgpr_writes = 0;
        for (const auto& op : block) {
            vcc_writes += op.GetOpcode() == IR::Opcode::SetVccLo ||
                          op.GetOpcode() == IR::Opcode::SetVccHi;
            sgpr_writes += op.GetOpcode() == IR::Opcode::SetScalarRegister;
        }
        if (vcc_writes != (sd ? 0u : 2u) || sgpr_writes != (sd ? 2u : 0u)) {
            std::fprintf(stderr, "FAIL: SDWA compare with SD=%d writes the wrong mask\n", sd);
            return 1;
        }
    }
    std::puts("PASS: Pro shader decoding and translation (67 cases)");
}

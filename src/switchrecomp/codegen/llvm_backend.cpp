#include "switchrecomp/codegen/llvm_backend.hpp"

#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/runtime/fp.hpp"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <iomanip>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace switchrecomp::codegen
{

namespace
{

using namespace llvm;

[[nodiscard]] Error expected_error(ErrorCode code, const char* prefix, llvm::Error error)
{
    return make_error(code, std::string(prefix) + llvm::toString(std::move(error)));
}

[[nodiscard]] IntegerType* integer_type(LLVMContext& context, ir::Type type)
{
    switch (type.kind())
    {
    case ir::TypeKind::I1: return Type::getInt1Ty(context);
    case ir::TypeKind::I8: return Type::getInt8Ty(context);
    case ir::TypeKind::I16: return Type::getInt16Ty(context);
    case ir::TypeKind::I32: return Type::getInt32Ty(context);
    case ir::TypeKind::I64: return Type::getInt64Ty(context);
    case ir::TypeKind::F32:
    case ir::TypeKind::F64:
    case ir::TypeKind::V128:
    case ir::TypeKind::Void: break;
    }
    return nullptr;
}

[[nodiscard]] std::string function_verification_error(Function& function, Module& module)
{
    std::string message;
    raw_string_ostream stream(message);
    if (verifyFunction(function, &stream))
    {
        stream.flush();
        return "LLVM function verification failed: " + message;
    }
    if (verifyModule(module, &stream))
    {
        stream.flush();
        return "LLVM module verification failed: " + message;
    }
    return {};
}

class ModuleLowerer
{
  public:
    ModuleLowerer(LLVMContext& context, const ir::Function& function)
        : context_(context), function_(function), module_(std::make_unique<Module>("switchrecomp", context)),
          builder_(context)
    {
        cpu_type_ = StructType::create(context_, "switchrecomp.CpuState");
        auto* x_type = ArrayType::get(Type::getInt64Ty(context_), 31U);
        auto* flags_type = ArrayType::get(Type::getInt8Ty(context_), 4U);
        auto* vector_array = ArrayType::get(vector_type(), 32U);
        cpu_type_->setBody({x_type, Type::getInt64Ty(context_), Type::getInt64Ty(context_), flags_type,
                            Type::getInt32Ty(context_), Type::getInt32Ty(context_), vector_array});
    }

    [[nodiscard]] Result<std::unique_ptr<Module>> run()
    {
        const auto verified = ir::verify(function_);
        if (!verified)
        {
            return Result<std::unique_ptr<Module>>::failure(verified.error());
        }
        llvm_function_ = Function::Create(
            FunctionType::get(Type::getInt32Ty(context_),
                              {PointerType::getUnqual(cpu_type_), PointerType::getUnqual(Type::getInt8Ty(context_))},
                              false),
            Function::ExternalLinkage, function_.name(), module_.get());
        entry_ = BasicBlock::Create(context_, "entry", llvm_function_);
        builder_.SetInsertPoint(entry_);
        cpu_ = llvm_function_->getArg(0);
        runtime_ = llvm_function_->getArg(1);
        values_.resize(function_.values().size(), nullptr);

        blocks_.reserve(function_.blocks().size());
        for (const auto& block : function_.blocks())
        {
            blocks_.push_back(BasicBlock::Create(context_, "block_" + std::to_string(block.id),
                                                 llvm_function_));
        }
        builder_.CreateBr(blocks_[function_.entry_block()]);

        for (const auto& block : function_.blocks())
        {
            builder_.SetInsertPoint(blocks_[block.id]);
            for (const auto& instruction : block.instructions)
            {
                const auto lowered = lower_instruction(instruction);
                if (!lowered)
                {
                    return Result<std::unique_ptr<Module>>::failure(lowered.error());
                }
            }
            const auto terminated = lower_terminator(block.terminator);
            if (!terminated)
            {
                return Result<std::unique_ptr<Module>>::failure(terminated.error());
            }
        }

        const auto verification = function_verification_error(*llvm_function_, *module_);
        if (!verification.empty())
        {
            return Result<std::unique_ptr<Module>>::failure(
                make_error(ErrorCode::LlvmVerificationFailed, verification));
        }
        return Result<std::unique_ptr<Module>>::success(std::move(module_));
    }

    [[nodiscard]] const std::string& function_name() const noexcept { return function_.name(); }

  private:
    [[nodiscard]] llvm::Value* value(ir::ValueId id) const noexcept
    {
        return id < values_.size() ? values_[id] : nullptr;
    }

    [[nodiscard]] Type* type(ir::Type type) const
    {
        if (type.is_void())
        {
            return Type::getVoidTy(context_);
        }
        switch (type.kind())
        {
        case ir::TypeKind::F32: return Type::getFloatTy(context_);
        case ir::TypeKind::F64: return Type::getDoubleTy(context_);
        case ir::TypeKind::V128: return vector_type();
        default: return integer_type(context_, type);
        }
    }

    [[nodiscard]] StructType* vector_type() const
    {
        return StructType::get(context_, {Type::getInt64Ty(context_), Type::getInt64Ty(context_)});
    }

    [[nodiscard]] Value* cpu_element(unsigned int index, Value* array_index)
    {
        return builder_.CreateGEP(cpu_type_, cpu_, {builder_.getInt32(0), builder_.getInt32(index),
                                                    array_index}, "cpu.element");
    }

    [[nodiscard]] Value* register_pointer(const ir::GuestRegister& reg)
    {
        if (reg.is_stack_pointer)
        {
            return builder_.CreateStructGEP(cpu_type_, cpu_, 1U, "cpu.sp");
        }
        return cpu_element(0U, builder_.getInt32(reg.index));
    }

    [[nodiscard]] Value* flag_pointer(ir::Flag flag)
    {
        return cpu_element(3U, builder_.getInt32(static_cast<unsigned int>(flag)));
    }

    [[nodiscard]] Value* vector_pointer(std::uint8_t index)
    {
        return builder_.CreateGEP(cpu_type_, cpu_, {builder_.getInt32(0), builder_.getInt32(6U),
                                                    builder_.getInt32(index)}, "cpu.v");
    }

    [[nodiscard]] Result<Value*> require_value(ir::ValueId id)
    {
        auto* result = value(id);
        if (result == nullptr)
        {
            return Result<Value*>::failure(
                make_error(ErrorCode::InvalidIrValue, "LLVM lowering encountered an invalid value id"));
        }
        return Result<Value*>::success(result);
    }

    void assign(const ir::Instruction& instruction, Value* value)
    {
        if (instruction.result != ir::invalid_value)
        {
            values_[instruction.result] = value;
        }
    }

    [[nodiscard]] Result<void> checked_runtime_call(CallInst* call)
    {
        auto* failed = BasicBlock::Create(context_, "runtime.error", llvm_function_);
        auto* continued = BasicBlock::Create(context_, "runtime.continue", llvm_function_);
        auto* nonzero = builder_.CreateICmpNE(call, builder_.getInt32(0), "runtime.failed");
        builder_.CreateCondBr(nonzero, failed, continued);
        builder_.SetInsertPoint(failed);
        builder_.CreateRet(call);
        builder_.SetInsertPoint(continued);
        return Result<void>::success();
    }

    [[nodiscard]] FunctionCallee runtime_load()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {runtime_->getType(), Type::getInt64Ty(context_),
                                        Type::getInt8Ty(context_), PointerType::getUnqual(Type::getInt64Ty(context_))},
                                       false);
        return module_->getOrInsertFunction("switchrecomp_runtime_guest_load", type);
    }

    [[nodiscard]] FunctionCallee runtime_store()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {runtime_->getType(), Type::getInt64Ty(context_),
                                        Type::getInt8Ty(context_), Type::getInt64Ty(context_)}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_guest_store", type);
    }

    [[nodiscard]] FunctionCallee runtime_address_add()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {runtime_->getType(), Type::getInt64Ty(context_),
                                        Type::getInt64Ty(context_), PointerType::getUnqual(Type::getInt64Ty(context_))},
                                       false);
        return module_->getOrInsertFunction("switchrecomp_runtime_guest_address_add", type);
    }

    [[nodiscard]] FunctionCallee runtime_address_add_value()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {runtime_->getType(), Type::getInt64Ty(context_),
                                        Type::getInt64Ty(context_), Type::getInt8Ty(context_),
                                        PointerType::getUnqual(Type::getInt64Ty(context_))}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_guest_address_add_value", type);
    }

    [[nodiscard]] FunctionCallee runtime_trap()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {runtime_->getType(), PointerType::getUnqual(Type::getInt8Ty(context_))},
                                       false);
        return module_->getOrInsertFunction("switchrecomp_runtime_trap", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_load()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {runtime_->getType(), Type::getInt64Ty(context_),
                                        PointerType::getUnqual(vector_type())}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_guest_load_vector", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_store()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {runtime_->getType(), Type::getInt64Ty(context_),
                                        PointerType::getUnqual(vector_type())}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_guest_store_vector", type);
    }

    [[nodiscard]] FunctionCallee runtime_fp_binary()
    {
        auto* type = FunctionType::get(Type::getInt64Ty(context_),
                                       {PointerType::getUnqual(cpu_type_), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_), Type::getInt64Ty(context_),
                                        Type::getInt64Ty(context_)}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_fp_binary", type);
    }

    [[nodiscard]] FunctionCallee runtime_fp_unary()
    {
        auto* type = FunctionType::get(Type::getInt64Ty(context_),
                                       {PointerType::getUnqual(cpu_type_), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_), Type::getInt64Ty(context_)}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_fp_unary", type);
    }

    [[nodiscard]] FunctionCallee runtime_fp_compare()
    {
        auto* type = FunctionType::get(Type::getInt32Ty(context_),
                                       {PointerType::getUnqual(cpu_type_), Type::getInt8Ty(context_),
                                        Type::getInt64Ty(context_), Type::getInt64Ty(context_),
                                        Type::getInt8Ty(context_)}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_fp_compare", type);
    }

    [[nodiscard]] FunctionCallee runtime_fp_convert()
    {
        auto* type = FunctionType::get(Type::getInt64Ty(context_),
                                       {PointerType::getUnqual(cpu_type_), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_), Type::getInt8Ty(context_),
                                        Type::getInt64Ty(context_)}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_fp_convert", type);
    }

    [[nodiscard]] FunctionCallee runtime_fp_round()
    {
        auto* type = FunctionType::get(Type::getInt64Ty(context_),
                                       {PointerType::getUnqual(cpu_type_), Type::getInt8Ty(context_),
                                        Type::getInt64Ty(context_), Type::getInt8Ty(context_)}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_fp_round", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_extract()
    {
        auto* type = FunctionType::get(Type::getInt64Ty(context_),
                                       {PointerType::getUnqual(vector_type()), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_)}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_vector_extract", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_insert()
    {
        auto* type = FunctionType::get(Type::getVoidTy(context_),
                                       {PointerType::getUnqual(vector_type()), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_), Type::getInt64Ty(context_),
                                        PointerType::getUnqual(vector_type())}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_vector_insert", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_broadcast()
    {
        auto* type = FunctionType::get(Type::getVoidTy(context_),
                                       {Type::getInt64Ty(context_), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_), PointerType::getUnqual(vector_type())}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_vector_broadcast", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_binary()
    {
        auto* type = FunctionType::get(Type::getVoidTy(context_),
                                       {PointerType::getUnqual(cpu_type_), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_), PointerType::getUnqual(vector_type()),
                                        PointerType::getUnqual(vector_type()), PointerType::getUnqual(vector_type())}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_vector_binary", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_compare()
    {
        auto* type = FunctionType::get(Type::getVoidTy(context_),
                                       {PointerType::getUnqual(cpu_type_), Type::getInt8Ty(context_),
                                        Type::getInt8Ty(context_), PointerType::getUnqual(vector_type()),
                                        PointerType::getUnqual(vector_type()), PointerType::getUnqual(vector_type())}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_vector_compare", type);
    }

    [[nodiscard]] FunctionCallee runtime_vector_shuffle()
    {
        auto* type = FunctionType::get(Type::getVoidTy(context_),
                                       {Type::getInt8Ty(context_), Type::getInt8Ty(context_),
                                        PointerType::getUnqual(vector_type()), PointerType::getUnqual(vector_type()),
                                        Type::getInt8Ty(context_), PointerType::getUnqual(vector_type())}, false);
        return module_->getOrInsertFunction("switchrecomp_runtime_vector_shuffle", type);
    }

    [[nodiscard]] Value* fp_bits(Value* value, ir::Type source_type)
    {
        const auto width = source_type.bit_width();
        auto* integer = width == 32U ? Type::getInt32Ty(context_) : Type::getInt64Ty(context_);
        auto* bits = builder_.CreateBitCast(value, integer, "fp.bits");
        return width == 32U ? builder_.CreateZExt(bits, Type::getInt64Ty(context_), "fp.bits.wide") : bits;
    }

    [[nodiscard]] Value* fp_value(Value* bits, ir::Type result_type)
    {
        auto* narrowed = result_type.bit_width() == 32U
                             ? builder_.CreateTrunc(bits, Type::getInt32Ty(context_), "fp.result.narrow")
                             : bits;
        return builder_.CreateBitCast(narrowed, type(result_type), "fp.result");
    }

    [[nodiscard]] unsigned int arrangement_bits(ir::VectorArrangement arrangement) const noexcept
    {
        switch (arrangement)
        {
        case ir::VectorArrangement::B8:
        case ir::VectorArrangement::B16: return 8U;
        case ir::VectorArrangement::H4:
        case ir::VectorArrangement::H8: return 16U;
        case ir::VectorArrangement::S2:
        case ir::VectorArrangement::S4: return 32U;
        case ir::VectorArrangement::D1:
        case ir::VectorArrangement::D2: return 64U;
        case ir::VectorArrangement::Raw128: return 0U;
        }
        return 0U;
    }

    [[nodiscard]] unsigned int arrangement_lanes(ir::VectorArrangement arrangement) const noexcept
    {
        switch (arrangement)
        {
        case ir::VectorArrangement::B8: return 8U;
        case ir::VectorArrangement::B16: return 16U;
        case ir::VectorArrangement::H4: return 4U;
        case ir::VectorArrangement::H8: return 8U;
        case ir::VectorArrangement::S2: return 2U;
        case ir::VectorArrangement::S4: return 4U;
        case ir::VectorArrangement::D1: return 1U;
        case ir::VectorArrangement::D2: return 2U;
        case ir::VectorArrangement::Raw128: return 0U;
        }
        return 0U;
    }

    [[nodiscard]] Result<void> lower_instruction(const ir::Instruction& instruction)
    {
        const auto result_type = type(instruction.result_type);
        switch (instruction.opcode)
        {
        case ir::Opcode::Constant:
            if (instruction.result_type.is_integer())
            {
                assign(instruction, ConstantInt::get(cast<IntegerType>(result_type), instruction.constant));
            }
            else if (instruction.result_type == ir::f32_type() || instruction.result_type == ir::f64_type())
            {
                auto* integer = instruction.result_type == ir::f32_type()
                                     ? ConstantInt::get(Type::getInt32Ty(context_), instruction.constant)
                                     : ConstantInt::get(Type::getInt64Ty(context_), instruction.constant);
                assign(instruction, builder_.CreateBitCast(integer, result_type, "fp.constant"));
            }
            else if (instruction.result_type == ir::v128_type())
            {
                assign(instruction, ConstantStruct::get(cast<StructType>(result_type),
                                                         {ConstantInt::get(Type::getInt64Ty(context_), instruction.constant),
                                                          ConstantInt::get(Type::getInt64Ty(context_), instruction.constant_high)}));
            }
            return Result<void>::success();
        case ir::Opcode::Nop:
            return Result<void>::success();
        case ir::Opcode::SetPc:
            builder_.CreateStore(builder_.getInt64(instruction.source.guest_pc),
                                 builder_.CreateStructGEP(cpu_type_, cpu_, 2U, "cpu.pc"));
            return Result<void>::success();
        case ir::Opcode::ReadRegister:
        {
            if (instruction.reg.is_zero)
            {
                assign(instruction, ConstantInt::get(cast<IntegerType>(result_type), 0U));
                return Result<void>::success();
            }
            Value* loaded = builder_.CreateLoad(Type::getInt64Ty(context_), register_pointer(instruction.reg),
                                                "register.read");
            if (instruction.reg.width == ir::RegisterWidth::W32)
            {
                loaded = builder_.CreateTrunc(loaded, Type::getInt32Ty(context_), "register.w");
            }
            assign(instruction, loaded);
            return Result<void>::success();
        }
        case ir::Opcode::ReadVectorRegister:
            assign(instruction, builder_.CreateLoad(vector_type(), vector_pointer(instruction.vector_index), "vector.read"));
            return Result<void>::success();
        case ir::Opcode::WriteVectorRegister:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source) return Result<void>::failure(source.error());
            builder_.CreateStore(source.value(), vector_pointer(instruction.vector_index));
            return Result<void>::success();
        }
        case ir::Opcode::ReadFpControl:
            assign(instruction, builder_.CreateLoad(Type::getInt32Ty(context_),
                                                     builder_.CreateStructGEP(cpu_type_, cpu_, 4U, "cpu.fpcr"), "fpcr.read"));
            return Result<void>::success();
        case ir::Opcode::WriteFpControl:
        case ir::Opcode::WriteFpStatus:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source) return Result<void>::failure(source.error());
            builder_.CreateStore(source.value(), builder_.CreateStructGEP(cpu_type_, cpu_,
                                                                            instruction.opcode == ir::Opcode::WriteFpControl ? 4U : 5U,
                                                                            instruction.opcode == ir::Opcode::WriteFpControl ? "cpu.fpcr" : "cpu.fpsr"));
            return Result<void>::success();
        }
        case ir::Opcode::ReadFpStatus:
            assign(instruction, builder_.CreateLoad(Type::getInt32Ty(context_),
                                                     builder_.CreateStructGEP(cpu_type_, cpu_, 5U, "cpu.fpsr"), "fpsr.read"));
            return Result<void>::success();
        case ir::Opcode::WriteRegister:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source)
            {
                return Result<void>::failure(source.error());
            }
            if (!instruction.reg.is_zero)
            {
                auto* store_value = source.value();
                if (instruction.reg.width == ir::RegisterWidth::W32)
                {
                    store_value = builder_.CreateZExt(store_value, Type::getInt64Ty(context_), "register.w.zext");
                }
                builder_.CreateStore(store_value, register_pointer(instruction.reg));
            }
            return Result<void>::success();
        }
        case ir::Opcode::ReadFlag:
        {
            auto* loaded = builder_.CreateLoad(Type::getInt8Ty(context_), flag_pointer(instruction.flag), "flag.read");
            assign(instruction, builder_.CreateICmpNE(loaded, builder_.getInt8(0), "flag.bool"));
            return Result<void>::success();
        }
        case ir::Opcode::WriteFlag:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source)
            {
                return Result<void>::failure(source.error());
            }
            builder_.CreateStore(builder_.CreateZExt(source.value(), Type::getInt8Ty(context_), "flag.byte"),
                                 flag_pointer(instruction.flag));
            return Result<void>::success();
        }
        case ir::Opcode::Add:
        case ir::Opcode::Sub:
        case ir::Opcode::Mul:
        case ir::Opcode::And:
        case ir::Opcode::Or:
        case ir::Opcode::Xor:
        case ir::Opcode::ShiftLeft:
        case ir::Opcode::LogicalShiftRight:
        case ir::Opcode::ArithmeticShiftRight:
        case ir::Opcode::RotateRight:
        {
            const auto left = require_value(instruction.operands[0]);
            const auto right = require_value(instruction.operands[1]);
            if (!left || !right)
            {
                return Result<void>::failure(!left ? left.error() : right.error());
            }
            Value* lowered = nullptr;
            switch (instruction.opcode)
            {
            case ir::Opcode::Add: lowered = builder_.CreateAdd(left.value(), right.value(), "add"); break;
            case ir::Opcode::Sub: lowered = builder_.CreateSub(left.value(), right.value(), "sub"); break;
            case ir::Opcode::Mul: lowered = builder_.CreateMul(left.value(), right.value(), "mul"); break;
            case ir::Opcode::And: lowered = builder_.CreateAnd(left.value(), right.value(), "and"); break;
            case ir::Opcode::Or: lowered = builder_.CreateOr(left.value(), right.value(), "or"); break;
            case ir::Opcode::Xor: lowered = builder_.CreateXor(left.value(), right.value(), "xor"); break;
            case ir::Opcode::ShiftLeft: lowered = builder_.CreateShl(left.value(), right.value(), "shl"); break;
            case ir::Opcode::LogicalShiftRight: lowered = builder_.CreateLShr(left.value(), right.value(), "lshr"); break;
            case ir::Opcode::ArithmeticShiftRight: lowered = builder_.CreateAShr(left.value(), right.value(), "ashr"); break;
            case ir::Opcode::RotateRight:
            {
                auto* width = ConstantInt::get(right.value()->getType(),
                                               instruction.result_type.bit_width());
                auto* inverse = builder_.CreateSub(width, right.value(), "ror.inverse");
                auto* right_shift = builder_.CreateLShr(left.value(), right.value(), "ror.right");
                auto* left_shift = builder_.CreateShl(left.value(), inverse, "ror.left");
                lowered = builder_.CreateOr(right_shift, left_shift, "ror");
                break;
            }
            default: break;
            }
            assign(instruction, lowered);
            return Result<void>::success();
        }
        case ir::Opcode::Not:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source)
            {
                return Result<void>::failure(source.error());
            }
            assign(instruction, builder_.CreateNot(source.value(), "not"));
            return Result<void>::success();
        }
        case ir::Opcode::Truncate:
        case ir::Opcode::ZeroExtend:
        case ir::Opcode::SignExtend:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source)
            {
                return Result<void>::failure(source.error());
            }
            Value* lowered = instruction.opcode == ir::Opcode::Truncate
                                 ? builder_.CreateTrunc(source.value(), result_type, "trunc")
                                 : instruction.opcode == ir::Opcode::ZeroExtend
                                       ? builder_.CreateZExt(source.value(), result_type, "zext")
                                       : builder_.CreateSExt(source.value(), result_type, "sext");
            assign(instruction, lowered);
            return Result<void>::success();
        }
        case ir::Opcode::CompareEqual:
        case ir::Opcode::CompareNotEqual:
        case ir::Opcode::CompareUnsigned:
        case ir::Opcode::CompareSigned:
        {
            const auto left = require_value(instruction.operands[0]);
            const auto right = require_value(instruction.operands[1]);
            if (!left || !right)
            {
                return Result<void>::failure(!left ? left.error() : right.error());
            }
            CmpInst::Predicate predicate = CmpInst::ICMP_EQ;
            if (instruction.opcode == ir::Opcode::CompareNotEqual)
            {
                predicate = CmpInst::ICMP_NE;
            }
            else if (instruction.opcode == ir::Opcode::CompareUnsigned)
            {
                predicate = CmpInst::ICMP_ULT;
            }
            else if (instruction.opcode == ir::Opcode::CompareSigned)
            {
                predicate = CmpInst::ICMP_SLT;
            }
            assign(instruction, builder_.CreateICmp(predicate, left.value(), right.value(), "compare"));
            return Result<void>::success();
        }
        case ir::Opcode::Select:
        {
            const auto condition = require_value(instruction.operands[0]);
            const auto when_true = require_value(instruction.operands[1]);
            const auto when_false = require_value(instruction.operands[2]);
            if (!condition || !when_true || !when_false)
            {
                return Result<void>::failure(!condition ? condition.error() : !when_true ? when_true.error() : when_false.error());
            }
            assign(instruction, builder_.CreateSelect(condition.value(), when_true.value(), when_false.value(), "select"));
            return Result<void>::success();
        }
        case ir::Opcode::FpBinary:
        {
            const auto left = require_value(instruction.operands[0]);
            const auto right = require_value(instruction.operands[1]);
            if (!left || !right) return Result<void>::failure(!left ? left.error() : right.error());
            const auto bits = builder_.CreateCall(runtime_fp_binary(),
                                                  {cpu_, builder_.getInt8(static_cast<unsigned int>(instruction.fp_binary)),
                                                   builder_.getInt8(instruction.result_type.bit_width()),
                                                   fp_bits(left.value(), instruction.result_type),
                                                   fp_bits(right.value(), instruction.result_type)}, "fp.binary.bits");
            assign(instruction, fp_value(bits, instruction.result_type));
            return Result<void>::success();
        }
        case ir::Opcode::FpUnary:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source) return Result<void>::failure(source.error());
            const auto bits = builder_.CreateCall(runtime_fp_unary(),
                                                  {cpu_, builder_.getInt8(static_cast<unsigned int>(instruction.fp_unary)),
                                                   builder_.getInt8(instruction.result_type.bit_width()),
                                                   fp_bits(source.value(), instruction.result_type)}, "fp.unary.bits");
            assign(instruction, fp_value(bits, instruction.result_type));
            return Result<void>::success();
        }
        case ir::Opcode::FpCompare:
        {
            const auto left = require_value(instruction.operands[0]);
            const auto right = require_value(instruction.operands[1]);
            const auto* left_definition = function_.value(instruction.operands[0]);
            const auto* right_definition = function_.value(instruction.operands[1]);
            if (!left || !right || left_definition == nullptr || right_definition == nullptr)
                return Result<void>::failure(!left ? left.error() : !right ? right.error()
                                                                            : make_error(ErrorCode::InvalidIrValue, "FP compare source is missing"));
            const auto bits = builder_.CreateCall(runtime_fp_compare(),
                                                  {cpu_, builder_.getInt8(left_definition->type.bit_width()),
                                                   fp_bits(left.value(), left_definition->type),
                                                   fp_bits(right.value(), right_definition->type),
                                                   builder_.getInt8(instruction.signaling ? 1U : 0U)}, "fp.compare");
            assign(instruction, bits);
            return Result<void>::success();
        }
        case ir::Opcode::FpConvert:
        {
            const auto source = require_value(instruction.operands[0]);
            const auto* source_definition = function_.value(instruction.operands[0]);
            if (!source || source_definition == nullptr) return Result<void>::failure(!source ? source.error() : make_error(ErrorCode::InvalidIrValue, "FP conversion source is missing"));
            const auto source_type = source_definition->type;
            Value* source_bits = source_type.is_floating() ? fp_bits(source.value(), source_type) : source.value();
            if (source_bits->getType()->getIntegerBitWidth() != 64U)
                source_bits = builder_.CreateZExt(source_bits, Type::getInt64Ty(context_), "fp.convert.wide");
            const auto bits = builder_.CreateCall(runtime_fp_convert(),
                                                  {cpu_, builder_.getInt8(static_cast<unsigned int>(instruction.fp_conversion)),
                                                   builder_.getInt8(source_type.bit_width()),
                                                   builder_.getInt8(instruction.result_type.bit_width()), source_bits}, "fp.convert.bits");
            assign(instruction, instruction.result_type.is_floating()
                                  ? fp_value(bits, instruction.result_type)
                                  : instruction.result_type == ir::i64_type() ? static_cast<Value*>(bits)
                                                                              : builder_.CreateTrunc(bits, type(instruction.result_type), "fp.convert.int"));
            return Result<void>::success();
        }
        case ir::Opcode::FpRound:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source) return Result<void>::failure(source.error());
            const auto bits = builder_.CreateCall(runtime_fp_round(),
                                                  {cpu_, builder_.getInt8(instruction.result_type.bit_width()),
                                                   fp_bits(source.value(), instruction.result_type),
                                                   builder_.getInt8(static_cast<unsigned int>(instruction.rounding_mode))}, "fp.round.bits");
            assign(instruction, fp_value(bits, instruction.result_type));
            return Result<void>::success();
        }
        case ir::Opcode::VectorExtractLane:
        {
            const auto source = require_value(instruction.operands[0]);
            if (!source) return Result<void>::failure(source.error());
            auto* storage = builder_.CreateAlloca(vector_type(), nullptr, "vector.extract.source");
            builder_.CreateStore(source.value(), storage);
            auto* bits = builder_.CreateCall(runtime_vector_extract(),
                                             {storage, builder_.getInt8(arrangement_bits(instruction.arrangement)),
                                              builder_.getInt8(instruction.lane_index)}, "vector.extract.bits");
            assign(instruction, instruction.result_type == ir::i64_type()
                                  ? static_cast<Value*>(bits)
                                  : builder_.CreateTrunc(bits, result_type, "vector.extract.narrow"));
            return Result<void>::success();
        }
        case ir::Opcode::VectorInsertLane:
        {
            const auto source = require_value(instruction.operands[0]);
            const auto lane = require_value(instruction.operands[1]);
            if (!source || !lane) return Result<void>::failure(!source ? source.error() : lane.error());
            auto* input = builder_.CreateAlloca(vector_type(), nullptr, "vector.insert.source");
            auto* output = builder_.CreateAlloca(vector_type(), nullptr, "vector.insert.result");
            builder_.CreateStore(source.value(), input);
            auto* lane_value = lane.value();
            if (lane_value->getType()->getIntegerBitWidth() != 64U)
                lane_value = builder_.CreateZExt(lane_value, Type::getInt64Ty(context_), "vector.insert.wide");
            builder_.CreateCall(runtime_vector_insert(),
                                {input, builder_.getInt8(arrangement_bits(instruction.arrangement)),
                                 builder_.getInt8(instruction.lane_index), lane_value, output});
            assign(instruction, builder_.CreateLoad(vector_type(), output, "vector.insert.value"));
            return Result<void>::success();
        }
        case ir::Opcode::VectorBroadcast:
        {
            const auto lane = require_value(instruction.operands[0]);
            if (!lane) return Result<void>::failure(lane.error());
            auto* lane_value = lane.value();
            if (lane_value->getType()->getIntegerBitWidth() != 64U)
                lane_value = builder_.CreateZExt(lane_value, Type::getInt64Ty(context_), "vector.broadcast.wide");
            auto* output = builder_.CreateAlloca(vector_type(), nullptr, "vector.broadcast.result");
            builder_.CreateCall(runtime_vector_broadcast(),
                                {lane_value, builder_.getInt8(arrangement_bits(instruction.arrangement)),
                                 builder_.getInt8(arrangement_lanes(instruction.arrangement)), output});
            assign(instruction, builder_.CreateLoad(vector_type(), output, "vector.broadcast.value"));
            return Result<void>::success();
        }
        case ir::Opcode::VectorBinary:
        case ir::Opcode::VectorCompare:
        {
            const auto left = require_value(instruction.operands[0]);
            const auto right = require_value(instruction.operands[1]);
            if (!left || !right) return Result<void>::failure(!left ? left.error() : right.error());
            auto* left_storage = builder_.CreateAlloca(vector_type(), nullptr, "vector.left");
            auto* right_storage = builder_.CreateAlloca(vector_type(), nullptr, "vector.right");
            auto* output = builder_.CreateAlloca(vector_type(), nullptr, "vector.result");
            builder_.CreateStore(left.value(), left_storage);
            builder_.CreateStore(right.value(), right_storage);
            if (instruction.opcode == ir::Opcode::VectorBinary)
            {
                builder_.CreateCall(runtime_vector_binary(),
                                    {cpu_, builder_.getInt8(static_cast<unsigned int>(instruction.vector_operation)),
                                     builder_.getInt8(static_cast<unsigned int>(instruction.arrangement)),
                                     left_storage, right_storage, output});
            }
            else
            {
                builder_.CreateCall(runtime_vector_compare(),
                                    {cpu_, builder_.getInt8(static_cast<unsigned int>(instruction.vector_compare)),
                                     builder_.getInt8(static_cast<unsigned int>(instruction.arrangement)),
                                     left_storage, right_storage, output});
            }
            assign(instruction, builder_.CreateLoad(vector_type(), output, "vector.value"));
            return Result<void>::success();
        }
        case ir::Opcode::VectorShuffle:
        {
            const auto left = require_value(instruction.operands[0]);
            const auto right = require_value(instruction.operands[1]);
            if (!left || !right) return Result<void>::failure(!left ? left.error() : right.error());
            auto* left_storage = builder_.CreateAlloca(vector_type(), nullptr, "shuffle.left");
            auto* right_storage = builder_.CreateAlloca(vector_type(), nullptr, "shuffle.right");
            auto* output = builder_.CreateAlloca(vector_type(), nullptr, "shuffle.result");
            builder_.CreateStore(left.value(), left_storage);
            builder_.CreateStore(right.value(), right_storage);
            builder_.CreateCall(runtime_vector_shuffle(),
                                {builder_.getInt8(instruction.vector_index),
                                 builder_.getInt8(static_cast<unsigned int>(instruction.arrangement)),
                                 left_storage, right_storage, builder_.getInt8(instruction.immediate), output});
            assign(instruction, builder_.CreateLoad(vector_type(), output, "shuffle.value"));
            return Result<void>::success();
        }
        case ir::Opcode::AddCarry:
        case ir::Opcode::SubCarry:
        case ir::Opcode::AddOverflow:
        case ir::Opcode::SubOverflow:
        {
            const auto left = require_value(instruction.operands[0]);
            const auto right = require_value(instruction.operands[1]);
            if (!left || !right)
            {
                return Result<void>::failure(!left ? left.error() : right.error());
            }
            if (instruction.opcode == ir::Opcode::AddCarry)
            {
                const auto sum = builder_.CreateAdd(left.value(), right.value(), "flag.add");
                assign(instruction, builder_.CreateICmpULT(sum, left.value(), "carry"));
            }
            else if (instruction.opcode == ir::Opcode::SubCarry)
            {
                assign(instruction, builder_.CreateICmpUGE(left.value(), right.value(), "carry"));
            }
            else
            {
                auto* integer = cast<IntegerType>(left.value()->getType());
                auto* xor_lr = builder_.CreateXor(left.value(), right.value(), "flag.xor.lr");
                auto* result = instruction.opcode == ir::Opcode::AddOverflow
                                   ? builder_.CreateAdd(left.value(), right.value(), "flag.add.result")
                                   : builder_.CreateSub(left.value(), right.value(), "flag.sub.result");
                auto* xor_result = builder_.CreateXor(left.value(), result, "flag.xor.result");
                auto* bits = instruction.opcode == ir::Opcode::AddOverflow
                                 ? builder_.CreateAnd(builder_.CreateNot(xor_lr), xor_result, "flag.add.bits")
                                 : builder_.CreateAnd(xor_lr, xor_result, "flag.sub.bits");
                auto* shifted = builder_.CreateLShr(bits, ConstantInt::get(integer, integer->getBitWidth() - 1U),
                                                    "flag.overflow");
                assign(instruction, builder_.CreateTrunc(shifted, Type::getInt1Ty(context_), "overflow"));
            }
            return Result<void>::success();
        }
        case ir::Opcode::EvaluateCondition:
        {
            const auto n = require_value(instruction.operands[0]);
            const auto z = require_value(instruction.operands[1]);
            const auto c = require_value(instruction.operands[2]);
            const auto v = require_value(instruction.operands[3]);
            if (!n || !z || !c || !v)
            {
                return Result<void>::failure(!n ? n.error() : !z ? z.error() : !c ? c.error() : v.error());
            }
            auto* not_z = builder_.CreateNot(z.value(), "not.z");
            auto* not_c = builder_.CreateNot(c.value(), "not.c");
            auto* not_n = builder_.CreateNot(n.value(), "not.n");
            auto* not_v = builder_.CreateNot(v.value(), "not.v");
            Value* lowered = nullptr;
            switch (instruction.condition)
            {
            case ir::ConditionCode::Eq: lowered = z.value(); break;
            case ir::ConditionCode::Ne: lowered = not_z; break;
            case ir::ConditionCode::Cs: lowered = c.value(); break;
            case ir::ConditionCode::Cc: lowered = not_c; break;
            case ir::ConditionCode::Mi: lowered = n.value(); break;
            case ir::ConditionCode::Pl: lowered = not_n; break;
            case ir::ConditionCode::Vs: lowered = v.value(); break;
            case ir::ConditionCode::Vc: lowered = not_v; break;
            case ir::ConditionCode::Hi: lowered = builder_.CreateAnd(c.value(), not_z, "hi"); break;
            case ir::ConditionCode::Ls: lowered = builder_.CreateOr(not_c, z.value(), "ls"); break;
            case ir::ConditionCode::Ge: lowered = builder_.CreateICmpEQ(n.value(), v.value(), "ge"); break;
            case ir::ConditionCode::Lt: lowered = builder_.CreateICmpNE(n.value(), v.value(), "lt"); break;
            case ir::ConditionCode::Gt:
                lowered = builder_.CreateAnd(not_z, builder_.CreateICmpEQ(n.value(), v.value(), "gt.sign"), "gt");
                break;
            case ir::ConditionCode::Le:
                lowered = builder_.CreateOr(z.value(), builder_.CreateICmpNE(n.value(), v.value(), "le.sign"), "le");
                break;
            case ir::ConditionCode::Al: lowered = builder_.getTrue(); break;
            case ir::ConditionCode::Nv: lowered = builder_.getFalse(); break;
            }
            assign(instruction, lowered);
            return Result<void>::success();
        }
        case ir::Opcode::GuestAddressAdd:
        {
            const auto base = require_value(instruction.operands[0]);
            if (!base)
            {
                return Result<void>::failure(base.error());
            }
            auto* output = builder_.CreateAlloca(Type::getInt64Ty(context_), nullptr, "guest.address");
            auto* call = builder_.CreateCall(runtime_address_add(),
                                             {runtime_, base.value(), ConstantInt::getSigned(Type::getInt64Ty(context_), instruction.immediate), output});
            const auto checked = checked_runtime_call(call);
            if (!checked)
            {
                return checked;
            }
            assign(instruction, builder_.CreateLoad(Type::getInt64Ty(context_), output, "guest.address.value"));
            return Result<void>::success();
        }
        case ir::Opcode::GuestAddressAddValue:
        {
            const auto base = require_value(instruction.operands[0]);
            const auto offset = require_value(instruction.operands[1]);
            if (!base || !offset)
            {
                return Result<void>::failure(!base ? base.error() : offset.error());
            }
            auto* output = builder_.CreateAlloca(Type::getInt64Ty(context_), nullptr, "guest.address");
            auto* call = builder_.CreateCall(runtime_address_add_value(),
                                             {runtime_, base.value(), offset.value(),
                                              builder_.getInt8(instruction.address_offset_signed ? 1U : 0U),
                                              output});
            const auto checked = checked_runtime_call(call);
            if (!checked)
            {
                return checked;
            }
            assign(instruction, builder_.CreateLoad(Type::getInt64Ty(context_), output,
                                                     "guest.address.value"));
            return Result<void>::success();
        }
        case ir::Opcode::GuestLoad:
        {
            const auto address = require_value(instruction.operands[0]);
            if (!address)
            {
                return Result<void>::failure(address.error());
            }
            auto* output = builder_.CreateAlloca(Type::getInt64Ty(context_), nullptr, "guest.load");
            auto* call = builder_.CreateCall(runtime_load(),
                                             {runtime_, address.value(), builder_.getInt8(instruction.memory_size), output});
            const auto checked = checked_runtime_call(call);
            if (!checked)
            {
                return checked;
            }
            Value* loaded = builder_.CreateLoad(Type::getInt64Ty(context_), output, "guest.load.value");
            if (instruction.result_type != ir::i64_type())
            {
                loaded = builder_.CreateTrunc(loaded, type(instruction.result_type), "guest.load.narrow");
            }
            assign(instruction, loaded);
            return Result<void>::success();
        }
        case ir::Opcode::GuestLoadVector:
        {
            const auto address = require_value(instruction.operands[0]);
            if (!address) return Result<void>::failure(address.error());
            auto* output = builder_.CreateAlloca(vector_type(), nullptr, "guest.load.vector");
            const auto checked = checked_runtime_call(builder_.CreateCall(
                runtime_vector_load(), {runtime_, address.value(), output}));
            if (!checked) return checked;
            assign(instruction, builder_.CreateLoad(vector_type(), output, "guest.load.vector.value"));
            return Result<void>::success();
        }
        case ir::Opcode::GuestStore:
        {
            const auto address = require_value(instruction.operands[0]);
            const auto stored_value = require_value(instruction.operands[1]);
            if (!address || !stored_value)
            {
                return Result<void>::failure(!address ? address.error() : stored_value.error());
            }
            auto* value = stored_value.value();
            if (value->getType()->getIntegerBitWidth() != 64U)
            {
                value = builder_.CreateZExt(value, Type::getInt64Ty(context_), "guest.store.narrow");
            }
            const auto checked = checked_runtime_call(builder_.CreateCall(
                runtime_store(), {runtime_, address.value(), builder_.getInt8(instruction.memory_size), value}));
            return checked;
        }
        case ir::Opcode::GuestStoreVector:
        {
            const auto address = require_value(instruction.operands[0]);
            const auto stored_value = require_value(instruction.operands[1]);
            if (!address || !stored_value)
                return Result<void>::failure(!address ? address.error() : stored_value.error());
            auto* storage = builder_.CreateAlloca(vector_type(), nullptr, "guest.store.vector");
            builder_.CreateStore(stored_value.value(), storage);
            return checked_runtime_call(builder_.CreateCall(runtime_vector_store(),
                                                             {runtime_, address.value(), storage}));
        }
        }
        return Result<void>::failure(make_error(ErrorCode::InvalidIrValue, "unknown IR opcode"));
    }

    [[nodiscard]] Result<void> lower_terminator(const ir::Terminator& terminator)
    {
        switch (terminator.kind)
        {
        case ir::TerminatorKind::Branch:
            builder_.CreateBr(blocks_[terminator.target]);
            return Result<void>::success();
        case ir::TerminatorKind::ConditionalBranch:
        {
            const auto condition = require_value(terminator.condition);
            if (!condition)
            {
                return Result<void>::failure(condition.error());
            }
            builder_.CreateCondBr(condition.value(), blocks_[terminator.target], blocks_[terminator.false_target]);
            return Result<void>::success();
        }
        case ir::TerminatorKind::DirectCall:
        case ir::TerminatorKind::IndirectBranch:
        case ir::TerminatorKind::IndirectCall:
        {
            const auto target = require_value(terminator.target_value);
            if (!target)
            {
                return Result<void>::failure(target.error());
            }
            builder_.CreateStore(target.value(), builder_.CreateStructGEP(cpu_type_, cpu_, 2U, "cpu.pc"));
            builder_.CreateRet(builder_.getInt32(0));
            return Result<void>::success();
        }
        case ir::TerminatorKind::Return:
            if (terminator.target_value != ir::invalid_value)
            {
                const auto target = require_value(terminator.target_value);
                if (!target)
                {
                    return Result<void>::failure(target.error());
                }
                auto* current_pc = builder_.CreateLoad(Type::getInt64Ty(context_),
                                                        builder_.CreateStructGEP(cpu_type_, cpu_, 2U, "cpu.pc"),
                                                        "cpu.pc.current");
                auto* nonzero = builder_.CreateICmpNE(target.value(), builder_.getInt64(0), "ret.target.valid");
                auto* selected = builder_.CreateSelect(nonzero, target.value(), current_pc, "ret.target");
                builder_.CreateStore(selected, builder_.CreateStructGEP(cpu_type_, cpu_, 2U, "cpu.pc"));
            }
            builder_.CreateRet(builder_.getInt32(0));
            return Result<void>::success();
        case ir::TerminatorKind::Trap:
        {
            auto* reason = builder_.CreateGlobalStringPtr(terminator.trap_reason, "trap.reason");
            auto* status = builder_.CreateCall(runtime_trap(), {runtime_, reason});
            builder_.CreateRet(status);
            return Result<void>::success();
        }
        }
        return Result<void>::failure(make_error(ErrorCode::InvalidIrBlock, "unknown IR terminator"));
    }

    LLVMContext& context_;
    const ir::Function& function_;
    std::unique_ptr<Module> module_;
    IRBuilder<> builder_;
    StructType* cpu_type_ = nullptr;
    Function* llvm_function_ = nullptr;
    BasicBlock* entry_ = nullptr;
    Value* cpu_ = nullptr;
    Value* runtime_ = nullptr;
    std::vector<BasicBlock*> blocks_;
    std::vector<Value*> values_;
};

} // namespace

struct LlvmBackend::Impl
{
    bool initialized = false;
};

LlvmBackend::LlvmBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LlvmBackend::LlvmBackend(LlvmBackend&&) noexcept = default;
LlvmBackend& LlvmBackend::operator=(LlvmBackend&&) noexcept = default;
LlvmBackend::~LlvmBackend() = default;

Result<std::unique_ptr<LlvmBackend>> LlvmBackend::create()
{
    try
    {
        InitializeNativeTarget();
        InitializeNativeTargetAsmPrinter();
        auto impl = std::make_unique<Impl>();
        impl->initialized = true;
        return Result<std::unique_ptr<LlvmBackend>>::success(
            std::unique_ptr<LlvmBackend>(new LlvmBackend(std::move(impl))));
    }
    catch (const std::bad_alloc&)
    {
        return Result<std::unique_ptr<LlvmBackend>>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate LLVM backend"));
    }
}

Result<std::string> LlvmBackend::lower_to_llvm_ir(const ir::Function& function) const
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Result<std::string>::failure(
            make_error(ErrorCode::JitCompilationFailed, "LLVM backend is not initialized"));
    }
    try
    {
        LLVMContext context;
        ModuleLowerer lowerer(context, function);
        const auto module = lowerer.run();
        if (!module)
        {
            return Result<std::string>::failure(module.error());
        }
        std::string text;
        raw_string_ostream output(text);
        module.value()->print(output, nullptr);
        output.flush();
        return Result<std::string>::success(std::move(text));
    }
    catch (const std::bad_alloc&)
    {
        return Result<std::string>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate LLVM IR"));
    }
}

Result<runtime::ExecutionResult> LlvmBackend::execute(const ir::Function& function,
                                                      runtime::CpuState& cpu,
                                                      runtime::RuntimeContext& runtime,
                                                      const runtime::ExecutionOptions& options) const
{
    (void)options;
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Result<runtime::ExecutionResult>::failure(
            make_error(ErrorCode::JitCompilationFailed, "LLVM backend is not initialized"));
    }
    const auto verified = ir::verify(function);
    if (!verified)
    {
        return Result<runtime::ExecutionResult>::failure(verified.error());
    }
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    auto context = std::make_unique<LLVMContext>();
    ModuleLowerer lowerer(*context, function);
    auto module = lowerer.run();
    if (!module)
    {
        return Result<runtime::ExecutionResult>::failure(module.error());
    }
    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit)
    {
        return Result<runtime::ExecutionResult>::failure(
            expected_error(ErrorCode::JitCompilationFailed, "failed to create LLVM JIT: ", jit.takeError()));
    }
    auto& dylib = (*jit)->getMainJITDylib();
    auto define = dylib.define(llvm::orc::absoluteSymbols({
        {(*jit)->mangleAndIntern("switchrecomp_runtime_guest_load"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_load),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_guest_store"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_store),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_guest_load_vector"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_load_vector),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_guest_store_vector"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_store_vector),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_guest_address_add"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_address_add),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_guest_address_add_value"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_address_add_value),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_trap"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_trap),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_fp_binary"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_fp_binary),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_fp_unary"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_fp_unary),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_fp_compare"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_fp_compare),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_fp_convert"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_fp_convert),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_fp_round"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_fp_round),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_vector_extract"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_vector_extract),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_vector_insert"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_vector_insert),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_vector_broadcast"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_vector_broadcast),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_vector_binary"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_vector_binary),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_vector_compare"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_vector_compare),
             llvm::JITSymbolFlags::Exported)},
        {(*jit)->mangleAndIntern("switchrecomp_runtime_vector_shuffle"),
         llvm::orc::ExecutorSymbolDef(
             llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_vector_shuffle),
             llvm::JITSymbolFlags::Exported)},
    }));
    if (define)
    {
        return Result<runtime::ExecutionResult>::failure(
            expected_error(ErrorCode::JitCompilationFailed, "failed to define runtime helpers: ",
                           std::move(define)));
    }
    auto added = (*jit)->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(module.value()), std::move(context)));
    if (added)
    {
        return Result<runtime::ExecutionResult>::failure(
            expected_error(ErrorCode::JitCompilationFailed, "failed to add LLVM module: ", std::move(added)));
    }
    auto symbol = (*jit)->lookup(function.name());
    if (!symbol)
    {
        return Result<runtime::ExecutionResult>::failure(
            expected_error(ErrorCode::JitCompilationFailed, "failed to lookup generated function: ", symbol.takeError()));
    }
    using GeneratedFunction = std::uint32_t (*)(runtime::CpuState*, runtime::RuntimeContext*);
    const auto address = symbol->getValue();
    auto generated = reinterpret_cast<GeneratedFunction>(static_cast<std::uintptr_t>(address));
    runtime.clear_error();
    const auto status = generated(&cpu, &runtime);
    if (status != 0U)
    {
        if (runtime.has_error)
        {
            return Result<runtime::ExecutionResult>::failure(runtime.last_error);
        }
        return Result<runtime::ExecutionResult>::failure(
            make_error(ErrorCode::JitCompilationFailed, "generated function returned an unknown status"));
    }
    runtime::ExecutionResult result;
    result.final_guest_pc = cpu.pc;
    return Result<runtime::ExecutionResult>::success(result);
}

} // namespace switchrecomp::codegen

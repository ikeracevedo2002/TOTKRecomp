#include "switchrecomp/codegen/llvm_backend.hpp"

#include "switchrecomp/ir/verifier.hpp"

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
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace switchrecomp::codegen
{
namespace
{
using namespace llvm;

[[nodiscard]] bool is_m9_opcode(ir::Opcode opcode) noexcept
{
    switch (opcode)
    {
    case ir::Opcode::AtomicLoad: case ir::Opcode::AtomicStore:
    case ir::Opcode::ExclusiveLoad: case ir::Opcode::ExclusiveStore:
    case ir::Opcode::ClearExclusive: case ir::Opcode::MemoryBarrier:
    case ir::Opcode::ReadSystemRegister: case ir::Opcode::WriteSystemRegister:
        return true;
    default: return false;
    }
}

[[nodiscard]] bool contains_m9(const ir::Function& function) noexcept
{
    for (const auto& block : function.blocks())
        for (const auto& instruction : block.instructions)
            if (is_m9_opcode(instruction.opcode)) return true;
    return false;
}

[[nodiscard]] switchrecomp::Error llvm_error(ErrorCode code, const char* prefix, llvm::Error error)
{
    return make_error(code, std::string(prefix) + llvm::toString(std::move(error)));
}

class M9Lowerer
{
  public:
    M9Lowerer(LLVMContext& context, const ir::Function& function)
        : context_(context), function_(function), module_(std::make_unique<Module>("switchrecomp.m9", context)),
          builder_(context)
    {
        cpu_type_ = StructType::create(context_, "switchrecomp.CpuState");
        auto* x_type = ArrayType::get(Type::getInt64Ty(context_), 31U);
        auto* flags_type = ArrayType::get(Type::getInt8Ty(context_), 4U);
        auto* vector_pair = StructType::get(context_, {Type::getInt64Ty(context_), Type::getInt64Ty(context_)});
        auto* vector_array = ArrayType::get(vector_pair, 32U);
        cpu_type_->setBody({x_type, Type::getInt64Ty(context_), Type::getInt64Ty(context_), flags_type,
                            Type::getInt32Ty(context_), Type::getInt32Ty(context_), vector_array,
                            Type::getInt64Ty(context_), Type::getInt64Ty(context_)});
    }

    [[nodiscard]] Result<std::unique_ptr<Module>> run()
    {
        const auto verified = ir::verify(function_);
        if (!verified) return Result<std::unique_ptr<Module>>::failure(verified.error());

        llvm_function_ = Function::Create(
            FunctionType::get(Type::getInt32Ty(context_),
                {PointerType::getUnqual(cpu_type_), PointerType::getUnqual(Type::getInt8Ty(context_))}, false),
            Function::ExternalLinkage, function_.name(), module_.get());
        cpu_ = llvm_function_->getArg(0);
        runtime_ = llvm_function_->getArg(1);
        values_.resize(function_.values().size(), nullptr);
        blocks_.reserve(function_.blocks().size());
        for (const auto& block : function_.blocks())
            blocks_.push_back(BasicBlock::Create(context_, "block_" + std::to_string(block.id), llvm_function_));
        auto* entry = BasicBlock::Create(context_, "entry", llvm_function_, blocks_.empty() ? nullptr : blocks_.front());
        builder_.SetInsertPoint(entry);
        builder_.CreateBr(blocks_[function_.entry_block()]);

        for (const auto& block : function_.blocks())
        {
            builder_.SetInsertPoint(blocks_[block.id]);
            for (const auto& instruction : block.instructions)
            {
                const auto lowered = lower(instruction);
                if (!lowered) return Result<std::unique_ptr<Module>>::failure(lowered.error());
            }
            const auto term = lower_terminator(block.terminator);
            if (!term) return Result<std::unique_ptr<Module>>::failure(term.error());
        }

        std::string verification;
        raw_string_ostream stream(verification);
        if (verifyFunction(*llvm_function_, &stream) || verifyModule(*module_, &stream))
        {
            stream.flush();
            return Result<std::unique_ptr<Module>>::failure(
                make_error(ErrorCode::LlvmVerificationFailed, "Milestone 9 LLVM verification failed: " + verification));
        }
        return Result<std::unique_ptr<Module>>::success(std::move(module_));
    }

  private:
    [[nodiscard]] IntegerType* int_type(ir::Type type) const
    {
        switch (type.kind())
        {
        case ir::TypeKind::I1: return Type::getInt1Ty(context_);
        case ir::TypeKind::I8: return Type::getInt8Ty(context_);
        case ir::TypeKind::I16: return Type::getInt16Ty(context_);
        case ir::TypeKind::I32: return Type::getInt32Ty(context_);
        case ir::TypeKind::I64: return Type::getInt64Ty(context_);
        default: return nullptr;
        }
    }

    [[nodiscard]] Value* value(ir::ValueId id) const noexcept
    {
        return id < values_.size() ? values_[id] : nullptr;
    }

    [[nodiscard]] Value* reg_pointer(const ir::GuestRegister& reg)
    {
        if (reg.is_stack_pointer) return builder_.CreateStructGEP(cpu_type_, cpu_, 1U, "cpu.sp");
        return builder_.CreateGEP(cpu_type_, cpu_,
            {builder_.getInt32(0), builder_.getInt32(0), builder_.getInt32(reg.index)}, "cpu.x");
    }

    [[nodiscard]] Value* pc_pointer() { return builder_.CreateStructGEP(cpu_type_, cpu_, 2U, "cpu.pc"); }

    void set_result(const ir::Instruction& instruction, Value* result)
    {
        if (instruction.result != ir::invalid_value && instruction.result < values_.size())
            values_[instruction.result] = result;
    }

    [[nodiscard]] FunctionCallee helper(StringRef name, Type* result, ArrayRef<Type*> args)
    {
        return module_->getOrInsertFunction(name, FunctionType::get(result, args, false));
    }

    void guard(Value* status)
    {
        auto* ok = BasicBlock::Create(context_, "runtime.ok", llvm_function_);
        auto* fail = BasicBlock::Create(context_, "runtime.fail", llvm_function_);
        builder_.CreateCondBr(builder_.CreateICmpEQ(status, builder_.getInt32(0)), ok, fail);
        builder_.SetInsertPoint(fail);
        builder_.CreateRet(builder_.getInt32(1));
        builder_.SetInsertPoint(ok);
    }

    [[nodiscard]] Value* widen_to_i64(Value* input)
    {
        if (input->getType()->isIntegerTy(64)) return input;
        return builder_.CreateZExt(input, Type::getInt64Ty(context_));
    }

    [[nodiscard]] Value* narrow_from_i64(Value* input, ir::Type type)
    {
        auto* target = int_type(type);
        return target->isIntegerTy(64) ? input : builder_.CreateTrunc(input, target);
    }

    [[nodiscard]] Result<void> lower(const ir::Instruction& instruction)
    {
        const auto operand = [&](std::size_t index) -> Result<Value*> {
            if (index >= instruction.operands.size() || value(instruction.operands[index]) == nullptr)
                return Result<Value*>::failure(make_error(ErrorCode::InvalidIrOperand,
                    "Milestone 9 LLVM lowering encountered an unavailable operand"));
            return Result<Value*>::success(value(instruction.operands[index]));
        };

        switch (instruction.opcode)
        {
        case ir::Opcode::Constant:
            if (auto* type = int_type(instruction.result_type))
                set_result(instruction, ConstantInt::get(type, instruction.constant));
            else return Result<void>::failure(make_error(ErrorCode::UnsupportedInstruction,
                "Milestone 9 LLVM path supports integer constants only"));
            return Result<void>::success();
        case ir::Opcode::Nop: return Result<void>::success();
        case ir::Opcode::SetPc:
            builder_.CreateStore(builder_.getInt64(instruction.source.guest_pc), pc_pointer());
            return Result<void>::success();
        case ir::Opcode::ReadRegister:
        {
            if (instruction.reg.is_zero)
            {
                set_result(instruction, ConstantInt::get(int_type(instruction.result_type), 0U));
                return Result<void>::success();
            }
            auto* loaded = builder_.CreateLoad(Type::getInt64Ty(context_), reg_pointer(instruction.reg));
            set_result(instruction, instruction.result_type == ir::i32_type()
                ? builder_.CreateTrunc(loaded, Type::getInt32Ty(context_)) : loaded);
            return Result<void>::success();
        }
        case ir::Opcode::WriteRegister:
        {
            const auto input = operand(0U); if (!input) return Result<void>::failure(input.error());
            if (instruction.reg.is_zero) return Result<void>::success();
            builder_.CreateStore(widen_to_i64(input.value()), reg_pointer(instruction.reg));
            return Result<void>::success();
        }
        case ir::Opcode::Add: case ir::Opcode::Sub: case ir::Opcode::Mul:
        case ir::Opcode::And: case ir::Opcode::Or: case ir::Opcode::Xor:
        {
            const auto left = operand(0U), right = operand(1U);
            if (!left || !right) return Result<void>::failure(!left ? left.error() : right.error());
            Value* result = nullptr;
            switch (instruction.opcode)
            {
            case ir::Opcode::Add: result = builder_.CreateAdd(left.value(), right.value()); break;
            case ir::Opcode::Sub: result = builder_.CreateSub(left.value(), right.value()); break;
            case ir::Opcode::Mul: result = builder_.CreateMul(left.value(), right.value()); break;
            case ir::Opcode::And: result = builder_.CreateAnd(left.value(), right.value()); break;
            case ir::Opcode::Or: result = builder_.CreateOr(left.value(), right.value()); break;
            default: result = builder_.CreateXor(left.value(), right.value()); break;
            }
            set_result(instruction, result); return Result<void>::success();
        }
        case ir::Opcode::Truncate: case ir::Opcode::ZeroExtend: case ir::Opcode::SignExtend:
        {
            const auto input = operand(0U); if (!input) return Result<void>::failure(input.error());
            auto* target = int_type(instruction.result_type);
            Value* result = instruction.opcode == ir::Opcode::Truncate ? builder_.CreateTrunc(input.value(), target)
                : instruction.opcode == ir::Opcode::ZeroExtend ? builder_.CreateZExt(input.value(), target)
                : builder_.CreateSExt(input.value(), target);
            set_result(instruction, result); return Result<void>::success();
        }
        case ir::Opcode::CompareEqual: case ir::Opcode::CompareNotEqual:
        case ir::Opcode::CompareUnsigned: case ir::Opcode::CompareSigned:
        {
            const auto left = operand(0U), right = operand(1U);
            if (!left || !right) return Result<void>::failure(!left ? left.error() : right.error());
            auto pred = CmpInst::ICMP_EQ;
            if (instruction.opcode == ir::Opcode::CompareNotEqual) pred = CmpInst::ICMP_NE;
            else if (instruction.opcode == ir::Opcode::CompareUnsigned) pred = CmpInst::ICMP_ULT;
            else if (instruction.opcode == ir::Opcode::CompareSigned) pred = CmpInst::ICMP_SLT;
            set_result(instruction, builder_.CreateICmp(pred, left.value(), right.value()));
            return Result<void>::success();
        }
        case ir::Opcode::Select:
        {
            const auto cond = operand(0U), yes = operand(1U), no = operand(2U);
            if (!cond || !yes || !no) return Result<void>::failure(!cond ? cond.error() : !yes ? yes.error() : no.error());
            set_result(instruction, builder_.CreateSelect(cond.value(), yes.value(), no.value()));
            return Result<void>::success();
        }
        case ir::Opcode::GuestAddressAdd:
        {
            const auto base = operand(0U); if (!base) return Result<void>::failure(base.error());
            auto callee = helper("switchrecomp_runtime_guest_address_add", Type::getInt32Ty(context_),
                {runtime_->getType(), Type::getInt64Ty(context_), Type::getInt64Ty(context_),
                 PointerType::getUnqual(Type::getInt64Ty(context_))});
            auto* output = builder_.CreateAlloca(Type::getInt64Ty(context_));
            auto* status = builder_.CreateCall(callee, {runtime_, base.value(), builder_.getInt64(
                static_cast<std::uint64_t>(instruction.immediate)), output});
            guard(status); set_result(instruction, builder_.CreateLoad(Type::getInt64Ty(context_), output));
            return Result<void>::success();
        }
        case ir::Opcode::GuestAddressAddValue:
        {
            const auto base = operand(0U), offset = operand(1U);
            if (!base || !offset) return Result<void>::failure(!base ? base.error() : offset.error());
            auto callee = helper("switchrecomp_runtime_guest_address_add_value", Type::getInt32Ty(context_),
                {runtime_->getType(), Type::getInt64Ty(context_), Type::getInt64Ty(context_),
                 Type::getInt8Ty(context_), PointerType::getUnqual(Type::getInt64Ty(context_))});
            auto* output = builder_.CreateAlloca(Type::getInt64Ty(context_));
            auto* status = builder_.CreateCall(callee, {runtime_, base.value(), widen_to_i64(offset.value()),
                builder_.getInt8(instruction.address_offset_signed ? 1U : 0U), output});
            guard(status); set_result(instruction, builder_.CreateLoad(Type::getInt64Ty(context_), output));
            return Result<void>::success();
        }
        case ir::Opcode::GuestLoad: case ir::Opcode::AtomicLoad: case ir::Opcode::ExclusiveLoad:
        {
            const auto address = operand(0U); if (!address) return Result<void>::failure(address.error());
            StringRef name = instruction.opcode == ir::Opcode::GuestLoad ? "switchrecomp_runtime_guest_load"
                : instruction.opcode == ir::Opcode::AtomicLoad ? "switchrecomp_runtime_atomic_load"
                : "switchrecomp_runtime_exclusive_load";
            std::vector<Type*> args{runtime_->getType(), Type::getInt64Ty(context_), Type::getInt8Ty(context_)};
            if (instruction.opcode != ir::Opcode::GuestLoad) args.push_back(Type::getInt8Ty(context_));
            args.push_back(PointerType::getUnqual(Type::getInt64Ty(context_)));
            auto callee = helper(name, Type::getInt32Ty(context_), args);
            auto* output = builder_.CreateAlloca(Type::getInt64Ty(context_));
            std::vector<Value*> call_args{runtime_, address.value(), builder_.getInt8(instruction.memory_size)};
            if (instruction.opcode != ir::Opcode::GuestLoad)
                call_args.push_back(builder_.getInt8(static_cast<std::uint8_t>(instruction.memory_order)));
            call_args.push_back(output);
            guard(builder_.CreateCall(callee, call_args));
            set_result(instruction, narrow_from_i64(builder_.CreateLoad(Type::getInt64Ty(context_), output),
                                                     instruction.result_type));
            return Result<void>::success();
        }
        case ir::Opcode::GuestStore: case ir::Opcode::AtomicStore:
        {
            const auto address = operand(0U), stored = operand(1U);
            if (!address || !stored) return Result<void>::failure(!address ? address.error() : stored.error());
            StringRef name = instruction.opcode == ir::Opcode::GuestStore ? "switchrecomp_runtime_guest_store"
                                                                         : "switchrecomp_runtime_atomic_store";
            std::vector<Type*> args{runtime_->getType(), Type::getInt64Ty(context_), Type::getInt8Ty(context_),
                                    Type::getInt64Ty(context_)};
            if (instruction.opcode == ir::Opcode::AtomicStore) args.push_back(Type::getInt8Ty(context_));
            auto callee = helper(name, Type::getInt32Ty(context_), args);
            std::vector<Value*> call_args{runtime_, address.value(), builder_.getInt8(instruction.memory_size),
                                          widen_to_i64(stored.value())};
            if (instruction.opcode == ir::Opcode::AtomicStore)
                call_args.push_back(builder_.getInt8(static_cast<std::uint8_t>(instruction.memory_order)));
            guard(builder_.CreateCall(callee, call_args)); return Result<void>::success();
        }
        case ir::Opcode::ExclusiveStore:
        {
            const auto address = operand(0U), stored = operand(1U);
            if (!address || !stored) return Result<void>::failure(!address ? address.error() : stored.error());
            auto callee = helper("switchrecomp_runtime_exclusive_store", Type::getInt32Ty(context_),
                {runtime_->getType(), Type::getInt64Ty(context_), Type::getInt8Ty(context_),
                 Type::getInt64Ty(context_), Type::getInt8Ty(context_),
                 PointerType::getUnqual(Type::getInt32Ty(context_))});
            auto* output = builder_.CreateAlloca(Type::getInt32Ty(context_));
            guard(builder_.CreateCall(callee, {runtime_, address.value(), builder_.getInt8(instruction.memory_size),
                widen_to_i64(stored.value()), builder_.getInt8(static_cast<std::uint8_t>(instruction.memory_order)), output}));
            set_result(instruction, builder_.CreateLoad(Type::getInt32Ty(context_), output));
            return Result<void>::success();
        }
        case ir::Opcode::ClearExclusive:
        {
            auto callee = helper("switchrecomp_runtime_clear_exclusive", Type::getInt32Ty(context_), {runtime_->getType()});
            guard(builder_.CreateCall(callee, {runtime_})); return Result<void>::success();
        }
        case ir::Opcode::MemoryBarrier:
        {
            auto callee = helper("switchrecomp_runtime_memory_barrier", Type::getInt32Ty(context_),
                {runtime_->getType(), Type::getInt8Ty(context_), Type::getInt8Ty(context_)});
            guard(builder_.CreateCall(callee, {runtime_, builder_.getInt8(static_cast<std::uint8_t>(instruction.barrier_kind)),
                builder_.getInt8(static_cast<std::uint8_t>(instruction.barrier_option))}));
            return Result<void>::success();
        }
        case ir::Opcode::ReadSystemRegister:
        {
            auto callee = helper("switchrecomp_runtime_read_system_register", Type::getInt32Ty(context_),
                {runtime_->getType(), Type::getInt8Ty(context_), PointerType::getUnqual(Type::getInt64Ty(context_))});
            auto* output = builder_.CreateAlloca(Type::getInt64Ty(context_));
            guard(builder_.CreateCall(callee, {runtime_, builder_.getInt8(static_cast<std::uint8_t>(instruction.system_register)), output}));
            set_result(instruction, builder_.CreateLoad(Type::getInt64Ty(context_), output));
            return Result<void>::success();
        }
        case ir::Opcode::WriteSystemRegister:
        {
            const auto input = operand(0U); if (!input) return Result<void>::failure(input.error());
            auto callee = helper("switchrecomp_runtime_write_system_register", Type::getInt32Ty(context_),
                {runtime_->getType(), Type::getInt8Ty(context_), Type::getInt64Ty(context_)});
            guard(builder_.CreateCall(callee, {runtime_, builder_.getInt8(static_cast<std::uint8_t>(instruction.system_register)),
                widen_to_i64(input.value())})); return Result<void>::success();
        }
        default:
            return Result<void>::failure(make_error(ErrorCode::UnsupportedInstruction,
                "Milestone 9 LLVM path encountered an unsupported legacy operation mixed with concurrency IR"));
        }
    }

    [[nodiscard]] Result<void> lower_terminator(const ir::Terminator& terminator)
    {
        switch (terminator.kind)
        {
        case ir::TerminatorKind::Branch:
            builder_.CreateBr(blocks_[terminator.target]); return Result<void>::success();
        case ir::TerminatorKind::ConditionalBranch:
        {
            auto* condition = value(terminator.condition);
            if (condition == nullptr) return Result<void>::failure(make_error(ErrorCode::InvalidIrValue,
                "conditional branch references an unavailable value"));
            if (!condition->getType()->isIntegerTy(1))
                condition = builder_.CreateICmpNE(condition, ConstantInt::get(condition->getType(), 0U));
            builder_.CreateCondBr(condition, blocks_[terminator.target], blocks_[terminator.false_target]);
            return Result<void>::success();
        }
        case ir::TerminatorKind::Return:
            if (terminator.target_value != ir::invalid_value && value(terminator.target_value) != nullptr)
                builder_.CreateStore(widen_to_i64(value(terminator.target_value)), pc_pointer());
            builder_.CreateRet(builder_.getInt32(0)); return Result<void>::success();
        case ir::TerminatorKind::DirectCall:
        case ir::TerminatorKind::IndirectBranch:
        case ir::TerminatorKind::IndirectCall:
            if (terminator.target_value == ir::invalid_value || value(terminator.target_value) == nullptr)
                return Result<void>::failure(make_error(ErrorCode::InvalidIrValue,
                    "indirect/direct-call terminator lacks a target value"));
            builder_.CreateStore(widen_to_i64(value(terminator.target_value)), pc_pointer());
            builder_.CreateRet(builder_.getInt32(0)); return Result<void>::success();
        case ir::TerminatorKind::Trap:
        {
            auto* text = builder_.CreateGlobalStringPtr(terminator.trap_reason);
            auto callee = helper("switchrecomp_runtime_trap", Type::getInt32Ty(context_),
                {runtime_->getType(), PointerType::getUnqual(Type::getInt8Ty(context_))});
            builder_.CreateCall(callee, {runtime_, text}); builder_.CreateRet(builder_.getInt32(1));
            return Result<void>::success();
        }
        }
        return Result<void>::failure(make_error(ErrorCode::InvalidControlFlow, "unknown LLVM terminator"));
    }

    LLVMContext& context_;
    const ir::Function& function_;
    std::unique_ptr<Module> module_;
    IRBuilder<> builder_;
    StructType* cpu_type_ = nullptr;
    Function* llvm_function_ = nullptr;
    Value* cpu_ = nullptr;
    Value* runtime_ = nullptr;
    std::vector<Value*> values_;
    std::vector<BasicBlock*> blocks_;
};

[[nodiscard]] Result<std::unique_ptr<Module>> lower_m9(LLVMContext& context, const ir::Function& function)
{
    try { return M9Lowerer(context, function).run(); }
    catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<Module>>::failure(make_error(ErrorCode::ResourceLimit, "unable to allocate Milestone 9 LLVM IR"));
    }
}

[[nodiscard]] Result<void> define_m9_symbols(llvm::orc::LLJIT& jit)
{
    auto& dylib = jit.getMainJITDylib();
    auto definition = dylib.define(llvm::orc::absoluteSymbols({
        {jit.mangleAndIntern("switchrecomp_runtime_guest_load"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_load), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_guest_store"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_store), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_guest_address_add"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_address_add), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_guest_address_add_value"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_guest_address_add_value), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_atomic_load"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_atomic_load), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_atomic_store"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_atomic_store), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_exclusive_load"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_exclusive_load), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_exclusive_store"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_exclusive_store), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_clear_exclusive"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_clear_exclusive), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_memory_barrier"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_memory_barrier), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_read_system_register"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_read_system_register), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_write_system_register"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_write_system_register), llvm::JITSymbolFlags::Exported)},
        {jit.mangleAndIntern("switchrecomp_runtime_trap"), llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&runtime::switchrecomp_runtime_trap), llvm::JITSymbolFlags::Exported)},
    }));
    if (definition) return Result<void>::failure(llvm_error(ErrorCode::JitCompilationFailed,
        "failed to define Milestone 9 runtime helpers: ", std::move(definition)));
    return Result<void>::success();
}
} // namespace

Result<std::string> LlvmBackend::lower_to_llvm_ir(const ir::Function& function) const
{
    if (!contains_m9(function)) return lower_to_llvm_ir_legacy(function);
    auto context = std::make_unique<LLVMContext>();
    auto module = lower_m9(*context, function);
    if (!module) return Result<std::string>::failure(module.error());
    std::string text;
    raw_string_ostream stream(text);
    module.value()->print(stream, nullptr);
    stream.flush();
    return Result<std::string>::success(std::move(text));
}

Result<runtime::ExecutionResult> LlvmBackend::execute(const ir::Function& function,
                                                      runtime::CpuState& cpu,
                                                      runtime::RuntimeContext& runtime_context,
                                                      const runtime::ExecutionOptions& options) const
{
    if (!contains_m9(function)) return execute_legacy(function, cpu, runtime_context, options);
    const auto verified = ir::verify(function);
    if (!verified) return Result<runtime::ExecutionResult>::failure(verified.error());

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    auto context = std::make_unique<LLVMContext>();
    auto module = lower_m9(*context, function);
    if (!module) return Result<runtime::ExecutionResult>::failure(module.error());
    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit) return Result<runtime::ExecutionResult>::failure(llvm_error(ErrorCode::JitCompilationFailed,
        "failed to create Milestone 9 LLVM JIT: ", jit.takeError()));
    const auto symbols = define_m9_symbols(**jit);
    if (!symbols) return Result<runtime::ExecutionResult>::failure(symbols.error());
    auto added = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(module.value()), std::move(context)));
    if (added) return Result<runtime::ExecutionResult>::failure(llvm_error(ErrorCode::JitCompilationFailed,
        "failed to add Milestone 9 LLVM module: ", std::move(added)));
    auto symbol = (*jit)->lookup(function.name());
    if (!symbol) return Result<runtime::ExecutionResult>::failure(llvm_error(ErrorCode::JitCompilationFailed,
        "failed to lookup Milestone 9 generated function: ", symbol.takeError()));

    using GeneratedFunction = std::uint32_t (*)(runtime::CpuState*, runtime::RuntimeContext*);
    auto generated = reinterpret_cast<GeneratedFunction>(static_cast<std::uintptr_t>(symbol->getValue()));
    runtime_context.clear_error();
    runtime_context.cpu = &cpu;
    const auto status = generated(&cpu, &runtime_context);
    if (status != 0U)
        return Result<runtime::ExecutionResult>::failure(runtime_context.has_error ? runtime_context.last_error
            : make_error(ErrorCode::JitCompilationFailed, "Milestone 9 generated function returned an unknown status"));
    runtime::ExecutionResult result;
    result.final_guest_pc = cpu.pc;
    return Result<runtime::ExecutionResult>::success(result);
}

} // namespace switchrecomp::codegen

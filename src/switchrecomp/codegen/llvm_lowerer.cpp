#include "switchrecomp/codegen/llvm_lowerer.hpp"

#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/ir/verifier.hpp"

#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#if defined(SWITCHRECOMP_HAS_LLVM)
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/raw_ostream.h>
#endif

namespace switchrecomp::codegen
{

namespace
{

#if defined(SWITCHRECOMP_HAS_LLVM)
[[nodiscard]] std::string generated_name(const ir::IrFunction& function)
{
    if (!function.name.empty())
    {
        return function.name;
    }
    std::ostringstream output;
    output << "switchrecomp_guest_fn_" << std::hex << std::setw(16) << std::setfill('0')
           << function.entry;
    return output.str();
}
#endif

} // namespace

#if !defined(SWITCHRECOMP_HAS_LLVM)

Result<LlvmModule> lower_to_llvm(const ir::IrFunction& function)
{
    const auto verified = ir::verify_function(function);
    if (!verified)
    {
        return Result<LlvmModule>::failure(verified.error());
    }
    return Result<LlvmModule>::failure(make_error(
        ErrorCode::LLVMUnavailable,
        "LLVM backend is not available; configure with an installed LLVM development package"));
}

#else

namespace
{

[[nodiscard]] llvm::Type* llvm_type(llvm::LLVMContext& context, ir::IrType type)
{
    switch (type)
    {
    case ir::IrType::I1:
        return llvm::Type::getInt1Ty(context);
    case ir::IrType::I8:
        return llvm::Type::getInt8Ty(context);
    case ir::IrType::I16:
        return llvm::Type::getInt16Ty(context);
    case ir::IrType::I32:
        return llvm::Type::getInt32Ty(context);
    case ir::IrType::I64:
        return llvm::Type::getInt64Ty(context);
    }
    return nullptr;
}

class LoweringContext
{
  public:
    LoweringContext(const ir::IrFunction& source, llvm::LLVMContext& context,
                    llvm::Module& module, llvm::Function& function)
        : source_(source), context_(context), module_(module), function_(function),
          pointer_type_(llvm::Type::getInt8PtrTy(context))
    {
    }

    [[nodiscard]] Result<void> run()
    {
        prologue_ = llvm::BasicBlock::Create(context_, "prologue", &function_);
        for (const auto& block : source_.blocks)
        {
            llvm_blocks_.emplace(
                block.id,
                llvm::BasicBlock::Create(context_, block_name(block), &function_));
        }
        error_ = llvm::BasicBlock::Create(context_, "runtime_error", &function_);
        exit_ = llvm::BasicBlock::Create(context_, "completed", &function_);

        llvm::IRBuilder<> prologue_builder(prologue_);
        for (const auto& block : source_.blocks)
        {
            for (const auto& instruction : block.instructions)
            {
                if (instruction.result.valid())
                {
                    slots_.emplace(instruction.result,
                                   prologue_builder.CreateAlloca(llvm_type(context_, instruction.type),
                                                                 nullptr,
                                                                 value_name(instruction.result)));
                }
            }
        }
        const auto entry = llvm_blocks_.find(source_.entry_block);
        if (entry == llvm_blocks_.end())
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidBlockId, "LLVM lowering entry block does not exist"));
        }
        prologue_builder.CreateBr(entry->second);

        llvm::IRBuilder<> error_builder(error_);
        error_builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 1U));
        llvm::IRBuilder<> exit_builder(exit_);
        exit_builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0U));

        for (const auto& block : source_.blocks)
        {
            const auto found = llvm_blocks_.find(block.id);
            if (found == llvm_blocks_.end())
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidBlockId, "LLVM lowering block does not exist"));
            }
            llvm::IRBuilder<> builder(found->second);
            for (const auto& instruction : block.instructions)
            {
                const auto lowered = lower_instruction(builder, instruction);
                if (!lowered)
                {
                    return lowered;
                }
            }
        }
        return Result<void>::success();
    }

  private:
    [[nodiscard]] static std::string block_name(const ir::IrBasicBlock& block)
    {
        std::ostringstream output;
        output << "bb_" << std::hex << std::setw(16) << std::setfill('0') << block.guest_start;
        return output.str();
    }

    [[nodiscard]] static std::string value_name(ir::ValueId id)
    {
        return "value_" + std::to_string(id.value);
    }

    [[nodiscard]] llvm::Value* load_value(llvm::IRBuilder<>& builder, ir::ValueId id)
    {
        const auto found = slots_.find(id);
        return found == slots_.end()
                   ? nullptr
                   : builder.CreateLoad(found->second->getAllocatedType(), found->second,
                                        value_name(id) + "_load");
    }

    [[nodiscard]] Result<void> store_result(llvm::IRBuilder<>& builder, ir::ValueId id,
                                            llvm::Value* value)
    {
        const auto found = slots_.find(id);
        if (found == slots_.end() || value == nullptr)
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidValueId, "LLVM lowering encountered an unknown value"));
        }
        builder.CreateStore(value, found->second);
        return Result<void>::success();
    }

    [[nodiscard]] llvm::Value* coerce(llvm::IRBuilder<>& builder, llvm::Value* value,
                                      llvm::Type* type)
    {
        if (value == nullptr || value->getType() == type)
        {
            return value;
        }
        const auto from = value->getType()->getIntegerBitWidth();
        const auto to = type->getIntegerBitWidth();
        return from < to ? builder.CreateZExt(value, type) : builder.CreateTrunc(value, type);
    }

    [[nodiscard]] llvm::FunctionCallee runtime_function(llvm::StringRef name,
                                                         llvm::FunctionType* type)
    {
        return module_.getOrInsertFunction(name, type);
    }

    void check_runtime_status(llvm::IRBuilder<>& builder, llvm::Value* status)
    {
        auto* continuation = llvm::BasicBlock::Create(
            context_, "runtime_ok_" + std::to_string(runtime_block_counter_++), &function_);
        builder.CreateCondBr(status, error_, continuation);
        builder.SetInsertPoint(continuation);
    }

    [[nodiscard]] llvm::Value* context_argument() const { return function_.getArg(0); }

    [[nodiscard]] Result<void> lower_instruction(llvm::IRBuilder<>& builder,
                                                  const ir::IrInstruction& instruction)
    {
        const auto integer64 = llvm::Type::getInt64Ty(context_);
        const auto integer32 = llvm::Type::getInt32Ty(context_);
        const auto integer8 = llvm::Type::getInt8Ty(context_);
        switch (instruction.opcode)
        {
        case ir::IrOpcode::Constant:
        {
            auto* value = llvm::ConstantInt::get(llvm_type(context_, instruction.type),
                                                 ir::mask_value(instruction.type,
                                                                instruction.immediate));
            return store_result(builder, instruction.result, value);
        }
        case ir::IrOpcode::ReadRegister:
        {
            const auto type = llvm::FunctionType::get(integer64,
                                                      {pointer_type_, integer8, integer8,
                                                       integer8, integer8},
                                                      false);
            const auto callee = runtime_function("switchrecomp_read_register", type);
            const auto width = instruction.reg->width == aarch64::RegisterWidth::W32 ? 32U : 64U;
            auto* value = builder.CreateCall(
                callee, {context_argument(), llvm::ConstantInt::get(integer8, instruction.reg->index),
                         llvm::ConstantInt::get(integer8, width),
                         llvm::ConstantInt::get(integer8, instruction.reg->is_stack_pointer ? 1U : 0U),
                         llvm::ConstantInt::get(integer8, instruction.reg->is_zero ? 1U : 0U)},
                value_name(instruction.result));
            return store_result(builder, instruction.result,
                                coerce(builder, value, llvm_type(context_, instruction.type)));
        }
        case ir::IrOpcode::WriteRegister:
        {
            const auto type = llvm::FunctionType::get(integer32,
                                                      {pointer_type_, integer8, integer8,
                                                       integer8, integer8, integer64},
                                                      false);
            const auto callee = runtime_function("switchrecomp_write_register", type);
            auto* value = coerce(builder, load_value(builder, instruction.operands[0]), integer64);
            if (value == nullptr)
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidValueId, "write_reg uses an unknown value"));
            }
            auto* status = builder.CreateCall(
                callee, {context_argument(), llvm::ConstantInt::get(integer8, instruction.reg->index),
                         llvm::ConstantInt::get(integer8,
                                                instruction.reg->width == aarch64::RegisterWidth::W32 ? 32U : 64U),
                         llvm::ConstantInt::get(integer8, instruction.reg->is_stack_pointer ? 1U : 0U),
                         llvm::ConstantInt::get(integer8, instruction.reg->is_zero ? 1U : 0U), value});
            check_runtime_status(builder, status);
            return Result<void>::success();
        }
        case ir::IrOpcode::Add:
        case ir::IrOpcode::Sub:
        case ir::IrOpcode::And:
        case ir::IrOpcode::Or:
        case ir::IrOpcode::Xor:
        {
            auto* left = load_value(builder, instruction.operands[0]);
            auto* right = load_value(builder, instruction.operands[1]);
            if (left == nullptr || right == nullptr)
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidValueId, "binary operation uses an unknown value"));
            }
            llvm::Value* result = nullptr;
            if (instruction.opcode == ir::IrOpcode::Add)
                result = builder.CreateAdd(left, right);
            else if (instruction.opcode == ir::IrOpcode::Sub)
                result = builder.CreateSub(left, right);
            else if (instruction.opcode == ir::IrOpcode::And)
                result = builder.CreateAnd(left, right);
            else if (instruction.opcode == ir::IrOpcode::Or)
                result = builder.CreateOr(left, right);
            else
                result = builder.CreateXor(left, right);
            return store_result(builder, instruction.result, result);
        }
        case ir::IrOpcode::CompareEqual:
        case ir::IrOpcode::CompareNotEqual:
        {
            auto* left = load_value(builder, instruction.operands[0]);
            auto* right = load_value(builder, instruction.operands[1]);
            if (left == nullptr || right == nullptr)
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidValueId, "comparison uses an unknown value"));
            }
            const auto predicate = instruction.opcode == ir::IrOpcode::CompareEqual
                                       ? llvm::CmpInst::ICMP_EQ
                                       : llvm::CmpInst::ICMP_NE;
            return store_result(builder, instruction.result, builder.CreateICmp(predicate, left, right));
        }
        case ir::IrOpcode::GuestLoad:
        {
            auto* address = load_value(builder, instruction.operands[0]);
            if (address == nullptr)
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidValueId, "guest_load uses an unknown address"));
            }
            auto* output = slots_.at(instruction.result);
            const auto output_type = instruction.access_size == 4U ? integer32 : integer64;
            const auto function_type = llvm::FunctionType::get(
                integer32, {pointer_type_, integer64, output_type->getPointerTo()}, false);
            const auto callee = runtime_function(
                instruction.access_size == 4U ? "switchrecomp_guest_read_u32"
                                              : "switchrecomp_guest_read_u64",
                function_type);
            auto* status = builder.CreateCall(callee, {context_argument(), address, output});
            check_runtime_status(builder, status);
            return Result<void>::success();
        }
        case ir::IrOpcode::GuestStore:
        {
            auto* address = load_value(builder, instruction.operands[0]);
            auto* value = load_value(builder, instruction.operands[1]);
            if (address == nullptr || value == nullptr)
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidValueId, "guest_store uses an unknown value"));
            }
            const auto output_type = instruction.access_size == 4U ? integer32 : integer64;
            const auto function_type = llvm::FunctionType::get(
                integer32, {pointer_type_, integer64, output_type}, false);
            const auto callee = runtime_function(
                instruction.access_size == 4U ? "switchrecomp_guest_write_u32"
                                              : "switchrecomp_guest_write_u64",
                function_type);
            auto* status = builder.CreateCall(callee, {context_argument(), address,
                                                       coerce(builder, value, output_type)});
            check_runtime_status(builder, status);
            return Result<void>::success();
        }
        case ir::IrOpcode::Branch:
            builder.CreateBr(llvm_blocks_.at(instruction.target.value()));
            return Result<void>::success();
        case ir::IrOpcode::ConditionalBranch:
        {
            auto* condition = load_value(builder, instruction.operands[0]);
            if (condition == nullptr)
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidValueId, "conditional branch uses an unknown value"));
            }
            builder.CreateCondBr(condition, llvm_blocks_.at(instruction.true_target.value()),
                                 llvm_blocks_.at(instruction.false_target.value()));
            return Result<void>::success();
        }
        case ir::IrOpcode::Return:
        {
            const auto type = llvm::FunctionType::get(integer32, {pointer_type_, integer64}, false);
            const auto callee = runtime_function("switchrecomp_set_guest_pc", type);
            auto* status = builder.CreateCall(callee, {context_argument(),
                                                       llvm::ConstantInt::get(integer64,
                                                                              instruction.source.guest_pc)});
            check_runtime_status(builder, status);
            builder.CreateBr(exit_);
            return Result<void>::success();
        }
        case ir::IrOpcode::Nop:
            return Result<void>::success();
        }
        return Result<void>::failure(
            make_error(ErrorCode::Unsupported, "LLVM lowering encountered an unknown IR opcode"));
    }

    const ir::IrFunction& source_;
    llvm::LLVMContext& context_;
    llvm::Module& module_;
    llvm::Function& function_;
    llvm::Type* pointer_type_;
    llvm::BasicBlock* prologue_ = nullptr;
    llvm::BasicBlock* error_ = nullptr;
    llvm::BasicBlock* exit_ = nullptr;
    std::map<ir::ValueId, llvm::AllocaInst*> slots_;
    std::map<ir::BlockId, llvm::BasicBlock*> llvm_blocks_;
    std::size_t runtime_block_counter_ = 0U;
};

} // namespace

Result<LlvmModule> lower_to_llvm(const ir::IrFunction& function)
{
    const auto verified = ir::verify_function(function);
    if (!verified)
    {
        return Result<LlvmModule>::failure(verified.error());
    }
    llvm::LLVMContext context;
    const auto name = generated_name(function);
    llvm::Module module("switchrecomp_" + name, context);
    module.setTargetTriple(llvm::sys::getDefaultTargetTriple());
    auto* pointer_type = llvm::Type::getInt8PtrTy(context);
    auto* function_type = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context), {pointer_type}, false);
    auto* generated = llvm::Function::Create(function_type, llvm::Function::ExternalLinkage, name,
                                             module);
    auto* context_argument = generated->getArg(0);
    context_argument->setName("runtime_context");
    LoweringContext lowering(function, context, module, *generated);
    const auto lowered = lowering.run();
    if (!lowered)
    {
        return Result<LlvmModule>::failure(lowered.error());
    }
    std::string diagnostics;
    llvm::raw_string_ostream diagnostic_stream(diagnostics);
    if (llvm::verifyFunction(*generated, &diagnostic_stream) ||
        llvm::verifyModule(module, &diagnostic_stream))
    {
        diagnostic_stream.flush();
        return Result<LlvmModule>::failure(make_error(
            ErrorCode::LLVMVerificationFailed,
            "LLVM verification failed for " + name + ": " + diagnostics));
    }
    diagnostic_stream.flush();
    std::string text;
    llvm::raw_string_ostream output(text);
    module.print(output, nullptr);
    output.flush();
    return Result<LlvmModule>::success(LlvmModule{
        "switchrecomp_" + name, name, std::move(text), GuestNativeMapping{function.entry, name}});
}

#endif

} // namespace switchrecomp::codegen

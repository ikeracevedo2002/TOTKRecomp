#include "switchrecomp/ir/printer.hpp"

#include <iomanip>
#include <sstream>

namespace switchrecomp::ir
{

namespace
{

void print_value(std::ostringstream& output, ValueId id)
{
    output << '%' << id;
}

void print_operands(std::ostringstream& output, const Instruction& instruction)
{
    for (std::size_t index = 0U; index < instruction.operands.size(); ++index)
    {
        if (index != 0U)
        {
            output << ", ";
        }
        print_value(output, instruction.operands[index]);
    }
}

} // namespace

std::string print(const Function& function)
{
    std::ostringstream output;
    output << "function " << function.name() << " @0x" << std::hex << std::setw(16)
           << std::setfill('0') << function.guest_entry() << std::dec << " {\n";
    for (const auto& block : function.blocks())
    {
        output << "block_" << block.id << " (0x" << std::hex << std::setw(16)
               << std::setfill('0') << block.guest_start << std::dec << "):\n";
        for (const auto& instruction : block.instructions)
        {
            output << "    ";
            if (instruction.result != invalid_value)
            {
                print_value(output, instruction.result);
                output << ':' << type_name(instruction.result_type) << " = ";
            }
            output << opcode_name(instruction.opcode);
            if (instruction.opcode == Opcode::Constant)
            {
                output << " 0x" << std::hex << instruction.constant << std::dec;
            }
            else if (instruction.opcode == Opcode::ReadRegister ||
                     instruction.opcode == Opcode::WriteRegister)
            {
                output << ' ' << register_name(instruction.reg);
                if (!instruction.operands.empty())
                {
                    output << ", ";
                    print_operands(output, instruction);
                }
            }
            else if (instruction.opcode == Opcode::ReadFlag ||
                     instruction.opcode == Opcode::WriteFlag)
            {
                output << ' ' << flag_name(instruction.flag);
                if (!instruction.operands.empty())
                {
                    output << ", ";
                    print_operands(output, instruction);
                }
            }
            else if (instruction.opcode == Opcode::EvaluateCondition)
            {
                output << ' ' << condition_code_name(instruction.condition);
                if (!instruction.operands.empty())
                {
                    output << ", ";
                    print_operands(output, instruction);
                }
            }
            else if (instruction.opcode == Opcode::GuestAddressAdd)
            {
                output << ' ';
                print_operands(output, instruction);
                output << ", " << instruction.immediate;
            }
            else if (instruction.opcode == Opcode::GuestLoad ||
                     instruction.opcode == Opcode::GuestStore)
            {
                output << ' ';
                print_operands(output, instruction);
                output << " (" << static_cast<unsigned int>(instruction.memory_size) << " bytes)";
            }
            else if (!instruction.operands.empty())
            {
                output << ' ';
                print_operands(output, instruction);
            }
            output << "\n";
        }
        output << "    ";
        switch (block.terminator.kind)
        {
        case TerminatorKind::Branch:
            output << "branch block_" << block.terminator.target;
            break;
        case TerminatorKind::ConditionalBranch:
            output << "cond_branch ";
            print_value(output, block.terminator.condition);
            output << ", block_" << block.terminator.target << ", block_"
                   << block.terminator.false_target;
            break;
        case TerminatorKind::Return:
            output << "return";
            break;
        case TerminatorKind::Trap:
            output << "trap \"" << block.terminator.trap_reason << '"';
            break;
        }
        output << "\n\n";
    }
    output << "}\n";
    return output.str();
}

} // namespace switchrecomp::ir

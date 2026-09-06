#include "switchrecomp/ir/printer.hpp"

#include "switchrecomp/aarch64/register.hpp"

#include <iomanip>
#include <sstream>

namespace switchrecomp::ir
{

namespace
{

void print_value(std::ostream& output, ValueId value)
{
    output << '%' << value.value;
}

void print_source(std::ostream& output, const IrSourceLocation& source)
{
    output << " ; guest=0x" << std::hex << std::setw(16) << std::setfill('0')
           << source.guest_pc << " opcode=0x" << std::setw(8) << source.opcode << std::dec;
}

} // namespace

std::string print_function(const IrFunction& function)
{
    std::ostringstream output;
    output << "function " << function.name << " entry=0x" << std::hex << std::setw(16)
           << std::setfill('0') << function.entry << std::dec << " {\n";
    for (const auto& block : function.blocks)
    {
        output << "bb_" << std::hex << std::setw(16) << std::setfill('0') << block.guest_start
               << std::dec << ":\n";
        for (const auto& instruction : block.instructions)
        {
            output << "    ";
            if (instruction.result.valid())
            {
                print_value(output, instruction.result);
                output << ':' << type_name(instruction.type) << " = ";
            }
            output << opcode_name(instruction.opcode);
            if (instruction.opcode == IrOpcode::Constant)
            {
                output << " 0x" << std::hex << mask_value(instruction.type, instruction.immediate)
                       << std::dec;
            }
            else if (instruction.opcode == IrOpcode::ReadRegister ||
                     instruction.opcode == IrOpcode::WriteRegister)
            {
                output << ' ' << aarch64::register_name(instruction.reg.value());
            }
            for (const auto operand : instruction.operands)
            {
                output << ' ';
                print_value(output, operand);
            }
            if (instruction.opcode == IrOpcode::GuestLoad ||
                instruction.opcode == IrOpcode::GuestStore)
            {
                output << " [size=" << static_cast<unsigned int>(instruction.access_size) << ']';
            }
            if (instruction.opcode == IrOpcode::Branch)
            {
                output << " bb" << instruction.target->value;
            }
            if (instruction.opcode == IrOpcode::ConditionalBranch)
            {
                output << " true=bb" << instruction.true_target->value << " false=bb"
                       << instruction.false_target->value;
            }
            print_source(output, instruction.source);
            output << '\n';
        }
    }
    output << "}\n";
    return output.str();
}

} // namespace switchrecomp::ir

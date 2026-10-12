// Structural checks of application SPIR-V before SPIRV-Cross parses it
// (spirv_input.h). Operand kinds come from glslang's SPIR-V tables
// (SPIRV/doc.cpp); this file includes glslang's spirv.hpp, which cannot
// share a translation unit with SPIRV-Cross's.

#include <SPIRV/doc.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wmgl::shader {

namespace {

// SPIRV-Cross allocates a table entry per id of the bound.
constexpr uint32_t kMaxBound = 1u << 22;

}  // namespace

bool spirvStructureValid(const std::vector<uint32_t>& words, std::string& error) {
    spv::Parameterize();
    if (words.size() < 5) return error = "no header", false;
    const uint32_t bound = words[3];
    if (!bound || bound > kMaxBound) return error = "id bound " + std::to_string(bound), false;
    for (size_t at = 5; at < words.size();) {
        const uint32_t count = words[at] >> 16, opcode = words[at] & 0xffffu;
        const std::string where = "instruction " + std::to_string(opcode) + " at word " + std::to_string(at);
        if (!count || count > words.size() - at) return error = where + " does not fit the module", false;
        const spv::InstructionParameters& desc = spv::InstructionDesc[opcode];
        if (std::string(spv::OpcodeString(static_cast<int>(opcode))) == "Bad")
            return error = where + " is not a known opcode", false;
        const size_t end = at + count;
        size_t w = at + 1;
        bool fits = true;
        const auto id = [&](size_t i) {
            if (i >= end) return fits = false;
            return words[i] != 0 && words[i] < bound;
        };
        if (desc.hasType() && !id(w++)) return error = where + ": result type", false;
        if (desc.hasResult() && !id(w++)) return error = where + ": result", false;
        for (int o = 0; o < desc.operands.getNum() && fits; ++o) {
            const spv::OperandClass kind = desc.operands.getClass(o);
            if (w >= end) {
                // Lists may be empty, and optional operands absent.
                const bool needed = !desc.operands.isOptional(o) && kind != spv::OperandVariableIds &&
                                    kind != spv::OperandVariableLiterals && kind != spv::OperandVariableIdLiteral &&
                                    kind != spv::OperandVariableLiteralId && kind != spv::OperandVariableLiteralStrings &&
                                    kind != spv::OperandOptionalLiteral && kind != spv::OperandOptionalLiteralString;
                if (needed) fits = false;
                break;
            }
            switch (kind) {
            case spv::OperandId:
                if (!id(w++)) return error = where + ": operand", false;
                break;
            case spv::OperandVariableIds:
                for (; w < end; ++w)
                    if (!id(w)) return error = where + ": operand", false;
                break;
            case spv::OperandVariableIdLiteral:  // (id, literal) pairs
                for (; w < end; w += 2)
                    if (!id(w)) return error = where + ": operand", false;
                w = end;
                break;
            case spv::OperandVariableLiteralId:  // (literal, id) pairs
                for (; w < end; w += 2)
                    if (!id(w + 1)) return error = where + ": operand", false;
                w = end;
                break;
            case spv::OperandLiteralString:
            case spv::OperandOptionalLiteralString:
                // Through the word with the terminating zero byte.
                while (w < end) {
                    const uint32_t chars = words[w++];
                    if (!(chars & 0xffu) || !(chars & 0xff00u) || !(chars & 0xff0000u) || !(chars & 0xff000000u)) break;
                }
                break;
            case spv::OperandVariableLiterals:
            case spv::OperandVariableLiteralStrings:
                w = end;
                break;
            default:  // a literal number or an enumerant: one word
                ++w;
                break;
            }
        }
        if (!fits) return error = where + " is too short", false;
        at = end;
    }
    return true;
}

}  // namespace wmgl::shader

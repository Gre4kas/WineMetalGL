#include "core/shader/spirv_input.h"

#include <spirv_glsl.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace wmgl::shader {
namespace {

// The tag of a stage in the names of its outputs.
const char* stageTag(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return "v";
    case Stage::TessControl: return "tc";
    case Stage::TessEval: return "te";
    case Stage::Geometry: return "g";
    case Stage::Fragment: return "f";
    default: return "c";
    }
}

spv::ExecutionModel executionModel(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return spv::ExecutionModelVertex;
    case Stage::TessControl: return spv::ExecutionModelTessellationControl;
    case Stage::TessEval: return spv::ExecutionModelTessellationEvaluation;
    case Stage::Geometry: return spv::ExecutionModelGeometry;
    case Stage::Fragment: return spv::ExecutionModelFragment;
    default: return spv::ExecutionModelGLCompute;
    }
}

// Walks the module's instructions: fn(opcode, operands, count).
void forEachInstruction(const std::vector<uint32_t>& words,
                        const std::function<void(uint32_t, const uint32_t*, uint32_t)>& fn) {
    for (size_t at = 5; at < words.size();) {
        const uint32_t count = words[at] >> 16, opcode = words[at] & 0xffffu;
        if (count == 0 || at + count > words.size()) break;
        fn(opcode, &words[at + 1], count - 1);
        at += count;
    }
}

std::string literalString(const uint32_t* operands, uint32_t count) {
    std::string out;
    for (uint32_t i = 0; i < count; ++i)
        for (int b = 0; b < 4; ++b) {
            const char c = static_cast<char>((operands[i] >> (8 * b)) & 0xffu);
            if (!c) return out;
            out += c;
        }
    return out;
}

// Names the module's unnamed resources and every inter-stage variable
// (see spirv_input.h). Needs the module's internals, hence the subclass.
class SpirvToGlsl : public spirv_cross::CompilerGLSL {
public:
    using CompilerGLSL::CompilerGLSL;

protected:
    // GL atomic counters (AtomicCounter storage), which SPIRV-Cross's GLSL
    // declares and reads as plain uints: "uniform atomic_uint" and the
    // atomicCounter* functions.
    std::string type_to_glsl(const spirv_cross::SPIRType& type, uint32_t id) override {
        const auto* var = id ? maybe_get<spirv_cross::SPIRVariable>(id) : nullptr;
        if (type.basetype == spirv_cross::SPIRType::UInt &&
            (type.storage == spv::StorageClassAtomicCounter || (var && var->storage == spv::StorageClassAtomicCounter)))
            return "atomic_uint";
        return CompilerGLSL::type_to_glsl(type, id);
    }
    // Built-ins SPIRV-Cross's GLSL lacks.
    std::string builtin_to_glsl(spv::BuiltIn builtin, spv::StorageClass storage) override {
        if (builtin == spv::BuiltInPatchVertices) return "gl_PatchVerticesIn";
        return CompilerGLSL::builtin_to_glsl(builtin, storage);
    }
    const char* to_storage_qualifiers_glsl(const spirv_cross::SPIRVariable& var) override {
        if (var.storage == spv::StorageClassAtomicCounter) return "uniform ";
        return CompilerGLSL::to_storage_qualifiers_glsl(var);
    }
    void emit_instruction(const spirv_cross::Instruction& instruction) override {
        const uint32_t* ops = stream(instruction);
        const auto op = static_cast<spv::Op>(instruction.op);
        const char* function = nullptr;
        switch (op) {
        case spv::OpAtomicLoad: function = "atomicCounter"; break;
        case spv::OpAtomicIAdd: function = "atomicCounterAdd"; break;
        case spv::OpAtomicISub: function = "atomicCounterSubtract"; break;
        case spv::OpAtomicUMin: case spv::OpAtomicSMin: function = "atomicCounterMin"; break;
        case spv::OpAtomicUMax: case spv::OpAtomicSMax: function = "atomicCounterMax"; break;
        case spv::OpAtomicAnd: function = "atomicCounterAnd"; break;
        case spv::OpAtomicOr: function = "atomicCounterOr"; break;
        case spv::OpAtomicXor: function = "atomicCounterXor"; break;
        case spv::OpAtomicExchange: function = "atomicCounterExchange"; break;
        case spv::OpAtomicCompareExchange: function = "atomicCounterCompSwap"; break;
        default: break;
        }
        if (!function || instruction.length < 3 || expression_type(ops[2]).storage != spv::StorageClassAtomicCounter)
            return CompilerGLSL::emit_instruction(instruction);
        forced_temporaries.insert(ops[1]);
        std::string call = std::string(function) + "(" + to_non_uniform_aware_expression(ops[2]);
        if (op == spv::OpAtomicCompareExchange) call += ", " + to_expression(ops[7]) + ", " + to_expression(ops[6]);
        else if (op != spv::OpAtomicLoad) call += ", " + to_expression(ops[5]);
        emit_op(ops[0], ops[1], call + ")", false);
        flush_all_atomic_capable_variables();
    }

public:

    void nameResources(Stage stage) {
        using namespace spirv_cross;
        const std::string p = kSpirvNamePrefix;
        std::set<std::string> structNames;
        ir.for_each_typed_id<SPIRVariable>([&](uint32_t id, SPIRVariable& var) {
            if (var.storage == spv::StorageClassFunction || var.storage == spv::StorageClassPrivate ||
                var.storage == spv::StorageClassWorkgroup)
                return;
            const SPIRType& type = get<SPIRType>(var.basetype);
            const SPIRType& base = get<SPIRType>(type.self);
            const bool block = has_decoration(base.self, spv::DecorationBlock) || has_decoration(base.self, spv::DecorationBufferBlock);
            const bool builtin = has_decoration(id, spv::DecorationBuiltIn) ||
                                 (block && !base.member_types.empty() && has_member_decoration(base.self, 0, spv::DecorationBuiltIn));
            if (builtin) return;
            auto location = [&]() -> std::string {
                uint32_t l = has_decoration(id, spv::DecorationLocation) ? get_decoration(id, spv::DecorationLocation)
                             : block && has_member_decoration(base.self, 0, spv::DecorationLocation)
                                 ? get_member_decoration(base.self, 0, spv::DecorationLocation)
                                 : 0xffffffffu;
                if (l == 0xffffffffu) return "v" + std::to_string(id);
                std::string out = std::to_string(l);
                if (has_decoration(id, spv::DecorationComponent))
                    out += "c" + std::to_string(get_decoration(id, spv::DecorationComponent));
                if (has_decoration(id, spv::DecorationIndex)) out += "i" + std::to_string(get_decoration(id, spv::DecorationIndex));
                return out;
            };
            auto binding = [&]() {
                return has_decoration(id, spv::DecorationBinding) ? std::to_string(get_decoration(id, spv::DecorationBinding))
                                                                 : "v" + std::to_string(id);
            };
            auto nameMembers = [&](const SPIRType& t) {
                for (uint32_t m = 0; m < t.member_types.size(); ++m)
                    if (get_member_name(t.self, m).empty()) set_member_name(t.self, m, p + "m" + std::to_string(m));
            };
            const bool input = var.storage == spv::StorageClassInput, output = var.storage == spv::StorageClassOutput;
            if (input || output) {
                // The application-facing ends keep their names; the rest match by location.
                const bool end = (input && stage == Stage::Vertex) || (output && stage == Stage::Fragment);
                const std::string kind = has_decoration(id, spv::DecorationPatch) ? "p" : "l";
                if (end) {
                    if (get_name(id).empty()) set_name(id, p + (input ? "a" : "o") + location());
                    return;
                }
                // Outputs carry their stage's tag; inputs get the previous
                // stage's at link time (spirvLinkInputs), as a stage's input
                // and output at one location need different names.
                const std::string side = input ? "in_" : std::string(stageTag(stage)) + "_";
                set_name(id, p + side + kind + location());
                if (block) {
                    set_name(base.self, p + side + "L" + kind + location());
                    for (uint32_t m = 0; m < base.member_types.size(); ++m) set_member_name(base.self, m, p + "m" + std::to_string(m));
                }
                return;
            }
            if (var.storage == spv::StorageClassUniform || var.storage == spv::StorageClassStorageBuffer) {
                const bool storage = var.storage == spv::StorageClassStorageBuffer || has_decoration(base.self, spv::DecorationBufferBlock);
                if (get_name(base.self).empty()) set_name(base.self, p + (storage ? "S" : "B") + binding());
                if (get_name(id).empty()) set_name(id, p + (storage ? "s" : "b") + binding());
                nameMembers(base);
                return;
            }
            if (var.storage == spv::StorageClassAtomicCounter) {
                if (get_name(id).empty())
                    set_name(id, p + "c" + binding() + "o" +
                                     std::to_string(has_decoration(id, spv::DecorationOffset) ? get_decoration(id, spv::DecorationOffset) : 0));
                return;
            }
            if (var.storage == spv::StorageClassUniformConstant && get_name(id).empty())
                set_name(id, p + "u" + (has_decoration(id, spv::DecorationLocation) ? location() : "b" + binding()));
        });
        // Unnamed structures: one name per layout, so stages declaring the same
        // structure agree (GLSL matches uniform structures by name).
        ir.for_each_typed_id<SPIRType>([&](uint32_t id, SPIRType& type) {
            if (type.basetype != SPIRType::Struct || type.self != id || !get_name(id).empty()) return;
            if (has_decoration(id, spv::DecorationBlock) || has_decoration(id, spv::DecorationBufferBlock)) return;
            std::string layout;
            for (uint32_t member : type.member_types) layout += type_to_glsl(get<SPIRType>(member), 0) + ";";
            std::string name = p + "T" + std::to_string(std::hash<std::string>()(layout) % 1000000007u);
            while (!structNames.insert(name).second) name += "x";
            set_name(id, name);
            for (uint32_t m = 0; m < type.member_types.size(); ++m)
                if (get_member_name(id, m).empty()) set_member_name(id, m, p + "m" + std::to_string(m));
        });
    }
};

}  // namespace

bool spirvHeaderValid(const std::vector<uint32_t>& words) {
    // Versions 1.0 to 1.6.
    return words.size() >= 5 && words[0] == 0x07230203u && words[1] >= 0x00010000u && words[1] <= 0x00010600u;
}

const std::vector<const char*>& spirvExtensions() {
    static const std::vector<const char*> extensions = {"SPV_KHR_shader_draw_parameters", "SPV_KHR_storage_buffer_storage_class",
                                                        "SPV_KHR_subgroup_vote", "SPV_KHR_no_integer_wrap_decoration"};
    return extensions;
}

SpirvSpecialization specializeSpirv(const std::vector<uint32_t>& words, Stage stage, const std::string& entry,
                                    const std::vector<std::pair<uint32_t, uint32_t>>& constants) {
    SpirvSpecialization out;
    if (!spirvHeaderValid(words)) {
        out.log = "error: not a SPIR-V 1.0-1.6 module\n";
        return out;
    }
    std::string unknownExtension;
    forEachInstruction(words, [&](uint32_t opcode, const uint32_t* operands, uint32_t count) {
        if (opcode == spv::OpName || opcode == spv::OpMemberName) out.named = true;
        if (opcode == spv::OpExtension) {
            const std::string name = literalString(operands, count);
            const auto& known = spirvExtensions();
            if (std::none_of(known.begin(), known.end(), [&](const char* e) { return name == e; })) unknownExtension = name;
        }
    });
    try {
        SpirvToGlsl glsl(words);
        bool found = false;
        for (const spirv_cross::EntryPoint& point : glsl.get_entry_points_and_stages())
            found |= point.name == entry && point.execution_model == executionModel(stage);
        if (!found) {
            out.status = SpirvSpecialization::kBadEntryPoint;
            return out;
        }
        const spirv_cross::SmallVector<spirv_cross::SpecializationConstant> specs = glsl.get_specialization_constants();
        for (const auto& [index, value] : constants) {
            auto it = std::find_if(specs.begin(), specs.end(), [&](const auto& c) { return c.constant_id == index; });
            if (it == specs.end()) {
                out.status = SpirvSpecialization::kBadConstant;
                return out;
            }
            glsl.get_constant(it->id).m.c[0].r[0].u32 = value;
        }
        if (!unknownExtension.empty()) {
            out.log = "error: the module uses SPIR-V extension " + unknownExtension + ", which is not in GL_SPIR_V_EXTENSIONS\n";
            return out;
        }
        glsl.set_entry_point(entry, executionModel(stage));
        glsl.nameResources(stage);
        spirv_cross::CompilerGLSL::Options options = glsl.get_common_options();
        options.version = 460;
        options.es = false;
        options.vulkan_semantics = false;
        options.separate_shader_objects = true;  // keeps the locations of stage variables
        options.enable_420pack_extension = true;
        glsl.set_common_options(options);
        out.glsl = glsl.compile();
        out.status = SpirvSpecialization::kOk;
    } catch (const std::exception& error) {
        out.log = std::string("error: SPIR-V module cannot be used: ") + error.what() + "\n";
    }
    return out;
}

std::string spirvLinkInputs(const std::string& glsl, Stage previous) {
    const std::string from = std::string(kSpirvNamePrefix) + "in_", to = std::string(kSpirvNamePrefix) + stageTag(previous) + "_";
    std::string out = glsl;
    for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size())) out.replace(at, from.size(), to);
    return out;
}

std::string spirvDisplayName(const LinkResult& link, const std::string& name) {
    if (!link.spirv) return name;
    if (link.spirvUnnamed || name.find(kSpirvNamePrefix) != std::string::npos) return std::string();
    return name;
}

}  // namespace wmgl::shader

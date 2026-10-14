#include "core/shader/spirv_passes.h"

#define SPV_ENABLE_UTILITY_CODE
#include <spirv.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace wmgl::shader {
namespace {

constexpr uint32_t kOpTypeVector = 23, kOpConstantComposite = 44, kOpCompositeConstruct = 80,
                   kOpCompositeExtract = 81, kOpImageGather = 96, kOpImageDrefGather = 97;
// Image operand masks, in operand order.
constexpr uint32_t kBias = 0x1, kLod = 0x2, kGrad = 0x4, kConstOffset = 0x8, kOffset = 0x10, kConstOffsets = 0x20;

uint32_t operandWords(uint32_t bit) { return bit == kGrad ? 2 : 1; }

}  // namespace

std::vector<uint32_t> lowerGatherOffsets(std::vector<uint32_t> spirv) {
    if (spirv.size() < 5) return spirv;
    std::map<uint32_t, uint32_t> vectorComponent;              // vector type -> component type
    std::map<uint32_t, std::vector<uint32_t>> composites;     // constant composite -> constituents
    bool found = false;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count || i + count > spirv.size()) return spirv;
        if (op == kOpTypeVector && count >= 4) vectorComponent[spirv[i + 1]] = spirv[i + 2];
        if (op == kOpConstantComposite && count >= 3)
            composites[spirv[i + 2]].assign(spirv.begin() + static_cast<long>(i) + 3, spirv.begin() + static_cast<long>(i + count));
        if ((op == kOpImageGather || op == kOpImageDrefGather) && count > 7 && (spirv[i + 6] & kConstOffsets)) found = true;
        i += count;
    }
    if (!found) return spirv;
    uint32_t bound = spirv[3];
    std::vector<uint32_t> out(spirv.begin(), spirv.begin() + 5);
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        const bool gather = (op == kOpImageGather || op == kOpImageDrefGather) && count > 7 && (spirv[i + 6] & kConstOffsets);
        auto offsets = composites.end();
        uint32_t mask = 0;
        size_t offsetsAt = 0;
        if (gather) {
            mask = spirv[i + 6];
            offsetsAt = i + 7;
            for (uint32_t bit = 1; bit < kConstOffsets; bit <<= 1)
                if (mask & bit) offsetsAt += operandWords(bit);
            if (offsetsAt < i + count) offsets = composites.find(spirv[offsetsAt]);
        }
        auto component = gather ? vectorComponent.find(spirv[i + 1]) : vectorComponent.end();
        if (!gather || offsets == composites.end() || offsets->second.size() != 4 || component == vectorComponent.end() ||
            (mask & (kConstOffset | kOffset))) {
            out.insert(out.end(), spirv.begin() + static_cast<long>(i), spirv.begin() + static_cast<long>(i + count));
            i += count;
            continue;
        }
        const uint32_t resultType = spirv[i + 1], result = spirv[i + 2];
        uint32_t texels[4];
        for (int k = 0; k < 4; ++k) {
            // The same gather with operand ConstOffset (offsets[k]) in place of ConstOffsets.
            const uint32_t newMask = (mask & ~kConstOffsets) | kConstOffset;
            std::vector<uint32_t> words(spirv.begin() + static_cast<long>(i) + 1, spirv.begin() + static_cast<long>(i) + 6);
            const uint32_t gathered = bound++;
            words[1] = gathered;
            words.push_back(newMask);
            size_t at = i + 7;
            for (uint32_t bit = 1; bit != 0 && at <= i + count; bit <<= 1) {
                if (bit == kConstOffset) words.push_back(offsets->second[static_cast<size_t>(k)]);
                if (!(mask & bit)) continue;
                const uint32_t n = operandWords(bit);
                if (bit != kConstOffsets) words.insert(words.end(), spirv.begin() + static_cast<long>(at), spirv.begin() + static_cast<long>(at + n));
                at += n;
                if (at >= i + count) break;
            }
            out.push_back((static_cast<uint32_t>(words.size() + 1) << 16) | op);
            out.insert(out.end(), words.begin(), words.end());
            texels[k] = bound++;
            out.insert(out.end(), {(5u << 16) | kOpCompositeExtract, component->second, texels[k], gathered, 3u});
        }
        out.insert(out.end(), {(7u << 16) | kOpCompositeConstruct, resultType, result, texels[0], texels[1], texels[2], texels[3]});
        i += count;
    }
    out[3] = bound;
    return out;
}

namespace {

constexpr uint32_t kOpTypeInt = 21, kOpTypeImage = 25, kOpConstant = 43, kOpFunction = 54, kOpLoad = 61,
                   kOpVectorShuffle = 79, kOpImageQuerySize = 104, kOpSDiv = 135;
constexpr uint32_t kDim2D = 1, kDimCube = 3;

}  // namespace

std::vector<uint32_t> cubeStorageImagesAs2DArrays(std::vector<uint32_t> spirv) {
    if (spirv.size() < 5) return spirv;
    // Storage cube image types (Sampled = 2) and whether each is a cube array.
    std::map<uint32_t, bool> cubes;
    std::map<uint32_t, uint32_t> vectorType;  // component << 8 | count -> vector type
    std::map<uint32_t, uint32_t> loadType;    // OpLoad result -> its type
    uint32_t signedInt = 0, six = 0;
    size_t firstFunction = 0;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count || i + count > spirv.size()) return spirv;
        if (op == kOpTypeImage && count >= 9 && spirv[i + 3] == kDimCube && spirv[i + 7] == 2) cubes[spirv[i + 1]] = spirv[i + 5] != 0;
        if (op == kOpTypeInt && count >= 4 && spirv[i + 2] == 32 && spirv[i + 3] == 1) signedInt = spirv[i + 1];
        if (op == kOpTypeVector && count >= 4) vectorType[(spirv[i + 2] << 8) | spirv[i + 3]] = spirv[i + 1];
        if (op == kOpConstant && count == 4 && signedInt && spirv[i + 1] == signedInt && spirv[i + 3] == 6) six = spirv[i + 2];
        if (op == kOpLoad && count >= 4) loadType[spirv[i + 2]] = spirv[i + 1];
        if (op == kOpFunction && !firstFunction) firstFunction = i;
        i += count;
    }
    if (cubes.empty() || !firstFunction) return spirv;
    // The image types become 2D arrays.
    for (size_t i = 5; i < firstFunction;) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (op == kOpTypeImage && cubes.count(spirv[i + 1])) {
            spirv[i + 3] = kDim2D;
            spirv[i + 5] = 1;
        }
        i += count;
    }
    auto queried = [&](size_t i) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (op != kOpImageQuerySize || count != 4) return cubes.end();
        auto load = loadType.find(spirv[i + 3]);
        return load == loadType.end() ? cubes.end() : cubes.find(load->second);
    };
    bool sizes = false, arraySizes = false;
    for (size_t i = firstFunction; i < spirv.size(); i += spirv[i] >> 16)
        if (auto cube = queried(i); cube != cubes.end()) (cube->second ? arraySizes : sizes) = true;
    if (!sizes && !arraySizes) return spirv;
    if (!signedInt) return spirv;  // imageSize returns ivecN, so the type exists
    uint32_t bound = spirv[3];
    std::vector<uint32_t> declarations;
    uint32_t ivec3 = vectorType.count((signedInt << 8) | 3) ? vectorType[(signedInt << 8) | 3] : 0;
    if (!ivec3) {
        ivec3 = bound++;
        declarations.insert(declarations.end(), {(4u << 16) | kOpTypeVector, ivec3, signedInt, 3u});
    }
    if (arraySizes && !six) {
        six = bound++;
        declarations.insert(declarations.end(), {(4u << 16) | kOpConstant, signedInt, six, 6u});
    }
    std::vector<uint32_t> out(spirv.begin(), spirv.begin() + static_cast<long>(firstFunction));
    out.insert(out.end(), declarations.begin(), declarations.end());
    for (size_t i = firstFunction; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16;
        auto cube = queried(i);
        if (cube == cubes.end()) {
            out.insert(out.end(), spirv.begin() + static_cast<long>(i), spirv.begin() + static_cast<long>(i + count));
            i += count;
            continue;
        }
        const uint32_t resultType = spirv[i + 1], result = spirv[i + 2], image = spirv[i + 3];
        const uint32_t size = bound++;
        out.insert(out.end(), {(4u << 16) | kOpImageQuerySize, ivec3, size, image});
        if (!cube->second) {
            out.insert(out.end(), {(7u << 16) | kOpVectorShuffle, resultType, result, size, size, 0u, 1u});
        } else {
            // (w, h, layer-faces / 6).
            const uint32_t width = bound++, height = bound++, layers = bound++, cubesCount = bound++;
            out.insert(out.end(), {(5u << 16) | kOpCompositeExtract, signedInt, width, size, 0u});
            out.insert(out.end(), {(5u << 16) | kOpCompositeExtract, signedInt, height, size, 1u});
            out.insert(out.end(), {(5u << 16) | kOpCompositeExtract, signedInt, layers, size, 2u});
            out.insert(out.end(), {(5u << 16) | kOpSDiv, signedInt, cubesCount, layers, six});
            out.insert(out.end(), {(6u << 16) | kOpCompositeConstruct, resultType, result, width, height, cubesCount});
        }
        i += count;
    }
    out[3] = bound;
    return out;
}

}  // namespace wmgl::shader

namespace wmgl::shader {
namespace {

constexpr uint32_t kOpTypeIntOp = 21, kOpTypeArray = 28, kOpTypePointer = 32, kOpConstantOp = 43,
                   kOpVariable = 59, kOpDecorate = 71, kOpAccessChain = 65, kOpInBoundsAccessChain = 66, kOpIAdd = 128,
                   kOpIMul = 132, kOpFunctionOp = 54;
constexpr uint32_t kUniformConstant = 0, kInput = 1, kOutput = 3, kDecorationBuiltIn = 11;

}  // namespace

std::vector<uint32_t> flattenArraysOfArrays(std::vector<uint32_t> spirv) {
    if (spirv.size() < 5) return spirv;
    struct ArrayType { uint32_t element, length; };
    std::map<uint32_t, ArrayType> arrays;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;  // id -> (storage, pointee)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> constants;  // id -> (type, value)
    std::map<uint32_t, bool> intTypes;                             // 32-bit int type -> signed
    std::map<uint32_t, size_t> variables;                          // id -> instruction index
    std::set<uint32_t> builtIns;
    size_t firstFunction = 0;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count || i + count > spirv.size()) return spirv;
        if (op == kOpTypeArray && count == 4) arrays[spirv[i + 1]] = {spirv[i + 2], spirv[i + 3]};
        if (op == kOpTypePointer && count == 4) pointers[spirv[i + 1]] = {spirv[i + 2], spirv[i + 3]};
        if (op == kOpConstantOp && count == 4) constants[spirv[i + 2]] = {spirv[i + 1], spirv[i + 3]};
        if (op == kOpTypeIntOp && count == 4 && spirv[i + 2] == 32) intTypes[spirv[i + 1]] = spirv[i + 3] != 0;
        if (op == kOpVariable && count >= 4 && !firstFunction) variables[spirv[i + 2]] = i;
        if (op == kOpDecorate && count >= 4 && spirv[i + 2] == kDecorationBuiltIn) builtIns.insert(spirv[i + 1]);
        if (op == kOpFunctionOp && !firstFunction) firstFunction = i;
        i += count;
    }
    if (!firstFunction) return spirv;
    // Candidates: arrays of arrays of opaque types (UniformConstant) or of
    // stage inputs and outputs, which MSL cannot declare.
    struct Candidate { std::vector<uint32_t> dims; uint32_t element; };
    std::map<uint32_t, Candidate> candidates;
    for (const auto& [id, at] : variables) {
        const uint32_t storage = spirv[at + 3];
        if ((storage != kUniformConstant && storage != kInput && storage != kOutput) || builtIns.count(id)) continue;
        auto pointer = pointers.find(spirv[at + 1]);
        if (pointer == pointers.end()) continue;
        Candidate c;
        uint32_t type = pointer->second.second;
        while (arrays.count(type)) {
            auto length = constants.find(arrays[type].length);
            if (length == constants.end()) break;
            c.dims.push_back(length->second.second);
            type = arrays[type].element;
        }
        if (c.dims.size() < 2 || arrays.count(type)) continue;
        c.element = type;
        candidates[id] = c;
    }
    if (candidates.empty()) return spirv;
    // Every use in code must be an access chain through all the dimensions.
    for (size_t i = firstFunction; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        const bool chain = op == kOpAccessChain || op == kOpInBoundsAccessChain;
        for (uint32_t w = 1; w < count; ++w) {
            auto c = candidates.find(spirv[i + w]);
            if (c == candidates.end()) continue;
            if (!(chain && w == 3 && count - 4 >= c->second.dims.size())) candidates.erase(c);
        }
        i += count;
    }
    if (candidates.empty()) return spirv;
    uint32_t bound = spirv[3];
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> constantIds;  // (type, value) -> id
    for (const auto& [id, c] : constants) constantIds[{c.first, c.second}] = id;
    uint32_t uintType = 0;
    for (const auto& [id, isSigned] : intTypes)
        if (!isSigned) uintType = id;
    std::vector<uint32_t> declarations;  // new types and constants, before the first variable
    auto constant = [&](uint32_t type, uint32_t value) {
        auto it = constantIds.find({type, value});
        if (it != constantIds.end()) return it->second;
        const uint32_t id = bound++;
        declarations.insert(declarations.end(), {(4u << 16) | kOpConstantOp, type, id, value});
        constantIds[{type, value}] = id;
        return id;
    };
    if (!uintType) {
        uintType = bound++;
        declarations.insert(declarations.end(), {(4u << 16) | kOpTypeIntOp, uintType, 32u, 0u});
    }
    std::map<uint32_t, uint32_t> newPointer;  // variable -> flattened pointer type
    for (const auto& [id, c] : candidates) {
        uint32_t total = 1;
        for (uint32_t d : c.dims) total *= d;
        const uint32_t array = bound++, pointer = bound++;
        const uint32_t length = constant(uintType, total);
        declarations.insert(declarations.end(), {(4u << 16) | kOpTypeArray, array, c.element, length});
        declarations.insert(declarations.end(), {(4u << 16) | kOpTypePointer, pointer, spirv[variables[id] + 3], array});
        newPointer[id] = pointer;
    }
    // Result types of code values (for the index arithmetic).
    std::map<uint32_t, uint32_t> valueType;
    for (const auto& [id, c] : constants) valueType[id] = c.first;
    for (size_t i = firstFunction; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16;
        if (count >= 3) valueType[spirv[i + 2]] = spirv[i + 1];
        i += count;
    }
    std::vector<uint32_t> code;
    for (size_t i = firstFunction; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        auto c = (op == kOpAccessChain || op == kOpInBoundsAccessChain) ? candidates.find(spirv[i + 3]) : candidates.end();
        if (c == candidates.end()) {
            code.insert(code.end(), spirv.begin() + static_cast<long>(i), spirv.begin() + static_cast<long>(i + count));
            i += count;
            continue;
        }
        // ((i0 * d1 + i1) * d2 + i2) ... in the first index's integer type.
        const std::vector<uint32_t>& dims = c->second.dims;
        uint32_t linear = spirv[i + 4];
        uint32_t type = valueType.count(linear) ? valueType[linear] : uintType;
        if (!intTypes.count(type)) type = uintType;
        for (size_t d = 1; d < dims.size(); ++d) {
            const uint32_t scaled = bound++, sum = bound++;
            code.insert(code.end(), {(5u << 16) | kOpIMul, type, scaled, linear, constant(type, dims[d])});
            code.insert(code.end(), {(5u << 16) | kOpIAdd, type, sum, scaled, spirv[i + 4 + d]});
            linear = sum;
        }
        std::vector<uint32_t> words = {spirv[i + 1], spirv[i + 2], spirv[i + 3], linear};
        words.insert(words.end(), spirv.begin() + static_cast<long>(i + 4 + dims.size()), spirv.begin() + static_cast<long>(i + count));
        code.push_back((static_cast<uint32_t>(words.size() + 1) << 16) | op);
        code.insert(code.end(), words.begin(), words.end());
        i += count;
    }
    // The global section, then the new declarations, then the flattened
    // variables (moved after them, with their new pointer types).
    std::vector<uint32_t> out(spirv.begin(), spirv.begin() + 5), moved;
    for (size_t i = 5; i < firstFunction;) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        auto p = op == kOpVariable ? newPointer.find(spirv[i + 2]) : newPointer.end();
        if (p != newPointer.end()) {
            const size_t start = moved.size();
            moved.insert(moved.end(), spirv.begin() + static_cast<long>(i), spirv.begin() + static_cast<long>(i + count));
            moved[start + 1] = p->second;
        } else {
            out.insert(out.end(), spirv.begin() + static_cast<long>(i), spirv.begin() + static_cast<long>(i + count));
        }
        i += count;
    }
    out.insert(out.end(), declarations.begin(), declarations.end());
    out.insert(out.end(), moved.begin(), moved.end());
    out.insert(out.end(), code.begin(), code.end());
    out[3] = bound;
    return out;
}

namespace {

constexpr uint32_t kOpTypeImageOp = 25, kOpTypeArrayOp = 28, kOpTypeRuntimeArray = 29, kOpTypePointerOp = 32,
                   kOpFunctionFirst = 54, kOpVariableOp = 59, kOpLoadOp = 61,
                   kOpAccessChainOp = 65, kOpInBoundsAccessChainOp = 66, kOpDecorateOp = 71, kOpMemberDecorate = 72,
                   kOpDecorationGroup = 73, kOpGroupDecorate = 74, kOpGroupMemberDecorate = 75, kOpCopyObject = 83,
                   kOpImageRead = 98, kOpImageWrite = 99, kOpImageQuerySizeLod = 103, kOpImageQuerySizeOp = 104,
                   kOpImageQueryLod = 105, kOpImageQueryLevels = 106, kOpImageQuerySamples = 107,
                   kOpImageSparseRead = 320, kOpDecorateId = 332, kOpDecorateString = 5632,
                   kOpMemberDecorateString = 5633;
constexpr uint32_t kDecorationNonWritable = 24, kDecorationNonReadable = 25;
constexpr uint32_t kRead = 1, kWrite = 2;

bool isAnnotation(uint32_t op) {
    return (op >= kOpDecorateOp && op <= kOpGroupMemberDecorate) || op == kOpDecorateId || op == kOpDecorateString ||
           op == kOpMemberDecorateString;
}

}  // namespace

std::vector<uint32_t> inferStorageImageAccess(std::vector<uint32_t> spirv) {
    if (spirv.size() < 5) return spirv;
    std::set<uint32_t> imageTypes, pointerTypes;
    std::map<uint32_t, uint32_t> access;   // storage image variable -> kRead | kWrite
    std::map<uint32_t, uint32_t> origin;   // loads and access chains of one -> the variable
    std::map<uint32_t, std::set<uint32_t>> decorations;
    size_t firstFunction = 0, annotationsEnd = 0, firstType = 0;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count || i + count > spirv.size()) return spirv;
        if (op == kOpFunctionFirst) {
            firstFunction = i;
            break;
        }
        if (isAnnotation(op)) annotationsEnd = i + count;
        if (op == kOpDecorateOp && count >= 3) decorations[spirv[i + 1]].insert(spirv[i + 2]);
        if (op == kOpTypeImageOp && count >= 9 && spirv[i + 7] == 2) imageTypes.insert(spirv[i + 1]);
        if ((op == kOpTypeArrayOp || op == kOpTypeRuntimeArray) && count >= 3 && imageTypes.count(spirv[i + 2]))
            imageTypes.insert(spirv[i + 1]);
        if (op == kOpTypePointerOp && count >= 4 && imageTypes.count(spirv[i + 3])) pointerTypes.insert(spirv[i + 1]);
        if (op == kOpVariableOp && count >= 4 && pointerTypes.count(spirv[i + 1])) access[spirv[i + 2]] = 0;
        if (!firstType && op >= 19 && op <= 39) firstType = i;
        i += count;
    }
    if (access.empty() || !firstFunction) return spirv;
    for (auto& [variable, mask] : access) origin[variable] = variable;
    auto mark = [&](uint32_t id, uint32_t bits) {
        if (auto it = origin.find(id); it != origin.end()) access[it->second] |= bits;
    };
    for (size_t i = firstFunction; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count || i + count > spirv.size()) return spirv;
        switch (op) {
        case kOpLoadOp:
        case kOpAccessChainOp:
        case kOpInBoundsAccessChainOp:
        case kOpCopyObject:
            if (count >= 4) {
                if (auto it = origin.find(spirv[i + 3]); it != origin.end()) origin[spirv[i + 2]] = it->second;
                for (uint32_t w = 4; w < count; ++w) mark(spirv[i + w], kRead | kWrite);
            }
            break;
        case kOpImageRead:
        case kOpImageSparseRead:
            if (count >= 4) mark(spirv[i + 3], kRead);
            break;
        case kOpImageWrite:
            if (count >= 2) mark(spirv[i + 1], kWrite);
            break;
        case kOpImageQuerySizeLod:
        case kOpImageQuerySizeOp:
        case kOpImageQueryLod:
        case kOpImageQueryLevels:
        case kOpImageQuerySamples:
            break;
        default:
            // Atomics (OpImageTexelPointer), function arguments and anything
            // else: read and written, as without this pass.
            for (uint32_t w = 1; w < count; ++w) mark(spirv[i + w], kRead | kWrite);
            break;
        }
        i += count;
    }
    std::vector<uint32_t> added;
    for (const auto& [variable, mask] : access) {
        const std::set<uint32_t>& has = decorations[variable];
        if (!(mask & kWrite) && !has.count(kDecorationNonWritable))
            added.insert(added.end(), {(3u << 16) | kOpDecorateOp, variable, kDecorationNonWritable});
        else if (!(mask & kRead) && !has.count(kDecorationNonReadable))
            added.insert(added.end(), {(3u << 16) | kOpDecorateOp, variable, kDecorationNonReadable});
    }
    if (added.empty()) return spirv;
    const size_t at = annotationsEnd ? annotationsEnd : firstType;
    if (!at) return spirv;
    spirv.insert(spirv.begin() + static_cast<long>(at), added.begin(), added.end());
    return spirv;
}

namespace {

// A module's declarations (before its first function), for flatVaryingSources.
struct Declarations {
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> decorations;  // id -> decoration -> its first operand
    std::map<uint32_t, std::vector<uint32_t>> types;               // type -> its instruction's words
    std::map<uint32_t, uint32_t> constants;                        // 32-bit scalar constants
    struct Variable {
        uint32_t type, storage, initializer;
    };
    std::map<uint32_t, Variable> variables;  // module-scope variables
    size_t firstFunction = 0;

    bool read(const std::vector<uint32_t>& spirv) {
        if (spirv.size() < 5) return false;
        for (size_t i = 5; i < spirv.size();) {
            const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size()) return false;
            const uint32_t* w = &spirv[i];
            if (op == spv::OpFunction) {
                if (!firstFunction) firstFunction = i;
            } else if (firstFunction) {
            } else if (op == spv::OpName && count >= 3) {
                const char* name = reinterpret_cast<const char*>(w + 2);
                names[w[1]] = std::string(name, strnlen(name, (count - 2) * 4));
            } else if (op == spv::OpDecorate && count >= 3) {
                decorations[w[1]][w[2]] = count > 3 ? w[3] : 0;
            } else if (op >= spv::OpTypeVoid && op < spv::OpTypeForwardPointer) {
                types[w[1]].assign(w, w + count);
            } else if (op == spv::OpConstant && count == 4) {
                constants[w[2]] = w[3];
            } else if (op == spv::OpVariable && count >= 4) {
                variables[w[2]] = {w[1], w[3], count > 4 ? w[4] : 0};
            }
            i += count;
        }
        return firstFunction != 0;
    }
    const uint32_t* decoration(uint32_t id, spv::Decoration which) const {
        auto it = decorations.find(id);
        if (it == decorations.end()) return nullptr;
        auto found = it->second.find(which);
        return found == it->second.end() ? nullptr : &found->second;
    }
    uint32_t opcode(uint32_t type) const {
        auto it = types.find(type);
        return it == types.end() ? 0 : it->second[0] & 0xffff;
    }
    // The type a variable of pointer type `pointer` holds.
    uint32_t pointee(uint32_t pointer) const {
        auto it = types.find(pointer);
        return it != types.end() && opcode(pointer) == spv::OpTypePointer && it->second.size() >= 4 ? it->second[3] : 0;
    }
    // The type under `type`'s vectors, matrices and arrays.
    uint32_t element(uint32_t type) const {
        for (int depth = 0; depth < 16; ++depth) {
            const uint32_t op = opcode(type);
            if (op != spv::OpTypeVector && op != spv::OpTypeMatrix && op != spv::OpTypeArray) return type;
            type = types.at(type)[2];
        }
        return 0;
    }
    // The locations a varying of `type` takes; 0 when not known here
    // (structures, 64-bit types).
    uint32_t locations(uint32_t type) const {
        auto it = types.find(type);
        if (it == types.end()) return 0;
        const std::vector<uint32_t>& t = it->second;
        switch (t[0] & 0xffff) {
        case spv::OpTypeBool:
            return 1;
        case spv::OpTypeInt:
        case spv::OpTypeFloat:
            return t.size() >= 3 && t[2] <= 32 ? 1 : 0;
        case spv::OpTypeVector:
            return t.size() >= 4 ? locations(t[2]) : 0;
        case spv::OpTypeMatrix:
            return t.size() >= 4 ? t[3] * locations(t[2]) : 0;
        case spv::OpTypeArray: {
            auto length = t.size() >= 4 ? constants.find(t[3]) : constants.end();
            return length != constants.end() ? length->second * locations(t[2]) : 0;
        }
        default:
            return 0;
        }
    }
};

// A function's blocks and their control dependences: the branches that
// decide whether a block runs, which are those with the block on a path
// from one of their targets to their immediate post-dominator.
struct ControlFlow {
    struct Block {
        uint32_t condition = 0;         // the OpBranchConditional condition or OpSwitch selector
        std::vector<uint32_t> targets;  // labels; none: the block leaves the function
        std::vector<int> successors;    // -1: the function's exit
        std::vector<int> predecessors;
        std::vector<int> controlledBy;  // blocks whose branch decides whether this one runs
    };
    std::vector<Block> blocks;
    std::map<uint32_t, int> index;  // label -> block
    // A block that never reaches the exit (an endless loop): every branch
    // is taken to control every block.
    bool everywhere = false;

    bool analyze() {
        const int n = static_cast<int>(blocks.size()), exit = n;
        for (int b = 0; b < n; ++b) {
            for (uint32_t label : blocks[b].targets) {
                auto it = index.find(label);
                if (it == index.end()) return false;
                blocks[b].successors.push_back(it->second);
                blocks[it->second].predecessors.push_back(b);
            }
            if (blocks[b].successors.empty()) blocks[b].successors.push_back(-1);
        }
        // Blocks that reach the exit, walking predecessors back from it.
        std::vector<bool> reaches(n, false);
        std::vector<int> work;
        for (int b = 0; b < n; ++b)
            if (blocks[b].successors[0] == -1) reaches[b] = true, work.push_back(b);
        while (!work.empty()) {
            const int b = work.back();
            work.pop_back();
            for (int p : blocks[b].predecessors)
                if (!reaches[p]) reaches[p] = true, work.push_back(p);
        }
        if (std::find(reaches.begin(), reaches.end(), false) != reaches.end()) {
            everywhere = true;
            return true;
        }
        // Post-dominator sets, one bit per block and the exit.
        const size_t words = static_cast<size_t>(n + 1 + 63) / 64;
        std::vector<std::vector<uint64_t>> pdom(static_cast<size_t>(n + 1), std::vector<uint64_t>(words, ~uint64_t{0}));
        auto set = [](std::vector<uint64_t>& bits, int b) { bits[static_cast<size_t>(b) / 64] |= uint64_t{1} << (b % 64); };
        auto has = [](const std::vector<uint64_t>& bits, int b) { return (bits[static_cast<size_t>(b) / 64] >> (b % 64)) & 1; };
        pdom[static_cast<size_t>(exit)].assign(words, 0);
        set(pdom[static_cast<size_t>(exit)], exit);
        for (bool changed = true; changed;) {
            changed = false;
            for (int b = n - 1; b >= 0; --b) {
                std::vector<uint64_t> meet(words, ~uint64_t{0});
                for (int s : blocks[b].successors)
                    for (size_t w = 0; w < words; ++w) meet[w] &= pdom[static_cast<size_t>(s < 0 ? exit : s)][w];
                set(meet, b);
                if (meet != pdom[static_cast<size_t>(b)]) pdom[static_cast<size_t>(b)] = std::move(meet), changed = true;
            }
        }
        std::vector<int> sizes(static_cast<size_t>(n));
        for (int b = 0; b < n; ++b)
            for (uint64_t w : pdom[static_cast<size_t>(b)]) sizes[static_cast<size_t>(b)] += __builtin_popcountll(w);
        for (int b = 0; b < n; ++b) {
            if (!blocks[b].condition) continue;
            // The immediate post-dominator: the strict post-dominator that
            // every other one post-dominates.
            const std::vector<uint64_t>& mine = pdom[static_cast<size_t>(b)];
            int ipdom = exit;
            for (int p = 0; p < n; ++p)
                if (p != b && has(mine, p) && sizes[static_cast<size_t>(p)] == sizes[static_cast<size_t>(b)] - 1) ipdom = p;
            std::vector<bool> seen(n, false);
            for (int s : blocks[b].successors)
                if (s >= 0 && s != ipdom && !seen[s]) seen[s] = true, work.push_back(s);
            while (!work.empty()) {
                const int c = work.back();
                work.pop_back();
                blocks[c].controlledBy.push_back(b);
                for (int s : blocks[c].successors)
                    if (s >= 0 && s != ipdom && !seen[s]) seen[s] = true, work.push_back(s);
            }
        }
        return true;
    }
};

// Storage a store makes visible to other invocations.
bool sharedStorage(uint32_t storage) {
    switch (storage) {
    case spv::StorageClassUniform:  // BufferBlock storage blocks
    case spv::StorageClassStorageBuffer:
    case spv::StorageClassPhysicalStorageBuffer:
    case spv::StorageClassImage:
    case spv::StorageClassWorkgroup:
    case spv::StorageClassCrossWorkgroup:
    case spv::StorageClassAtomicCounter:
    case spv::StorageClassGeneric:
        return true;
    default:
        return false;
    }
}

}  // namespace

FlatSources flatVaryingSources(const std::vector<uint32_t>& vertex, const std::vector<uint32_t>& fragment,
                               const std::function<bool(const std::string& name, uint32_t& locations)>& attribute) {
    const FlatSources varies;
    Declarations fs, vs;
    if (!fs.read(fragment) || !vs.read(vertex)) return varies;

    // The locations of the fragment stage's flat inputs (integers are flat
    // whatever their declaration).
    std::set<uint32_t> flat;
    for (const auto& [id, v] : fs.variables) {
        if (v.storage != spv::StorageClassInput || fs.decoration(id, spv::DecorationBuiltIn)) continue;
        const uint32_t type = fs.pointee(v.type), element = fs.element(type);
        // Blocks may have flat members (OpMemberDecorate, not read here).
        if (!element || fs.opcode(element) == spv::OpTypeStruct || fs.decoration(id, spv::DecorationPerVertexKHR))
            return varies;
        if (!fs.decoration(id, spv::DecorationFlat) && fs.opcode(element) != spv::OpTypeInt) continue;
        const uint32_t* location = fs.decoration(id, spv::DecorationLocation);
        const uint32_t count = fs.locations(type);
        if (!location || !count) return varies;
        for (uint32_t l = 0; l < count; ++l) flat.insert(*location + l);
    }
    if (flat.empty()) return varies;

    // The vertex stage's outputs at those locations (the linker matched
    // varyings by name and gave both stages the same locations).
    std::set<uint32_t> outputs;
    for (const auto& [id, v] : vs.variables) {
        if (v.storage != spv::StorageClassOutput || vs.decoration(id, spv::DecorationBuiltIn)) continue;
        const uint32_t type = vs.pointee(v.type);
        const uint32_t* location = vs.decoration(id, spv::DecorationLocation);
        if (!location) {
            auto name = vs.names.find(type);
            if (vs.opcode(type) == spv::OpTypeStruct && name != vs.names.end() && name->second == "gl_PerVertex") continue;
            return varies;
        }
        const uint32_t count = vs.locations(type);
        if (!count) {
            if (*location <= *flat.rbegin()) return varies;
            continue;
        }
        if (flat.lower_bound(*location) != flat.lower_bound(*location + count)) outputs.insert(id);
    }

    // What reaches each variable's contents and each value: attribute
    // locations, or kVaries for anything that differs between vertices.
    // Iterated to a fixed point (loops store after they load).
    constexpr uint64_t kVaries = uint64_t{1} << 63;
    std::map<uint32_t, uint64_t> content, value, address;  // address: the indices of an access chain
    std::map<uint32_t, uint32_t> root;                     // pointer -> its variable
    for (const auto& [id, v] : vs.variables) {
        uint64_t bits = 0;
        if (v.storage == spv::StorageClassInput) {
            if (const uint32_t* builtin = vs.decoration(id, spv::DecorationBuiltIn)) {
                const bool perInstance = *builtin == spv::BuiltInInstanceId || *builtin == spv::BuiltInInstanceIndex ||
                                         *builtin == spv::BuiltInBaseVertex || *builtin == spv::BuiltInBaseInstance ||
                                         *builtin == spv::BuiltInDrawIndex;
                bits = perInstance ? 0 : kVaries;
            } else {
                uint32_t locations = 0;
                auto name = vs.names.find(id);
                bits = name != vs.names.end() && attribute(name->second, locations) ? locations : kVaries;
            }
        } else if (v.storage != spv::StorageClassUniformConstant && v.storage != spv::StorageClassUniform &&
                   v.storage != spv::StorageClassPushConstant && v.storage != spv::StorageClassStorageBuffer &&
                   v.storage != spv::StorageClassOutput && v.storage != spv::StorageClassPrivate) {
            bits = kVaries;
        }
        content[id] = bits;
    }
    // Functions' parameters, in order. glslang passes most arguments as
    // pointers: such a parameter is a variable of its own, which takes the
    // contents of every argument and gives its own back (out parameters).
    std::map<uint32_t, std::vector<uint32_t>> parameters;
    std::map<uint32_t, uint64_t> returned;  // function -> what its return values carry
    std::map<uint32_t, uint64_t> called;    // function -> the conditions deciding whether it runs
    std::map<uint32_t, ControlFlow> flows;
    {
        uint32_t function = 0;
        ControlFlow* flow = nullptr;
        for (size_t i = vs.firstFunction; i < vertex.size(); i += vertex[i] >> 16) {
            const uint32_t count = vertex[i] >> 16, op = vertex[i] & 0xffff;
            const uint32_t* w = &vertex[i];
            ControlFlow::Block* block = flow && !flow->blocks.empty() ? &flow->blocks.back() : nullptr;
            if (op == spv::OpFunction && count >= 3) {
                function = w[2];
                flow = &flows[function];
            } else if (op == spv::OpFunctionParameter && count >= 3) {
                parameters[function].push_back(w[2]);
                if (vs.opcode(w[1]) == spv::OpTypePointer) root[w[2]] = w[2];
            } else if (op == spv::OpVariable && count >= 4) {
                root[w[2]] = w[2];  // a function's variable
            } else if (op == spv::OpLabel && count >= 2 && flow) {
                flow->index[w[1]] = static_cast<int>(flow->blocks.size());
                flow->blocks.emplace_back();
            } else if (op == spv::OpBranch && count >= 2 && block) {
                block->targets = {w[1]};
            } else if (op == spv::OpBranchConditional && count >= 4 && block) {
                block->condition = w[1];
                block->targets = {w[2], w[3]};
            } else if (op == spv::OpSwitch && count >= 3 && block) {
                // 32-bit selectors: (literal, label) pairs.
                block->condition = w[1];
                block->targets = {w[2]};
                for (uint32_t k = 4; k < count; k += 2) block->targets.push_back(w[k]);
            } else if (op == spv::OpFunctionEnd && flow) {
                if (!flow->analyze()) return varies;
                flow = nullptr;
            }
        }
    }
    auto get = [](const std::map<uint32_t, uint64_t>& bits, uint32_t id) {
        auto it = bits.find(id);
        return it == bits.end() ? uint64_t{0} : it->second;
    };
    auto rootOf = [&](uint32_t pointer) {
        auto it = root.find(pointer);
        return it == root.end() ? pointer : it->second;
    };
    auto isPointer = [&](uint32_t id) { return root.count(id) || vs.variables.count(id); };
    auto storageOf = [&](uint32_t variable) {
        auto it = vs.variables.find(variable);
        return it == vs.variables.end() ? static_cast<uint32_t>(spv::StorageClassFunction) : it->second.storage;
    };
    bool changed = true;
    auto merge = [&](std::map<uint32_t, uint64_t>& bits, uint32_t id, uint64_t add) {
        uint64_t& at = bits[id];
        if ((at | add) != at) {
            at |= add;
            changed = true;
        }
    };
    for (int pass = 0; changed; ++pass) {
        if (pass == 64) return varies;
        changed = false;
        uint32_t function = 0;
        const ControlFlow* flow = nullptr;
        int block = -1;
        // What decides whether block `b` of the current function runs: the
        // conditions of the branches it depends on, and its function's calls.
        auto controlOf = [&](int b) {
            uint64_t bits = get(called, function);
            if (!flow || b < 0) return bits | kVaries;
            const ControlFlow::Block& at = flow->blocks[static_cast<size_t>(b)];
            if (flow->everywhere) {
                for (const ControlFlow::Block& other : flow->blocks)
                    if (other.condition) bits |= get(value, other.condition);
                return bits;
            }
            for (int c : at.controlledBy) bits |= get(value, flow->blocks[static_cast<size_t>(c)].condition);
            return bits;
        };
        for (size_t i = vs.firstFunction; i < vertex.size();) {
            const uint32_t count = vertex[i] >> 16, op = vertex[i] & 0xffff;
            const uint32_t* w = &vertex[i];
            if ((op >= spv::OpAtomicLoad && op <= spv::OpAtomicXor) || op == spv::OpAtomicFlagTestAndSet ||
                op == spv::OpAtomicFlagClear || op == spv::OpAtomicFMinEXT || op == spv::OpAtomicFMaxEXT ||
                op == spv::OpAtomicFAddEXT || op == spv::OpImageWrite || op == spv::OpImageTexelPointer)
                return varies;
            const uint64_t control = op == spv::OpStore || op == spv::OpCopyMemory || op == spv::OpFunctionCall ||
                                             op == spv::OpReturnValue || op == spv::OpExtInst
                                         ? controlOf(block)
                                         : 0;
            switch (op) {
            case spv::OpFunction:
                if (count >= 3) {
                    function = w[2];
                    flow = &flows[function];
                    block = -1;
                }
                break;
            case spv::OpLabel:
                if (count >= 2 && flow) {
                    auto it = flow->index.find(w[1]);
                    block = it == flow->index.end() ? -1 : it->second;
                }
                break;
            case spv::OpFunctionCall: {
                if (count < 4) return varies;
                auto callee = parameters.find(w[3]);
                const size_t arguments = count - 4;
                if (arguments && (callee == parameters.end() || callee->second.size() != arguments)) return varies;
                for (size_t k = 0; k < arguments; ++k) {
                    const uint32_t argument = w[4 + k], parameter = callee->second[k];
                    if (root.count(parameter) && !isPointer(argument)) return varies;
                    if (!isPointer(argument)) {
                        merge(value, parameter, get(value, argument));
                        continue;
                    }
                    if (!root.count(parameter) || sharedStorage(storageOf(rootOf(argument)))) return varies;
                    merge(content, parameter, get(content, rootOf(argument)) | get(address, argument));
                    merge(content, rootOf(argument), get(content, parameter) | get(address, argument) | control);
                }
                merge(called, w[3], control);
                merge(value, w[2], get(returned, w[3]));
                break;
            }
            case spv::OpReturnValue:
                if (count >= 2) merge(returned, function, get(value, w[1]) | control);
                break;
            case spv::OpFunctionParameter:  // takes its call sites' arguments
                break;
            case spv::OpVariable:
                if (count > 4) merge(content, w[2], get(value, w[4]));
                break;
            case spv::OpAccessChain:
            case spv::OpInBoundsAccessChain:
            case spv::OpPtrAccessChain:
            case spv::OpInBoundsPtrAccessChain: {
                if (count < 4) return varies;
                root[w[2]] = rootOf(w[3]);
                uint64_t bits = get(address, w[3]);
                for (uint32_t k = 4; k < count; ++k) bits |= get(value, w[k]);
                merge(address, w[2], bits);
                break;
            }
            case spv::OpLoad:
                if (count < 4) return varies;
                merge(value, w[2], get(content, rootOf(w[3])) | get(address, w[3]));
                break;
            case spv::OpStore:
            case spv::OpCopyMemory: {
                if (count < 3) return varies;
                const uint32_t target = rootOf(w[1]);
                if (sharedStorage(storageOf(target))) return varies;
                const uint64_t source =
                    op == spv::OpStore ? get(value, w[2]) : get(content, rootOf(w[2])) | get(address, w[2]);
                merge(content, target, source | get(address, w[1]) | control);
                break;
            }
            case spv::OpPhi: {
                // Which value arrives depends on the branches that decide
                // which predecessor ran, and on those ending in a branch.
                if (vs.opcode(w[1]) == spv::OpTypePointer) return varies;
                uint64_t bits = 0;
                for (uint32_t k = 3; k + 1 < count; k += 2) {
                    bits |= get(value, w[k]);
                    auto parent = flow ? flow->index.find(w[k + 1]) : std::map<uint32_t, int>::const_iterator();
                    if (!flow || parent == flow->index.end()) return varies;
                    bits |= controlOf(parent->second) | get(value, flow->blocks[static_cast<size_t>(parent->second)].condition);
                }
                merge(value, w[2], bits);
                break;
            }
            default: {
                bool hasResult = false, hasType = false;
                spv::HasResultAndType(static_cast<spv::Op>(op), &hasResult, &hasType);
                if (!hasResult || !hasType || count < 3) break;
                // A pointer from anything else (OpSelect, a conversion)
                // has no known variable.
                if (op != spv::OpCopyObject && vs.opcode(w[1]) == spv::OpTypePointer) return varies;
                // Every operand that is a value or a pointer; a literal that
                // happens to equal an id only adds sources.
                uint64_t bits = 0;
                for (uint32_t k = 3; k < count; ++k)
                    bits |= get(value, w[k]) | (isPointer(w[k]) ? get(content, rootOf(w[k])) | get(address, w[k]) : 0);
                if (op == spv::OpCopyObject && count >= 4 && isPointer(w[3])) {
                    root[w[2]] = rootOf(w[3]);
                    merge(address, w[2], get(address, w[3]));
                }
                // Extended instructions (modf, frexp) may write through a pointer operand.
                if (op == spv::OpExtInst)
                    for (uint32_t k = 5; k < count; ++k)
                        if (isPointer(w[k])) {
                            if (sharedStorage(storageOf(rootOf(w[k])))) return varies;
                            merge(content, rootOf(w[k]), bits | control);
                        }
                merge(value, w[2], bits);
                break;
            }
            }
            i += count;
        }
    }
    uint64_t bits = 0;
    for (uint32_t id : outputs) bits |= get(content, id);
    if (bits & kVaries) return varies;
    return {false, static_cast<uint32_t>(bits)};
}

}  // namespace wmgl::shader

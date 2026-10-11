#include "core/shader/spirv_passes.h"

#include <map>

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

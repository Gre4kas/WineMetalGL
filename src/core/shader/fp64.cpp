#include "core/shader/fp64.h"

#define SPV_ENABLE_UTILITY_CODE
#include <SPIRV/GLSL.std.450.h>
#include <SPIRV/spirv.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace wmgl::shader {
namespace {

// The soft-float core as MSL text (fp64_soft.h, stringified).
#define WMGL_FP64_SOURCE(...) #__VA_ARGS__
const char* const kSoftCore =
#include "core/shader/fp64_soft.h"
    ;
#undef WMGL_FP64_SOURCE

const char* const kLibraryParts[] = {
#include "core/shader/fp64_library.inc"
};

const char* const kMslPrelude = R"WMGL(
// WineMetalGL soft-float doubles (GL_ARB_gpu_shader_fp64).
static inline int wmgl_clz64(ulong x) { return int(clz(x)); }
static inline ulong wmgl_mulhi64(ulong a, ulong b) { return mulhi(a, b); }
static inline uint wmgl_f32_bits(float f) { return as_type<uint>(f); }
static inline float wmgl_bits_f32(uint u) { return as_type<float>(u); }
)WMGL";

using spv::Op;

struct Inst {
    Op op;
    std::vector<uint32_t> w;  // operands, after the opcode word
};

bool hasResult(Op op, bool* type) {
    bool result = false, resultType = false;
    spv::HasResultAndType(op, &result, &resultType);
    if (type) *type = resultType;
    return result;
}

uint32_t resultId(const Inst& i) {
    bool type = false;
    if (!hasResult(i.op, &type)) return 0;
    return i.w[type ? 1 : 0];
}

uint32_t resultTypeOf(const Inst& i) {
    bool type = false;
    if (!hasResult(i.op, &type) || !type) return 0;
    return i.w[0];
}

std::vector<uint32_t> stringWords(const std::string& s) {
    std::vector<uint32_t> words((s.size() + 4) / 4, 0);
    std::memcpy(words.data(), s.data(), s.size());
    return words;
}

std::string wordsString(const std::vector<uint32_t>& w, size_t from) {
    std::string s;
    for (size_t i = from; i < w.size(); ++i)
        for (int b = 0; b < 4; ++b) {
            const char c = static_cast<char>((w[i] >> (8 * b)) & 0xff);
            if (!c) return s;
            s += c;
        }
    return s;
}

bool isTypeOrGlobal(Op op) {
    switch (op) {
    case Op::OpTypeVoid: case Op::OpTypeBool: case Op::OpTypeInt: case Op::OpTypeFloat: case Op::OpTypeVector:
    case Op::OpTypeMatrix: case Op::OpTypeImage: case Op::OpTypeSampler: case Op::OpTypeSampledImage:
    case Op::OpTypeArray: case Op::OpTypeRuntimeArray: case Op::OpTypeStruct: case Op::OpTypeOpaque:
    case Op::OpTypePointer: case Op::OpTypeFunction: case Op::OpTypeForwardPointer: case Op::OpConstantTrue:
    case Op::OpConstantFalse: case Op::OpConstant: case Op::OpConstantComposite: case Op::OpConstantSampler:
    case Op::OpConstantNull: case Op::OpSpecConstantTrue: case Op::OpSpecConstantFalse: case Op::OpSpecConstant:
    case Op::OpSpecConstantComposite: case Op::OpSpecConstantOp: case Op::OpVariable: case Op::OpUndef:
        return true;
    default:
        return false;
    }
}

// Shape of a double value: columns x rows (scalar 1x1, vector 1xN).
struct Shape {
    uint32_t cols = 0, rows = 0;
    bool valid() const { return cols != 0; }
};

class Lowering {
public:
    Lowering(std::vector<uint32_t>& spirv, std::string& log) : spirv_(spirv), log_(log) {}
    bool run();

private:
    // ---- module
    void parse();
    std::vector<uint32_t> serialize();
    uint32_t newId() { return bound_++; }
    bool fail(const std::string& message) {
        if (ok_) log_ += "error: double precision: " + message + "\n";
        ok_ = false;
        return false;
    }

    // ---- original types
    const Inst* typeDef(uint32_t id) const {
        auto it = types_.find(id);
        return it == types_.end() ? nullptr : &it->second;
    }
    Shape shape(uint32_t type) const;
    bool containsDouble(uint32_t type) const;
    uint32_t pointee(uint32_t ptrType) const { return typeDef(ptrType) ? typeDef(ptrType)->w[2] : 0; }
    uint32_t storageOf(uint32_t ptrType) const { return typeDef(ptrType) ? typeDef(ptrType)->w[1] : 0; }

    // ---- new globals
    uint32_t uintType();
    uint32_t uintConst(uint32_t value);
    uint32_t lateType(Op op, std::vector<uint32_t> operands);  // deduplicated
    uint32_t uvec(uint32_t n) { return n == 1 ? uintType() : lateType(Op::OpTypeVector, {uintType(), n}); }
    uint32_t u64vec(uint32_t n);
    uint32_t arrayOf(uint32_t element, uint32_t count) { return lateType(Op::OpTypeArray, {element, uintConst(count)}); }
    uint32_t pointerTo(uint32_t storage, uint32_t type) { return lateType(Op::OpTypePointer, {storage, type}); }
    uint32_t nullOf(uint32_t type);
    void name(uint32_t id, const std::string& s) {
        std::vector<uint32_t> w = {id};
        for (uint32_t x : stringWords(s)) w.push_back(x);
        newNames_.push_back({Op::OpName, w});
    }

    // ---- stubs
    uint32_t stub(const std::string& lib, uint32_t resultType, const std::vector<uint32_t>& argTypes);

    // ---- passes
    void transformTypes();
    bool rewriteInterfaces();
    bool lowerFunctions();
    bool lowerInstruction(const Inst& in, std::vector<Inst>& out);
    uint32_t valueType(uint32_t id) const {
        auto it = valueTypes_.find(id);
        return it == valueTypes_.end() ? 0 : it->second;
    }
    void emitCall(std::vector<Inst>& out, const std::string& lib, uint32_t type, uint32_t id, const std::vector<uint32_t>& args);
    // Element-wise store and load of an array value through `pointer`
    // (nested arrays down to their vector elements).
    void storeElements(std::vector<Inst>& out, uint32_t pointer, uint32_t type, uint32_t value);
    uint32_t loadElements(std::vector<Inst>& out, uint32_t pointer, uint32_t type);

    // Interface value conversions: words of a (lowered) double-holding value
    // and back.
    void toWords(std::vector<Inst>& out, uint32_t type, uint32_t value, std::vector<uint32_t>& words);
    uint32_t fromWords(std::vector<Inst>& out, uint32_t type, const std::vector<uint32_t>& words, size_t& at);
    uint32_t interfaceType(uint32_t type);  // Metal-passable type for a double-holding type
    uint32_t packInterface(std::vector<Inst>& out, uint32_t ifaceType, const std::vector<uint32_t>& words, size_t& at);
    void unpackInterface(std::vector<Inst>& out, uint32_t ifaceType, uint32_t value, std::vector<uint32_t>& words);
    uint32_t wordsOfInterface(uint32_t ifaceType) const;

    std::vector<uint32_t>& spirv_;
    std::string& log_;
    bool ok_ = true;
    std::vector<uint32_t> header_;
    std::vector<Inst> insts_;
    uint32_t bound_ = 0;
    uint32_t f64_ = 0;
    spv::ExecutionModel model_ = spv::ExecutionModelMax;
    uint32_t entryFunction_ = 0;
    uint32_t glslStd450_ = 0;

    std::unordered_map<uint32_t, Inst> types_;          // original and new type definitions
    std::unordered_map<uint32_t, uint32_t> valueTypes_;  // result id -> type id
    std::unordered_map<uint32_t, Shape> matrices_;      // double matrix type -> shape
    std::unordered_map<uint32_t, uint32_t> constValues_;  // 32-bit int constant id -> value

    // New instructions by destination.
    std::vector<Inst> early_;  // start of the types section (uint type and constants)
    std::vector<Inst> late_;   // end of the global section
    std::vector<Inst> newNames_, newDecorations_;
    std::vector<Inst> stubFunctions_;
    std::vector<Inst> pendingVars_;  // Function variables for the function being lowered
    std::set<uint32_t> bufferLoads_;  // values loaded from Uniform, StorageBuffer or PushConstant storage
    std::map<std::pair<Op, std::vector<uint32_t>>, uint32_t> lateTypes_;
    std::map<uint32_t, uint32_t> uintConsts_, nulls_;
    std::map<std::string, uint32_t> stubs_;
    uint32_t uint_ = 0;
    bool uintIsEarly_ = false;
    std::set<size_t> removed_;  // instruction indices dropped from the output

    // Row-major double matrix members: struct id -> member -> row-array type.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> rowMajorMembers_;
    std::unordered_map<uint32_t, uint32_t> rowArrayOf_;  // row-array type id -> matrix type id
    // Interface variables: original id (now private) -> new interface variable.
    struct Interface {
        uint32_t privateVar = 0, type = 0;
        bool input = false, split = false;
        // An interface block: its member types, and their Metal-facing types.
        bool block = false;
        std::vector<uint32_t> members, mappedMembers;
        // The Metal-passable variables: one (a varying), or the split parts
        // of a vertex input (name suffixes in `labels`); zero words follow a
        // part in the word stream when it is the short second half of a
        // dvec3 column (the layout of a varying's uvec4 pair).
        std::vector<uint32_t> vars, types, padAfter;
        std::vector<std::string> labels;
    };
    // Vertex inputs: the columns (rows each) of a double-holding type, in
    // order (array elements, then matrix columns).
    void inputColumns(uint32_t type, std::vector<uint32_t>& rows);
    std::vector<Interface> interfaces_;
};

Shape Lowering::shape(uint32_t type) const {
    if (type == f64_) return {1, 1};
    if (auto m = matrices_.find(type); m != matrices_.end()) return m->second;
    const Inst* t = typeDef(type);
    if (t && t->op == Op::OpTypeVector && t->w[1] == f64_) return {1, t->w[2]};
    return {};
}

bool Lowering::containsDouble(uint32_t type) const {
    if (shape(type).valid()) return true;
    const Inst* t = typeDef(type);
    if (!t) return false;
    switch (t->op) {
    case Op::OpTypeArray: case Op::OpTypeRuntimeArray: return containsDouble(t->w[1]);
    case Op::OpTypePointer: return containsDouble(t->w[2]);
    case Op::OpTypeStruct:
        for (size_t i = 1; i < t->w.size(); ++i)
            if (containsDouble(t->w[i])) return true;
        return false;
    default: return false;
    }
}

void Lowering::parse() {
    header_.assign(spirv_.begin(), spirv_.begin() + 5);
    bound_ = spirv_[3];
    for (size_t i = 5; i < spirv_.size();) {
        const uint32_t count = spirv_[i] >> 16;
        if (!count || i + count > spirv_.size()) break;
        Inst in{static_cast<Op>(spirv_[i] & 0xffff), std::vector<uint32_t>(spirv_.begin() + i + 1, spirv_.begin() + i + count)};
        insts_.push_back(std::move(in));
        i += count;
    }
    for (const Inst& in : insts_) {
        const uint32_t id = resultId(in), type = resultTypeOf(in);
        if (id && type) valueTypes_[id] = type;
        if (in.op >= Op::OpTypeVoid && in.op <= Op::OpTypeForwardPointer && id) types_[id] = in;
        if (in.op == Op::OpTypeFloat && in.w[1] == 64) f64_ = in.w[0];
        if (in.op == Op::OpConstant && in.w.size() == 3) constValues_[in.w[1]] = in.w[2];
        if (in.op == Op::OpEntryPoint) {
            model_ = static_cast<spv::ExecutionModel>(in.w[0]);
            entryFunction_ = in.w[1];
        }
        if (in.op == Op::OpExtInstImport && wordsString(in.w, 1) == "GLSL.std.450") glslStd450_ = in.w[0];
    }
}

std::vector<uint32_t> Lowering::serialize() {
    // Section boundaries in the original order.
    size_t firstAnnotation = insts_.size(), firstType = insts_.size(), firstFunction = insts_.size();
    for (size_t i = 0; i < insts_.size(); ++i) {
        const Op op = insts_[i].op;
        const bool annotation = op == Op::OpDecorate || op == Op::OpMemberDecorate || op == Op::OpDecorationGroup ||
                                op == Op::OpGroupDecorate || op == Op::OpGroupMemberDecorate || op == Op::OpDecorateId ||
                                op == Op::OpDecorateString || op == Op::OpMemberDecorateString;
        if (annotation && firstAnnotation == insts_.size()) firstAnnotation = i;
        if (isTypeOrGlobal(op) && op != Op::OpVariable && firstType == insts_.size()) firstType = i;
        if (op == Op::OpVariable && firstType == insts_.size()) firstType = i;
        if (op == Op::OpFunction && firstFunction == insts_.size()) firstFunction = i;
    }
    if (firstAnnotation > firstType) firstAnnotation = firstType;
    std::vector<uint32_t> out = header_;
    auto emit = [&](const Inst& in) {
        out.push_back(static_cast<uint32_t>((in.w.size() + 1) << 16) | static_cast<uint32_t>(in.op));
        out.insert(out.end(), in.w.begin(), in.w.end());
    };
    for (size_t i = 0; i <= insts_.size(); ++i) {
        if (i == firstAnnotation)
            for (const Inst& in : newNames_) emit(in);
        if (i == firstType) {
            for (const Inst& in : newDecorations_) emit(in);
            for (const Inst& in : early_) emit(in);
        }
        if (i == firstFunction || (i == insts_.size() && firstFunction == insts_.size()))
            for (const Inst& in : late_) emit(in);
        if (i == insts_.size()) break;
        if (removed_.count(i)) continue;
        emit(insts_[i]);
    }
    for (const Inst& in : stubFunctions_) emit(in);
    out[3] = bound_;
    return out;
}

// ------------------------------------------------------------ new globals

uint32_t Lowering::uintType() {
    if (uint_) return uint_;
    // An existing uint type moves to the start of the types section, where
    // the new constants (array lengths) need it.
    for (size_t i = 0; i < insts_.size(); ++i) {
        const Inst& in = insts_[i];
        if (in.op == Op::OpTypeInt && in.w[1] == 32 && in.w[2] == 0) {
            uint_ = in.w[0];
            early_.insert(early_.begin(), in);
            removed_.insert(i);
            return uint_;
        }
    }
    uint_ = newId();
    Inst t{Op::OpTypeInt, {uint_, 32, 0}};
    types_[uint_] = t;
    early_.insert(early_.begin(), t);
    return uint_;
}

uint32_t Lowering::uintConst(uint32_t value) {
    auto it = uintConsts_.find(value);
    if (it != uintConsts_.end()) return it->second;
    const uint32_t type = uintType();
    const uint32_t id = newId();
    early_.push_back({Op::OpConstant, {type, id, value}});
    valueTypes_[id] = type;
    constValues_[id] = value;
    uintConsts_[value] = id;
    return id;
}

uint32_t Lowering::lateType(Op op, std::vector<uint32_t> operands) {
    // Existing identical types are reused when they are not the matrix or
    // float type this pass changed.
    auto key = std::make_pair(op, operands);
    auto it = lateTypes_.find(key);
    if (it != lateTypes_.end()) return it->second;
    if (op != Op::OpTypeFunction) {
        for (const auto& [id, t] : types_) {
            if (t.op != op || t.w.size() != operands.size() + 1) continue;
            if (!std::equal(operands.begin(), operands.end(), t.w.begin() + 1)) continue;
            if (op == Op::OpTypeArray && matrices_.count(id)) continue;
            lateTypes_[key] = id;
            return id;
        }
    }
    const uint32_t id = newId();
    std::vector<uint32_t> w = {id};
    w.insert(w.end(), operands.begin(), operands.end());
    Inst t{op, w};
    types_[id] = t;
    late_.push_back(t);
    lateTypes_[key] = id;
    return id;
}

uint32_t Lowering::u64vec(uint32_t n) {
    if (n == 1) return f64_;
    for (const auto& [id, t] : types_)
        if (t.op == Op::OpTypeVector && t.w[1] == f64_ && t.w[2] == n) return id;
    return lateType(Op::OpTypeVector, {f64_, n});
}

uint32_t Lowering::nullOf(uint32_t type) {
    auto it = nulls_.find(type);
    if (it != nulls_.end()) return it->second;
    const uint32_t id = newId();
    late_.push_back({Op::OpConstantNull, {type, id}});
    valueTypes_[id] = type;
    nulls_[type] = id;
    return id;
}

uint32_t Lowering::stub(const std::string& lib, uint32_t resultType, const std::vector<uint32_t>& argTypes) {
    std::string key = lib + ":" + std::to_string(resultType);
    for (uint32_t t : argTypes) key += "," + std::to_string(t);
    auto it = stubs_.find(key);
    if (it != stubs_.end()) return it->second;
    std::vector<uint32_t> fnOperands = {resultType};
    fnOperands.insert(fnOperands.end(), argTypes.begin(), argTypes.end());
    const uint32_t fnType = lateType(Op::OpTypeFunction, fnOperands);
    const uint32_t fn = newId();
    const uint32_t null = nullOf(resultType);
    stubFunctions_.push_back({Op::OpFunction, {resultType, fn, 0, fnType}});
    for (size_t a = 0; a < argTypes.size(); ++a) {
        const uint32_t p = newId();
        stubFunctions_.push_back({Op::OpFunctionParameter, {argTypes[a], p}});
        name(p, "wmgl_p" + std::to_string(a));
    }
    stubFunctions_.push_back({Op::OpLabel, {newId()}});
    stubFunctions_.push_back({Op::OpReturnValue, {null}});
    stubFunctions_.push_back({Op::OpFunctionEnd, {}});
    // finishFp64Msl reads the library function from the name: wmgl_s<N>_<lib>.
    name(fn, "wmgl_s" + std::to_string(stubs_.size()) + "_" + lib);
    stubs_[key] = fn;
    return fn;
}

// ------------------------------------------------------------------ types

void Lowering::transformTypes() {
    // Matrices first (their shapes are needed by everything after).
    for (Inst& in : insts_) {
        if (in.op != Op::OpTypeMatrix) continue;
        const Inst* column = typeDef(in.w[1]);
        if (!column || column->op != Op::OpTypeVector || column->w[1] != f64_) continue;
        matrices_[in.w[0]] = {in.w[2], column->w[2]};
    }
    for (size_t i = 0; i < insts_.size(); ++i) {
        Inst& in = insts_[i];
        if (in.op == Op::OpCapability && in.w[0] == spv::CapabilityFloat64) {
            in.w[0] = spv::CapabilityInt64;
            for (const Inst& other : insts_)
                if (other.op == Op::OpCapability && other.w[0] == spv::CapabilityInt64 && &other != &in) removed_.insert(i);
        } else if (in.op == Op::OpTypeFloat && in.w[0] == f64_) {
            in = {Op::OpTypeInt, {f64_, 64, 0}};
            types_[f64_] = in;
        } else if (in.op == Op::OpTypeMatrix && matrices_.count(in.w[0])) {
            const uint32_t id = in.w[0], column = in.w[1], cols = in.w[2];
            in = {Op::OpTypeArray, {id, column, uintConst(cols)}};
            types_[id] = in;
        }
    }
    // Strides: a double matrix's MatrixStride becomes its array type's
    // ArrayStride (column-major), or the member becomes an array of rows.
    std::map<uint32_t, uint32_t> arrayStride;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> matrixStride;
    std::set<std::pair<uint32_t, uint32_t>> rowMajor;
    auto memberMatrix = [&](uint32_t structId, uint32_t member) -> uint32_t {
        const Inst* s = typeDef(structId);
        if (!s || s->op != Op::OpTypeStruct || member + 1 >= s->w.size()) return 0;
        uint32_t t = s->w[member + 1];
        while (const Inst* d = typeDef(t)) {
            if (matrices_.count(t)) return t;
            if (d->op != Op::OpTypeArray && d->op != Op::OpTypeRuntimeArray) return 0;
            t = d->w[1];
        }
        return 0;
    };
    for (size_t i = 0; i < insts_.size(); ++i) {
        const Inst& in = insts_[i];
        if (in.op != Op::OpMemberDecorate) continue;
        const uint32_t m = memberMatrix(in.w[0], in.w[1]);
        if (!m) continue;
        const auto key = std::make_pair(in.w[0], in.w[1]);
        if (in.w[2] == spv::DecorationMatrixStride) {
            matrixStride[key] = in.w[3];
            removed_.insert(i);
        } else if (in.w[2] == spv::DecorationRowMajor) {
            rowMajor.insert(key);
            removed_.insert(i);
        } else if (in.w[2] == spv::DecorationColMajor) {
            removed_.insert(i);
        }
    }
    for (const auto& [key, stride] : matrixStride) {
        const uint32_t m = memberMatrix(key.first, key.second);
        if (rowMajor.count(key)) continue;
        auto [it, inserted] = arrayStride.emplace(m, stride);
        if (!inserted && it->second != stride) fail("column-major matrices of one type with different strides");
    }
    for (const auto& [m, stride] : arrayStride) newDecorations_.push_back({Op::OpDecorate, {m, spv::DecorationArrayStride, stride}});
    // Row-major members: arrays of row vectors, created before the struct.
    std::map<uint32_t, std::vector<Inst>> beforeStruct;
    std::map<uint32_t, size_t> structIndex;
    for (size_t i = 0; i < insts_.size(); ++i)
        if (insts_[i].op == Op::OpTypeStruct) structIndex[insts_[i].w[0]] = i;
    for (const auto& key : rowMajor) {
        const uint32_t structId = key.first, member = key.second;
        auto stride = matrixStride.find(key);
        if (stride == matrixStride.end()) {
            fail("row-major matrix without a stride");
            continue;
        }
        std::vector<Inst>& defs = beforeStruct[structId];
        // Rebuild the member type with every matrix replaced by an array of rows.
        std::function<uint32_t(uint32_t)> rebuild = [&](uint32_t t) -> uint32_t {
            if (auto mat = matrices_.find(t); mat != matrices_.end()) {
                const Shape s = mat->second;
                // The row vector (C components): an existing type declared
                // before the struct, or a new one.
                uint32_t row = 0;
                for (size_t i = 0; i < structIndex[structId]; ++i) {
                    const Inst& d = insts_[i];
                    if (d.op == Op::OpTypeVector && d.w[1] == f64_ && d.w[2] == s.cols) row = d.w[0];
                }
                for (const Inst& d : defs)
                    if (d.op == Op::OpTypeVector && d.w[1] == f64_ && d.w[2] == s.cols) row = d.w[0];
                if (s.cols == 1) row = f64_;
                if (!row) {
                    row = newId();
                    defs.push_back({Op::OpTypeVector, {row, f64_, s.cols}});
                    types_[row] = defs.back();
                }
                const uint32_t rows = newId();
                defs.push_back({Op::OpTypeArray, {rows, row, uintConst(s.rows)}});
                types_[rows] = defs.back();
                newDecorations_.push_back({Op::OpDecorate, {rows, spv::DecorationArrayStride, stride->second}});
                rowArrayOf_[rows] = t;
                return rows;
            }
            const Inst* d = typeDef(t);
            if (!d || (d->op != Op::OpTypeArray && d->op != Op::OpTypeRuntimeArray)) return t;
            const uint32_t element = rebuild(d->w[1]);
            const uint32_t id = newId();
            Inst a = *d;
            a.w[0] = id;
            a.w[1] = element;
            defs.push_back(a);
            types_[id] = a;
            for (const Inst& dec : insts_)
                if (dec.op == Op::OpDecorate && dec.w[0] == t && dec.w[1] == spv::DecorationArrayStride)
                    newDecorations_.push_back({Op::OpDecorate, {id, spv::DecorationArrayStride, dec.w[2]}});
            return id;
        };
        Inst& s = insts_[structIndex[structId]];
        const uint32_t replaced = rebuild(s.w[member + 1]);
        s.w[member + 1] = replaced;
        types_[structId] = s;
        rowMajorMembers_[key] = replaced;
    }
    // Insert the row types before their structs (by rebuilding insts_).
    if (!beforeStruct.empty()) {
        std::vector<Inst> rebuilt;
        std::set<size_t> removed;
        for (size_t i = 0; i < insts_.size(); ++i) {
            if (insts_[i].op == Op::OpTypeStruct) {
                auto it = beforeStruct.find(insts_[i].w[0]);
                if (it != beforeStruct.end())
                    for (const Inst& d : it->second) rebuilt.push_back(d);
            }
            if (removed_.count(i)) removed.insert(rebuilt.size());
            rebuilt.push_back(insts_[i]);
        }
        insts_ = std::move(rebuilt);
        removed_ = std::move(removed);
    }
}

// ------------------------------------------------------------- interfaces

uint32_t Lowering::interfaceType(uint32_t type) {
    // Words per location: 4 (a uvec4) or 2 (a lone double as uvec2).
    const Shape s = shape(type);
    if (s.valid()) {
        if (s.cols == 1) {
            if (s.rows == 1) return uvec(2);
            if (s.rows == 2) return uvec(4);
            return arrayOf(uvec(4), 2);
        }
        return arrayOf(uvec(4), s.cols * (s.rows <= 2 ? 1 : 2));
    }
    const Inst* t = typeDef(type);
    if (t && t->op == Op::OpTypeArray && containsDouble(t->w[1])) {
        const uint32_t element = interfaceType(t->w[1]);
        const uint32_t count = constValues_.count(t->w[2]) ? constValues_[t->w[2]] : 1;
        const Inst* e = typeDef(element);
        if (e && e->op == Op::OpTypeArray) return arrayOf(e->w[1], count * constValues_[e->w[2]]);
        return arrayOf(element, count);
    }
    fail("unsupported stage input or output type");
    return type;
}

uint32_t Lowering::wordsOfInterface(uint32_t ifaceType) const {
    const Inst* t = typeDef(ifaceType);
    if (!t) return 1;
    if (t->op == Op::OpTypeVector) return t->w[2];
    if (t->op == Op::OpTypeArray) {
        auto c = constValues_.find(t->w[2]);
        return (c == constValues_.end() ? 1 : c->second) * wordsOfInterface(t->w[1]);
    }
    return 1;
}

void Lowering::toWords(std::vector<Inst>& out, uint32_t type, uint32_t value, std::vector<uint32_t>& words) {
    const Shape s = shape(type);
    if (type == f64_) {
        const uint32_t pair = newId();
        out.push_back({Op::OpBitcast, {uvec(2), pair, value}});
        for (uint32_t c = 0; c < 2; ++c) {
            const uint32_t w = newId();
            out.push_back({Op::OpCompositeExtract, {uintType(), w, pair, c}});
            words.push_back(w);
        }
        return;
    }
    if (s.valid() && s.cols == 1) {
        for (uint32_t c = 0; c < s.rows; ++c) {
            const uint32_t x = newId();
            out.push_back({Op::OpCompositeExtract, {f64_, x, value, c}});
            toWords(out, f64_, x, words);
        }
        // A dvec3 passes as two uvec4s: pad to whole locations.
        if (s.rows == 3)
            for (int p = 0; p < 2; ++p) words.push_back(uintConst(0));
        return;
    }
    const Inst* t = typeDef(type);
    uint32_t element = 0, count = 0;
    if (s.valid()) {
        element = u64vec(s.rows);
        count = s.cols;
    } else if (t && t->op == Op::OpTypeArray) {
        element = t->w[1];
        count = constValues_[t->w[2]];
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t x = newId();
        out.push_back({Op::OpCompositeExtract, {element, x, value, i}});
        toWords(out, element, x, words);
    }
}

uint32_t Lowering::fromWords(std::vector<Inst>& out, uint32_t type, const std::vector<uint32_t>& words, size_t& at) {
    const Shape s = shape(type);
    if (type == f64_) {
        const uint32_t pair = newId(), value = newId();
        out.push_back({Op::OpCompositeConstruct, {uvec(2), pair, words[at], words[at + 1]}});
        out.push_back({Op::OpBitcast, {f64_, value, pair}});
        at += 2;
        return value;
    }
    std::vector<uint32_t> parts;
    if (s.valid() && s.cols == 1) {
        for (uint32_t c = 0; c < s.rows; ++c) parts.push_back(fromWords(out, f64_, words, at));
        if (s.rows == 3) at += 2;
    } else {
        const Inst* t = typeDef(type);
        uint32_t element = 0, count = 0;
        if (s.valid()) {
            element = u64vec(s.rows);
            count = s.cols;
        } else if (t && t->op == Op::OpTypeArray) {
            element = t->w[1];
            count = constValues_[t->w[2]];
        }
        for (uint32_t i = 0; i < count; ++i) parts.push_back(fromWords(out, element, words, at));
    }
    const uint32_t value = newId();
    std::vector<uint32_t> w = {type, value};
    w.insert(w.end(), parts.begin(), parts.end());
    out.push_back({Op::OpCompositeConstruct, w});
    return value;
}

uint32_t Lowering::packInterface(std::vector<Inst>& out, uint32_t ifaceType, const std::vector<uint32_t>& words, size_t& at) {
    const Inst* t = typeDef(ifaceType);
    std::vector<uint32_t> parts;
    if (t->op == Op::OpTypeVector) {
        for (uint32_t c = 0; c < t->w[2]; ++c) parts.push_back(at < words.size() ? words[at++] : uintConst(0));
    } else {
        const uint32_t count = constValues_[t->w[2]];
        for (uint32_t i = 0; i < count; ++i) parts.push_back(packInterface(out, t->w[1], words, at));
    }
    const uint32_t value = newId();
    std::vector<uint32_t> w = {ifaceType, value};
    w.insert(w.end(), parts.begin(), parts.end());
    out.push_back({Op::OpCompositeConstruct, w});
    return value;
}

void Lowering::unpackInterface(std::vector<Inst>& out, uint32_t ifaceType, uint32_t value, std::vector<uint32_t>& words) {
    const Inst* t = typeDef(ifaceType);
    if (t->op == Op::OpTypeVector) {
        for (uint32_t c = 0; c < t->w[2]; ++c) {
            const uint32_t w = newId();
            out.push_back({Op::OpCompositeExtract, {uintType(), w, value, c}});
            words.push_back(w);
        }
        return;
    }
    const uint32_t count = constValues_[t->w[2]];
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t x = newId();
        out.push_back({Op::OpCompositeExtract, {t->w[1], x, value, i}});
        unpackInterface(out, t->w[1], x, words);
    }
}

void Lowering::inputColumns(uint32_t type, std::vector<uint32_t>& rows) {
    const Shape sh = shape(type);
    if (sh.valid()) {
        for (uint32_t c = 0; c < sh.cols; ++c) rows.push_back(sh.rows);
        return;
    }
    const Inst* t = typeDef(type);
    if (t && t->op == Op::OpTypeArray) {
        const uint32_t count = constValues_.count(t->w[2]) ? constValues_[t->w[2]] : 1;
        for (uint32_t e = 0; e < count; ++e) inputColumns(t->w[1], rows);
        return;
    }
    fail("unsupported vertex input type");
}

bool Lowering::rewriteInterfaces() {
    if (model_ != spv::ExecutionModelVertex && model_ != spv::ExecutionModelFragment) return true;
    std::set<uint32_t> builtins;
    for (const Inst& in : insts_)
        if ((in.op == Op::OpDecorate && in.w[1] == spv::DecorationBuiltIn) ||
            (in.op == Op::OpMemberDecorate && in.w[2] == spv::DecorationBuiltIn))
            builtins.insert(in.w[0]);
    for (size_t i = 0; i < insts_.size(); ++i) {
        Inst& in = insts_[i];
        if (in.op != Op::OpVariable) continue;
        const uint32_t storage = in.w[2];
        if (storage != spv::StorageClassInput && storage != spv::StorageClassOutput) continue;
        const uint32_t var = in.w[1], ptrType = in.w[0], type = pointee(ptrType);
        if (builtins.count(var) || builtins.count(type) || !containsDouble(type)) continue;
        const Inst* t = typeDef(type);
        Interface itf;
        itf.privateVar = var;
        itf.type = type;
        itf.input = storage == spv::StorageClassInput;
        itf.split = model_ == spv::ExecutionModelVertex && itf.input;
        if (t && t->op == Op::OpTypeStruct) {
            // An interface block: the Metal-facing block has every double
            // member mapped as a plain varying would be (same locations), the
            // others unchanged; the shader works on a private copy of the
            // original struct, converted member by member.
            if (itf.split) return fail("vertex input blocks");
            itf.block = true;
            std::vector<uint32_t> members(t->w.begin() + 1, t->w.end());
            std::vector<uint32_t> mapped;
            for (uint32_t m : members) mapped.push_back(containsDouble(m) ? interfaceType(m) : m);
            if (!ok_) return false;
            const uint32_t block = newId();
            std::vector<uint32_t> w = {block};
            w.insert(w.end(), mapped.begin(), mapped.end());
            late_.push_back({Op::OpTypeStruct, w});
            types_[block] = late_.back();
            itf.members = members;
            itf.mappedMembers = mapped;
            // The new block takes the original's decorations and names; the
            // original struct, now private, is no longer a Block.
            for (size_t d = 0; d < insts_.size(); ++d) {
                const Inst& dec = insts_[d];
                if (dec.op == Op::OpMemberDecorate && dec.w[0] == type) {
                    Inst copy = dec;
                    copy.w[0] = block;
                    newDecorations_.push_back(copy);
                } else if (dec.op == Op::OpDecorate && dec.w[0] == type) {
                    Inst copy = dec;
                    copy.w[0] = block;
                    newDecorations_.push_back(copy);
                    if (dec.w[1] == spv::DecorationBlock) removed_.insert(d);
                } else if (dec.op == Op::OpMemberName && dec.w[0] == type) {
                    Inst copy = dec;
                    copy.w[0] = block;
                    newNames_.push_back(copy);
                } else if (dec.op == Op::OpName && dec.w[0] == type) {
                    name(block, wordsString(dec.w, 1));
                }
            }
            itf.types.push_back(block);
            itf.padAfter.push_back(0);
            itf.labels.push_back("");
        } else if (itf.split) {
            // A vertex input takes one location per column; a column of three
            // or four doubles needs a second Metal attribute (placed by
            // compiler.cpp), so every column is its own variable or two.
            std::vector<uint32_t> rows;
            inputColumns(type, rows);
            for (size_t c = 0; c < rows.size(); ++c) {
                const uint32_t r = rows[c];
                itf.types.push_back(uvec(r == 1 ? 2 : 4));
                itf.padAfter.push_back(0);
                itf.labels.push_back("_wmgl_" + std::to_string(c) + "_0");
                if (r > 2) {
                    itf.types.push_back(uvec(r == 3 ? 2 : 4));
                    itf.padAfter.push_back(r == 3 ? 2 : 0);
                    itf.labels.push_back("_wmgl_" + std::to_string(c) + "_1");
                }
            }
        } else {
            itf.types.push_back(interfaceType(type));
            itf.padAfter.push_back(0);
            itf.labels.push_back("");
        }
        if (!ok_) return false;
        for (uint32_t partType : itf.types) {
            const uint32_t v = newId();
            late_.push_back({Op::OpVariable, {pointerTo(storage, partType), v, storage}});
            itf.vars.push_back(v);
        }
        // The original variable becomes the private copy, declared after its
        // (new) pointer type.
        late_.push_back({Op::OpVariable, {pointerTo(spv::StorageClassPrivate, type), var, spv::StorageClassPrivate}});
        valueTypes_[var] = late_.back().w[0];
        removed_.insert(i);
        interfaces_.push_back(itf);
    }
    if (interfaces_.empty()) return true;
    for (size_t i = 0; i < insts_.size(); ++i) {
        Inst& in = insts_[i];
        for (const Interface& itf : interfaces_) {
            if (in.op == Op::OpEntryPoint) {
                const size_t first = 2 + (wordsString(in.w, 2).size() + 4) / 4;  // after the name string
                for (size_t w = first; w < in.w.size(); ++w)
                    if (in.w[w] == itf.privateVar) {
                        in.w[w] = itf.vars[0];
                        in.w.insert(in.w.end(), itf.vars.begin() + 1, itf.vars.end());
                        break;
                    }
            } else if ((in.op == Op::OpDecorate || in.op == Op::OpDecorateId || in.op == Op::OpDecorateString) &&
                       in.w[0] == itf.privateVar) {
                // Split inputs get their locations from the linker (compiler.cpp).
                if (itf.split) removed_.insert(i);
                else in.w[0] = itf.vars[0];
            } else if (in.op == Op::OpName && in.w[0] == itf.privateVar) {
                // The interface variables keep the GL name (linking and
                // reflection use it); split parts add "_wmgl_<column>_<part>".
                const std::string glName = wordsString(in.w, 1);
                name(itf.privateVar, "wmgl_private_" + glName);
                removed_.insert(i);
                for (size_t p = 0; p < itf.vars.size(); ++p) name(itf.vars[p], glName + itf.labels[p]);
            }
        }
    }
    // Pointers derived from the private copies are Private pointers now.
    std::set<uint32_t> privatePointers;
    for (const Interface& itf : interfaces_) privatePointers.insert(itf.privateVar);
    for (Inst& in : insts_) {
        if ((in.op == Op::OpAccessChain || in.op == Op::OpInBoundsAccessChain || in.op == Op::OpCopyObject) &&
            privatePointers.count(in.w[2])) {
            in.w[0] = pointerTo(spv::StorageClassPrivate, pointee(in.w[0]));
            valueTypes_[in.w[1]] = in.w[0];
            privatePointers.insert(in.w[1]);
        }
    }
    return ok_;
}

// ------------------------------------------------------------ functions

void Lowering::emitCall(std::vector<Inst>& out, const std::string& lib, uint32_t type, uint32_t id,
                        const std::vector<uint32_t>& args) {
    std::vector<uint32_t> argTypes, passed;
    for (uint32_t a : args) {
        const uint32_t t = valueType(a);
        argTypes.push_back(t);
        const Inst* d = typeDef(t);
        if ((d && d->op == Op::OpTypeArray) || bufferLoads_.count(a)) {
            // Arrays (double matrices) and values loaded from buffers pass
            // through a local copy: SPIRV-Cross forwards buffer loads into
            // call arguments as they are stored (native arrays, or doubles
            // padded to wider vectors in std140 arrays), which do not
            // convert to the stub's parameter; a store unpacks them.
            const uint32_t tmp = newId(), copy = newId();
            pendingVars_.push_back({Op::OpVariable, {pointerTo(spv::StorageClassFunction, t), tmp, spv::StorageClassFunction}});
            out.push_back({Op::OpStore, {tmp, a}});
            out.push_back({Op::OpLoad, {t, copy, tmp}});
            valueTypes_[copy] = t;
            passed.push_back(copy);
        } else {
            passed.push_back(a);
        }
    }
    std::vector<uint32_t> w = {type, id, stub(lib, type, argTypes)};
    w.insert(w.end(), passed.begin(), passed.end());
    out.push_back({Op::OpFunctionCall, w});
    valueTypes_[id] = type;
}

void Lowering::storeElements(std::vector<Inst>& out, uint32_t pointer, uint32_t type, uint32_t value) {
    const Inst* d = typeDef(type);
    if (!d || d->op != Op::OpTypeArray) {
        out.push_back({Op::OpStore, {pointer, value}});
        return;
    }
    const uint32_t storage = storageOf(valueType(pointer)), element = d->w[1];
    const uint32_t count = constValues_.count(d->w[2]) ? constValues_[d->w[2]] : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = newId(), part = newId();
        out.push_back({Op::OpAccessChain, {pointerTo(storage, element), at, pointer, uintConst(i)}});
        valueTypes_[at] = pointerTo(storage, element);
        out.push_back({Op::OpCompositeExtract, {element, part, value, i}});
        valueTypes_[part] = element;
        storeElements(out, at, element, part);
    }
}

uint32_t Lowering::loadElements(std::vector<Inst>& out, uint32_t pointer, uint32_t type) {
    const Inst* d = typeDef(type);
    if (!d || d->op != Op::OpTypeArray) {
        const uint32_t value = newId();
        out.push_back({Op::OpLoad, {type, value, pointer}});
        valueTypes_[value] = type;
        return value;
    }
    const uint32_t storage = storageOf(valueType(pointer)), element = d->w[1];
    const uint32_t count = constValues_.count(d->w[2]) ? constValues_[d->w[2]] : 0;
    std::vector<uint32_t> w = {type, newId()};
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = newId();
        out.push_back({Op::OpAccessChain, {pointerTo(storage, element), at, pointer, uintConst(i)}});
        valueTypes_[at] = pointerTo(storage, element);
        w.push_back(loadElements(out, at, element));
    }
    out.push_back({Op::OpCompositeConstruct, w});
    valueTypes_[w[1]] = type;
    return w[1];
}

bool Lowering::lowerInstruction(const Inst& in, std::vector<Inst>& out) {
    bool hasType = false;
    const bool result = hasResult(in.op, &hasType);
    if (!result || !hasType) {
        out.push_back(in);
        return true;
    }
    const uint32_t type = in.w[0], id = in.w[1];
    auto isD = [&](uint32_t value) { return shape(valueType(value)).valid(); };
    const bool resultDouble = shape(type).valid();
    auto operandsDouble = [&](size_t from) {
        for (size_t i = from; i < in.w.size(); ++i)
            if (isD(in.w[i])) return true;
        return false;
    };
    auto call = [&](const std::string& lib, std::vector<uint32_t> args) {
        emitCall(out, lib, type, id, args);
        return true;
    };
    auto args = [&](size_t from) { return std::vector<uint32_t>(in.w.begin() + static_cast<long>(from), in.w.end()); };
    switch (in.op) {
    case Op::OpFNegate: if (resultDouble) return call("dneg", args(2)); break;
    case Op::OpFAdd: if (resultDouble) return call("dadd", args(2)); break;
    case Op::OpFSub: if (resultDouble) return call("dsub", args(2)); break;
    case Op::OpFMul: if (resultDouble) return call("dmul", args(2)); break;
    case Op::OpFDiv: if (resultDouble) return call("ddiv", args(2)); break;
    case Op::OpFMod: if (resultDouble) return call("dmod", args(2)); break;
    case Op::OpFRem: if (resultDouble) return call("drem", args(2)); break;
    case Op::OpVectorTimesScalar: if (resultDouble) return call("dmul_vs", args(2)); break;
    case Op::OpMatrixTimesScalar: if (resultDouble) return call("dmul_ms", args(2)); break;
    case Op::OpMatrixTimesVector: if (resultDouble) return call("dmul_mv", args(2)); break;
    case Op::OpVectorTimesMatrix: if (resultDouble) return call("dmul_vm", args(2)); break;
    case Op::OpMatrixTimesMatrix: if (resultDouble) return call("dmul_mm", args(2)); break;
    case Op::OpOuterProduct: if (resultDouble) return call("douter", args(2)); break;
    case Op::OpTranspose: if (resultDouble) return call("dtranspose", args(2)); break;
    case Op::OpDot: if (operandsDouble(2)) return call("ddot", args(2)); break;
    case Op::OpFOrdEqual: if (operandsDouble(2)) return call("dcmp_oeq", args(2)); break;
    case Op::OpFUnordEqual: if (operandsDouble(2)) return call("dcmp_ueq", args(2)); break;
    case Op::OpFOrdNotEqual: if (operandsDouble(2)) return call("dcmp_one", args(2)); break;
    case Op::OpFUnordNotEqual: if (operandsDouble(2)) return call("dcmp_une", args(2)); break;
    case Op::OpFOrdLessThan: if (operandsDouble(2)) return call("dcmp_olt", args(2)); break;
    case Op::OpFUnordLessThan: if (operandsDouble(2)) return call("dcmp_ult", args(2)); break;
    case Op::OpFOrdGreaterThan: if (operandsDouble(2)) return call("dcmp_ogt", args(2)); break;
    case Op::OpFUnordGreaterThan: if (operandsDouble(2)) return call("dcmp_ugt", args(2)); break;
    case Op::OpFOrdLessThanEqual: if (operandsDouble(2)) return call("dcmp_ole", args(2)); break;
    case Op::OpFUnordLessThanEqual: if (operandsDouble(2)) return call("dcmp_ule", args(2)); break;
    case Op::OpFOrdGreaterThanEqual: if (operandsDouble(2)) return call("dcmp_oge", args(2)); break;
    case Op::OpFUnordGreaterThanEqual: if (operandsDouble(2)) return call("dcmp_uge", args(2)); break;
    case Op::OpIsNan: if (operandsDouble(2)) return call("disnan", args(2)); break;
    case Op::OpIsInf: if (operandsDouble(2)) return call("disinf", args(2)); break;
    case Op::OpConvertFToS: if (operandsDouble(2)) return call("dto_i32", args(2)); break;
    case Op::OpConvertFToU: if (operandsDouble(2)) return call("dto_u32", args(2)); break;
    case Op::OpConvertSToF: if (resultDouble) return call("dfrom_i32", args(2)); break;
    case Op::OpConvertUToF: if (resultDouble) return call("dfrom_u32", args(2)); break;
    case Op::OpFConvert:
        if (resultDouble) return call("dfrom_f32", args(2));
        if (operandsDouble(2)) return call("dto_f32", args(2));
        break;
    case Op::OpBitcast:
        // double <-> uvec2 stays a bitcast (now u64 <-> uvec2).
        break;
    case Op::OpExtInst: {
        if (in.w[2] != glslStd450_ || (!resultDouble && !operandsDouble(4))) break;
        const uint32_t inst = in.w[3];
        const std::vector<uint32_t> a = args(4);
        switch (inst) {
        case GLSLstd450Round: return call("dround", a);
        case GLSLstd450RoundEven: return call("dround_even", a);
        case GLSLstd450Trunc: return call("dtrunc", a);
        case GLSLstd450FAbs: return call("dabs", a);
        case GLSLstd450FSign: return call("dsign", a);
        case GLSLstd450Floor: return call("dfloor", a);
        case GLSLstd450Ceil: return call("dceil", a);
        case GLSLstd450Fract: return call("dfract", a);
        case GLSLstd450Sqrt: return call("dsqrt", a);
        case GLSLstd450InverseSqrt: return call("dinversesqrt", a);
        case GLSLstd450Determinant: return call("ddeterminant", a);
        case GLSLstd450MatrixInverse: return call("dinverse", a);
        case GLSLstd450FMin: case GLSLstd450NMin: return call("dmin", a);
        case GLSLstd450FMax: case GLSLstd450NMax: return call("dmax", a);
        case GLSLstd450FClamp: case GLSLstd450NClamp: return call("dclamp", a);
        case GLSLstd450FMix: return call("dmix", a);
        case GLSLstd450Step: return call("dstep", a);
        case GLSLstd450SmoothStep: return call("dsmoothstep", a);
        case GLSLstd450Fma: return call("dfma", a);
        case GLSLstd450Ldexp: return call("dldexp", a);
        case GLSLstd450Length: return call("dlength", a);
        case GLSLstd450Distance: return call("ddistance", a);
        case GLSLstd450Cross: return call("dcross", a);
        case GLSLstd450Normalize: return call("dnormalize", a);
        case GLSLstd450FaceForward: return call("dfaceforward", a);
        case GLSLstd450Reflect: return call("dreflect", a);
        case GLSLstd450Refract: return call("drefract", a);
        case GLSLstd450PackDouble2x32: case GLSLstd450UnpackDouble2x32:
            out.push_back({Op::OpBitcast, {type, id, a[0]}});
            return true;
        case GLSLstd450Modf: {
            // modf(x, out i): the integer part is stored, the fraction returned.
            const uint32_t whole = newId();
            emitCall(out, "dmodf_int", type, whole, {a[0]});
            out.push_back({Op::OpStore, {a[1], whole}});
            return call("dmodf_frac", {a[0]});
        }
        case GLSLstd450ModfStruct: {
            const Inst* s = typeDef(type);
            const uint32_t frac = newId(), whole = newId();
            emitCall(out, "dmodf_frac", s->w[1], frac, {a[0]});
            emitCall(out, "dmodf_int", s->w[2], whole, {a[0]});
            out.push_back({Op::OpCompositeConstruct, {type, id, frac, whole}});
            return true;
        }
        case GLSLstd450Frexp: {
            const uint32_t exp = newId();
            emitCall(out, "dfrexp_exp", pointee(valueType(a[1])), exp, {a[0]});
            out.push_back({Op::OpStore, {a[1], exp}});
            return call("dfrexp_mant", {a[0]});
        }
        case GLSLstd450FrexpStruct: {
            const Inst* s = typeDef(type);
            const uint32_t mant = newId(), exp = newId();
            emitCall(out, "dfrexp_mant", s->w[1], mant, {a[0]});
            emitCall(out, "dfrexp_exp", s->w[2], exp, {a[0]});
            out.push_back({Op::OpCompositeConstruct, {type, id, mant, exp}});
            return true;
        }
        default:
            return fail("GLSL.std.450 instruction " + std::to_string(inst) + " on doubles");
        }
    }
    case Op::OpSpecConstantOp:
        if (resultDouble || operandsDouble(3)) return fail("specialization constant operations on doubles");
        break;
    default:
        break;
    }
    out.push_back(in);
    return true;
}

bool Lowering::lowerFunctions() {
    // Row-major matrix members: access chains that reach a whole matrix or a
    // column end at the matrix's row array, read through a transpose.
    std::unordered_map<uint32_t, uint32_t> rowPointers;  // pointer id -> row-array type it points to
    std::unordered_map<uint32_t, uint32_t> columnLoads;  // pointer id -> column index id (column of rows at the pointer)
    std::vector<Inst> out;
    // Interface conversions (fresh ids for every place they are emitted).
    // Copies an interface block member by member between the private copy
    // and the Metal-facing block (inputs: into the copy).
    auto convertBlock = [&](std::vector<Inst>& code, const Interface& itf) {
        const uint32_t storage = itf.input ? spv::StorageClassInput : spv::StorageClassOutput;
        for (size_t k = 0; k < itf.members.size(); ++k) {
            const uint32_t index = uintConst(static_cast<uint32_t>(k));
            const uint32_t from = newId(), to = newId(), loaded = newId();
            const uint32_t fromType = itf.input ? itf.mappedMembers[k] : itf.members[k];
            const uint32_t toType = itf.input ? itf.members[k] : itf.mappedMembers[k];
            code.push_back({Op::OpAccessChain, {pointerTo(itf.input ? storage : spv::StorageClassPrivate, fromType), from,
                                                itf.input ? itf.vars[0] : itf.privateVar, index}});
            valueTypes_[from] = pointerTo(itf.input ? storage : spv::StorageClassPrivate, fromType);
            // A block member that is an array (a mapped double) is a native
            // array in MSL: it moves element by element (SPIRV-Cross assigns
            // whole arrays, which MSL rejects for native arrays).
            if (itf.input) {
                const uint32_t elements = loadElements(code, from, fromType);
                code.push_back({Op::OpCopyObject, {fromType, loaded, elements}});
            } else {
                code.push_back({Op::OpLoad, {fromType, loaded, from}});
            }
            valueTypes_[loaded] = fromType;
            uint32_t value = loaded;
            if (fromType != toType) {
                std::vector<uint32_t> words;
                size_t at = 0;
                if (itf.input) {
                    unpackInterface(code, fromType, loaded, words);
                    value = fromWords(code, toType, words, at);
                } else {
                    toWords(code, fromType, loaded, words);
                    value = packInterface(code, toType, words, at);
                }
            }
            code.push_back({Op::OpAccessChain, {pointerTo(itf.input ? spv::StorageClassPrivate : storage, toType), to,
                                                itf.input ? itf.privateVar : itf.vars[0], index}});
            valueTypes_[to] = pointerTo(itf.input ? spv::StorageClassPrivate : storage, toType);
            if (itf.input) code.push_back({Op::OpStore, {to, value}});
            else storeElements(code, to, toType, value);
        }
    };
    auto convertInputs = [&](std::vector<Inst>& code) {
        for (const Interface& itf : interfaces_) {
            if (!itf.input) continue;
            if (itf.block) {
                convertBlock(code, itf);
                continue;
            }
            std::vector<uint32_t> words;
            for (size_t p = 0; p < itf.vars.size(); ++p) {
                const uint32_t loaded = newId();
                code.push_back({Op::OpLoad, {itf.types[p], loaded, itf.vars[p]}});
                unpackInterface(code, itf.types[p], loaded, words);
                for (uint32_t z = 0; z < itf.padAfter[p]; ++z) words.push_back(uintConst(0));
            }
            size_t at = 0;
            const uint32_t value = fromWords(code, itf.type, words, at);
            code.push_back({Op::OpStore, {itf.privateVar, value}});
        }
    };
    auto convertOutputs = [&](std::vector<Inst>& code) {
        for (const Interface& itf : interfaces_) {
            if (itf.input) continue;
            if (itf.block) {
                convertBlock(code, itf);
                continue;
            }
            const uint32_t loaded = newId();
            code.push_back({Op::OpLoad, {itf.type, loaded, itf.privateVar}});
            std::vector<uint32_t> words;
            toWords(code, itf.type, loaded, words);
            size_t at = 0;
            const uint32_t value = packInterface(code, itf.types[0], words, at);
            code.push_back({Op::OpStore, {itf.vars[0], value}});
        }
    };
    uint32_t currentFunction = 0;
    bool firstBlock = false;
    size_t varsAt = 0;  // where the current function's first block starts in `out`
    for (size_t i = 0; i < insts_.size(); ++i) {
        const Inst& in = insts_[i];
        if (removed_.count(i)) continue;  // dropped (all in the global section)
        if (in.op == Op::OpFunction) {
            currentFunction = in.w[1];
            firstBlock = true;
            varsAt = 0;
        }
        if (in.op == Op::OpLabel && currentFunction && !varsAt) {
            out.push_back(in);
            varsAt = out.size();
            continue;
        }
        if (!currentFunction) {
            out.push_back(in);
            continue;
        }
        // Entry point: inputs are converted after the first block's variables.
        if (firstBlock && currentFunction == entryFunction_ && in.op != Op::OpVariable && in.op != Op::OpLabel &&
            in.op != Op::OpFunction && in.op != Op::OpFunctionParameter && in.op != Op::OpLine && in.op != Op::OpNoLine) {
            convertInputs(out);
            firstBlock = false;
        }
        if (currentFunction == entryFunction_ && (in.op == Op::OpReturn || in.op == Op::OpReturnValue)) convertOutputs(out);
        if (in.op == Op::OpFunctionEnd) {
            currentFunction = 0;
            out.insert(out.begin() + static_cast<long>(varsAt), pendingVars_.begin(), pendingVars_.end());
            pendingVars_.clear();
        }
        // Row-major access chains.
        if (in.op == Op::OpAccessChain || in.op == Op::OpInBoundsAccessChain) {
            uint32_t t = pointee(valueType(in.w[2]));
            bool viaRows = false;
            size_t matrixLevel = 0;
            if (auto rp = rowPointers.find(in.w[2]); rp != rowPointers.end()) {
                t = rp->second;
                viaRows = true;
                matrixLevel = 3;
            }
            for (size_t k = 3; k < in.w.size() && !viaRows; ++k) {
                const Inst* d = typeDef(t);
                if (!d) break;
                if (d->op == Op::OpTypeStruct) {
                    const uint32_t member = constValues_[in.w[k]];
                    auto rm = rowMajorMembers_.find({t, member});
                    t = d->w[member + 1];
                    if (rm != rowMajorMembers_.end()) {
                        // Follow the (rebuilt) arrays down to the row array.
                        size_t j = k + 1;
                        while (j < in.w.size() && !rowArrayOf_.count(t)) {
                            t = typeDef(t)->w[1];
                            ++j;
                        }
                        if (!rowArrayOf_.count(t)) return fail("access to an array of row-major double matrices");
                        viaRows = true;
                        matrixLevel = j;
                        break;
                    }
                } else if (d->op == Op::OpTypeArray || d->op == Op::OpTypeRuntimeArray || d->op == Op::OpTypeVector) {
                    t = d->w[1];
                } else {
                    break;
                }
            }
            if (viaRows) {
                const uint32_t storage = storageOf(valueType(in.w[2]));
                const size_t extra = in.w.size() - matrixLevel;  // indices below the matrix: 0, 1 (column) or 2
                Inst chain = in;
                if (extra == 2) {
                    std::swap(chain.w[matrixLevel], chain.w[matrixLevel + 1]);  // [col][row] -> [row][col]
                    out.push_back(chain);
                    continue;
                }
                if (extra > 2) return fail("access below a row-major double matrix element");
                const uint32_t column = extra == 1 ? in.w[matrixLevel] : 0;
                chain.w.resize(matrixLevel);
                chain.w[0] = pointerTo(storage, t);
                valueTypes_[in.w[1]] = chain.w[0];
                rowPointers[in.w[1]] = t;
                if (column) columnLoads[in.w[1]] = column;
                if (chain.w.size() == 3) {
                    // No index left: the pointer is the base itself.
                    out.push_back({Op::OpCopyObject, {chain.w[0], in.w[1], in.w[2]}});
                } else {
                    out.push_back(chain);
                }
                continue;
            }
        }
        if (in.op == Op::OpLoad && rowPointers.count(in.w[2])) {
            const uint32_t rowsType = rowPointers[in.w[2]];
            const uint32_t rows = newId();
            out.push_back({Op::OpLoad, {rowsType, rows, in.w[2]}});
            valueTypes_[rows] = rowsType;
            if (auto c = columnLoads.find(in.w[2]); c != columnLoads.end())
                emitCall(out, "dcolumn", in.w[0], in.w[1], {rows, c->second});
            else
                emitCall(out, "dtranspose", in.w[0], in.w[1], {rows});
            continue;
        }
        for (size_t k = 0; k < in.w.size(); ++k)
            if (rowPointers.count(in.w[k]) && !(hasResult(in.op, nullptr) && k <= 1))
                return fail("unsupported use of a row-major double matrix");
        // Whole double matrices (now arrays) in threadgroup memory move column
        // by column: SPIRV-Cross's array copies into and out of threadgroup
        // memory do not take its spvUnsafeArray members.
        if ((in.op == Op::OpStore || in.op == Op::OpLoad) &&
            storageOf(valueType(in.op == Op::OpStore ? in.w[0] : in.w[2])) == spv::StorageClassWorkgroup) {
            const uint32_t pointer = in.op == Op::OpStore ? in.w[0] : in.w[2];
            const uint32_t type = pointee(valueType(pointer));
            const Inst* d = typeDef(type);
            if (d && d->op == Op::OpTypeArray && containsDouble(type)) {
                if (in.op == Op::OpStore)
                    storeElements(out, pointer, type, in.w[1]);
                else
                    out.push_back({Op::OpCopyObject, {type, in.w[1], loadElements(out, pointer, type)}});
                continue;
            }
        }
        if (in.op == Op::OpLoad) {
            const uint32_t storage = storageOf(valueType(in.w[2]));
            if (storage == spv::StorageClassUniform || storage == spv::StorageClassStorageBuffer ||
                storage == spv::StorageClassPushConstant) {
                // So do arrays of doubles loaded from buffers: SPIRV-Cross
                // copies a block's native arrays into spvUnsafeArray values by
                // plain assignment, which MSL rejects.
                const uint32_t type = pointee(valueType(in.w[2]));
                const Inst* d = typeDef(type);
                if (d && d->op == Op::OpTypeArray && containsDouble(type)) {
                    out.push_back({Op::OpCopyObject, {type, in.w[1], loadElements(out, in.w[2], type)}});
                    continue;
                }
                bufferLoads_.insert(in.w[1]);
            }
        }
        if (!lowerInstruction(in, out)) return false;
    }
    insts_ = std::move(out);
    removed_.clear();
    return ok_;
}

bool Lowering::run() {
    if (spirv_.size() < 5) return true;
    parse();
    if (!f64_) return true;
    uintType();  // placed first, before anything that needs it
    transformTypes();
    if (!ok_) return false;
    if (!rewriteInterfaces()) return false;
    if (!lowerFunctions()) return false;
    spirv_ = serialize();
    return ok_;
}

// ------------------------------------------------------------ MSL finish

// Splits a parameter list at top-level commas (template arguments nest).
std::vector<std::string> splitParameters(const std::string& list) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (char c : list) {
        if (c == '<' || c == '(' || c == '[') ++depth;
        if (c == '>' || c == ')' || c == ']') --depth;
        if (c == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

std::string lastIdentifier(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && !(std::isalnum(static_cast<unsigned char>(s[end - 1])) || s[end - 1] == '_')) --end;
    size_t begin = end;
    while (begin > 0 && (std::isalnum(static_cast<unsigned char>(s[begin - 1])) || s[begin - 1] == '_')) --begin;
    return s.substr(begin, end - begin);
}

}  // namespace

bool usesFp64(const std::vector<uint32_t>& spirv) {
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16;
        if (!count) break;
        if ((spirv[i] & 0xffff) == static_cast<uint32_t>(Op::OpTypeFloat) && count >= 3 && spirv[i + 2] == 64) return true;
        i += count;
    }
    return false;
}

bool lowerFp64(std::vector<uint32_t>& spirv, std::string& log) {
    if (!usesFp64(spirv)) return true;
    Lowering lowering(spirv, log);
    return lowering.run();
}

const std::string& fp64MslLibrary() {
    static const std::string library = std::string(kMslPrelude) + kSoftCore + "\n" + kLibraryParts[0];
    return library;
}

std::string finishFp64Msl(const std::string& msl) {
    if (msl.find("wmgl_s0_") == std::string::npos) return msl;
    std::string out = msl;
    size_t firstStub = std::string::npos;
    // Each stub "... wmgl_s<N>_<lib>(params)\n{ ... }" gets the body
    // "{ return wmgl_<lib>(param names); }".
    for (size_t at = out.find("wmgl_s"); at != std::string::npos; at = out.find("wmgl_s", at + 1)) {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(out[at - 1])) || out[at - 1] == '_')) continue;
        size_t p = at + 6;
        if (p >= out.size() || !std::isdigit(static_cast<unsigned char>(out[p]))) continue;
        while (p < out.size() && std::isdigit(static_cast<unsigned char>(out[p]))) ++p;
        if (out[p] != '_') continue;
        const size_t libStart = p + 1;
        size_t libEnd = libStart;
        while (libEnd < out.size() && (std::isalnum(static_cast<unsigned char>(out[libEnd])) || out[libEnd] == '_')) ++libEnd;
        if (out[libEnd] != '(') continue;
        // A definition: the parameter list is followed by a line with '{'.
        int depth = 0;
        size_t close = libEnd;
        for (; close < out.size(); ++close) {
            if (out[close] == '(') ++depth;
            if (out[close] == ')' && --depth == 0) break;
        }
        const size_t brace = out.find_first_not_of(" \t\n", close + 1);
        if (brace == std::string::npos || out[brace] != '{') continue;  // a call
        size_t end = brace;
        depth = 0;
        for (; end < out.size(); ++end) {
            if (out[end] == '{') ++depth;
            if (out[end] == '}' && --depth == 0) break;
        }
        std::string args;
        for (const std::string& param : splitParameters(out.substr(libEnd + 1, close - libEnd - 1))) {
            if (!args.empty()) args += ", ";
            args += lastIdentifier(param);
        }
        const std::string body = "{\n    return wmgl_" + out.substr(libStart, libEnd - libStart) + "(" + args + ");\n}";
        out.replace(brace, end - brace + 1, body);
        // The definition's first line starts the library insertion point.
        const size_t lineStart = out.rfind('\n', at);
        size_t defStart = lineStart == std::string::npos ? 0 : lineStart + 1;
        const size_t prevLine = defStart >= 2 ? out.rfind('\n', defStart - 2) : std::string::npos;
        const std::string previous = out.substr(prevLine == std::string::npos ? 0 : prevLine + 1,
                                                defStart - (prevLine == std::string::npos ? 0 : prevLine + 1));
        if (previous.find("static inline") != std::string::npos || previous.find("template") != std::string::npos)
            defStart = prevLine == std::string::npos ? 0 : prevLine + 1;
        if (firstStub == std::string::npos || defStart < firstStub) firstStub = defStart;
        at = brace;
    }
    if (firstStub == std::string::npos) return out;
    std::string library = fp64MslLibrary();
    if (out.find("struct spvUnsafeArray") != std::string::npos) library += kLibraryParts[1];
    out.insert(firstStub, library + "\n");
    return out;
}

}  // namespace wmgl::shader

#include "core/compat/arb_program.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "core/gl/types.h"

namespace wmgl::compat::arb {

// ------------------------------------------------------------ IR

// Inputs whose GLSL depends on the draw's state (EmitOptions).
enum class Input : uint8_t {
    None,
    // vertex: a conventional attribute (index = ConventionalAttrib - kConvVertex) or vertex.attrib[index]
    Conventional, Generic,
    // fragment
    Color, SecondaryColor, TexCoord, FogCoord, Position,
};

struct Operand {
    enum Base : uint8_t { Temp, Address, In, Static, Array } base = Temp;
    int index = 0;            // Temp/Address: register; In: unit or attribute; Array: array
    Input input = Input::None;
    std::string expr;         // Static: a vec4 GLSL expression
    // Array element: constant `offset`, or relative to address register
    // `relative` (component x) when relative >= 0.
    int offset = 0, relative = -1;
    uint8_t swizzle[4] = {0, 1, 2, 3};
    bool negate = false;
    // SWZ: per component 0-3 a source component, 4 zero, 5 one; negated per component.
    bool extended = false;
    uint8_t extSelect[4] = {0, 1, 2, 3};
    bool extNegate[4] = {};
};

enum class Output : uint8_t {
    None,
    // vertex
    Position, FrontColor, FrontSecondary, BackColor, BackSecondary, FogCoord, PointSize, TexCoord,
    // fragment
    Color, Depth,
};

struct Dest {
    enum Base : uint8_t { Temp, Address, Out, None } base = None;
    int index = 0;  // Temp/Address register; Out: texcoord unit or colour output
    Output output = Output::None;
    uint8_t mask = 0xf;
};

enum class Op : uint8_t {
    ABS, ADD, ARL, CMP, COS, DP3, DP4, DPH, DST, EX2, EXP, FLR, FRC, KIL, LG2, LIT, LOG, LRP, MAD, MAX, MIN, MOV, MUL,
    POW, RCP, RSQ, SCS, SGE, SIN, SLT, SUB, SWZ, TEX, TXB, TXP, XPD,
};

struct Instruction {
    Op op = Op::MOV;
    bool saturate = false;
    Dest dst;
    Operand src[3];
    int unit = 0;  // TEX/TXB/TXP
    TexTarget target = TexTarget::None;
};

struct ArrayDecl {
    std::vector<std::string> elements;  // vec4 GLSL expressions
    // All elements are program.env[base..] or program.local[base..]:
    // indexing reads the uniform array directly.
    enum Range : uint8_t { None, Env, Local } range = None;
    int base = 0;
};

struct Body {
    int temps = 0, addresses = 0;
    std::vector<ArrayDecl> arrays;
    std::vector<Instruction> code;
};

namespace {

// ------------------------------------------------------------ lexer

struct Token {
    enum Type : uint8_t { End, Ident, Number, Punct } type = End;
    std::string text;
    int pos = 0;
};

class Lexer {
public:
    Lexer(const char* s, size_t n, size_t start) : s_(s), n_(n), p_(start) {}

    Token next() {
        skip();
        Token t;
        t.pos = static_cast<int>(p_);
        if (p_ >= n_) return t;
        const char c = s_[p_];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            size_t e = p_ + 1;
            while (e < n_ && (std::isalnum(static_cast<unsigned char>(s_[e])) || s_[e] == '_' || s_[e] == '$')) ++e;
            t.type = Token::Ident;
            t.text.assign(s_ + p_, e - p_);
            p_ = e;
            prevValue_ = true;
            return t;
        }
        const bool fractionStart = c == '.' && p_ + 1 < n_ && std::isdigit(static_cast<unsigned char>(s_[p_ + 1])) && !prevValue_;
        if (std::isdigit(static_cast<unsigned char>(c)) || fractionStart) {
            size_t e = p_;
            while (e < n_ && std::isdigit(static_cast<unsigned char>(s_[e]))) ++e;
            // A '.' ends an integer when a second '.' follows (the ".." of a range).
            if (e < n_ && s_[e] == '.' && !(e + 1 < n_ && s_[e + 1] == '.')) {
                ++e;
                while (e < n_ && std::isdigit(static_cast<unsigned char>(s_[e]))) ++e;
            }
            if (e < n_ && (s_[e] == 'e' || s_[e] == 'E')) {
                size_t x = e + 1;
                if (x < n_ && (s_[x] == '+' || s_[x] == '-')) ++x;
                if (x < n_ && std::isdigit(static_cast<unsigned char>(s_[x]))) {
                    while (x < n_ && std::isdigit(static_cast<unsigned char>(s_[x]))) ++x;
                    e = x;
                }
            }
            t.type = Token::Number;
            t.text.assign(s_ + p_, e - p_);
            p_ = e;
            prevValue_ = true;
            return t;
        }
        t.type = Token::Punct;
        if (c == '.' && p_ + 1 < n_ && s_[p_ + 1] == '.') {
            t.text = "..";
            p_ += 2;
        } else {
            t.text.assign(1, c);
            ++p_;
        }
        prevValue_ = c == ']';
        return t;
    }

private:
    void skip() {
        while (p_ < n_) {
            const char c = s_[p_];
            if (c == '#') {
                while (p_ < n_ && s_[p_] != '\n' && s_[p_] != '\r') ++p_;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++p_;
            } else {
                break;
            }
        }
    }
    const char* s_;
    size_t n_, p_;
    bool prevValue_ = false;  // the last token was a name, number or ']' ('.' then starts a member)
};

struct ParseError {
    int pos;
    std::string message;
};

std::string fmt(double v) {
    if (std::isinf(v)) return v < 0 ? "(-1.0/0.0)" : "(1.0/0.0)";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string vec4(const double v[4]) { return "vec4(" + fmt(v[0]) + ", " + fmt(v[1]) + ", " + fmt(v[2]) + ", " + fmt(v[3]) + ")"; }

// Row `r` of a GLSL (column-major) matrix expression.
std::string row(const std::string& m, int r) {
    const std::string i = "[" + std::to_string(r) + "]";
    return "vec4(" + m + "[0]" + i + ", " + m + "[1]" + i + ", " + m + "[2]" + i + ", " + m + "[3]" + i + ")";
}

// ------------------------------------------------------------ parser

}  // namespace

const char* envName(Kind kind, bool env) {
    if (kind == Kind::Vertex) return env ? "arb_vpEnv" : "arb_vpLocal";
    return env ? "arb_fpEnv" : "arb_fpLocal";
}

namespace {

struct Symbol {
    enum Kind : uint8_t { Temp, Address, Attrib, Param, ParamArray, Result } kind;
    int index = 0;           // Temp/Address register, ParamArray array
    Input input = Input::None;
    int inputIndex = 0;      // Attrib
    std::string expr;        // Param (static)
    Output output = Output::None;
    int outputIndex = 0;     // Output
};

class Parser {
public:
    Parser(Kind kind, const char* s, size_t n, Parsed& out) : kind_(kind), lex_(s, n, 10), out_(out) {
        body_ = std::make_shared<Body>();
        advance();
    }

    void run() {
        while (true) {
            if (tok_.type == Token::Ident && tok_.text == "END") break;
            if (tok_.type == Token::End) fail("END expected");
            statement();
        }
        // Anything after END is ignored.
        if (out_.instructions > Limits::kInstructions) failAt(0, "too many instructions");
        if (body_->temps > Limits::kTemporaries) failAt(0, "too many temporaries");
        if (out_.parameters > Limits::kParameters) failAt(0, "too many parameters");
        if (kind_ == Kind::Vertex && out_.positionInvariant && out_.positionWritten)
            failAt(0, "result.position written by a position-invariant program");
        out_.temporaries = body_->temps;
        out_.addressRegisters = body_->addresses;
        out_.body = body_;
    }

private:
    bool vertex() const { return kind_ == Kind::Vertex; }

    [[noreturn]] void fail(const std::string& message) { throw ParseError{tok_.pos, message}; }
    [[noreturn]] void failAt(int pos, const std::string& message) { throw ParseError{pos, message}; }

    void advance() { tok_ = lex_.next(); }
    bool is(const char* punct) const { return tok_.type == Token::Punct && tok_.text == punct; }
    bool isIdent(const char* name) const { return tok_.type == Token::Ident && tok_.text == name; }
    void expect(const char* punct) {
        if (!is(punct)) fail(std::string("'") + punct + "' expected");
        advance();
    }
    void expectIdent(const char* name) {
        if (!isIdent(name)) fail(std::string("'") + name + "' expected");
        advance();
    }
    std::string ident() {
        if (tok_.type != Token::Ident) fail("identifier expected");
        std::string s = tok_.text;
        advance();
        return s;
    }
    int integer() {
        if (tok_.type != Token::Number || tok_.text.find_first_of(".eE") != std::string::npos) fail("integer expected");
        const long v = std::strtol(tok_.text.c_str(), nullptr, 10);
        if (v > 1 << 20) fail("integer too large");
        advance();
        return static_cast<int>(v);
    }
    // [n], with `def` when the brackets are optional and absent (def >= 0).
    int optionalIndex(int def, int limit, const char* what) {
        if (!is("[")) {
            if (def < 0) fail("'[' expected");
            return def;
        }
        advance();
        const int pos = tok_.pos;
        const int n = integer();
        expect("]");
        if (n >= limit) failAt(pos, std::string(what) + " index out of range");
        return n;
    }

    // Signed number ("-1", "+.5", "2").
    double number() {
        bool negative = false;
        if (is("-") || is("+")) {
            negative = is("-");
            advance();
        }
        if (tok_.type != Token::Number) fail("number expected");
        const double v = std::strtod(tok_.text.c_str(), nullptr);
        advance();
        return negative ? -v : v;
    }

    // { a [, b [, c [, d]]] }: missing components are (0, 0, 0, 1) defaults.
    std::string vectorConstant() {
        expect("{");
        double v[4] = {0, 0, 0, 1};
        int n = 0;
        do {
            if (n == 4) fail("too many components");
            v[n++] = number();
        } while (is(",") && (advance(), true));
        expect("}");
        return vec4(v);
    }

    static bool reserved(const std::string& s) {
        static const char* const kWords[] = {
            "ABS", "ADD", "ARL", "CMP", "COS", "DP3", "DP4", "DPH", "DST", "EX2", "EXP", "FLR", "FRC", "KIL", "LG2", "LIT",
            "LOG", "LRP", "MAD", "MAX", "MIN", "MOV", "MUL", "POW", "RCP", "RSQ", "SCS", "SGE", "SIN", "SLT", "SUB", "SWZ",
            "TEX", "TXB", "TXP", "XPD", "ADDRESS", "ALIAS", "ATTRIB", "END", "OPTION", "OUTPUT", "PARAM", "TEMP", "fragment",
            "program", "result", "state", "vertex", "texture"};
        for (const char* w : kWords)
            if (s == w) return true;
        return s.size() > 4 && s.compare(s.size() - 4, 4, "_SAT") == 0;
    }

    void declare(const std::string& name, int pos, Symbol symbol) {
        if (reserved(name)) failAt(pos, "reserved word used as a name: " + name);
        if (!symbols_.emplace(name, std::move(symbol)).second) failAt(pos, "name declared twice: " + name);
    }

    void statement() {
        const int pos = tok_.pos;
        const std::string word = ident();
        if (word == "OPTION") option(pos);
        else if (word == "ATTRIB") attribStatement();
        else if (word == "PARAM") paramStatement();
        else if (word == "TEMP") registers(Symbol::Temp);
        else if (word == "ADDRESS" && vertex()) registers(Symbol::Address);
        else if (word == "OUTPUT") outputStatement();
        else if (word == "ALIAS") aliasStatement();
        else instruction(word, pos);
        expect(";");
    }

    void option(int pos) {
        const int at = tok_.pos;
        const std::string name = ident();
        if (!options_.insert(name).second) failAt(at, "option repeated: " + name);
        if (vertex() && name == "ARB_position_invariant") {
            out_.positionInvariant = true;
        } else if (!vertex() && (name == "ARB_fog_linear" || name == "ARB_fog_exp" || name == "ARB_fog_exp2")) {
            if (out_.fog) failAt(at, "two fog options");
            out_.fog = name == "ARB_fog_linear" ? GL_LINEAR : name == "ARB_fog_exp" ? GL_EXP : GL_EXP2;
        } else if (!vertex() && (name == "ARB_precision_hint_fastest" || name == "ARB_precision_hint_nicest")) {
            if (options_.count("ARB_precision_hint_fastest") && options_.count("ARB_precision_hint_nicest"))
                failAt(at, "two precision hints");
        } else if (!vertex() && name == "ARB_draw_buffers") {
            out_.drawBuffers = true;
        } else if (!vertex() && name == "ARB_fragment_program_shadow") {
            shadow_ = true;
        } else {
            failAt(at, "unsupported option: " + name);
        }
        (void)pos;
    }

    void registers(Symbol::Kind kind) {
        do {
            const int pos = tok_.pos;
            const std::string name = ident();
            Symbol s{kind};
            if (kind == Symbol::Temp) {
                s.index = body_->temps++;
            } else {
                s.index = body_->addresses++;
                if (body_->addresses > Limits::kAddressRegisters) failAt(pos, "too many address registers");
            }
            declare(name, pos, s);
        } while (is(",") && (advance(), true));
    }

    void attribStatement() {
        const int pos = tok_.pos;
        const std::string name = ident();
        expect("=");
        Symbol s{Symbol::Attrib};
        inputBinding(s.input, s.inputIndex);
        ++out_.attribs;
        declare(name, pos, s);
    }

    void outputStatement() {
        const int pos = tok_.pos;
        const std::string name = ident();
        expect("=");
        Symbol s{Symbol::Result};
        resultBinding(s.output, s.outputIndex);
        declare(name, pos, s);
    }

    void aliasStatement() {
        const int pos = tok_.pos;
        const std::string name = ident();
        expect("=");
        const int at = tok_.pos;
        const std::string target = ident();
        auto it = symbols_.find(target);
        if (it == symbols_.end()) failAt(at, "undefined name: " + target);
        declare(name, pos, it->second);
    }

    void paramStatement() {
        const int pos = tok_.pos;
        const std::string name = ident();
        if (is("[")) {
            advance();
            int size = -1;
            if (!is("]")) size = integer();
            expect("]");
            expect("=");
            expect("{");
            ArrayDecl array;
            std::vector<std::pair<int, int>> ranges;  // (kind 1 env / 2 local, index) per element, else (0, 0)
            do {
                std::vector<std::string> items;
                std::vector<std::pair<int, int>> sources;
                paramItems(true, items, &sources);
                array.elements.insert(array.elements.end(), items.begin(), items.end());
                ranges.insert(ranges.end(), sources.begin(), sources.end());
            } while (is(",") && (advance(), true));
            expect("}");
            if (size >= 0 && size != static_cast<int>(array.elements.size()))
                failAt(pos, "array size does not match its initializers");
            if (array.elements.empty()) failAt(pos, "empty array");
            // One contiguous env or local range: index the uniform array.
            if (ranges[0].first) {
                bool contiguous = true;
                for (size_t i = 1; i < ranges.size(); ++i)
                    contiguous &= ranges[i].first == ranges[0].first && ranges[i].second == ranges[0].second + static_cast<int>(i);
                if (contiguous) {
                    array.range = ranges[0].first == 1 ? ArrayDecl::Env : ArrayDecl::Local;
                    array.base = ranges[0].second;
                }
            }
            out_.parameters += static_cast<int>(array.elements.size());
            Symbol s{Symbol::ParamArray};
            s.index = static_cast<int>(body_->arrays.size());
            body_->arrays.push_back(std::move(array));
            declare(name, pos, s);
        } else {
            expect("=");
            std::vector<std::string> items;
            paramItems(false, items, nullptr);
            ++out_.parameters;
            Symbol s{Symbol::Param};
            s.expr = items[0];
            declare(name, pos, s);
        }
    }

    // A PARAM initializer: a constant, a state binding or program parameter
    // (several vectors when `multiple`: a matrix, rows or a range).
    void paramItems(bool multiple, std::vector<std::string>& items, std::vector<std::pair<int, int>>* sources) {
        auto source = [&](int kind, int index) {
            if (sources) sources->emplace_back(kind, index);
        };
        if (is("{")) {
            items.push_back(vectorConstant());
            source(0, 0);
            return;
        }
        if (tok_.type == Token::Number || is("-") || is("+")) {
            const double v = number();
            const double c[4] = {v, v, v, v};
            items.push_back(vec4(c));
            source(0, 0);
            return;
        }
        const int pos = tok_.pos;
        const std::string head = ident();
        if (head == "state") {
            expect(".");
            const size_t before = items.size();
            stateBinding(multiple, items);
            for (size_t i = before; i < items.size(); ++i) source(0, 0);
        } else if (head == "program") {
            expect(".");
            const int at = tok_.pos;
            const std::string which = ident();
            if (which != "env" && which != "local") failAt(at, "program.env or program.local expected");
            const bool env = which == "env";
            const int limit = env ? Limits::kEnvParameters : Limits::kLocalParameters;
            expect("[");
            const int ipos = tok_.pos;
            const int first = integer();
            int last = first;
            if (multiple && is("..")) {
                advance();
                last = integer();
            }
            expect("]");
            if (first >= limit || last >= limit || last < first) failAt(ipos, "parameter index out of range");
            for (int i = first; i <= last; ++i) {
                items.push_back(param(env, i));
                source(env ? 1 : 2, i);
            }
        } else {
            failAt(pos, "parameter binding expected");
        }
    }

    std::string param(bool env, int index) {
        int& count = env ? out_.envCount : out_.localCount;
        count = std::max(count, index + 1);
        return std::string(envName(kind_, env)) + "[" + std::to_string(index) + "]";
    }

    // state.<...> after "state.".
    void stateBinding(bool multiple, std::vector<std::string>& items) {
        const int pos = tok_.pos;
        const std::string what = ident();
        auto face = [&]() -> std::string {  // [.front|.back] (only when followed by them)
            if (is(".")) {
                // Peek: ".front"/".back" select the face; anything else is the property.
                Lexer save = lex_;
                Token t = tok_;
                advance();
                if (isIdent("front") || isIdent("back")) {
                    const std::string f = tok_.text;
                    advance();
                    return f;
                }
                lex_ = save;
                tok_ = t;
            }
            return "front";
        };
        if (what == "material") {
            const std::string f = face();
            const std::string mat = f == "back" ? "gl_BackMaterial" : "gl_FrontMaterial";
            expect(".");
            const int at = tok_.pos;
            const std::string prop = ident();
            if (prop == "ambient" || prop == "diffuse" || prop == "specular" || prop == "emission")
                items.push_back(mat + "." + prop);
            else if (prop == "shininess")
                items.push_back("vec4(" + mat + ".shininess, 0.0, 0.0, 1.0)");
            else
                failAt(at, "material property expected");
        } else if (what == "light") {
            const int n = optionalIndex(-1, 8, "light");
            const std::string L = "gl_LightSource[" + std::to_string(n) + "]";
            expect(".");
            const int at = tok_.pos;
            const std::string prop = ident();
            if (prop == "ambient" || prop == "diffuse" || prop == "specular" || prop == "position") {
                items.push_back(L + "." + prop);
            } else if (prop == "attenuation") {
                items.push_back("vec4(" + L + ".constantAttenuation, " + L + ".linearAttenuation, " + L +
                                ".quadraticAttenuation, " + L + ".spotExponent)");
            } else if (prop == "spot") {
                expect(".");
                expectIdent("direction");
                items.push_back("vec4(" + L + ".spotDirection, " + L + ".spotCosCutoff)");
            } else if (prop == "half") {
                items.push_back("vec4(" + L + ".halfVector.xyz, 1.0)");
            } else {
                failAt(at, "light property expected");
            }
        } else if (what == "lightmodel") {
            expect(".");
            const int at = tok_.pos;
            std::string prop = ident();
            std::string f = "front";
            if (prop == "front" || prop == "back") {
                f = prop;
                expect(".");
                prop = ident();
            }
            if (prop == "ambient" && f == "front") items.push_back("gl_LightModel.ambient");
            else if (prop == "scenecolor")
                items.push_back(f == "back" ? "gl_BackLightModelProduct.sceneColor" : "gl_FrontLightModelProduct.sceneColor");
            else failAt(at, "light model property expected");
        } else if (what == "lightprod") {
            const int n = optionalIndex(-1, 8, "light");
            const std::string f = face();
            expect(".");
            const int at = tok_.pos;
            const std::string prop = ident();
            if (prop != "ambient" && prop != "diffuse" && prop != "specular") failAt(at, "light product property expected");
            items.push_back(std::string(f == "back" ? "gl_BackLightProduct[" : "gl_FrontLightProduct[") + std::to_string(n) +
                            "]." + prop);
        } else if (what == "texgen" && vertex()) {
            const int n = optionalIndex(0, Limits::kTexCoords, "texture coordinate");
            expect(".");
            const int at = tok_.pos;
            const std::string plane = ident();
            if (plane != "eye" && plane != "object") failAt(at, "eye or object expected");
            expect(".");
            const int cpos = tok_.pos;
            const std::string c = ident();
            if (c != "s" && c != "t" && c != "r" && c != "q") failAt(cpos, "s, t, r or q expected");
            const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c[0])));
            items.push_back(std::string(plane == "eye" ? "gl_EyePlane" : "gl_ObjectPlane") + upper + "[" + std::to_string(n) + "]");
        } else if (what == "texenv") {
            const int n = optionalIndex(0, Limits::kTexCoords, "texture unit");
            expect(".");
            expectIdent("color");
            items.push_back("gl_TextureEnvColor[" + std::to_string(n) + "]");
        } else if (what == "fog") {
            expect(".");
            const int at = tok_.pos;
            const std::string prop = ident();
            if (prop == "color") items.push_back("gl_Fog.color");
            else if (prop == "params") items.push_back("vec4(gl_Fog.density, gl_Fog.start, gl_Fog.end, gl_Fog.scale)");
            else failAt(at, "fog property expected");
        } else if (what == "clip" && vertex()) {
            const int n = optionalIndex(-1, 8, "clip plane");
            expect(".");
            expectIdent("plane");
            items.push_back("gl_ClipPlane[" + std::to_string(n) + "]");
        } else if (what == "point" && vertex()) {
            expect(".");
            const int at = tok_.pos;
            const std::string prop = ident();
            if (prop == "size")
                items.push_back("vec4(gl_Point.size, gl_Point.sizeMin, gl_Point.sizeMax, gl_Point.fadeThresholdSize)");
            else if (prop == "attenuation")
                items.push_back("vec4(gl_Point.distanceConstantAttenuation, gl_Point.distanceLinearAttenuation, "
                                "gl_Point.distanceQuadraticAttenuation, 1.0)");
            else failAt(at, "point property expected");
        } else if (what == "depth") {
            expect(".");
            expectIdent("range");
            items.push_back("vec4(gl_DepthRange.near, gl_DepthRange.far, gl_DepthRange.diff, 1.0)");
        } else if (what == "matrix") {
            matrixBinding(multiple, items);
        } else {
            failAt(pos, "state binding expected");
        }
    }

    void matrixBinding(bool multiple, std::vector<std::string>& items) {
        expect(".");
        const int pos = tok_.pos;
        const std::string name = ident();
        std::string base;  // GLSL matrix built-in without the modifier suffix
        int program = -1;
        if (name == "modelview") {
            optionalIndex(0, 1, "modelview matrix");  // vertex blending (ARB_vertex_blend) is not supported
            base = "gl_ModelViewMatrix";
        } else if (name == "projection") {
            base = "gl_ProjectionMatrix";
        } else if (name == "mvp") {
            base = "gl_ModelViewProjectionMatrix";
        } else if (name == "texture") {
            const int n = optionalIndex(0, Limits::kTexCoords, "texture matrix");
            base = "gl_TextureMatrix[" + std::to_string(n) + "]";
        } else if (name == "program") {
            program = optionalIndex(-1, Limits::kProgramMatrices, "program matrix");
        } else {
            failAt(pos, name == "palette" ? "matrix palettes are not supported" : "matrix name expected");
        }
        enum { Plain, Inverse, Transpose, InvTrans } modifier = Plain;
        int first = 0, last = 3;
        bool rows = false;
        while (is(".")) {
            advance();
            const int at = tok_.pos;
            const std::string m = ident();
            if (m == "row") {
                expect("[");
                first = last = integer();
                if (multiple && is("..")) {
                    advance();
                    last = integer();
                }
                expect("]");
                if (first > 3 || last > 3 || last < first) failAt(at, "matrix row out of range");
                rows = true;
                break;
            }
            if (modifier != Plain) failAt(at, "row expected");
            if (m == "inverse") modifier = Inverse;
            else if (m == "transpose") modifier = Transpose;
            else if (m == "invtrans") modifier = InvTrans;
            else failAt(at, "matrix modifier or row expected");
        }
        if (!rows && !multiple) failAt(pos, "a matrix row is needed here");
        std::string m;
        bool transpose = modifier == Transpose || modifier == InvTrans;
        if (program >= 0) {
            const bool inverse = modifier == Inverse || modifier == InvTrans;
            const std::string n = std::to_string(program);
            m = inverse ? "arb_pmInv[" + n + "]" : "arb_pm[" + n + "]";
            out_.programMatrices |= static_cast<uint8_t>(1u << program);
            if (inverse) out_.programMatrixInverses |= static_cast<uint8_t>(1u << program);
        } else {
            static const char* const kSuffix[] = {"", "Inverse", "Transpose", "InverseTranspose"};
            // The GLSL built-ins have every variant: index into the array ones after the suffix.
            const size_t bracket = base.find('[');
            m = bracket == std::string::npos ? base + kSuffix[modifier]
                                             : base.substr(0, bracket) + kSuffix[modifier] + base.substr(bracket);
            transpose = false;
        }
        // Row r of the transpose is column r of the matrix.
        for (int r = first; r <= last; ++r) items.push_back(transpose ? m + "[" + std::to_string(r) + "]" : row(m, r));
    }

    // vertex.* / fragment.* attribute bindings.
    void inputBinding(Input& input, int& index) {
        const int pos = tok_.pos;
        const std::string head = ident();
        if (head != (vertex() ? "vertex" : "fragment")) failAt(pos, vertex() ? "vertex binding expected" : "fragment binding expected");
        expect(".");
        const int at = tok_.pos;
        const std::string what = ident();
        if (vertex()) {
            input = Input::Conventional;
            if (what == "position") {
                index = 0;
            } else if (what == "normal") {
                index = 1;
            } else if (what == "color") {
                index = 2;
                if (is(".")) {
                    Lexer save = lex_;
                    Token t = tok_;
                    advance();
                    if (isIdent("primary") || isIdent("secondary")) {
                        index = tok_.text == "primary" ? 2 : 3;
                        advance();
                    } else {
                        lex_ = save;
                        tok_ = t;
                    }
                }
            } else if (what == "fogcoord") {
                index = 4;
            } else if (what == "texcoord") {
                index = 5 + optionalIndex(0, Limits::kTexCoords, "texture coordinate");
            } else if (what == "attrib") {
                index = optionalIndex(-1, Limits::kVertexAttribs, "vertex attribute");
                if (index == 0) {
                    index = 0;  // generic attribute 0 is the position
                } else {
                    input = Input::Generic;
                }
            } else if (what == "weight" || what == "matrixindex") {
                failAt(at, "vertex blending (ARB_vertex_blend, ARB_matrix_palette) is not supported");
            } else {
                failAt(at, "vertex attribute expected");
            }
            if (input == Input::Conventional) out_.conventionalRead |= 1u << index;
            else out_.genericRead |= 1u << index;
        } else {
            index = 0;
            if (what == "color") {
                input = Input::Color;
                if (is(".")) {
                    Lexer save = lex_;
                    Token t = tok_;
                    advance();
                    if (isIdent("primary") || isIdent("secondary")) {
                        if (tok_.text == "secondary") input = Input::SecondaryColor;
                        advance();
                    } else {
                        lex_ = save;
                        tok_ = t;
                    }
                }
                out_.colorRead[input == Input::Color ? 0 : 1] = true;
            } else if (what == "texcoord") {
                input = Input::TexCoord;
                index = optionalIndex(0, Limits::kTexCoords, "texture coordinate");
                out_.texCoordRead |= 1u << index;
            } else if (what == "fogcoord") {
                input = Input::FogCoord;
                out_.fogCoordRead = true;
            } else if (what == "position") {
                input = Input::Position;
                out_.positionRead = true;
            } else {
                failAt(at, "fragment attribute expected");
            }
        }
    }

    // result.* bindings.
    void resultBinding(Output& output, int& index) {
        const int pos = tok_.pos;
        if (!isIdent("result")) failAt(pos, "result binding expected");
        advance();
        expect(".");
        const int at = tok_.pos;
        const std::string what = ident();
        index = 0;
        auto peekIdent = [&](std::initializer_list<const char*> names) -> std::string {
            if (!is(".")) return {};
            Lexer save = lex_;
            Token t = tok_;
            advance();
            for (const char* n : names)
                if (isIdent(n)) {
                    advance();
                    return n;
                }
            lex_ = save;
            tok_ = t;
            return {};
        };
        if (vertex()) {
            if (what == "position") {
                output = Output::Position;
            } else if (what == "color") {
                const std::string first = peekIdent({"front", "back", "primary", "secondary"});
                bool back = first == "back", secondary = first == "secondary";
                if (first == "front" || first == "back") secondary = peekIdent({"primary", "secondary"}) == "secondary";
                output = back ? (secondary ? Output::BackSecondary : Output::BackColor)
                              : (secondary ? Output::FrontSecondary : Output::FrontColor);
            } else if (what == "fogcoord") {
                output = Output::FogCoord;
            } else if (what == "pointsize") {
                output = Output::PointSize;
            } else if (what == "texcoord") {
                output = Output::TexCoord;
                index = optionalIndex(0, Limits::kTexCoords, "texture coordinate");
            } else {
                failAt(at, "vertex result expected");
            }
        } else {
            if (what == "color") {
                output = Output::Color;
                if (out_.drawBuffers && is("[")) index = optionalIndex(0, 8, "colour output");
            } else if (what == "depth") {
                output = Output::Depth;
            } else {
                failAt(at, "fragment result expected");
            }
        }
    }

    // .x / .xyzw (also .rgba in fragment programs). Returns the count (1 or 4).
    int swizzle(uint8_t out[4]) {
        const int pos = tok_.pos;
        const std::string s = ident();
        if (s.size() != 1 && s.size() != 4) failAt(pos, "swizzle expected");
        bool xyzw = false, rgba = false;
        for (size_t i = 0; i < s.size(); ++i) {
            const char* p = std::strchr("xyzw", s[i]);
            const char* q = vertex() ? nullptr : std::strchr("rgba", s[i]);
            if (!s[i] || (!p && !q)) failAt(pos, "swizzle expected");
            xyzw |= p != nullptr;
            rgba |= q != nullptr;
            out[i] = static_cast<uint8_t>(p ? p - "xyzw" : q - "rgba");
        }
        if (xyzw && rgba) failAt(pos, "mixed swizzle sets");
        if (s.size() == 1) out[1] = out[2] = out[3] = out[0];
        return static_cast<int>(s.size());
    }

    uint8_t writeMask() {
        const int pos = tok_.pos;
        const std::string s = ident();
        uint8_t mask = 0;
        int last = -1;
        bool xyzw = false, rgba = false;
        for (char c : s) {
            const char* p = std::strchr("xyzw", c);
            const char* q = vertex() ? nullptr : std::strchr("rgba", c);
            if (!c || (!p && !q)) failAt(pos, "write mask expected");
            const int i = static_cast<int>(p ? p - "xyzw" : q - "rgba");
            xyzw |= p != nullptr;
            rgba |= q != nullptr;
            if (i <= last) failAt(pos, "write mask components out of order");
            last = i;
            mask |= static_cast<uint8_t>(1u << i);
        }
        if (xyzw && rgba) failAt(pos, "mixed write mask sets");
        return mask;
    }

    // A source operand; `scalar`: a scalar suffix is required.
    Operand source(bool scalar) {
        Operand o;
        if (is("-") || is("+")) {
            o.negate = is("-");
            advance();
            if (tok_.type == Token::Number) {  // a negative literal
                const double v = std::strtod(tok_.text.c_str(), nullptr);
                advance();
                const double c[4] = {v, v, v, v};
                o.base = Operand::Static;
                o.expr = vec4(c);
                return finishSource(o, scalar);
            }
        }
        const int pos = tok_.pos;
        if (is("{")) {
            o.base = Operand::Static;
            o.expr = vectorConstant();
            ++out_.parameters;
        } else if (tok_.type == Token::Number) {
            const double v = number();
            const double c[4] = {v, v, v, v};
            o.base = Operand::Static;
            o.expr = vec4(c);
            ++out_.parameters;
        } else if (isIdent("vertex") || isIdent("fragment")) {
            o.base = Operand::In;
            inputBinding(o.input, o.index);
        } else if (isIdent("state") || isIdent("program")) {
            std::vector<std::string> items;
            paramItems(false, items, nullptr);
            o.base = Operand::Static;
            o.expr = items[0];
            ++out_.parameters;
        } else {
            const std::string name = ident();
            auto it = symbols_.find(name);
            if (it == symbols_.end()) failAt(pos, "undefined name: " + name);
            const Symbol& s = it->second;
            switch (s.kind) {
            case Symbol::Temp: o.base = Operand::Temp; o.index = s.index; break;
            case Symbol::Attrib: o.base = Operand::In; o.input = s.input; o.index = s.inputIndex; break;
            case Symbol::Param: o.base = Operand::Static; o.expr = s.expr; break;
            case Symbol::ParamArray: {
                o.base = Operand::Array;
                o.index = s.index;
                const int size = static_cast<int>(body_->arrays[static_cast<size_t>(s.index)].elements.size());
                expect("[");
                const int ipos = tok_.pos;
                if (tok_.type == Token::Ident) {
                    if (!vertex()) failAt(ipos, "relative addressing is not available in fragment programs");
                    const std::string a = ident();
                    auto at = symbols_.find(a);
                    if (at == symbols_.end() || at->second.kind != Symbol::Address) failAt(ipos, "address register expected");
                    o.relative = at->second.index;
                    expect(".");
                    expectIdent("x");
                    if (is("+") || is("-")) {
                        const bool minus = is("-");
                        advance();
                        const int opos = tok_.pos;
                        const int n = integer();
                        if (n > 255) failAt(opos, "address offset out of range");
                        o.offset = minus ? -n : n;
                    }
                } else {
                    o.offset = integer();
                    if (o.offset >= size) failAt(ipos, "array index out of range");
                }
                expect("]");
                break;
            }
            case Symbol::Address: failAt(pos, "address register used as a source");
            case Symbol::Result: failAt(pos, "result used as a source");
            }
        }
        return finishSource(o, scalar);
    }

    Operand finishSource(Operand& o, bool scalar) {
        if (is(".")) {
            advance();
            const int pos = tok_.pos;
            const int n = swizzle(o.swizzle);
            if (scalar && n != 1) failAt(pos, "scalar suffix expected");
        } else if (scalar) {
            fail("scalar suffix expected");
        }
        return o;
    }

    // SWZ's extended swizzle: four of [-](0|1|x|y|z|w).
    void extendedSwizzle(Operand& o) {
        o.extended = true;
        for (int i = 0; i < 4; ++i) {
            expect(",");
            bool negate = false;
            if (is("-") || is("+")) {
                negate = is("-");
                advance();
            }
            const int pos = tok_.pos;
            uint8_t select = 0;
            if (tok_.type == Token::Number && (tok_.text == "0" || tok_.text == "1")) {
                select = tok_.text == "0" ? 4 : 5;
                advance();
            } else {
                const std::string c = ident();
                const char* p = c.size() == 1 ? std::strchr("xyzw", c[0]) : nullptr;
                const char* q = c.size() == 1 && !vertex() ? std::strchr("rgba", c[0]) : nullptr;
                if (!p && !q) failAt(pos, "extended swizzle component expected");
                select = static_cast<uint8_t>(p ? p - "xyzw" : q - "rgba");
            }
            o.extSelect[i] = select;
            o.extNegate[i] = negate;
        }
    }

    Dest dest(bool address) {
        Dest d;
        const int pos = tok_.pos;
        if (isIdent("result")) {
            d.base = Dest::Out;
            resultBinding(d.output, d.index);
        } else {
            const std::string name = ident();
            auto it = symbols_.find(name);
            if (it == symbols_.end()) failAt(pos, "undefined name: " + name);
            const Symbol& s = it->second;
            if (address) {
                if (s.kind != Symbol::Address) failAt(pos, "address register expected");
                d.base = Dest::Address;
                d.index = s.index;
            } else if (s.kind == Symbol::Temp) {
                d.base = Dest::Temp;
                d.index = s.index;
            } else if (s.kind == Symbol::Result) {
                d.base = Dest::Out;
                d.output = s.output;
                d.index = s.outputIndex;
            } else {
                failAt(pos, "temporary or result expected");
            }
        }
        if (address != (d.base == Dest::Address)) failAt(pos, "address register expected");
        if (is(".")) {
            advance();
            d.mask = writeMask();
        }
        if (address && d.mask != 1) failAt(pos, "ARL writes the x component");
        if (d.base == Dest::Out) noteOutput(d);
        return d;
    }

    void noteOutput(const Dest& d) {
        switch (d.output) {
        case Output::Position: out_.positionWritten = true; break;
        case Output::FrontColor: out_.colorWritten[0] = true; break;
        case Output::FrontSecondary: out_.colorWritten[1] = true; break;
        case Output::BackColor: out_.colorWritten[2] = true; break;
        case Output::BackSecondary: out_.colorWritten[3] = true; break;
        case Output::FogCoord: out_.fogCoordWritten = true; break;
        case Output::PointSize: out_.pointSizeWritten = true; break;
        case Output::TexCoord: out_.texCoordWritten |= 1u << d.index; break;
        case Output::Color: out_.colorOutputs |= 1u << d.index; break;
        case Output::Depth: out_.depthWritten = true; break;
        case Output::None: break;
        }
    }

    void instruction(const std::string& word, int pos) {
        struct Info {
            const char* name;
            Op op;
            char shape;  // v: vector srcs, s: scalar srcs, t: texture, k: KIL, a: ARL, w: SWZ
            int sources;
            uint8_t stages;  // 1 vertex, 2 fragment
        };
        static const Info kOps[] = {
            {"ABS", Op::ABS, 'v', 1, 3}, {"ADD", Op::ADD, 'v', 2, 3}, {"ARL", Op::ARL, 'a', 1, 1}, {"CMP", Op::CMP, 'v', 3, 2},
            {"COS", Op::COS, 's', 1, 2}, {"DP3", Op::DP3, 'v', 2, 3}, {"DP4", Op::DP4, 'v', 2, 3}, {"DPH", Op::DPH, 'v', 2, 3},
            {"DST", Op::DST, 'v', 2, 3}, {"EX2", Op::EX2, 's', 1, 3}, {"EXP", Op::EXP, 's', 1, 1}, {"FLR", Op::FLR, 'v', 1, 3},
            {"FRC", Op::FRC, 'v', 1, 3}, {"KIL", Op::KIL, 'k', 1, 2}, {"LG2", Op::LG2, 's', 1, 3}, {"LIT", Op::LIT, 'v', 1, 3},
            {"LOG", Op::LOG, 's', 1, 1}, {"LRP", Op::LRP, 'v', 3, 2}, {"MAD", Op::MAD, 'v', 3, 3}, {"MAX", Op::MAX, 'v', 2, 3},
            {"MIN", Op::MIN, 'v', 2, 3}, {"MOV", Op::MOV, 'v', 1, 3}, {"MUL", Op::MUL, 'v', 2, 3}, {"POW", Op::POW, 's', 2, 3},
            {"RCP", Op::RCP, 's', 1, 3}, {"RSQ", Op::RSQ, 's', 1, 3}, {"SCS", Op::SCS, 's', 1, 2}, {"SGE", Op::SGE, 'v', 2, 3},
            {"SIN", Op::SIN, 's', 1, 2}, {"SLT", Op::SLT, 'v', 2, 3}, {"SUB", Op::SUB, 'v', 2, 3}, {"SWZ", Op::SWZ, 'w', 1, 3},
            {"TEX", Op::TEX, 't', 1, 2}, {"TXB", Op::TXB, 't', 1, 2}, {"TXP", Op::TXP, 't', 1, 2}, {"XPD", Op::XPD, 'v', 2, 3},
        };
        std::string name = word;
        bool saturate = false;
        if (!vertex() && name.size() > 4 && name.compare(name.size() - 4, 4, "_SAT") == 0) {
            saturate = true;
            name.resize(name.size() - 4);
        }
        const Info* info = nullptr;
        for (const Info& i : kOps)
            if (name == i.name && (i.stages & (vertex() ? 1 : 2))) info = &i;
        if (!info) failAt(pos, "instruction expected: " + word);
        if (saturate && info->op == Op::KIL) failAt(pos, "KIL takes no _SAT");
        Instruction in;
        in.op = info->op;
        in.saturate = saturate;
        ++out_.instructions;
        switch (info->shape) {
        case 'k':
            in.src[0] = source(false);
            out_.kills = true;
            ++out_.texInstructions;
            break;
        case 'a':
            in.dst = dest(true);
            expect(",");
            in.src[0] = source(true);
            ++out_.aluInstructions;
            break;
        case 'w':
            in.dst = dest(false);
            expect(",");
            in.src[0] = source(false);
            if (in.src[0].swizzle[0] != 0 || in.src[0].swizzle[1] != 1 || in.src[0].swizzle[2] != 2 || in.src[0].swizzle[3] != 3)
                failAt(pos, "SWZ takes an unswizzled source");
            extendedSwizzle(in.src[0]);
            ++out_.aluInstructions;
            break;
        case 't': {
            in.dst = dest(false);
            expect(",");
            in.src[0] = source(false);
            expect(",");
            expectIdent("texture");
            const int upos = tok_.pos;
            in.unit = optionalIndex(0, Limits::kTextureUnits, "texture image unit");
            expect(",");
            const int tpos = tok_.pos;
            if (tok_.type != Token::Ident && tok_.type != Token::Number) failAt(tpos, "texture target expected");
            const std::string t = tok_.text;
            advance();
            // "1D", "2D", "3D" lex as a number followed by "D".
            std::string target = t;
            if (tok_.type == Token::Ident && tok_.pos == tpos + static_cast<int>(t.size()) && target.size() == 1 &&
                std::isdigit(static_cast<unsigned char>(target[0]))) {
                target += tok_.text;
                advance();
            }
            if (target == "1D") in.target = TexTarget::T1D;
            else if (target == "2D") in.target = TexTarget::T2D;
            else if (target == "3D") in.target = TexTarget::T3D;
            else if (target == "CUBE") in.target = TexTarget::Cube;
            else if (target == "RECT") in.target = TexTarget::Rect;
            else if (shadow_ && target == "SHADOW1D") in.target = TexTarget::Shadow1D;
            else if (shadow_ && target == "SHADOW2D") in.target = TexTarget::Shadow2D;
            else if (shadow_ && target == "SHADOWRECT") in.target = TexTarget::ShadowRect;
            else failAt(tpos, "texture target expected");
            TexTarget& used = out_.textures[in.unit];
            if (used != TexTarget::None && used != in.target) failAt(upos, "texture image unit used with two targets");
            used = in.target;
            ++out_.texInstructions;
            break;
        }
        default:
            in.dst = dest(false);
            for (int i = 0; i < info->sources; ++i) {
                expect(",");
                in.src[i] = source(info->shape == 's');
            }
            ++out_.aluInstructions;
            break;
        }
        body_->code.push_back(std::move(in));
    }

    Kind kind_;
    Lexer lex_;
    Token tok_;
    Parsed& out_;
    std::shared_ptr<Body> body_;
    std::map<std::string, Symbol> symbols_;
    std::set<std::string> options_;
    bool shadow_ = false;
};

// ------------------------------------------------------------ GLSL

const char kComp[] = "xyzw";

std::string maskSuffix(uint8_t mask) {
    std::string s;
    for (int i = 0; i < 4; ++i)
        if (mask & (1u << i)) s += kComp[i];
    return s;
}

class Emitter {
public:
    Emitter(const Parsed& p, const EmitOptions& o) : p_(p), o_(o), b_(*p.body) {}

    std::string run() {
        const bool vs = p_.kind == Kind::Vertex;
        std::string s = "#version 130\n";
        const std::string colorIn = o_.flat ? "flat in" : "in", colorOut = o_.flat ? "flat out" : "out";
        if (p_.envCount) s += std::string("uniform vec4 ") + envName(p_.kind, true) + "[" + std::to_string(p_.envCount) + "];\n";
        if (p_.localCount) s += std::string("uniform vec4 ") + envName(p_.kind, false) + "[" + std::to_string(p_.localCount) + "];\n";
        if (p_.programMatrices) s += "uniform mat4 arb_pm[8];\n";
        if (p_.programMatrixInverses) s += "uniform mat4 arb_pmInv[8];\n";
        if (vs) {
            for (const char* v : {"ff_front", "ff_frontSecondary", "ff_back", "ff_backSecondary"}) s += colorOut + " vec4 " + v + ";\n";
            for (int i = 0; i < Limits::kTexCoords; ++i) s += "out vec4 ff_tc" + std::to_string(i) + ";\n";
            s += "out float ff_fogCoord;\n";
            for (int n = 1; n < Limits::kVertexAttribs; ++n)
                if (genericUsed(n)) s += "in vec4 arb_attrib" + std::to_string(n) + ";\n";
        } else {
            if (p_.colorRead[0]) {
                s += colorIn + " vec4 ff_front;\n";
                if (o_.twoSide) s += colorIn + " vec4 ff_back;\n";
            }
            if (p_.colorRead[1]) {
                s += colorIn + " vec4 ff_frontSecondary;\n";
                if (o_.twoSide) s += colorIn + " vec4 ff_backSecondary;\n";
            }
            for (int i = 0; i < Limits::kTexCoords; ++i)
                if ((p_.texCoordRead >> i & 1u) && !(o_.pointCoordReplace >> i & 1u)) s += "in vec4 ff_tc" + std::to_string(i) + ";\n";
            if (p_.fogCoordRead || p_.fog) s += "in float ff_fogCoord;\n";
            for (int u = 0; u < Limits::kTextureUnits; ++u)
                if (p_.textures[u] != TexTarget::None)
                    s += std::string("uniform ") + samplerType(p_.textures[u]) + " ff_tex" + std::to_string(u) + ";\n";
        }
        s += "float arb_pow(float x, float y) { return y == 0.0 ? 1.0 : pow(abs(x), y); }\n"
             "vec4 arb_lit(vec4 s) {\n"
             "    float e = clamp(s.w, -128.0, 128.0);\n"
             "    return vec4(1.0, max(s.x, 0.0), s.x > 0.0 ? arb_pow(max(s.y, 0.0), e) : 0.0, 1.0);\n"
             "}\n";
        if (vs)
            s += "vec4 arb_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
                 "vec4 arb_log(float x) {\n"
                 "    float a = abs(x), e = floor(log2(a));\n"
                 "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
                 "}\n";
        s += "void main() {\n";
        for (int i = 0; i < b_.temps; ++i) s += "    vec4 t" + std::to_string(i) + " = vec4(0.0);\n";
        for (int i = 0; i < b_.addresses; ++i) s += "    ivec4 a" + std::to_string(i) + " = ivec4(0);\n";
        for (size_t i = 0; i < b_.arrays.size(); ++i) {
            const ArrayDecl& a = b_.arrays[i];
            if (a.range != ArrayDecl::None) continue;
            s += "    vec4 p" + std::to_string(i) + "[" + std::to_string(a.elements.size()) + "] = vec4[](";
            for (size_t e = 0; e < a.elements.size(); ++e) s += (e ? ", " : "") + a.elements[e];
            s += ");\n";
        }
        if (vs) {
            s += "    ff_front = vec4(0.0);\n    ff_frontSecondary = vec4(0.0);\n";
            s += "    ff_back = vec4(0.0);\n    ff_backSecondary = vec4(0.0);\n";
            for (int i = 0; i < Limits::kTexCoords; ++i) s += "    ff_tc" + std::to_string(i) + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
            s += "    ff_fogCoord = 0.0;\n";
            if (p_.positionInvariant) {
                s += "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n";
                if (o_.clipPlanes) s += "    vec4 arb_eye = gl_ModelViewMatrix * gl_Vertex;\n";
                for (int i = 0; i < 8; ++i)
                    if (o_.clipPlanes >> i & 1u)
                        s += "    gl_ClipDistance[" + std::to_string(i) + "] = dot(arb_eye, gl_ClipPlane[" + std::to_string(i) + "]);\n";
            }
        }
        for (const Instruction& in : b_.code) s += instruction(in);
        if (vs) {
            // Back colours the program does not write are its front colours.
            if (!p_.colorWritten[2]) s += "    ff_back = ff_front;\n";
            if (!p_.colorWritten[3]) s += "    ff_backSecondary = ff_frontSecondary;\n";
        } else if (p_.fog) {
            std::string f;
            switch (p_.fog) {
            case GL_LINEAR: f = "(gl_Fog.end - ff_fogCoord) * gl_Fog.scale"; break;
            case GL_EXP: f = "exp(-gl_Fog.density * ff_fogCoord)"; break;
            default: f = "exp(-(gl_Fog.density * ff_fogCoord) * (gl_Fog.density * ff_fogCoord))"; break;
            }
            const std::string color = p_.drawBuffers && (p_.colorOutputs & ~1u) ? "gl_FragData[0]" : "gl_FragColor";
            s += "    " + color + ".rgb = mix(gl_Fog.color.rgb, " + color + ".rgb, clamp(" + f + ", 0.0, 1.0));\n";
        }
        s += "}\n";
        return s;
    }

private:
    static const char* samplerType(TexTarget t) {
        switch (t) {
        case TexTarget::T1D: return "sampler1D";
        case TexTarget::T3D: return "sampler3D";
        case TexTarget::Cube: return "samplerCube";
        case TexTarget::Rect: return "sampler2DRect";
        case TexTarget::Shadow1D: return "sampler1DShadow";
        case TexTarget::Shadow2D: return "sampler2DShadow";
        case TexTarget::ShadowRect: return "sampler2DRectShadow";
        default: return "sampler2D";
        }
    }

    bool genericUsed(int n) const { return p_.genericRead >> n & 1u; }

    static std::string conventional(int c) {
        switch (c) {
        case 0: return "gl_Vertex";
        case 1: return "vec4(gl_Normal, 1.0)";
        case 2: return "gl_Color";
        case 3: return "gl_SecondaryColor";
        case 4: return "vec4(gl_FogCoord, 0.0, 0.0, 1.0)";
        default: return "gl_MultiTexCoord" + std::to_string(c - 5);
        }
    }

    std::string input(const Operand& o) const {
        switch (o.input) {
        case Input::Conventional: return conventional(o.index);
        case Input::Generic: return "arb_attrib" + std::to_string(o.index);
        case Input::Color: return o_.twoSide ? "(gl_FrontFacing ? ff_front : ff_back)" : "ff_front";
        case Input::SecondaryColor:
            return o_.twoSide ? "(gl_FrontFacing ? ff_frontSecondary : ff_backSecondary)" : "ff_frontSecondary";
        case Input::TexCoord:
            if (o_.pointCoordReplace >> o.index & 1u) return "vec4(gl_PointCoord, 0.0, 1.0)";
            return "ff_tc" + std::to_string(o.index);
        case Input::FogCoord: return "vec4(ff_fogCoord, 0.0, 0.0, 1.0)";
        case Input::Position: return "gl_FragCoord";
        case Input::None: break;
        }
        return "vec4(0.0)";
    }

    std::string base(const Operand& o) const {
        switch (o.base) {
        case Operand::Temp: return "t" + std::to_string(o.index);
        case Operand::Address: return "vec4(a" + std::to_string(o.index) + ")";
        case Operand::In: return input(o);
        case Operand::Static: return o.expr;
        case Operand::Array: {
            const ArrayDecl& a = b_.arrays[static_cast<size_t>(o.index)];
            const int size = static_cast<int>(a.elements.size());
            std::string index;
            if (o.relative >= 0) {
                // Out-of-range relative reads are undefined: clamped to the array.
                index = "clamp(a" + std::to_string(o.relative) + ".x" +
                        (o.offset ? (o.offset > 0 ? " + " : " - ") + std::to_string(std::abs(o.offset)) : std::string()) +
                        ", 0, " + std::to_string(size - 1) + ")";
            } else {
                index = std::to_string(o.offset);
            }
            if (a.range == ArrayDecl::None) return "p" + std::to_string(o.index) + "[" + index + "]";
            const std::string arr = envName(p_.kind, a.range == ArrayDecl::Env);
            if (o.relative < 0) return arr + "[" + std::to_string(a.base + o.offset) + "]";
            return arr + "[" + std::to_string(a.base) + " + " + index + "]";
        }
        }
        return "vec4(0.0)";
    }

    // The operand as a vec4 (swizzled and negated).
    std::string vec(const Operand& o) const {
        const std::string b = base(o);
        std::string v;
        if (o.extended) {
            v = "vec4(";
            for (int i = 0; i < 4; ++i) {
                const uint8_t sel = o.extSelect[i];
                std::string c = sel == 4 ? "0.0" : sel == 5 ? "1.0" : "(" + b + ")." + kComp[sel];
                if (o.extNegate[i]) c = "-" + c;
                v += (i ? ", " : "") + c;
            }
            v += ")";
        } else if (o.swizzle[0] == 0 && o.swizzle[1] == 1 && o.swizzle[2] == 2 && o.swizzle[3] == 3) {
            v = b;
        } else {
            v = "(" + b + ").";
            for (int i = 0; i < 4; ++i) v += kComp[o.swizzle[i]];
        }
        return o.negate ? "(-" + v + ")" : v;
    }

    // The operand's scalar (its replicated component).
    std::string scalar(const Operand& o) const {
        const std::string s = "(" + base(o) + ")." + kComp[o.swizzle[0]];
        return o.negate ? "(-" + s + ")" : s;
    }

    std::string texture(const Instruction& in) const {
        const std::string s = "ff_tex" + std::to_string(in.unit), c = vec(in.src[0]);
        const TexTarget t = in.target;
        const bool shadow = t == TexTarget::Shadow1D || t == TexTarget::Shadow2D || t == TexTarget::ShadowRect;
        std::string r;
        if (in.op == Op::TXP && t != TexTarget::Cube) {
            r = "textureProj(" + s + ", " + c + ")";
        } else {
            std::string coord;
            switch (t) {
            case TexTarget::T1D: coord = "(" + c + ").x"; break;
            case TexTarget::T2D: case TexTarget::Rect: coord = "(" + c + ").xy"; break;
            case TexTarget::Shadow1D: coord = "vec3((" + c + ").x, 0.0, (" + c + ").z)"; break;
            default: coord = "(" + c + ").xyz"; break;
            }
            const bool bias = in.op == Op::TXB && t != TexTarget::Rect && t != TexTarget::ShadowRect;
            r = "texture(" + s + ", " + coord + (bias ? ", (" + c + ").w" : std::string()) + ")";
        }
        return shadow ? "vec4(vec3(" + r + "), 1.0)" : r;
    }

    std::string value(const Instruction& in) const {
        const Operand* s = in.src;
        auto rep = [](const std::string& x) { return "vec4(" + x + ")"; };
        switch (in.op) {
        case Op::ABS: return "abs(" + vec(s[0]) + ")";
        case Op::ADD: return vec(s[0]) + " + " + vec(s[1]);
        case Op::CMP: return "mix(" + vec(s[2]) + ", " + vec(s[1]) + ", lessThan(" + vec(s[0]) + ", vec4(0.0)))";
        case Op::COS: return rep("cos(" + scalar(s[0]) + ")");
        case Op::DP3: return rep("dot((" + vec(s[0]) + ").xyz, (" + vec(s[1]) + ").xyz)");
        case Op::DP4: return rep("dot(" + vec(s[0]) + ", " + vec(s[1]) + ")");
        case Op::DPH: return rep("dot((" + vec(s[0]) + ").xyz, (" + vec(s[1]) + ").xyz) + (" + vec(s[1]) + ").w");
        case Op::DST: {
            const std::string a = vec(s[0]), b = vec(s[1]);
            return "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)";
        }
        case Op::EX2: return rep("exp2(" + scalar(s[0]) + ")");
        case Op::EXP: return "arb_exp(" + scalar(s[0]) + ")";
        case Op::FLR: return "floor(" + vec(s[0]) + ")";
        case Op::FRC: return "fract(" + vec(s[0]) + ")";
        case Op::LG2: return rep("log2(abs(" + scalar(s[0]) + "))");
        case Op::LIT: return "arb_lit(" + vec(s[0]) + ")";
        case Op::LOG: return "arb_log(" + scalar(s[0]) + ")";
        case Op::LRP: {
            const std::string t = vec(s[0]);
            return "(" + t + ") * (" + vec(s[1]) + ") + (vec4(1.0) - (" + t + ")) * (" + vec(s[2]) + ")";
        }
        case Op::MAD: return "(" + vec(s[0]) + ") * (" + vec(s[1]) + ") + (" + vec(s[2]) + ")";
        case Op::MAX: return "max(" + vec(s[0]) + ", " + vec(s[1]) + ")";
        case Op::MIN: return "min(" + vec(s[0]) + ", " + vec(s[1]) + ")";
        case Op::MOV: case Op::SWZ: return vec(s[0]);
        case Op::MUL: return "(" + vec(s[0]) + ") * (" + vec(s[1]) + ")";
        case Op::POW: return rep("arb_pow(" + scalar(s[0]) + ", " + scalar(s[1]) + ")");
        case Op::RCP: return rep("1.0 / " + scalar(s[0]));
        case Op::RSQ: return rep("inversesqrt(abs(" + scalar(s[0]) + "))");
        case Op::SCS: {
            const std::string x = scalar(s[0]);
            return "vec4(cos(" + x + "), sin(" + x + "), 0.0, 0.0)";
        }
        case Op::SGE: return "vec4(greaterThanEqual(" + vec(s[0]) + ", " + vec(s[1]) + "))";
        case Op::SIN: return rep("sin(" + scalar(s[0]) + ")");
        case Op::SLT: return "vec4(lessThan(" + vec(s[0]) + ", " + vec(s[1]) + "))";
        case Op::SUB: return "(" + vec(s[0]) + ") - (" + vec(s[1]) + ")";
        case Op::XPD: return "vec4(cross((" + vec(s[0]) + ").xyz, (" + vec(s[1]) + ").xyz), 0.0)";
        case Op::TEX: case Op::TXB: case Op::TXP: return texture(in);
        case Op::ARL: case Op::KIL: break;
        }
        return "vec4(0.0)";
    }

    std::string instruction(const Instruction& in) const {
        if (in.op == Op::KIL) return "    if (any(lessThan(" + vec(in.src[0]) + ", vec4(0.0)))) discard;\n";
        if (in.op == Op::ARL) return "    a" + std::to_string(in.dst.index) + ".x = int(floor(" + scalar(in.src[0]) + "));\n";
        std::string v = value(in);
        if (in.saturate) v = "clamp(" + v + ", 0.0, 1.0)";
        const Dest& d = in.dst;
        const std::string m = maskSuffix(d.mask);
        // Through a temporary: the destination may also be a source.
        std::string s = "    {\n        vec4 r = " + v + ";\n";
        auto masked = [&](const std::string& target) { return "        " + target + "." + m + " = r." + m + ";\n"; };
        switch (d.base) {
        case Dest::Temp: s += masked("t" + std::to_string(d.index)); break;
        case Dest::Out:
            switch (d.output) {
            case Output::Position: s += masked("gl_Position"); break;
            case Output::FrontColor: s += masked("ff_front"); break;
            case Output::FrontSecondary: s += masked("ff_frontSecondary"); break;
            case Output::BackColor: s += masked("ff_back"); break;
            case Output::BackSecondary: s += masked("ff_backSecondary"); break;
            case Output::TexCoord: s += masked("ff_tc" + std::to_string(d.index)); break;
            case Output::FogCoord:
                if (d.mask & 1u) s += "        ff_fogCoord = r.x;\n";
                break;
            case Output::PointSize:
                if (d.mask & 1u) s += "        gl_PointSize = r.x;\n";
                break;
            case Output::Color:
                if (p_.drawBuffers && (p_.colorOutputs & ~1u)) s += masked("gl_FragData[" + std::to_string(d.index) + "]");
                else s += masked("gl_FragColor");
                break;
            case Output::Depth:
                if (d.mask & 4u) s += "        gl_FragDepth = r.z;\n";
                break;
            case Output::None: break;
            }
            break;
        default: break;
        }
        return s + "    }\n";
    }

    const Parsed& p_;
    const EmitOptions& o_;
    const Body& b_;
};

}  // namespace

std::shared_ptr<const Parsed> parse(Kind kind, const char* source, size_t length) {
    auto out = std::make_shared<Parsed>();
    out->kind = kind;
    const char* header = kind == Kind::Vertex ? "!!ARBvp1.0" : "!!ARBfp1.0";
    if (length < 10 || std::memcmp(source, header, 10) != 0) {
        out->errorPosition = 0;
        out->error = std::string("program string does not start with ") + header;
        return out;
    }
    try {
        Parser parser(kind, source, length, *out);
        parser.run();
        out->ok = true;
    } catch (const ParseError& e) {
        out->ok = false;
        out->errorPosition = e.pos;
        out->error = e.message;
        out->body.reset();
    }
    return out;
}

std::string emitKey(const Parsed& p, const EmitOptions& o) {
    std::string k;
    auto put32 = [&](uint32_t v) { k.append(reinterpret_cast<const char*>(&v), 4); };
    k.push_back(static_cast<char>(o.flat));
    if (p.kind == Kind::Vertex) {
        k.push_back(static_cast<char>(p.positionInvariant ? o.clipPlanes : 0));
    } else {
        k.push_back(static_cast<char>(o.twoSide && (p.colorRead[0] || p.colorRead[1])));
        put32(o.pointCoordReplace & p.texCoordRead);
    }
    return k;
}

std::string emit(const Parsed& p, const EmitOptions& o) {
    if (!p.ok || !p.body) return {};
    return Emitter(p, o).run();
}

}  // namespace wmgl::compat::arb

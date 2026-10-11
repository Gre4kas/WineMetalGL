#include "core/shader/subroutines.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

namespace wmgl::shader {
namespace {

struct Token {
    enum Kind { Ident, Number, Punct } kind;
    std::string text;
    size_t begin, end;
};

// GLSL tokens outside comments and preprocessor directives.
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> tokens;
    const size_t n = s.size();
    size_t i = 0;
    bool lineStart = true;
    while (i < n) {
        const char c = s[i];
        if (c == '\n') {
            lineStart = true;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (s.compare(i, 2, "//") == 0) {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (s.compare(i, 2, "/*") == 0) {
            const size_t end = s.find("*/", i + 2);
            i = end == std::string::npos ? n : end + 2;
            continue;
        }
        if (c == '#' && lineStart) {
            while (i < n && s[i] != '\n') i += s[i] == '\\' && i + 1 < n ? 2 : 1;
            continue;
        }
        lineStart = false;
        const size_t start = i;
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (i < n && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            tokens.push_back({Token::Ident, s.substr(start, i - start), start, i});
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            while (i < n && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '_' ||
                             ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                ++i;
            tokens.push_back({Token::Number, s.substr(start, i - start), start, i});
        } else {
            ++i;
            tokens.push_back({Token::Punct, std::string(1, c), start, i});
        }
    }
    return tokens;
}

bool isQualifier(const std::string& t) {
    static const std::set<std::string> q = {"in", "out", "inout", "const", "highp", "mediump", "lowp", "precise",
                                            "coherent", "volatile", "restrict", "readonly", "writeonly"};
    return q.count(t) != 0;
}

// One parameter of a subroutine type or function.
struct Param {
    std::vector<std::string> qualifiers;
    std::string type;   // type tokens (without the name and array suffix)
    std::string name;   // empty if unnamed
    std::string array;  // "[2]" ...
};

// Parses the tokens between a function's parentheses.
std::vector<Param> parseParams(const std::vector<Token>& t, size_t open, size_t close) {
    std::vector<Param> params;
    size_t i = open + 1;
    while (i < close) {
        size_t end = i;
        int depth = 0;
        while (end < close && !(depth == 0 && t[end].text == ",")) {
            if (t[end].text == "(" || t[end].text == "[") ++depth;
            if (t[end].text == ")" || t[end].text == "]") --depth;
            ++end;
        }
        Param p;
        std::vector<std::string> words;
        size_t k = i;
        for (; k < end && t[k].text != "["; ++k) {
            if (t[k].kind == Token::Ident && isQualifier(t[k].text)) p.qualifiers.push_back(t[k].text);
            else words.push_back(t[k].text);
        }
        for (; k < end; ++k) p.array += t[k].text;
        if (words.size() >= 2) {
            p.name = words.back();
            words.pop_back();
        }
        for (size_t w = 0; w < words.size(); ++w) p.type += (w ? " " : "") + words[w];
        if (!(words.size() == 1 && words[0] == "void" && p.name.empty() && params.empty() && end == close))
            params.push_back(p);
        i = end + 1;
    }
    return params;
}

std::string signature(const std::string& ret, const std::vector<Param>& params) {
    std::string s = ret + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ",";
        for (const std::string& q : params[i].qualifiers)
            if (q == "in" || q == "out" || q == "inout" || q == "const") s += q + " ";
        s += params[i].type + params[i].array;
    }
    return s + ")";
}

size_t matching(const std::vector<Token>& t, size_t open) {
    const std::string o = t[open].text, c = o == "(" ? ")" : o == "[" ? "]" : "}";
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i].text == o) ++depth;
        else if (t[i].text == c && --depth == 0) return i;
    }
    return t.size();
}

struct TypeDecl {
    std::string name, ret;
    std::vector<Param> params;
};

}  // namespace

std::string subroutineSelectionUniform(Stage stage, const std::string& name) {
    return "wmgl_su" + std::to_string(static_cast<int>(stage)) + "_" + name;
}

bool isSubroutineSelectionUniform(const std::string& name) { return name.rfind("wmgl_su", 0) == 0; }

SubroutineLowering lowerSubroutines(Stage stage, const std::string& source) {
    SubroutineLowering out;
    out.source = source;
    if (source.find("subroutine") == std::string::npos) return out;
    const std::vector<Token> t = tokenize(source);
    if (std::none_of(t.begin(), t.end(), [](const Token& k) { return k.text == "subroutine"; })) return out;
    const std::string S = std::to_string(static_cast<int>(stage));
    struct Edit { size_t begin, end; std::string text; };
    std::vector<Edit> edits;
    std::map<std::string, TypeDecl> types;
    std::map<std::string, size_t> uniformDecl;     // uniform -> its declaration's edit
    auto error = [&](const std::string& message) {
        if (out.ok) out.log += "ERROR: " + message + "\n";
        out.ok = false;
    };
    // Blanks a source range but keeps its line breaks (so line numbers hold).
    auto blank = [&](size_t begin, size_t end) {
        std::string text;
        for (size_t i = begin; i < end; ++i)
            if (source[i] == '\n') text += '\n';
        return text;
    };
    // Parses "layout(a = N, b)" ending just before token `at`; returns its first token or `at`.
    auto layoutBefore = [&](size_t at, std::map<std::string, int>& values) {
        if (at < 2 || t[at - 1].text != ")") return at;
        int depth = 0;
        size_t open = at - 1;
        for (;; --open) {
            if (t[open].text == ")") ++depth;
            if (t[open].text == "(" && --depth == 0) break;
            if (open == 0) return at;
        }
        if (open == 0 || t[open - 1].text != "layout") return at;
        for (size_t k = open + 1; k + 2 < at; ++k)
            if (t[k].kind == Token::Ident && t[k + 1].text == "=" && t[k + 2].kind == Token::Number)
                values[t[k].text] = std::atoi(t[k + 2].text.c_str());
        return open - 1;
    };
    int depth = 0;
    std::map<std::string, std::vector<int>> uniformDims;
    for (size_t i = 0; i < t.size(); ++i) {
        const std::string& x = t[i].text;
        if (x == "{") ++depth;
        if (x == "}") --depth;
        if (x != "subroutine" || t[i].kind != Token::Ident) continue;
        if (depth != 0) {
            error("'subroutine' : only allowed at global scope");
            continue;
        }
        std::map<std::string, int> layout;
        const size_t start = layoutBefore(i, layout);
        if (i + 1 >= t.size()) break;
        if (t[i + 1].text == "uniform") {
            // subroutine uniform TYPE NAME[...];
            if (i + 3 >= t.size() || t[i + 2].kind != Token::Ident || t[i + 3].kind != Token::Ident) {
                error("'subroutine uniform' : malformed declaration");
                continue;
            }
            SubroutineStageInfo::Uniform u;
            u.type = t[i + 2].text;
            u.name = t[i + 3].text;
            size_t k = i + 4;
            std::string dims;
            while (k < t.size() && t[k].text == "[") {
                const size_t close = matching(t, k);
                if (close != k + 2 || t[k + 1].kind != Token::Number) {
                    error("'" + u.name + "' : subroutine uniform arrays need a constant size");
                    break;
                }
                u.dimensions.push_back(std::atoi(t[k + 1].text.c_str()));
                dims += "[" + t[k + 1].text + "]";
                k = close + 1;
            }
            if (k >= t.size() || t[k].text != ";") {
                error("'" + u.name + "' : expected ';' after a subroutine uniform");
                continue;
            }
            if (!types.count(u.type)) {
                error("'" + u.type + "' : undeclared subroutine type");
                continue;
            }
            if (auto loc = layout.find("location"); loc != layout.end()) u.location = loc->second;
            uniformDims[u.name] = u.dimensions;
            uniformDecl[u.name] = edits.size();
            edits.push_back({t[start].begin, t[k].end, "uniform int " + subroutineSelectionUniform(stage, u.name) + dims + ";"});
            out.info.uniforms.push_back(u);
            i = k;
            continue;
        }
        if (t[i + 1].text == "(") {
            // subroutine(T1, T2) RET NAME(PARAMS) ...
            const size_t close = matching(t, i + 1);
            std::vector<std::string> list;
            for (size_t k = i + 2; k < close; ++k)
                if (t[k].kind == Token::Ident) list.push_back(t[k].text);
            size_t open = close + 1;
            while (open < t.size() && t[open].text != "(") ++open;
            if (open >= t.size() || open < close + 3) {
                error("'subroutine' : expected a function definition");
                continue;
            }
            const std::string name = t[open - 1].text;
            std::string ret;
            for (size_t k = close + 1; k + 1 < open; ++k) ret += (ret.empty() ? "" : " ") + t[k].text;
            const size_t paramsClose = matching(t, open);
            const std::vector<Param> params = parseParams(t, open, paramsClose);
            for (const std::string& type : list) {
                auto it = types.find(type);
                if (it == types.end()) {
                    error("'" + type + "' : undeclared subroutine type");
                    continue;
                }
                if (signature(it->second.ret, it->second.params) != signature(ret, params))
                    error("'" + name + "' : function does not match subroutine type '" + type + "'");
            }
            auto existing = std::find_if(out.info.functions.begin(), out.info.functions.end(),
                                         [&](const SubroutineStageInfo::Function& f) { return f.name == name; });
            const bool definition = paramsClose + 1 < t.size() && t[paramsClose + 1].text == "{";
            if (existing == out.info.functions.end()) {
                SubroutineStageInfo::Function f;
                f.name = name;
                f.types = list;
                if (auto index = layout.find("index"); index != layout.end()) f.index = index->second;
                out.info.functions.push_back(f);
                out.info.functions.back().index = f.index;
            } else if (definition && existing->types != list) {
                error("'" + name + "' : subroutine functions cannot be overloaded");
            }
            edits.push_back({t[start].begin, t[close].end, blank(t[start].begin, t[close].end)});
            i = close;
            continue;
        }
        // subroutine RET NAME(PARAMS);
        size_t open = i + 1;
        while (open < t.size() && t[open].text != "(" && t[open].text != ";") ++open;
        if (open >= t.size() || t[open].text != "(" || open < i + 3) {
            error("'subroutine' : malformed subroutine type");
            continue;
        }
        TypeDecl type;
        type.name = t[open - 1].text;
        for (size_t k = i + 1; k + 1 < open; ++k) type.ret += (type.ret.empty() ? "" : " ") + t[k].text;
        const size_t close = matching(t, open);
        type.params = parseParams(t, open, close);
        if (close + 1 >= t.size() || t[close + 1].text != ";") {
            error("'" + type.name + "' : a subroutine type has no body");
            continue;
        }
        if (types.count(type.name)) error("'" + type.name + "' : subroutine type redefinition");
        types[type.name] = type;
        out.info.types.push_back(type.name);
        edits.push_back({t[start].begin, t[close + 1].end, blank(t[start].begin, t[close + 1].end)});
        i = close + 1;
    }
    if (!out.ok) return out;
    // Calls of subroutine uniforms: NAME(args) or NAME[i][j](args).
    depth = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].text == "{") ++depth;
        if (t[i].text == "}") --depth;
        if (depth == 0 || t[i].kind != Token::Ident || !uniformDims.count(t[i].text)) continue;
        if (i > 0 && (t[i - 1].text == "." )) continue;
        const std::string& name = t[i].text;
        const std::string dispatcher = "wmgl_sc" + S + "_" + name;
        std::vector<std::string> indices;
        size_t k = i + 1;
        while (k < t.size() && t[k].text == "[") {
            const size_t close = matching(t, k);
            indices.push_back(source.substr(t[k].end, t[close].begin - t[k].end));
            k = close + 1;
        }
        if (indices.empty() && k + 2 < t.size() && t[k].text == "." && t[k + 1].text == "length" && t[k + 2].text == "(") {
            // name.length() of an array: the selection array has its dimensions.
            edits.push_back({t[i].begin, t[i].end, subroutineSelectionUniform(stage, name)});
            continue;
        }
        if (k >= t.size() || t[k].text != "(") continue;  // not a call: glslang reports it
        if (indices.size() != uniformDims[name].size()) {
            error("'" + name + "' : subroutine uniform arrays must be indexed when called");
            break;
        }
        std::string call = dispatcher + "(";
        for (size_t d = 0; d < indices.size(); ++d) call += (d ? ", " : "") + std::string("int(") + indices[d] + ")";
        const bool args = k + 1 < t.size() && t[k + 1].text != ")";
        if (!indices.empty() && args) call += ", ";
        edits.push_back({t[i].begin, t[k].end, call});
        i = k;
    }
    if (!out.ok) return out;
    // Function indices: layout(index) first, then the lowest free ones.
    std::set<GLint> used;
    for (const auto& f : out.info.functions) {
        if (f.index < 0) continue;
        if (!used.insert(f.index).second) error("'" + f.name + "' : subroutine index already used");
    }
    GLint next = 0;
    for (auto& f : out.info.functions) {
        if (f.index >= 0) continue;
        while (used.count(next)) ++next;
        f.index = next;
        used.insert(next);
    }
    // Dispatchers: a prototype after each uniform, the definition at the end.
    std::string definitions;
    for (SubroutineStageInfo::Uniform& u : out.info.uniforms) {
        const TypeDecl& type = types[u.type];
        std::vector<const SubroutineStageInfo::Function*> compatible;
        for (const auto& f : out.info.functions)
            if (std::find(f.types.begin(), f.types.end(), u.type) != f.types.end()) compatible.push_back(&f);
        if (compatible.empty()) {
            error("'" + u.name + "' : no subroutine function matches subroutine uniform type '" + u.type + "'");
            continue;
        }
        for (const auto* f : compatible) u.compatible.push_back(f->index);
        std::string head = type.ret + " wmgl_sc" + S + "_" + u.name + "(", args;
        std::string selection = subroutineSelectionUniform(stage, u.name);
        for (size_t d = 0; d < u.dimensions.size(); ++d) {
            head += (d ? ", " : "") + std::string("int wmgl_i") + std::to_string(d);
            selection += "[wmgl_i" + std::to_string(d) + "]";
        }
        for (size_t p = 0; p < type.params.size(); ++p) {
            const Param& param = type.params[p];
            const std::string name = "wmgl_a" + std::to_string(p);
            if (p || !u.dimensions.empty()) head += ", ";
            for (const std::string& q : param.qualifiers) head += q + " ";
            head += param.type + " " + name + param.array;
            args += (p ? ", " : "") + name;
        }
        head += ")";
        edits[uniformDecl[u.name]].text += " " + head + ";";
        const bool isVoid = type.ret == "void";
        std::string body = head + " {\n    switch (" + selection + ") {\n";
        for (const auto* f : compatible) {
            const std::string call = f->name + "(" + args + ")";
            body += "    case " + std::to_string(f->index) + ": " + (isVoid ? call + "; return;\n" : "return " + call + ";\n");
        }
        const std::string fallback = compatible[0]->name + "(" + args + ")";
        body += "    default: " + (isVoid ? fallback + "; return;\n" : "return " + fallback + ";\n");
        body += "    }\n}\n";
        definitions += body;
    }
    if (!out.ok) return out;
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin < b.begin; });
    std::string result;
    size_t at = 0;
    for (const Edit& e : edits) {
        if (e.begin < at) continue;
        result.append(source, at, e.begin - at);
        result += e.text;
        at = e.end;
    }
    result.append(source, at, std::string::npos);
    if (!definitions.empty()) result += "\n" + definitions;
    out.source = result;
    return out;
}

}  // namespace wmgl::shader

// Minimal test registry for host tests (no GPU, no Wine).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wmgl_test {

struct Case {
    const char* name;
    std::function<void()> body;
};
std::vector<Case>& registry();
extern int failures;

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

}  // namespace wmgl_test

#define WMGL_CAT2(a, b) a##b
#define WMGL_CAT(a, b) WMGL_CAT2(a, b)
#define TEST(name)                                                                   \
    static void name();                                                              \
    static wmgl_test::Register WMGL_CAT(register_, name)(#name, name);               \
    static void name()
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++wmgl_test::failures;                                                   \
        }                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        auto va_ = (a);                                                              \
        auto vb_ = (b);                                                              \
        if (!(va_ == vb_)) {                                                         \
            std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed\n", __FILE__, __LINE__, #a, #b); \
            ++wmgl_test::failures;                                                   \
        }                                                                            \
    } while (0)

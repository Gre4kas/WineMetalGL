#include <cstring>

#include "test.h"

namespace wmgl_test {
std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}
int failures = 0;
}  // namespace wmgl_test

int main(int argc, char** argv) {
    int run = 0;
    for (const auto& test : wmgl_test::registry()) {
        if (argc > 1 && !std::strstr(test.name, argv[1])) continue;
        const int before = wmgl_test::failures;
        test.body();
        std::printf("%s %s\n", wmgl_test::failures == before ? "PASS" : "FAIL", test.name);
        ++run;
    }
    std::printf("%d tests, %d failed checks\n", run, wmgl_test::failures);
    return wmgl_test::failures ? 1 : 0;
}

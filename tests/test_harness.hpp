#ifndef MOOCODE_TEST_HARNESS_HPP
#define MOOCODE_TEST_HARNESS_HPP

// Tiny zero-dependency test harness. A test file does:
//   #include "test_harness.hpp"
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
// and links against test_main.cpp which calls moocode::test::run_all().

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace moocode::test {

struct Case {
    std::string name;
    std::function<void()> fn;
};

// Process-wide registry. Inline => one instance across all TUs of an exe.
inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

// Per-run failure counter for the case currently executing.
inline int& current_failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

inline void report_failure(const char* file, int line, const std::string& expr) {
    ++current_failures();
    std::fprintf(stderr, "    FAIL %s:%d  %s\n", file, line, expr.c_str());
}

// Run every registered case; print a summary. Returns process exit code.
inline int run_all() {
    int failed_cases = 0;
    for (auto& c : registry()) {
        current_failures() = 0;
        c.fn();
        if (current_failures() == 0) {
            std::fprintf(stderr, "[ ok ] %s\n", c.name.c_str());
        } else {
            std::fprintf(stderr, "[FAIL] %s (%d checks failed)\n", c.name.c_str(),
                         current_failures());
            ++failed_cases;
        }
    }
    std::fprintf(stderr, "\n%zu cases, %d failed\n", registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace moocode::test

// --- Macros -----------------------------------------------------------------
#define MOOCODE_CAT_(a, b) a##b
#define MOOCODE_CAT(a, b) MOOCODE_CAT_(a, b)

#define TEST(name)                                                            \
    static void MOOCODE_CAT(moocode_test_, __LINE__)();                       \
    static ::moocode::test::Registrar MOOCODE_CAT(moocode_reg_, __LINE__){    \
        name, &MOOCODE_CAT(moocode_test_, __LINE__)};                         \
    static void MOOCODE_CAT(moocode_test_, __LINE__)()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond))                                                          \
            ::moocode::test::report_failure(__FILE__, __LINE__, #cond);       \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto _a = (a);  /* by value: (a) may return a ref into a temporary */ \
        auto _b = (b);                                                        \
        if (!(_a == _b)) {                                                    \
            std::ostringstream _os;                                           \
            _os << #a " == " #b;                                              \
            ::moocode::test::report_failure(__FILE__, __LINE__, _os.str());   \
        }                                                                     \
    } while (0)

#endif  // MOOCODE_TEST_HARNESS_HPP

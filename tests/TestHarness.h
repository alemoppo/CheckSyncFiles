#pragma once

// Minimal dependency-free test harness.
//
// Usage:
//   int main() {
//     TEST("description", [] {
//         CHECK(expr);
//         CHECK_EQ(a, b);
//     });
//     return TestSummary();
//   }

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct TestCase {
    std::string name;
    bool passed = true;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

inline void Register(const std::string& name, const std::function<void()>& fn) {
    TestCase tc;
    tc.name = name;
    tc.passed = true;
    Registry().push_back(tc);
    std::cout << "[RUN ] " << name << "\n" << std::flush;
    try {
        fn();
    } catch (const std::exception& e) {
        Registry().back().passed = false;
        std::cerr << "  unexpected exception: " << e.what() << "\n";
    } catch (...) {
        Registry().back().passed = false;
        std::cerr << "  unexpected non-standard exception\n";
    }
    std::cout << (Registry().back().passed ? "[PASS] " : "[FAIL] ") << name << "\n"
              << std::flush;
}

inline int Summary() {
    int failed = 0;
    for (const auto& tc : Registry()) {
        if (tc.passed) {
            std::cout << "[PASS] " << tc.name << "\n";
        } else {
            ++failed;
            std::cout << "[FAIL] " << tc.name << "\n";
        }
    }
    std::cout << "\n" << (Registry().size() - failed) << "/"
              << Registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}

inline void ReportFailure(const char* file, int line, const std::string& msg) {
    std::cerr << "  CHECK failed at " << file << ":" << line << "  " << msg << "\n";
    // Mark the currently registered test as failed.
    if (!Registry().empty()) Registry().back().passed = false;
}

} // namespace test

#define BV_TEST_CONCAT_(a, b) a##b
#define BV_TEST_CONCAT(a, b) BV_TEST_CONCAT_(a, b)

// Registers the lambda `...` as a test and runs it immediately.
#define TEST(name, ...)                                                    \
    static const bool BV_TEST_CONCAT(test_reg_, __LINE__) =                \
        (test::Register(name, __VA_ARGS__), true)

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            test::ReportFailure(__FILE__, __LINE__, "(" #cond ") is false");         \
        }                                                                            \
    } while (0)

#define CHECK_MSG(cond, ...)                                                         \
    do {                                                                             \
        if (!(cond)) {                                                               \
            test::ReportFailure(__FILE__, __LINE__, "(" #cond ") is false: " __VA_ARGS__); \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        auto va = (a);                                                               \
        auto vb = (b);                                                               \
        if (!(va == vb)) {                                                           \
            std::ostringstream oss;                                                  \
            oss << "(" #a " == " #b ") failed: " << va << " != " << vb;              \
            test::ReportFailure(__FILE__, __LINE__, oss.str());                      \
        }                                                                            \
    } while (0)

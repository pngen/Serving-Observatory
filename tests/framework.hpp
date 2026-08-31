
#pragma once
// Serving Observatory — minimal deterministic test harness (no external deps).
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace sobftest {

struct Test {
    std::string name;
    void (*fn)();
};

inline std::vector<Test>& registry() {
    static std::vector<Test> r;
    return r;
}
inline void add(std::string name, void (*fn)()) { registry().push_back({ std::move(name), fn }); }
inline int& failures() { static int f = 0; return f; }
inline int& assertions() { static int a = 0; return a; }
inline bool& verbose() { static bool v = false; return v; }

// Stringify any value for diagnostics (handles arithmetic, std::string, and
// types with to_hex()).
template <class T>
inline std::string to_str(const T& v) {
    if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(v);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return v;
    } else if constexpr (std::is_same_v<T, const char*>) {
        return v ? v : "(null)";
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        return std::string(v);
    } else if constexpr (requires { v.to_hex(); }) {
        return std::string(v.to_hex());
    } else {
        return "<?>";
    }
}

inline void report_fail(const char* expr, const char* file, int line, const std::string& msg = {}) {
    ++failures();
    std::printf("  FAIL [%s:%d] %s%s\n", file, line, expr, msg.empty() ? "" : (" -- " + msg).c_str());
}
inline void check(bool cond, const char* expr, const char* file, int line) {
    ++assertions();
    if (!cond) report_fail(expr, file, line);
}

template <class A, class B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++assertions();
    if (!(a == b)) {
        std::string m = std::string(ea) + "=" + to_str(a) + " vs " + eb + "=" + to_str(b);
        report_fail("(a==b)", file, line, m);
    }
}
inline void check_near(double a, double b, double eps, const char* ea, const char* eb, const char* file, int line) {
    ++assertions();
    if (std::fabs(a - b) > eps) {
        std::string m = std::string(ea) + "=" + std::to_string(a) + " vs " + eb + "=" + std::to_string(b);
        report_fail("(a~b)", file, line, m);
    }
}

inline int run_all(bool verbose_flag = true) {
    verbose() = verbose_flag;
    int passed = 0, failed = 0;
    for (auto& t : registry()) {
        int before = failures();
        try {
            t.fn();
        } catch (const std::exception& e) {
            report_fail("test threw exception", "(test)", 0, e.what());
        } catch (...) {
            report_fail("test threw unknown exception", "(test)", 0, "");
        }
        if (failures() == before) { ++passed; } else { ++failed; }
    }
    std::printf("\n==== serving observatory tests ====\n");
    std::printf("  tests: %zu  passed: %d  failed: %d  assertions: %d\n",
                registry().size(), passed, failed, assertions());
    return failed == 0 ? 0 : 1;
}

} // namespace sobftest

#define TEST(name) \
    static void name(); \
    namespace { struct reg_##name { reg_##name() { ::sobftest::add(#name, &name); } } reg_##name##_inst; } \
    static void name()

#define CHECK(expr) ::sobftest::check(!!(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::sobftest::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) ::sobftest::check_near((a), (b), (eps), #a, #b, __FILE__, __LINE__)

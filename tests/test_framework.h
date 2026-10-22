#pragma once
// Minimal self-registering test framework (no external dependencies).

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace hktest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry()
{
    static std::vector<Case> r;
    return r;
}

inline int& failures()
{
    static int f = 0;
    return f;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int run_all(const char* filter)
{
    int run = 0;
    for (auto& c : registry()) {
        if (filter && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = failures();
        c.fn();
        ++run;
        std::printf("%s %s\n", failures() == before ? "[ OK ]" : "[FAIL]", c.name);
    }
    std::printf("\n%d tests, %d failed checks\n", run, failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace hktest

#define HK_CONCAT2(a, b) a##b
#define HK_CONCAT(a, b) HK_CONCAT2(a, b)
#define TEST(name)                                                                         \
    static void name();                                                                    \
    static ::hktest::Register HK_CONCAT(reg_, name)(#name, name);                          \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);         \
            ++::hktest::failures();                                                        \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto va = (a);                                                               \
        const auto vb = (b);                                                               \
        if (!(va == vb)) {                                                                 \
            std::printf("  %s:%d: CHECK_EQ(%s, %s) failed: %lld vs %lld\n", __FILE__,      \
                        __LINE__, #a, #b, (long long)va, (long long)vb);                   \
            ++::hktest::failures();                                                        \
        }                                                                                  \
    } while (0)

#pragma once

// Minimal test harness: no dependencies, works on every platform the core builds on.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test
{
struct Case
{
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry()
{
    static std::vector<Case> s_cases;
    return s_cases;
}

inline int& Failures()
{
    static int s_failures = 0;
    return s_failures;
}

struct Registrar
{
    Registrar(const char* aName, std::function<void()> aFn) { Registry().push_back({aName, std::move(aFn)}); }
};

inline void Fail(const char* aFile, int aLine, const std::string& aMessage)
{
    ++Failures();
    std::printf("    FAIL %s:%d: %s\n", aFile, aLine, aMessage.c_str());
}
} // namespace test

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                                                \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                                                     \
    static ::test::Registrar TEST_CONCAT(test_reg_, __LINE__)(name, &TEST_CONCAT(test_fn_, __LINE__));                \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            ::test::Fail(__FILE__, __LINE__, "CHECK(" #expr ")");                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!((a) == (b)))                                                                                             \
            ::test::Fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")");                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        const double test_a_ = static_cast<double>(a);                                                                 \
        const double test_b_ = static_cast<double>(b);                                                                 \
        if (!(std::fabs(test_a_ - test_b_) <= static_cast<double>(eps)))                                               \
            ::test::Fail(__FILE__, __LINE__,                                                                           \
                         "CHECK_NEAR(" #a ", " #b ") " + std::to_string(test_a_) + " vs " + std::to_string(test_b_)); \
    } while (0)

#define REQUIRE(expr)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            ::test::Fail(__FILE__, __LINE__, "REQUIRE(" #expr ")");                                                    \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

// A tiny test harness: assertion macros and a registry, no framework needed.
//
//   struct TstThing { void works() { T_VERIFY(1 + 1 == 2); } };
//   T_MAIN(TstThing, works)
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace minitest {

inline int &failures()
{
    static int n = 0;
    return n;
}

inline void fail(const char *file, int line, const char *expr, const char *message = nullptr)
{
    std::fprintf(stderr, "%s:%d: check failed: %s%s%s\n", file, line, expr, message ? "  --  " : "",
                 message ? message : "");
    ++failures();
}

inline const char *cstr(const std::string &s) { return s.c_str(); }

inline bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

}  // namespace minitest

#define T_VERIFY(cond)                                                       \
    do {                                                                     \
        if (!(cond))                                                         \
            minitest::fail(__FILE__, __LINE__, #cond);                       \
    } while (0)

#define T_VERIFY2(cond, message)                                             \
    do {                                                                     \
        if (!(cond))                                                         \
            minitest::fail(__FILE__, __LINE__, #cond, (message));            \
    } while (0)

#define T_COMPARE(actual, expected)                                          \
    do {                                                                     \
        if (!((actual) == (expected)))                                       \
            minitest::fail(__FILE__, __LINE__, #actual " == " #expected);    \
    } while (0)

// Runs the listed member functions of a fresh instance each; returns non-zero if any check failed.
#define T_RUN(Class, name)                                                   \
    do {                                                                     \
        const int before = minitest::failures();                             \
        Class instance;                                                      \
        instance.name();                                                     \
        std::printf("%s %s\n", minitest::failures() == before ? "PASS" : "FAIL", #name); \
    } while (0)

#define T_SUMMARY()                                                          \
    (std::printf("%d failure(s)\n", minitest::failures()), minitest::failures() == 0 ? 0 : 1)

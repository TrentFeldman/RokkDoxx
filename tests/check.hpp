// The tests' whole framework: count failures, print them, return the exit code.
#pragma once

#include <cstdio>
#include <string>

inline int g_fail = 0;

inline void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_fail;
    }
}

inline int report() {
    if (g_fail == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_fail);
    return 1;
}

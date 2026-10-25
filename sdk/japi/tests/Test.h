#pragma once
#include <cstdio>
extern int failures;
extern unsigned checks;
inline void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
}
void HostTests();

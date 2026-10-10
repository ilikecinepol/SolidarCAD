#pragma once

#include <cstdlib>
#include <iostream>

namespace solidar::test {

[[noreturn]] inline void failCheck(const char* expression, const char* file,
                                   int line) {
  std::cerr << file << ':' << line << ": CHECK(" << expression
            << ") failed\n";
  std::exit(EXIT_FAILURE);
}

}  // namespace solidar::test

// Unlike the standard debug-only assertion, CHECK remains active in Release
// builds and is valid in
// helper functions, constructors and lambdas because failure terminates the
// test process instead of returning from the current function.
#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition))                                                      \
      ::solidar::test::failCheck(#condition, __FILE__, __LINE__);          \
  } while (false)

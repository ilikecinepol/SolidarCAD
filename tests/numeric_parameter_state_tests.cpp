#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "model/NumericParameterState.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main() {
  solidar::NumericParameterState value;
  value.reset(0.0, 0.0, 10.0);
  CHECK(value.value() == 0.0);
  value.accept(*value.candidate(0.5));
  CHECK(value.value() == 0.5);
  value.accept(*value.candidate(2.0));
  CHECK(value.value() == 2.0);
  CHECK(!value.candidate(std::numeric_limits<double>::quiet_NaN()));
  CHECK(!value.candidate(std::numeric_limits<double>::infinity()));
  CHECK(*value.candidate(-4.0) == 0.0);
  CHECK(*value.candidate(100.0) == 10.0);
  // A rejected geometry candidate is deliberately not accepted.
  CHECK(value.lastValidValue() == 2.0);
  value.accept(*value.candidate(1.0));
  CHECK(value.value() == 1.0);
  return EXIT_SUCCESS;
}

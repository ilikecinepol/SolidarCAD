#include <cstdlib>
#include <iostream>
#include <vector>

#include "ui/EdgeSelectionState.h"

namespace {

bool check(bool condition, const char* expression, int line) {
  if (condition) return true;
  std::cerr << __FILE__ << ':' << line << ": " << expression << '\n';
  return false;
}

#define CHECK(condition)                         \
  do {                                           \
    if (!check((condition), #condition, __LINE__)) return EXIT_FAILURE; \
  } while (false)

}  // namespace

int main() {
  std::vector<std::size_t> selected;
  solidar::updateEdgeSelection(selected, 3, true);
  solidar::updateEdgeSelection(selected, 8, true);
  CHECK((selected == std::vector<std::size_t>{3, 8}));

  solidar::updateEdgeSelection(selected, 3, true);
  CHECK((selected == std::vector<std::size_t>{8}));

  solidar::updateEdgeSelection(selected, 2, false);
  CHECK((selected == std::vector<std::size_t>{2}));
  return EXIT_SUCCESS;
}

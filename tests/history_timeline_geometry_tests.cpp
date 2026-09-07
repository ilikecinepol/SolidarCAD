#include <cstdlib>
#include <iostream>
#include "ui/HistoryTimelineWidget.h"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)
int main() {
  solidar::HistoryTimelineGeometry one{1};
  CHECK(one.trackStart() == one.center(0));
  CHECK(one.trackEnd() == one.center(0));
  solidar::HistoryTimelineGeometry two{2};
  CHECK(two.trackStart() == two.center(0));
  CHECK(two.trackEnd() == two.center(1));
  solidar::HistoryTimelineGeometry four{4};
  CHECK(four.center(0) == 26 && four.center(1) == 62);
  CHECK(four.center(3) == 134);
  CHECK(four.nearestIndex(81) == 2);
  CHECK(four.nearestIndex(79) == 1);
  solidar::HistoryTimelineGeometry twenty{20};
  CHECK(twenty.contentWidth() == 28 + 24 + 19 * 36);
  CHECK(twenty.nearestIndex(-100) == 0);
  CHECK(twenty.nearestIndex(10000) == 19);
  return EXIT_SUCCESS;
}

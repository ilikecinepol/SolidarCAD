#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace solidar {

inline void updateEdgeSelection(std::vector<std::size_t>& selected,
                                std::size_t clicked, bool toggle) {
  if (!toggle) {
    selected = {clicked};
    return;
  }

  const auto found = std::find(selected.begin(), selected.end(), clicked);
  if (found == selected.end())
    selected.push_back(clicked);
  else
    selected.erase(found);
}

}  // namespace solidar

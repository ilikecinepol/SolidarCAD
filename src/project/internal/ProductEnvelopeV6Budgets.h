#pragma once

#include <QtTypes>

namespace solidar::project::detail {

[[nodiscard]] constexpr bool fitsProductEnvelopeV6Budget(
    qsizetype current, qsizetype delta, qsizetype limit) noexcept {
  return current >= 0 && delta >= 0 && limit >= 0 && current <= limit &&
         delta <= limit - current;
}

[[nodiscard]] inline bool addProductEnvelopeV6Budget(
    qsizetype delta, qsizetype limit, qsizetype* total) noexcept {
  if (!total || !fitsProductEnvelopeV6Budget(*total, delta, limit))
    return false;
  *total += delta;
  return true;
}

}  // namespace solidar::project::detail

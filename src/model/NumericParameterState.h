#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace solidar {

// Authoritative scalar shared by an editor, a manipulator and a preview.
// Invalid candidates never replace the last value accepted by geometry.
class NumericParameterState {
 public:
  void reset(double value, double minimum, double maximum) noexcept {
    minimum_ = minimum;
    maximum_ = maximum;
    const auto normalized = candidate(value);
    current_ = normalized.value_or(minimum_);
    lastValid_ = current_;
  }

  [[nodiscard]] std::optional<double> candidate(double value) const noexcept {
    if (!std::isfinite(value) || !std::isfinite(minimum_) ||
        !std::isfinite(maximum_) || minimum_ > maximum_)
      return std::nullopt;
    return std::clamp(value, minimum_, maximum_);
  }

  void accept(double value) noexcept {
    current_ = value;
    lastValid_ = value;
  }

  void setRange(double minimum, double maximum) noexcept {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum > maximum)
      return;
    minimum_ = minimum;
    maximum_ = maximum;
    current_ = std::clamp(current_, minimum_, maximum_);
    lastValid_ = std::clamp(lastValid_, minimum_, maximum_);
  }

  [[nodiscard]] double value() const noexcept { return current_; }
  [[nodiscard]] double lastValidValue() const noexcept { return lastValid_; }
  [[nodiscard]] double minimum() const noexcept { return minimum_; }
  [[nodiscard]] double maximum() const noexcept { return maximum_; }

 private:
  double minimum_{};
  double maximum_{100000.0};
  double current_{};
  double lastValid_{};
};

}  // namespace solidar

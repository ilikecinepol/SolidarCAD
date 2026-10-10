#pragma once

namespace solidar::detail {

inline thread_local unsigned explicitIdReservationPauseDepth = 0;

class ScopedExplicitIdReservationPause final {
 public:
  ScopedExplicitIdReservationPause() noexcept {
    ++explicitIdReservationPauseDepth;
  }
  ~ScopedExplicitIdReservationPause() {
    --explicitIdReservationPauseDepth;
  }

  ScopedExplicitIdReservationPause(
      const ScopedExplicitIdReservationPause&) = delete;
  ScopedExplicitIdReservationPause& operator=(
      const ScopedExplicitIdReservationPause&) = delete;
};

[[nodiscard]] inline bool explicitIdReservationEnabled() noexcept {
  return explicitIdReservationPauseDepth == 0;
}

}  // namespace solidar::detail

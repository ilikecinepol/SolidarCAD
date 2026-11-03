#pragma once

#include <cstdint>
#include <optional>

#include "ui/AssemblyViewportPicking.h"

namespace solidar {

enum class AssemblyViewportRulerCommitStatus : std::uint8_t {
  AcceptedFirstPoint,
  AcceptedSecondPoint,
  RejectedMiss,
  RejectedUncertain,
  RejectedStaleOrForeign,
};

// Pure transient measurement state.  It stores guarded snap values only and
// never owns model, publication-coordinator, QObject, paint, or input state.
class AssemblyViewportRuler final {
 public:
  void clear() noexcept;
  void updateHover(const AssemblyViewportSnapQueryResult& query) noexcept;
  [[nodiscard]] AssemblyViewportRulerCommitStatus commit(
      const AssemblyViewportSnapQueryResult& query) noexcept;
  void validateCurrent(
      const AssemblyScenePublicationHandle& currentPublication) noexcept;

  [[nodiscard]] const std::optional<AssemblyViewportSnapHit>& firstPoint()
      const noexcept {
    return first_;
  }
  [[nodiscard]] const std::optional<AssemblyViewportSnapHit>& secondPoint()
      const noexcept {
    return second_;
  }
  [[nodiscard]] const std::optional<AssemblyViewportSnapHit>& hoverPoint()
      const noexcept {
    return hover_;
  }
  [[nodiscard]] std::optional<double> distance() const noexcept;

 private:
  [[nodiscard]] static bool exactAndCurrent(
      const AssemblyViewportSnapHit& hit) noexcept;
  [[nodiscard]] bool compatible(
      const AssemblyViewportSnapHit& hit) const noexcept;

  std::optional<AssemblyViewportSnapHit> first_;
  std::optional<AssemblyViewportSnapHit> second_;
  std::optional<AssemblyViewportSnapHit> hover_;
};

}  // namespace solidar

#pragma once

#include <string>

namespace solidar {

// Stable, Qt-free failure contract shared by geometry builders, tool sessions
// and the UI boundary. `detail` remains diagnostic-only and must never drive
// control flow or localization.
enum class OperationFailureCode {
  None,
  Unknown,
  InvalidInput,
  MissingSource,
  TopologyIndexUnavailable,
  TopologyReferenceMismatch,
  TopologyReferenceMissing,
  TopologyReferenceAmbiguous,
  TopologyReferenceInvalid,
  TopologyResolutionUnexpected,
  NonPlanarFace,
  UnsupportedSurface,
  UnsupportedGeometry,
  NoIntersection,
  InvalidProfileOpen,
  InvalidProfileOverlap,
  InvalidProfile,
  BodiesDoNotTouch,
  GeometryOperationFailed,
};

struct OperationFailure {
  OperationFailureCode code{OperationFailureCode::None};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return code != OperationFailureCode::None;
  }
};

}  // namespace solidar

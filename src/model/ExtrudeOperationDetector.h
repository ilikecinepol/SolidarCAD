#pragma once

#include "model/ExtrudeFeature.h"

class TopoDS_Shape;

namespace solidar {
struct DocumentSketch;

struct NormalizedExtrusionInput {
  double distanceMm{0.0};
  bool reversed{false};
};

[[nodiscard]] NormalizedExtrusionInput normalizeExtrusionInput(
    double distanceMm, bool reversed) noexcept;

[[nodiscard]] ExtrudeOperation detectExtrudeOperation(
    const DocumentSketch& profile, double distanceMm, bool reversed,
    const TopoDS_Shape* targetBody, bool faceSupported);

}  // namespace solidar

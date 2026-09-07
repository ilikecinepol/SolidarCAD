#pragma once

#include <optional>

#include "model/SketchPlacement.h"

class TopoDS_Shape;

namespace solidar {

struct EdgeManipulatorGeometry {
  Point3d midpoint{};
  Vector3d outwardDirection{0.0, 0.0, 1.0};
};

// Derives presentation geometry exclusively from the selected edge and its
// adjacent faces. Translating the model in world space cannot change direction.
[[nodiscard]] std::optional<EdgeManipulatorGeometry>
localEdgeManipulatorGeometry(const TopoDS_Shape& body,
                             const TopoDS_Shape& edge);

}  // namespace solidar

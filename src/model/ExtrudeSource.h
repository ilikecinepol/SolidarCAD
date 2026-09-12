#pragma once

#include <variant>

#include "model/SketchPlacement.h"

namespace solidar {

// An Extrude feature is driven either by a Sketch profile (the original v1
// behavior) or directly by a persistent Face reference (native face extrusion).
struct SketchExtrudeSource {
  SketchId sketchId{kInvalidSketchId};
};

struct FaceExtrudeSource {
  FaceReference face;
};

using ExtrudeSource = std::variant<SketchExtrudeSource, FaceExtrudeSource>;

}  // namespace solidar

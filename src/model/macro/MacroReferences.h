#pragma once

#include <optional>
#include <variant>

#include "model/Document.h"

namespace solidar::macro {

enum class SketchPointKind { LineStart, LineEnd, CircleCenter };

struct SketchPointReference {
  SketchId sketchId{kInvalidSketchId};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  SketchPointKind point{SketchPointKind::LineStart};
};

struct SketchEntityReference {
  SketchId sketchId{kInvalidSketchId};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
};

struct SketchReference { SketchId sketchId{kInvalidSketchId}; };
struct BodyReference { BodyId bodyId{kInvalidBodyId}; };

using MacroInputBinding =
    std::variant<SketchPointReference, SketchEntityReference, SketchReference,
                 FaceReference, EdgeReference, BodyReference>;

struct CreatedObjectReference {
  SketchId sketchId{kInvalidSketchId};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
};

[[nodiscard]] std::optional<sketch::Point> resolveSketchPoint(
    const Document& document, const SketchPointReference& reference) noexcept;

}  // namespace solidar::macro

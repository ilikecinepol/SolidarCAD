#include "model/macro/MacroReferences.h"

namespace solidar::macro {

std::optional<sketch::Point> resolveSketchPoint(
    const Document& document, const SketchPointReference& reference) noexcept {
  const DocumentSketch* profile = document.findSketch(reference.sketchId);
  if (!profile || reference.geometryId == sketch::kInvalidGeometryId)
    return std::nullopt;
  switch (reference.point) {
    case SketchPointKind::LineStart:
    case SketchPointKind::LineEnd: {
      const auto index = profile->geometry.lineIndex(reference.geometryId);
      if (!index) return std::nullopt;
      const auto& line = profile->geometry.lines()[*index];
      return reference.point == SketchPointKind::LineStart ? line.start
                                                           : line.end;
    }
    case SketchPointKind::CircleCenter: {
      const auto index = profile->geometry.circleIndex(reference.geometryId);
      if (!index) return std::nullopt;
      return profile->geometry.circles()[*index].center;
    }
  }
  return std::nullopt;
}

}  // namespace solidar::macro

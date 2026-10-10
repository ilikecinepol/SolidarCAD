#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "ui/SketchHitTester.h"

namespace solidar {

struct SketchHitSceneOptions {
  SketchHitTolerancePolicy tolerance;
  std::vector<sketch::GeometryId> hiddenGeometry;
  std::optional<SketchScreenSegment> xAxis;
  std::optional<SketchScreenSegment> yAxis;
  std::optional<SketchHitPoint> origin;
  int curveSamples{72};
};

// Narrow read-only UI adapter. It materializes an immutable screen-space
// scene; the pure tester never owns or calls back into SketchCanvas.
class SketchHitSceneAdapter final {
 public:
  using Projector = std::function<SketchHitPoint(sketch::Point)>;

  [[nodiscard]] static SketchHitScene build(
      const sketch::Sketch& sketch, const Projector& projector,
      const SketchHitSceneOptions& options = {});
};

}  // namespace solidar

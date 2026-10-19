#pragma once

#include <memory>
#include <string>
#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "sketch/Sketch.h"

class TopoDS_Edge;

namespace solidar {
struct DocumentSketch;

struct SketchProfileEdgeBuild {
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  std::shared_ptr<const TopoDS_Edge> edge;
};

// Owning, traversal-order-independent result used by live OCCT history
// capture. Each solid sketch primitive maps to exactly one edge of face.
struct SketchProfileFaceBuild {
  std::shared_ptr<const TopoDS_Face> face;
  std::vector<SketchProfileEdgeBuild> edges;
};

[[nodiscard]] bool buildPlanarFaceFromSketchWithEdges(
    const DocumentSketch& profile, SketchProfileFaceBuild* result,
    std::string* error);

bool buildPlanarFaceFromSketch(const DocumentSketch& profile,
                               TopoDS_Face* face, std::string* error);
bool buildExtrusionPrismFromSketch(const DocumentSketch& profile,
                                   double distanceMm, bool reversed,
                                   TopoDS_Shape* prism, std::string* error);

}  // namespace solidar

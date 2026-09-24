#include "model/EdgeFeatureLimits.h"

#include <BRepExtrema_DistShapeShape.hxx>
#include <Precision.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace solidar {

namespace {

bool containsEdge(const TopoDS_Face& face, const TopoDS_Edge& edge) {
  for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More();
       explorer.Next()) {
    if (explorer.Current().IsSame(edge)) return true;
  }
  return false;
}

bool sharesVertex(const TopoDS_Face& face, const TopoDS_Edge& edge) {
  for (TopExp_Explorer edgeVertices(edge, TopAbs_VERTEX); edgeVertices.More();
       edgeVertices.Next()) {
    for (TopExp_Explorer faceVertices(face, TopAbs_VERTEX); faceVertices.More();
         faceVertices.Next()) {
      if (edgeVertices.Current().IsSame(faceVertices.Current())) return true;
    }
  }
  return false;
}

bool belongsToSelectedRegion(const TopoDS_Face& face,
                             const std::vector<TopoDS_Edge>& edges) {
  return std::any_of(edges.begin(), edges.end(), [&](const TopoDS_Edge& edge) {
    return containsEdge(face, edge) || sharesVertex(face, edge);
  });
}

}  // namespace

std::optional<double> minimumEdgeFeatureClearance(
    const TopoDS_Shape& shape, const std::vector<TopoDS_Edge>& edges) {
  double minimum = std::numeric_limits<double>::max();
  for (const auto& edge : edges) {
    for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
      const auto face = TopoDS::Face(faces.Current());
      if (belongsToSelectedRegion(face, edges)) continue;

      BRepExtrema_DistShapeShape distance(edge, face);
      distance.Perform();
      if (!distance.IsDone() || distance.NbSolution() == 0) continue;
      const double value = distance.Value();
      if (value > Precision::Confusion()) minimum = std::min(minimum, value);
    }
  }
  if (!std::isfinite(minimum) || minimum == std::numeric_limits<double>::max())
    return std::nullopt;
  return minimum;
}

}  // namespace solidar

#include "model/FilletBuilder.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/EdgeFeatureLimits.h"
#include "model/TopologyReferenceResolver.h"
#include "model/ShapeContainerUtils.h"

namespace solidar {

namespace {

bool staysWithinSourceBounds(const TopoDS_Shape& source,
                            const TopoDS_Shape& result) {
  Bnd_Box sourceBounds;
  Bnd_Box resultBounds;
  BRepBndLib::AddOptimal(source, sourceBounds, false, false);
  BRepBndLib::AddOptimal(result, resultBounds, false, false);
  if (sourceBounds.IsVoid() || resultBounds.IsVoid()) return false;

  double sourceMinX = 0.0, sourceMinY = 0.0, sourceMinZ = 0.0;
  double sourceMaxX = 0.0, sourceMaxY = 0.0, sourceMaxZ = 0.0;
  double resultMinX = 0.0, resultMinY = 0.0, resultMinZ = 0.0;
  double resultMaxX = 0.0, resultMaxY = 0.0, resultMaxZ = 0.0;
  sourceBounds.Get(sourceMinX, sourceMinY, sourceMinZ, sourceMaxX, sourceMaxY,
                   sourceMaxZ);
  resultBounds.Get(resultMinX, resultMinY, resultMinZ, resultMaxX, resultMaxY,
                   resultMaxZ);
  const double scale = std::max(
      {1.0, sourceMaxX - sourceMinX, sourceMaxY - sourceMinY,
       sourceMaxZ - sourceMinZ});
  const double tolerance = scale * 1e-6;
  return resultMinX >= sourceMinX - tolerance &&
         resultMinY >= sourceMinY - tolerance &&
         resultMinZ >= sourceMinZ - tolerance &&
         resultMaxX <= sourceMaxX + tolerance &&
         resultMaxY <= sourceMaxY + tolerance &&
         resultMaxZ <= sourceMaxZ + tolerance;
}

}  // namespace

std::shared_ptr<TopoDS_Shape> buildFilletShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& edgeIndices,
    double radiusMm, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return std::shared_ptr<TopoDS_Shape>{};
  };
  if (baseShape.IsNull()) return fail("Fillet base shape is missing");
  if (!std::isfinite(radiusMm) || radiusMm <= 0.0)
    return fail("Fillet radius must be a finite positive value");
  if (edgeIndices.empty()) return fail("Fillet requires at least one edge");
  try {
    std::string mappingError;
    auto selection = mapEdgesToOwningSolids(baseShape, edgeIndices, &mappingError);
    if (!selection) return fail("Fillet " + mappingError);
    for (std::size_t solidIndex = 0; solidIndex < selection->solids.size();
         ++solidIndex) {
      const auto& indices = selection->localEdgeIndices[solidIndex];
      if (indices.empty()) continue;
      BRepFilletAPI_MakeFillet maker(selection->solids[solidIndex]);
      std::vector<TopoDS_Edge> selectedEdges;
      selectedEdges.reserve(indices.size());
      for (const auto edgeIndex : indices) {
        const auto edge = resolveEdge(selection->solids[solidIndex], edgeIndex);
        if (!edge) return fail("Fillet edge could not be resolved in owning solid");
        maker.Add(radiusMm, *edge);
        selectedEdges.push_back(*edge);
      }
      if (const auto clearance = minimumEdgeFeatureClearance(
              selection->solids[solidIndex], selectedEdges)) {
        const double tolerance = std::max(1e-7, *clearance * 1e-6);
        if (radiusMm >= *clearance - tolerance)
          return fail("Fillet exceeds the source shape boundary");
      }
      maker.Build();
      if (!maker.IsDone() || maker.Shape().IsNull())
        return fail("Fillet could not be built with the requested radius");
      const TopoDS_Shape result = maker.Shape();
      if (!BRepCheck_Analyzer(result).IsValid())
        return fail("Fillet result is invalid or self-intersecting");
      if (!staysWithinSourceBounds(selection->solids[solidIndex], result))
        return fail("Fillet exceeds the source shape boundary");
      selection->solids[solidIndex] = result;
    }
    auto result = rebuildSolidContainer(selection->solids);
    if (!result || result->IsNull() || !BRepCheck_Analyzer(*result).IsValid())
      return fail("Fillet result is invalid or self-intersecting");
    return result;
  } catch (const Standard_Failure&) {
    return fail("Fillet could not be built with the requested radius");
  } catch (...) {
    return fail("Unexpected Fillet geometry error");
  }
}

}  // namespace solidar

#include "model/ChamferBuilder.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/TopologyReferenceResolver.h"
#include "model/EdgeFeatureLimits.h"
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

std::shared_ptr<TopoDS_Shape> buildChamferShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& edgeIndices,
    double distanceMm, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return std::shared_ptr<TopoDS_Shape>{};
  };
  if (baseShape.IsNull()) return fail("Chamfer base shape is missing");
  if (!std::isfinite(distanceMm) || distanceMm <= 0.0)
    return fail("Chamfer distance must be a finite positive value");
  if (edgeIndices.empty()) return fail("Chamfer requires at least one edge");
  try {
    std::string mappingError;
    auto selection = mapEdgesToOwningSolids(baseShape, edgeIndices, &mappingError);
    if (!selection) return fail("Chamfer " + mappingError);
    for (std::size_t solidIndex = 0; solidIndex < selection->solids.size();
         ++solidIndex) {
      const auto& indices = selection->localEdgeIndices[solidIndex];
      if (indices.empty()) continue;
      BRepFilletAPI_MakeChamfer maker(selection->solids[solidIndex]);
      std::vector<TopoDS_Edge> selectedEdges;
      selectedEdges.reserve(indices.size());
      for (const auto edgeIndex : indices) {
        const auto edge = resolveEdge(selection->solids[solidIndex], edgeIndex);
        if (!edge) return fail("Chamfer edge could not be resolved in owning solid");
        maker.Add(distanceMm, *edge);
        selectedEdges.push_back(*edge);
      }
      if (const auto clearance = minimumEdgeFeatureClearance(
              selection->solids[solidIndex], selectedEdges)) {
        const double tolerance = std::max(1e-7, *clearance * 1e-6);
        if (distanceMm >= *clearance - tolerance)
          return fail("Chamfer exceeds the source shape boundary");
      }
      maker.Build();
      if (!maker.IsDone() || maker.Shape().IsNull())
        return fail("Chamfer could not be built with the requested distance");
      const TopoDS_Shape result = maker.Shape();
      if (!BRepCheck_Analyzer(result).IsValid())
        return fail("Chamfer result is invalid or self-intersecting");
      if (!staysWithinSourceBounds(selection->solids[solidIndex], result))
        return fail("Chamfer exceeds the source shape boundary");
      selection->solids[solidIndex] = result;
    }
    auto result = rebuildSolidContainer(selection->solids);
    if (!result || result->IsNull() || !BRepCheck_Analyzer(*result).IsValid())
      return fail("Chamfer result is invalid or self-intersecting");
    return result;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message
                    ? std::string("OCCT Chamfer error: ") + message
                    : "Chamfer could not be built with the requested distance");
  } catch (const std::exception& failure) {
    return fail(std::string("Chamfer error: ") + failure.what());
  } catch (...) {
    return fail("Unexpected Chamfer geometry error");
  }
}

}  // namespace solidar

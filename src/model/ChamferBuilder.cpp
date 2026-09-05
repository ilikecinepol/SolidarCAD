#include "model/ChamferBuilder.h"

#include <BRepFilletAPI_MakeChamfer.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/TopologyReferenceResolver.h"
#include "model/ShapeContainerUtils.h"

namespace solidar {

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
      for (const auto edgeIndex : indices) {
        const auto edge = resolveEdge(selection->solids[solidIndex], edgeIndex);
        if (!edge) return fail("Chamfer edge could not be resolved in owning solid");
        maker.Add(distanceMm, *edge);
      }
      maker.Build();
      if (!maker.IsDone() || maker.Shape().IsNull())
        return fail("Chamfer could not be built with the requested distance");
      selection->solids[solidIndex] = maker.Shape();
    }
    return rebuildSolidContainer(selection->solids);
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

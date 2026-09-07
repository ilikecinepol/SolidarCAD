#include "model/FilletBuilder.h"

#include <BRepFilletAPI_MakeFillet.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/TopologyReferenceResolver.h"
#include "model/ShapeContainerUtils.h"

namespace solidar {

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
      for (const auto edgeIndex : indices) {
        const auto edge = resolveEdge(selection->solids[solidIndex], edgeIndex);
        if (!edge) return fail("Fillet edge could not be resolved in owning solid");
        maker.Add(radiusMm, *edge);
      }
      maker.Build();
      if (!maker.IsDone() || maker.Shape().IsNull())
        return fail("Fillet could not be built with the requested radius");
      selection->solids[solidIndex] = maker.Shape();
    }
    return rebuildSolidContainer(selection->solids);
  } catch (const Standard_Failure&) {
    return fail("Fillet could not be built with the requested radius");
  } catch (...) {
    return fail("Unexpected Fillet geometry error");
  }
}

}  // namespace solidar

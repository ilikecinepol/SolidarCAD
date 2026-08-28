#include "model/FilletBuilder.h"

#include <BRepFilletAPI_MakeFillet.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/TopologyReferenceResolver.h"

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
    BRepFilletAPI_MakeFillet maker(baseShape);
    for (const std::size_t edgeIndex : edgeIndices) {
      const auto edge = resolveEdge(baseShape, edgeIndex);
      if (!edge) return fail("Fillet edge could not be resolved");
      maker.Add(radiusMm, *edge);
    }
    maker.Build();
    if (!maker.IsDone() || maker.Shape().IsNull())
      return fail("Fillet could not be built with the requested radius");
    TopoDS_Shape result = maker.Shape();
    if (result.ShapeType() != TopAbs_SOLID) {
      TopExp_Explorer solids(result, TopAbs_SOLID);
      if (!solids.More()) return fail("Fillet result does not contain a solid");
      result = solids.Current();
      solids.Next();
      if (solids.More()) return fail("Fillet result contains multiple solids");
    }
    return std::make_shared<TopoDS_Shape>(result);
  } catch (const Standard_Failure&) {
    return fail("Fillet could not be built with the requested radius");
  } catch (...) {
    return fail("Unexpected Fillet geometry error");
  }
}

}  // namespace solidar

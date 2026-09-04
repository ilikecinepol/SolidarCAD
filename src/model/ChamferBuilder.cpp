#include "model/ChamferBuilder.h"

#include <BRepFilletAPI_MakeChamfer.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/TopologyReferenceResolver.h"

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
    BRepFilletAPI_MakeChamfer maker(baseShape);
    for (const std::size_t edgeIndex : edgeIndices) {
      const auto edge = resolveEdge(baseShape, edgeIndex);
      if (!edge) return fail("Chamfer edge could not be resolved");
      maker.Add(distanceMm, *edge);
    }
    maker.Build();
    if (!maker.IsDone() || maker.Shape().IsNull())
      return fail("Chamfer could not be built with the requested distance");
    TopoDS_Shape result = maker.Shape();
    if (result.ShapeType() != TopAbs_SOLID) {
      TopExp_Explorer solids(result, TopAbs_SOLID);
      if (!solids.More()) return fail("Chamfer result does not contain a solid");
      result = solids.Current();
      solids.Next();
      if (solids.More()) return fail("Chamfer result contains multiple solids");
    }
    return std::make_shared<TopoDS_Shape>(result);
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

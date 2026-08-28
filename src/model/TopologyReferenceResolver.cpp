#include "model/TopologyReferenceResolver.h"

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <Standard_Failure.hxx>

namespace solidar {

std::optional<TopoDS_Edge> resolveEdge(const TopoDS_Shape& shape,
                                       std::size_t edgeIndex) {
  if (shape.IsNull()) return std::nullopt;
  try {
    std::size_t current = 0;
    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
         explorer.Next(), ++current) {
      if (current == edgeIndex) return TopoDS::Edge(explorer.Current());
    }
  } catch (const Standard_Failure&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, const TopologyReference& reference) {
  if (reference.kind != TopologyKind::Edge || !reference.hasValidOwner())
    return std::nullopt;
  return resolveEdge(shape, reference.legacyIndex);
}

}  // namespace solidar

#include "model/TopologyReferenceResolver.h"

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

namespace solidar {

std::optional<TopoDS_Edge> resolveEdge(const TopoDS_Shape& shape,
                                       std::size_t edgeIndex) {
  if (shape.IsNull()) return std::nullopt;
  std::size_t current = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
       explorer.Next(), ++current) {
    if (current == edgeIndex) return TopoDS::Edge(explorer.Current());
  }
  return std::nullopt;
}

}  // namespace solidar

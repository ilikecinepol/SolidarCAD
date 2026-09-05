#include "model/ShapeContainerUtils.h"

#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <optional>
#include "model/TopologyReferenceResolver.h"

namespace solidar {

std::optional<SolidEdgeSelection> mapEdgesToOwningSolids(
    const TopoDS_Shape& shape, const std::vector<std::size_t>& globalIndices,
    std::string* error) {
  SolidEdgeSelection result;
  for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More();
       explorer.Next()) {
    const auto solid = explorer.Current();
    if (std::none_of(result.solids.begin(), result.solids.end(),
                     [&solid](const auto& item) { return item.IsSame(solid); }))
      result.solids.push_back(solid);
  }
  if (result.solids.empty()) {
    if (error) *error = "Shape does not contain a solid";
    return std::nullopt;
  }
  result.localEdgeIndices.resize(result.solids.size());
  for (const auto globalIndex : globalIndices) {
    const auto selected = resolveEdge(shape, globalIndex);
    if (!selected) {
      if (error) *error = "Selected edge could not be resolved";
      return std::nullopt;
    }
    bool owned = false;
    for (std::size_t solidIndex = 0; solidIndex < result.solids.size(); ++solidIndex) {
      std::size_t localIndex = 0;
      for (TopExp_Explorer edges(result.solids[solidIndex], TopAbs_EDGE);
           edges.More(); edges.Next(), ++localIndex) {
        if (edges.Current().IsSame(*selected)) {
          result.localEdgeIndices[solidIndex].push_back(localIndex);
          owned = true;
          break;
        }
      }
      if (owned) break;
    }
    if (!owned) {
      if (error) *error = "Selected edge owning solid could not be resolved";
      return std::nullopt;
    }
  }
  return result;
}

std::shared_ptr<TopoDS_Shape> rebuildSolidContainer(
    const std::vector<TopoDS_Shape>& solids) {
  if (solids.empty()) return {};
  if (solids.size() == 1)
    return std::make_shared<TopoDS_Shape>(solids.front());
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  for (const auto& solid : solids) builder.Add(compound, solid);
  return std::make_shared<TopoDS_Shape>(compound);
}

}  // namespace solidar

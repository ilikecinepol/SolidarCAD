#include "model/ShapeContainerUtils.h"

#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <optional>
#include <unordered_map>

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
  result.localEdges.resize(result.solids.size());
  std::unordered_map<std::size_t, std::vector<std::size_t>> requested;
  for (std::size_t i = 0; i < globalIndices.size(); ++i)
    requested[globalIndices[i]].push_back(i);
  std::vector<std::optional<TopoDS_Shape>> selected(globalIndices.size());
  std::size_t rawIndex = 0;
  for (TopExp_Explorer edges(shape, TopAbs_EDGE); edges.More();
       edges.Next(), ++rawIndex) {
    const auto found = requested.find(rawIndex);
    if (found == requested.end()) continue;
    for (const auto position : found->second) selected[position] = edges.Current();
  }
  for (const auto& selectedEdge : selected) {
    if (!selectedEdge) {
      if (error) *error = "Selected edge could not be resolved";
      return std::nullopt;
    }
    bool owned = false;
    for (std::size_t solidIndex = 0; solidIndex < result.solids.size(); ++solidIndex) {
      for (TopExp_Explorer edges(result.solids[solidIndex], TopAbs_EDGE);
           edges.More(); edges.Next()) {
        if (edges.Current().IsSame(*selectedEdge)) {
          result.localEdges[solidIndex].push_back(TopoDS::Edge(edges.Current()));
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

std::optional<SolidFaceSelection> mapFacesToOwningSolids(
    const TopoDS_Shape& shape, const std::vector<std::size_t>& globalIndices,
    std::string* error) {
  SolidFaceSelection result;
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
  result.localFaces.resize(result.solids.size());
  std::unordered_map<std::size_t, std::vector<std::size_t>> requested;
  for (std::size_t i = 0; i < globalIndices.size(); ++i)
    requested[globalIndices[i]].push_back(i);
  std::vector<std::optional<TopoDS_Shape>> selected(globalIndices.size());
  std::size_t rawIndex = 0;
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More();
       faces.Next(), ++rawIndex) {
    const auto found = requested.find(rawIndex);
    if (found == requested.end()) continue;
    for (const auto position : found->second) selected[position] = faces.Current();
  }
  for (const auto& selectedFace : selected) {
    if (!selectedFace) {
      if (error) *error = "Selected face could not be resolved";
      return std::nullopt;
    }
    bool owned = false;
    for (std::size_t solidIndex = 0; solidIndex < result.solids.size();
         ++solidIndex) {
      for (TopExp_Explorer faces(result.solids[solidIndex], TopAbs_FACE);
           faces.More(); faces.Next()) {
        if (faces.Current().IsSame(*selectedFace)) {
          result.localFaces[solidIndex].push_back(TopoDS::Face(faces.Current()));
          owned = true;
          break;
        }
      }
      if (owned) break;
    }
    if (!owned) {
      if (error) *error = "Selected face owning solid could not be resolved";
      return std::nullopt;
    }
  }
  return result;
}

std::shared_ptr<TopoDS_Shape> rebuildSolidContainer(
    const std::vector<TopoDS_Shape>& solids) {
  if (solids.empty()) return {};
  if (solids.size() == 1) {
    const auto& result = solids.front();
    if (result.ShapeType() == TopAbs_SOLID)
      return std::make_shared<TopoDS_Shape>(result);

    // Some OCCT builders wrap a single result solid in a compound. Keep the
    // feature's public shape stable as a solid while preserving genuine
    // multi-solid results below.
    std::optional<TopoDS_Shape> onlySolid;
    for (TopExp_Explorer explorer(result, TopAbs_SOLID); explorer.More();
         explorer.Next()) {
      if (!onlySolid) {
        onlySolid = explorer.Current();
      } else if (!onlySolid->IsSame(explorer.Current())) {
        onlySolid.reset();
        break;
      }
    }
    if (onlySolid) return std::make_shared<TopoDS_Shape>(*onlySolid);
    return std::make_shared<TopoDS_Shape>(result);
  }
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  for (const auto& solid : solids) builder.Add(compound, solid);
  return std::make_shared<TopoDS_Shape>(compound);
}

}  // namespace solidar

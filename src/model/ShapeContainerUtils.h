#pragma once

#include <TopoDS_Shape.hxx>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace solidar {

struct SolidEdgeSelection {
  std::vector<TopoDS_Shape> solids;
  std::vector<std::vector<std::size_t>> localEdgeIndices;
};

struct SolidFaceSelection {
  std::vector<TopoDS_Shape> solids;
  std::vector<std::vector<std::size_t>> localFaceIndices;
};

[[nodiscard]] std::optional<SolidEdgeSelection> mapEdgesToOwningSolids(
    const TopoDS_Shape& shape, const std::vector<std::size_t>& globalEdgeIndices,
    std::string* error = nullptr);
[[nodiscard]] std::optional<SolidFaceSelection> mapFacesToOwningSolids(
    const TopoDS_Shape& shape, const std::vector<std::size_t>& globalFaceIndices,
    std::string* error = nullptr);
[[nodiscard]] std::shared_ptr<TopoDS_Shape> rebuildSolidContainer(
    const std::vector<TopoDS_Shape>& solids);

}  // namespace solidar

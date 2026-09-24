#pragma once

#include <optional>
#include <vector>

#include <TopoDS_Edge.hxx>

class TopoDS_Shape;

namespace solidar {

// Returns the shortest distance from the selected edge group to a face outside
// that group's connected region. Such a face is the first external boundary
// that an expanding fillet or chamfer may collide with.
[[nodiscard]] std::optional<double> minimumEdgeFeatureClearance(
    const TopoDS_Shape& shape, const std::vector<TopoDS_Edge>& edges);

}  // namespace solidar

#pragma once

#include <cstddef>
#include <optional>

class TopoDS_Edge;
class TopoDS_Shape;

namespace solidar {

[[nodiscard]] std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, std::size_t edgeIndex);

}  // namespace solidar

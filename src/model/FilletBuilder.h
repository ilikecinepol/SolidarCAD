#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class TopoDS_Shape;

namespace solidar {

[[nodiscard]] std::shared_ptr<TopoDS_Shape> buildFilletShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& edgeIndices,
    double radiusMm, std::string* error = nullptr);

}  // namespace solidar

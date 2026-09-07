#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class TopoDS_Shape;

namespace solidar {

[[nodiscard]] std::shared_ptr<TopoDS_Shape> buildChamferShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& edgeIndices,
    double distanceMm, std::string* error = nullptr);

}  // namespace solidar

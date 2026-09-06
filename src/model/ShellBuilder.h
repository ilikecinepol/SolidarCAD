#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class TopoDS_Shape;

namespace solidar {

[[nodiscard]] std::shared_ptr<TopoDS_Shape> buildShellShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& faceIndices,
    double thicknessMm, bool outside, std::string* error = nullptr);

}  // namespace solidar

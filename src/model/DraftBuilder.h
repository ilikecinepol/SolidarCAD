#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pln.hxx>

class TopoDS_Shape;

namespace solidar {

[[nodiscard]] std::shared_ptr<TopoDS_Shape> buildDraftShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& faceIndices,
    const gp_Pln& neutralPlane, const gp_Dir& pullDirection,
    double angleDeg, bool reversed, std::string* error = nullptr);

}  // namespace solidar

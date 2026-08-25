#include "model/ShapeFeature.h"

#include <utility>

namespace solidar {

const ShapeFeature::ShapePtr& ShapeFeature::shape() const noexcept {
  return shape_;
}

bool ShapeFeature::hasShape() const noexcept { return bool(shape_); }

void ShapeFeature::setShape(ShapePtr shape) noexcept {
  shape_ = std::move(shape);
}

void ShapeFeature::clearShape() noexcept { shape_.reset(); }

}  // namespace solidar

#pragma once

#include <memory>

#include "model/Feature.h"

class TopoDS_Shape;

namespace solidar {

// Parametric features own their generated B-Rep result. The incomplete OCCT
// type keeps the document model buildable until the geometry adapter is linked.
class ShapeFeature : public Feature {
 public:
  using ShapePtr = std::shared_ptr<const TopoDS_Shape>;

  using Feature::Feature;
  ~ShapeFeature() override = default;

  [[nodiscard]] const ShapePtr& shape() const noexcept;
  [[nodiscard]] bool hasShape() const noexcept;
  void discardResult() noexcept;

 protected:
  void setShape(ShapePtr shape) noexcept;
  void clearShape() noexcept;

 private:
  ShapePtr shape_;
};

}  // namespace solidar

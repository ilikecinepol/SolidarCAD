#pragma once

#include <string>

#include "model/ShapeFeature.h"
#include "model/SketchPlacement.h"

namespace solidar {

[[nodiscard]] ShapeFeature::ShapePtr buildMovedShape(
    const TopoDS_Shape& source, Vector3d offsetMm,
    std::string* error = nullptr);

class MoveFeature final : public ShapeFeature {
 public:
  MoveFeature(FeatureId sourceFeatureId, Vector3d offsetMm,
              std::string name = {});
  MoveFeature(FeatureId id, FeatureId sourceFeatureId, Vector3d offsetMm,
              std::string name);

  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] Vector3d offsetMm() const noexcept;
  void setOffsetMm(Vector3d offsetMm) noexcept;
  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  Vector3d offsetMm_{};
};

}  // namespace solidar

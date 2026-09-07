#pragma once

#include "model/PatternTypes.h"
#include "model/ShapeFeature.h"

namespace solidar {
class CircularPatternFeature final : public ShapeFeature {
 public:
  CircularPatternFeature(FeatureId sourceFeatureId, PrincipalAxis axis,
                         int count, double angleDeg, std::string name = {});
  CircularPatternFeature(FeatureId id, FeatureId sourceFeatureId,
                         PrincipalAxis axis, int count, double angleDeg,
                         std::string name);
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] PrincipalAxis axis() const noexcept;
  [[nodiscard]] int count() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  void setAxis(PrincipalAxis value) noexcept;
  void setCount(int value) noexcept;
  void setAngleDeg(double value) noexcept;
  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;
 private:
  FeatureId sourceFeatureId_{};
  PrincipalAxis axis_{PrincipalAxis::Z};
  int count_{2};
  double angleDeg_{360.0};
};
}  // namespace solidar

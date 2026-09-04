#pragma once

#include "model/PatternTypes.h"
#include "model/ShapeFeature.h"

namespace solidar {
class LinearPatternFeature final : public ShapeFeature {
 public:
  LinearPatternFeature(FeatureId sourceFeatureId, PrincipalAxis direction,
                       int count, double spacingMm, std::string name = {});
  LinearPatternFeature(FeatureId id, FeatureId sourceFeatureId,
                       PrincipalAxis direction, int count, double spacingMm,
                       std::string name);
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] PrincipalAxis direction() const noexcept;
  [[nodiscard]] int count() const noexcept;
  [[nodiscard]] double spacingMm() const noexcept;
  void setDirection(PrincipalAxis value) noexcept;
  void setCount(int value) noexcept;
  void setSpacingMm(double value) noexcept;
  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;
 private:
  FeatureId sourceFeatureId_{};
  PrincipalAxis direction_{PrincipalAxis::X};
  int count_{2};
  double spacingMm_{10.0};
};
}  // namespace solidar

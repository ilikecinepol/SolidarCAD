#pragma once

#include <string>

#include "model/Body.h"
#include "model/PatternTypes.h"
#include "model/ShapeFeature.h"

namespace solidar {

[[nodiscard]] ShapeFeature::ShapePtr buildLinearPatternShape(
    const TopoDS_Shape& source, PrincipalAxis direction, int count,
    double spacingMm, std::string* error = nullptr,
    bool includeSource = true);

class LinearPatternFeature final : public ShapeFeature {
 public:
  LinearPatternFeature(FeatureId sourceFeatureId, PrincipalAxis direction,
                       int count, double spacingMm, std::string name = {});
  LinearPatternFeature(FeatureId id, FeatureId sourceFeatureId,
                       PrincipalAxis direction, int count, double spacingMm,
                       std::string name);
  LinearPatternFeature(BodyId sourceBodyId, FeatureId sourceFeatureId,
                       PrincipalAxis direction, int count, double spacingMm,
                       PatternOperation operation, std::string name = {});
  LinearPatternFeature(FeatureId id, BodyId sourceBodyId,
                       FeatureId sourceFeatureId, PrincipalAxis direction,
                       int count, double spacingMm, PatternOperation operation,
                       std::string name);
  [[nodiscard]] BodyId sourceBodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] PrincipalAxis direction() const noexcept;
  [[nodiscard]] int count() const noexcept;
  [[nodiscard]] double spacingMm() const noexcept;
  [[nodiscard]] PatternOperation operation() const noexcept;
  void setDirection(PrincipalAxis value) noexcept;
  void setCount(int value) noexcept;
  void setSpacingMm(double value) noexcept;
  [[nodiscard]] FeatureDependencies dependencies() const override;
  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::LinearPattern;
  }
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  BodyId sourceBodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{};
  PrincipalAxis direction_{PrincipalAxis::X};
  int count_{2};
  double spacingMm_{10.0};
  PatternOperation operation_{PatternOperation::Join};
};
}  // namespace solidar

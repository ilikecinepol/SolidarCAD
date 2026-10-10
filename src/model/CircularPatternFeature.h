#pragma once

#include <string>

#include "model/Body.h"
#include "model/PatternTypes.h"
#include "model/ShapeFeature.h"

namespace solidar {

[[nodiscard]] ShapeFeature::ShapePtr buildCircularPatternShape(
    const TopoDS_Shape& source, PrincipalAxis axis, int count,
    double angleDeg, std::string* error = nullptr,
    bool includeSource = true);

class CircularPatternFeature final : public ShapeFeature {
 public:
  CircularPatternFeature(FeatureId sourceFeatureId, PrincipalAxis axis,
                         int count, double angleDeg, std::string name = {});
  CircularPatternFeature(FeatureId id, FeatureId sourceFeatureId,
                         PrincipalAxis axis, int count, double angleDeg,
                         std::string name);
  CircularPatternFeature(BodyId sourceBodyId, FeatureId sourceFeatureId,
                         PrincipalAxis axis, int count, double angleDeg,
                         PatternOperation operation, std::string name = {});
  CircularPatternFeature(FeatureId id, BodyId sourceBodyId,
                         FeatureId sourceFeatureId, PrincipalAxis axis,
                         int count, double angleDeg, PatternOperation operation,
                         std::string name);
  [[nodiscard]] BodyId sourceBodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] PrincipalAxis axis() const noexcept;
  [[nodiscard]] int count() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  [[nodiscard]] PatternOperation operation() const noexcept;
  void setAxis(PrincipalAxis value) noexcept;
  void setCount(int value) noexcept;
  void setAngleDeg(double value) noexcept;
  [[nodiscard]] FeatureDependencies dependencies() const override;
  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::CircularPattern;
  }
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  BodyId sourceBodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{};
  PrincipalAxis axis_{PrincipalAxis::Z};
  int count_{2};
  double angleDeg_{kMaximumPatternAngleDeg};
  PatternOperation operation_{PatternOperation::Join};
};
}  // namespace solidar

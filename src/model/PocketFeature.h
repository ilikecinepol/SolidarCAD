#pragma once

#include "model/Document.h"
#include "model/ShapeFeature.h"

namespace solidar {

class PocketFeature final : public ShapeFeature {
 public:
  PocketFeature(SketchId profileSketchId, double depthMm,
                std::string name = {});
  PocketFeature(FeatureId id, SketchId profileSketchId, double depthMm,
                std::string name);

  [[nodiscard]] SketchId profileSketchId() const noexcept;
  [[nodiscard]] double depthMm() const noexcept;
  void setProfileSketchId(SketchId id) noexcept;
  void setDepthMm(double value) noexcept;

  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::Pocket;
  }
  [[nodiscard]] FeatureDependencies dependencies() const override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  SketchId profileSketchId_{kInvalidSketchId};
  double depthMm_{0.0};
};

}  // namespace solidar

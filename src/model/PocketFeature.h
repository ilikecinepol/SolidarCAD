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

  [[nodiscard]] std::string typeName() const override;
  [[nodiscard]] bool dependsOnSketch(SketchId sketchId) const noexcept override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  SketchId profileSketchId_{kInvalidSketchId};
  double depthMm_{0.0};
};

}  // namespace solidar

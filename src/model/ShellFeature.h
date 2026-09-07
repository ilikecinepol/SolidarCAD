#pragma once

#include <vector>

#include "model/ShapeFeature.h"
#include "model/SketchPlacement.h"

namespace solidar {

class ShellFeature final : public ShapeFeature {
 public:
  ShellFeature(FeatureId sourceFeatureId, std::vector<FaceReference> removedFaces,
               double thicknessMm, bool outside = false,
               std::string name = {});
  ShellFeature(FeatureId id, FeatureId sourceFeatureId,
               std::vector<FaceReference> removedFaces, double thicknessMm,
               bool outside, std::string name);

  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] const std::vector<FaceReference>& removedFaces() const noexcept;
  [[nodiscard]] double thicknessMm() const noexcept;
  [[nodiscard]] bool outside() const noexcept;
  void setRemovedFaces(std::vector<FaceReference> value);
  void setThicknessMm(double value) noexcept;
  void setOutside(bool value) noexcept;

  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::vector<FaceReference> removedFaces_;
  double thicknessMm_{2.0};
  bool outside_{false};
};

}  // namespace solidar

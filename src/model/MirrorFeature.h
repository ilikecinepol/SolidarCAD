#pragma once

#include "model/PatternTypes.h"
#include "model/ShapeFeature.h"

namespace solidar {

class MirrorFeature final : public ShapeFeature {
 public:
  MirrorFeature(FeatureId sourceFeatureId, MirrorPlane plane,
                std::string name = {});
  MirrorFeature(FeatureId id, FeatureId sourceFeatureId, MirrorPlane plane,
                std::string name);
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] MirrorPlane plane() const noexcept;
  void setPlane(MirrorPlane plane) noexcept;
  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  FeatureId sourceFeatureId_{};
  MirrorPlane plane_{MirrorPlane::YZ};
};

}  // namespace solidar

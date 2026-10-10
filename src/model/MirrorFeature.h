#pragma once

#include <string>

#include "model/PatternTypes.h"
#include "model/ShapeFeature.h"

namespace solidar {

[[nodiscard]] ShapeFeature::ShapePtr buildMirrorShape(
    const TopoDS_Shape& source, MirrorPlane plane,
    std::string* error = nullptr);

class MirrorFeature final : public ShapeFeature {
 public:
  MirrorFeature(FeatureId sourceFeatureId, MirrorPlane plane,
                std::string name = {});
  MirrorFeature(FeatureId id, FeatureId sourceFeatureId, MirrorPlane plane,
                std::string name);
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] MirrorPlane plane() const noexcept;
  void setPlane(MirrorPlane plane) noexcept;
  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::Mirror;
  }
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  FeatureId sourceFeatureId_{};
  MirrorPlane plane_{MirrorPlane::YZ};
};

}  // namespace solidar

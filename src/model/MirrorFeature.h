#pragma once

#include <optional>
#include <string>

#include "model/PatternTypes.h"
#include "model/ShapeFeature.h"
#include "model/SketchPlacement.h"

namespace solidar {

class TopologyIndex;

[[nodiscard]] ShapeFeature::ShapePtr buildMirrorShape(
    const TopoDS_Shape& source, MirrorPlane plane,
    std::string* error = nullptr);
[[nodiscard]] ShapeFeature::ShapePtr buildMirrorShape(
    const TopoDS_Shape& source, const FaceReference& planeFace,
    std::string* error = nullptr);
[[nodiscard]] ShapeFeature::ShapePtr buildMirrorShape(
    const TopoDS_Shape& source, const TopologyIndex& topology,
    const FaceReference& planeFace, std::string* error = nullptr);

class MirrorFeature final : public ShapeFeature {
 public:
  MirrorFeature(FeatureId sourceFeatureId, MirrorPlane plane,
                std::string name = {});
  MirrorFeature(FeatureId id, FeatureId sourceFeatureId, MirrorPlane plane,
                std::string name);
  MirrorFeature(FeatureId sourceFeatureId, FaceReference planeFace,
                std::string name = {});
  MirrorFeature(FeatureId id, FeatureId sourceFeatureId,
                FaceReference planeFace, std::string name);
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] MirrorPlane plane() const noexcept;
  [[nodiscard]] const std::optional<FaceReference>& planeFace() const noexcept;
  void setPlane(MirrorPlane plane) noexcept;
  void setPlaneFace(FaceReference planeFace);
  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::Mirror;
  }
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  FeatureId sourceFeatureId_{};
  MirrorPlane plane_{MirrorPlane::YZ};
  std::optional<FaceReference> planeFace_;
};

}  // namespace solidar

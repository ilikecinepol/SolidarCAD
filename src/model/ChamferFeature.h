#pragma once

#include <vector>

#include "model/ShapeFeature.h"
#include "model/SketchPlacement.h"

namespace solidar {

class ChamferFeature final : public ShapeFeature {
 public:
  ChamferFeature(EdgeReference edge, double distanceMm,
                 std::string name = {});
  ChamferFeature(std::vector<EdgeReference> edges, double distanceMm,
                 std::string name = {});
  ChamferFeature(FeatureId id, std::vector<EdgeReference> edges,
                 double distanceMm, std::string name);

  [[nodiscard]] const EdgeReference& edge() const noexcept;
  [[nodiscard]] const std::vector<EdgeReference>& edges() const noexcept;
  [[nodiscard]] double distanceMm() const noexcept;
  void setEdges(std::vector<EdgeReference> edges) noexcept;
  void setDistanceMm(double value) noexcept;

  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  std::vector<EdgeReference> edges_;
  double distanceMm_{};
};

}  // namespace solidar

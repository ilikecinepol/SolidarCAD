#pragma once

#include "model/ShapeFeature.h"
#include "model/SketchPlacement.h"

#include <vector>

namespace solidar {

class FilletFeature final : public ShapeFeature {
 public:
  FilletFeature(EdgeReference edge, double radiusMm, std::string name = {});
  FilletFeature(std::vector<EdgeReference> edges, double radiusMm,
                std::string name = {});
  FilletFeature(FeatureId id, EdgeReference edge, double radiusMm,
                std::string name);
  FilletFeature(FeatureId id, std::vector<EdgeReference> edges,
                double radiusMm, std::string name);

  [[nodiscard]] const EdgeReference& edge() const noexcept;
  [[nodiscard]] const std::vector<EdgeReference>& edges() const noexcept;
  [[nodiscard]] double radiusMm() const noexcept;
  void setEdge(EdgeReference edge) noexcept;
  void setEdges(std::vector<EdgeReference> edges) noexcept;
  void setRadiusMm(double value) noexcept;

  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  std::vector<EdgeReference> edges_;
  double radiusMm_{};
};

}  // namespace solidar

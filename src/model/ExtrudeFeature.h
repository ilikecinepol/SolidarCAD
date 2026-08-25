#pragma once

#include "model/Document.h"
#include "model/ShapeFeature.h"

namespace solidar {

class ExtrudeFeature final : public ShapeFeature {
 public:
  ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                 std::string name = {});
  ExtrudeFeature(FeatureId id, SketchId profileSketchId, double lengthMm,
                 std::string name);

  [[nodiscard]] SketchId profileSketchId() const noexcept;
  void setProfileSketchId(SketchId id) noexcept;
  [[nodiscard]] double lengthMm() const noexcept;
  void setLengthMm(double value) noexcept;

  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  SketchId profileSketchId_{kInvalidSketchId};
  double lengthMm_{0.0};
};

}  // namespace solidar

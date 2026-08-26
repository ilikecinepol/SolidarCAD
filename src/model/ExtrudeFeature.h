#pragma once

#include "model/Document.h"
#include "model/ShapeFeature.h"

namespace solidar {

enum class ExtrudeOperation { NewBody, Join, Cut };

class ExtrudeFeature final : public ShapeFeature {
 public:
  ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                 std::string name = {});
  ExtrudeFeature(SketchId profileSketchId, double lengthMm, std::string name,
                 ExtrudeOperation operation, bool reversed = false);
  ExtrudeFeature(FeatureId id, SketchId profileSketchId, double lengthMm,
                 std::string name);
  ExtrudeFeature(FeatureId id, SketchId profileSketchId, double lengthMm,
                 std::string name, ExtrudeOperation operation,
                 bool reversed = false);

  [[nodiscard]] SketchId profileSketchId() const noexcept;
  void setProfileSketchId(SketchId id) noexcept;
  [[nodiscard]] double lengthMm() const noexcept;
  void setLengthMm(double value) noexcept;
  [[nodiscard]] ExtrudeOperation operation() const noexcept;
  void setOperation(ExtrudeOperation value) noexcept;
  [[nodiscard]] bool reversed() const noexcept;
  void setReversed(bool value) noexcept;

  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  SketchId profileSketchId_{kInvalidSketchId};
  double lengthMm_{0.0};
  ExtrudeOperation operation_{ExtrudeOperation::NewBody};
  bool reversed_{false};
};

}  // namespace solidar

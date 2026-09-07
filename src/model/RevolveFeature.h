#pragma once

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ShapeFeature.h"

namespace solidar {

enum class AxisReferenceType {
  SketchHorizontalAxis = 0,
  SketchVerticalAxis = 1,
  SketchLine = 2,
  GlobalX = 3,
  GlobalY = 4,
  GlobalZ = 5
};

struct AxisReference {
  AxisReferenceType type{AxisReferenceType::SketchHorizontalAxis};
  SketchId sketchId{kInvalidSketchId};
  sketch::GeometryId lineId{sketch::kInvalidGeometryId};
  friend bool operator==(const AxisReference&, const AxisReference&) = default;
};

class RevolveFeature final : public ShapeFeature {
 public:
  RevolveFeature(SketchId profileSketchId, AxisReference axis,
                 double angleDeg = 360.0, std::string name = {},
                 ExtrudeOperation operation = ExtrudeOperation::NewBody,
                 bool reversed = false);
  RevolveFeature(FeatureId id, SketchId profileSketchId, AxisReference axis,
                 double angleDeg, std::string name,
                 ExtrudeOperation operation = ExtrudeOperation::NewBody,
                 bool reversed = false);

  [[nodiscard]] SketchId profileSketchId() const noexcept;
  [[nodiscard]] const AxisReference& axis() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  [[nodiscard]] ExtrudeOperation operation() const noexcept;
  [[nodiscard]] bool reversed() const noexcept;
  void setProfileSketchId(SketchId value) noexcept;
  void setAxis(AxisReference value) noexcept;
  void setAngleDeg(double value) noexcept;
  void setOperation(ExtrudeOperation value) noexcept;
  void setReversed(bool value) noexcept;

  [[nodiscard]] std::string typeName() const override;
  [[nodiscard]] bool dependsOnSketch(SketchId id) const noexcept override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  SketchId profileSketchId_{kInvalidSketchId};
  AxisReference axis_{};
  double angleDeg_{360.0};
  ExtrudeOperation operation_{ExtrudeOperation::NewBody};
  bool reversed_{false};
};

}  // namespace solidar

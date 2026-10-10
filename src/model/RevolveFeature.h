#pragma once

#include <optional>

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

[[nodiscard]] constexpr bool isSketchAxisReferenceType(
    AxisReferenceType type) noexcept {
  return type == AxisReferenceType::SketchHorizontalAxis ||
         type == AxisReferenceType::SketchVerticalAxis ||
         type == AxisReferenceType::SketchLine;
}

[[nodiscard]] constexpr bool isKnownAxisReferenceType(
    AxisReferenceType type) noexcept {
  switch (type) {
    case AxisReferenceType::SketchHorizontalAxis:
    case AxisReferenceType::SketchVerticalAxis:
    case AxisReferenceType::SketchLine:
    case AxisReferenceType::GlobalX:
    case AxisReferenceType::GlobalY:
    case AxisReferenceType::GlobalZ:
      return true;
  }
  return false;
}

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
  [[nodiscard]] const std::optional<sketch::Sketch>&
  profileOverride() const noexcept;
  void setProfileOverride(std::optional<sketch::Sketch> profile);
  void setAxis(AxisReference value) noexcept;
  void setAngleDeg(double value) noexcept;
  void setOperation(ExtrudeOperation value) noexcept;
  void setReversed(bool value) noexcept;

  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::Revolve;
  }
  [[nodiscard]] FeatureDependencies dependencies() const override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  SketchId profileSketchId_{kInvalidSketchId};
  std::optional<sketch::Sketch> profileOverride_;
  AxisReference axis_{};
  double angleDeg_{360.0};
  ExtrudeOperation operation_{ExtrudeOperation::NewBody};
  bool reversed_{false};
};

}  // namespace solidar

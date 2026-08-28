#pragma once

#include <optional>

#include "model/RevolveFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class RevolveToolSession final : public ToolSession {
 public:
  void begin(const Document& document, BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape = {});
  void setProfile(SketchId profileSketchId);
  void clearProfile();
  void setAxis(AxisReference axis);
  void clearAxis();
  void setAngleFromPanel(double angleDeg);
  void setAngleFromManipulator(double angleDeg);
  void setOperation(ExtrudeOperation operation);
  void setReversed(bool reversed);

  [[nodiscard]] SketchId profileSketchId() const noexcept;
  [[nodiscard]] const std::optional<AxisReference>& axis() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  [[nodiscard]] ExtrudeOperation operation() const noexcept;
  [[nodiscard]] bool reversed() const noexcept;
  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<AngularToolManipulator> manipulator() const;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  const Document* document_{};
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  ShapeFeature::ShapePtr baseShape_;
  SketchId profileSketchId_{kInvalidSketchId};
  std::optional<AxisReference> axis_;
  double angleDeg_{360.0};
  ExtrudeOperation operation_{ExtrudeOperation::NewBody};
  bool reversed_{false};
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
};

}  // namespace solidar

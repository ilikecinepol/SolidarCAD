#pragma once

#include <optional>

#include "model/CircularPatternFeature.h"
#include "model/NumericParameterState.h"
#include "model/ToolSession.h"

namespace solidar {

class CircularPatternToolSession final : public ToolSession {
 public:
  void begin(double angleDeg = kMaximumPatternAngleDeg, int count = 4,
             PatternOperation operation = PatternOperation::NewBody,
             std::optional<FeatureId> editingFeatureId = std::nullopt);
  void setBody(BodyId bodyId, FeatureId sourceFeatureId,
               ShapeFeature::ShapePtr sourceShape);
  void clearBody();
  void setAxis(PrincipalAxis axis);
  void clearAxis();
  void setAngleDeg(double angleDeg);
  void setCount(int count);
  void setOperation(PatternOperation operation);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] const std::optional<PrincipalAxis>& axis() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  [[nodiscard]] int count() const noexcept;
  [[nodiscard]] PatternOperation operation() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement>
  selectionRequirement() const override;
  [[nodiscard]] std::vector<ToolParameterDescriptor> parameters() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  [[nodiscard]] std::optional<AngularToolManipulator> manipulator() const;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  ShapeFeature::ShapePtr sourceShape_;
  std::optional<PrincipalAxis> axis_;
  NumericParameterState angle_;
  int count_{4};
  PatternOperation operation_{PatternOperation::NewBody};
  std::optional<FeatureId> editingFeatureId_;
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
};

}  // namespace solidar

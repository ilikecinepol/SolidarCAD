#pragma once

#include <optional>

#include "model/MoveFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class MoveToolSession final : public ToolSession {
 public:
  void begin(Vector3d offsetMm = {},
             std::optional<FeatureId> editingFeatureId = std::nullopt);
  void setBody(BodyId bodyId, FeatureId sourceFeatureId,
               ShapeFeature::ShapePtr sourceShape);
  void clearBody();
  void setOffsetMm(Vector3d offsetMm);
  void setOffsetComponent(int axisIndex, double valueMm);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] Vector3d offsetMm() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement>
  selectionRequirement() const override;
  [[nodiscard]] std::vector<ToolParameterDescriptor> parameters() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  [[nodiscard]] std::optional<TranslationToolManipulator> manipulator() const;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  ShapeFeature::ShapePtr sourceShape_;
  Vector3d offsetMm_{};
  std::optional<FeatureId> editingFeatureId_;
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
};

}  // namespace solidar

#pragma once

#include <optional>
#include <vector>

#include "model/ShapeFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class FilletToolSession final : public ToolSession {
 public:
  void begin(BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape,
             std::vector<EdgeReference> edges, double radiusMm,
             std::optional<FeatureId> editingFeatureId = std::nullopt);
  void setEdges(std::vector<EdgeReference> edges);
  void setRadiusFromPanel(double radiusMm);
  void setRadiusFromManipulator(double radiusMm);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] const std::vector<EdgeReference>& edges() const noexcept;
  [[nodiscard]] double radiusMm() const noexcept;
  [[nodiscard]] std::optional<LinearToolManipulator> manipulator() const;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement> selectionRequirement() const override;
  [[nodiscard]] std::vector<ToolParameterDescriptor> parameters() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::optional<FeatureId> editingFeatureId_;
  ShapeFeature::ShapePtr baseShape_;
  std::vector<EdgeReference> edges_;
  double radiusMm_{5.0};
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
};

}  // namespace solidar

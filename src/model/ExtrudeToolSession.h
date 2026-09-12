#pragma once

#include <optional>

#include "model/ExtrudeFeature.h"
#include "model/FaceExtrudeBuilder.h"
#include "model/NumericParameterState.h"
#include "model/ShapeFeature.h"
#include "model/ToolSession.h"

namespace solidar {

// Native face-extrusion tool session. Face source only: the legacy sketch
// extrusion path in MainWindow is unchanged.
class ExtrudeToolSession final : public ToolSession {
 public:
  void begin(BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape, FaceReference face,
             double lengthMm, ExtrudeOperation operation, bool reversed,
             std::optional<FeatureId> editingFeatureId = std::nullopt);
  void setFace(FaceReference face);
  void setLengthFromPanel(double lengthMm);
  void setLengthFromManipulator(double lengthMm);
  void setOperation(ExtrudeOperation operation);
  void setReversed(bool reversed);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] const FaceReference& face() const noexcept;
  [[nodiscard]] double lengthMm() const noexcept;
  [[nodiscard]] ExtrudeOperation operation() const noexcept;
  [[nodiscard]] bool reversed() const noexcept;
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
  bool trySetLength(double lengthMm);

  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::optional<FeatureId> editingFeatureId_;
  ShapeFeature::ShapePtr baseShape_;
  FaceReference face_;
  NumericParameterState length_;
  double minimumMm_{0.01};
  double maximumMm_{100000.0};
  ExtrudeOperation operation_{ExtrudeOperation::Join};
  bool reversed_{false};
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::optional<FaceExtrudeGeometry> geometry_;
  std::string error_;
};

}  // namespace solidar

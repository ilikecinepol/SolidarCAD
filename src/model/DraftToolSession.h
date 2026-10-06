#pragma once

#include <optional>
#include <vector>

#include "model/DraftFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class DraftToolSession final : public ToolSession {
 public:
  void begin(Document& document, BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape,
             std::vector<FaceReference> faces = {},
             std::optional<PlaneReference> neutralPlane = std::nullopt,
             std::optional<AxisReference> pullDirection = std::nullopt,
             double angleDeg = 5.0, bool reversed = false,
             std::optional<FeatureId> editingFeatureId = std::nullopt,
             std::optional<EdgeReference> rotationEdge = std::nullopt);
  void setFaces(std::vector<FaceReference> value);
  void setNeutralPlane(PlaneReference value);
  void clearNeutralPlane();
  void setPullDirection(AxisReference value);
  void clearPullDirection();
  // Principal axes derive their matching neutral plane atomically; a body
  // edge is stored separately as a persistent topological reference.
  bool setPrincipalAxis(int axisIndex);
  bool setRotationEdge(EdgeReference value);
  void clearPrincipalAxis();
  [[nodiscard]] std::optional<int> principalAxisIndex() const noexcept;
  void setAngleFromPanel(double value);
  void setAngleFromManipulator(double value);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] const std::vector<FaceReference>& faces() const noexcept;
  [[nodiscard]] const std::optional<PlaneReference>& neutralPlane() const noexcept;
  [[nodiscard]] const std::optional<AxisReference>& pullDirection() const noexcept;
  [[nodiscard]] const std::optional<EdgeReference>& rotationEdge() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  [[nodiscard]] std::optional<AngularToolManipulator> manipulator() const;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement> selectionRequirement() const override;
  [[nodiscard]] std::vector<ToolParameterDescriptor> parameters() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  Document* document_{};
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::optional<FeatureId> editingFeatureId_;
  ShapeFeature::ShapePtr baseShape_;
  std::vector<FaceReference> faces_;
  std::optional<PlaneReference> neutralPlane_;
  std::optional<AxisReference> pullDirection_;
  std::optional<EdgeReference> rotationEdge_;
  double angleDeg_{5.0};
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  ShapeFeature::ShapePtr lastValidPreviewShape_;
  std::string error_;
};

}  // namespace solidar

#pragma once

#include <optional>
#include <vector>

#include "model/DraftFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class DraftToolSession final : public ToolSession {
 public:
  void begin(const Document& document, BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape,
             std::vector<FaceReference> faces = {},
             std::optional<PlaneReference> neutralPlane = std::nullopt,
             std::optional<AxisReference> pullDirection = std::nullopt,
             double angleDeg = 5.0, bool reversed = false,
             std::optional<FeatureId> editingFeatureId = std::nullopt,
             std::optional<EdgeReference> rotationEdge = std::nullopt,
             std::shared_ptr<const TopologyIndex> topologyIndex = {});
  void setFaces(const Document& document, std::vector<FaceReference> value);
  void setNeutralPlane(const Document& document, PlaneReference value);
  void clearNeutralPlane(const Document& document);
  void setPullDirection(const Document& document, AxisReference value);
  void clearPullDirection(const Document& document);
  // Principal axes derive their matching neutral plane atomically; a body
  // edge is stored separately as a persistent topological reference.
  bool setPrincipalAxis(const Document& document, int axisIndex);
  bool setRotationEdge(const Document& document, EdgeReference value);
  void clearPrincipalAxis(const Document& document);
  [[nodiscard]] std::optional<int> principalAxisIndex() const noexcept;
  void setAngleFromPanel(const Document& document, double value);
  void setAngleFromManipulator(const Document& document, double value);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] const std::vector<FaceReference>& faces() const noexcept;
  [[nodiscard]] const std::optional<PlaneReference>& neutralPlane() const noexcept;
  [[nodiscard]] const std::optional<AxisReference>& pullDirection() const noexcept;
  [[nodiscard]] const std::optional<EdgeReference>& rotationEdge() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  [[nodiscard]] std::optional<AngularToolManipulator> manipulator(
      const Document& document) const;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement> selectionRequirement() const override;
  [[nodiscard]] std::vector<ToolParameterDescriptor> parameters() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  [[nodiscard]] OperationFailureCode errorCode() const noexcept override;
  bool updatePreview() override;
  bool updatePreview(const Document& document);
  void cancel() noexcept override;

 private:
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::optional<FeatureId> editingFeatureId_;
  ShapeFeature::ShapePtr baseShape_;
  std::shared_ptr<const TopologyIndex> topologyIndex_;
  std::string topologyIndexError_;
  std::vector<FaceReference> faces_;
  std::optional<PlaneReference> neutralPlane_;
  std::optional<AxisReference> pullDirection_;
  std::optional<EdgeReference> rotationEdge_;
  double angleDeg_{5.0};
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  ShapeFeature::ShapePtr lastValidPreviewShape_;
  std::string error_;
  OperationFailureCode errorCode_{OperationFailureCode::None};
};

}  // namespace solidar

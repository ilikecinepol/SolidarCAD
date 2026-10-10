#pragma once

#include <optional>
#include <cstdint>
#include <vector>

#include "model/ShapeFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class ShellToolSession final : public ToolSession {
 public:
  void begin(BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape,
             std::vector<FaceReference> removedFaces = {},
             double thicknessMm = 2.0, bool outside = false,
             std::optional<FeatureId> editingFeatureId = std::nullopt,
             std::shared_ptr<const TopologyIndex> topologyIndex = {});
  void setRemovedFaces(std::vector<FaceReference> value);
  void setThicknessFromPanel(double value);
  void setThicknessFromManipulator(double value);
  bool refineThicknessToBoundary(double requestedThicknessMm);
  void setOutside(bool value);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] const std::vector<FaceReference>& removedFaces() const noexcept;
  [[nodiscard]] double thicknessMm() const noexcept;
  [[nodiscard]] bool outside() const noexcept;
  [[nodiscard]] std::optional<double> maximumValidThicknessMm() const noexcept;
  [[nodiscard]] bool limitReached() const noexcept;
  [[nodiscard]] std::uint64_t previewBuildAttemptCount() const noexcept;
  [[nodiscard]] std::optional<LinearToolManipulator> manipulator() const;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement> selectionRequirement() const override;
  [[nodiscard]] std::vector<ToolParameterDescriptor> parameters() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  [[nodiscard]] OperationFailureCode errorCode() const noexcept override;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  bool trySetThickness(double value, bool refineBoundary);
  bool recoverBelowInvalidThickness(double upperInvalid);
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::optional<FeatureId> editingFeatureId_;
  ShapeFeature::ShapePtr baseShape_;
  std::shared_ptr<const TopologyIndex> topologyIndex_;
  std::string topologyIndexError_;
  std::vector<FaceReference> removedFaces_;
  double thicknessMm_{2.0};
  bool outside_{false};
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
  OperationFailureCode errorCode_{OperationFailureCode::None};
  std::optional<double> maximumValidThicknessMm_;
  std::optional<double> pendingRequestedThicknessMm_;
  bool limitReached_{false};
  std::uint64_t previewBuildAttemptCount_{};
};

}  // namespace solidar

#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "model/ShapeFeature.h"
#include "model/NumericParameterState.h"
#include "model/ToolSession.h"

namespace solidar {

class ChamferToolSession final : public ToolSession {
 public:
  using BuildShape = std::function<std::shared_ptr<TopoDS_Shape>(
      const TopoDS_Shape&, const std::vector<std::size_t>&, double,
      std::string*)>;

  explicit ChamferToolSession(BuildShape buildShape = {});
  void begin(BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape,
             std::vector<EdgeReference> edges, double distanceMm,
             std::optional<FeatureId> editingFeatureId = std::nullopt);
  void setEdges(std::vector<EdgeReference> edges);
  bool setDistanceFromPanel(double distanceMm);
  bool setDistanceFromManipulator(double distanceMm);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] const std::vector<EdgeReference>& edges() const noexcept;
  [[nodiscard]] double distanceMm() const noexcept;
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
  bool trySetDistance(double distanceMm);
  void updateValidatedMaximum(const std::vector<std::size_t>& edgeIndices);
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::optional<FeatureId> editingFeatureId_;
  ShapeFeature::ShapePtr baseShape_;
  std::vector<EdgeReference> edges_;
  BuildShape buildShape_;
  NumericParameterState distance_;
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
};

}  // namespace solidar

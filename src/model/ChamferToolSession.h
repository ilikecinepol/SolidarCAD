#pragma once

#include <optional>
#include <vector>

#include "model/ShapeFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class ChamferToolSession final : public ToolSession {
 public:
  void begin(BodyId bodyId, FeatureId sourceFeatureId,
             ShapeFeature::ShapePtr baseShape,
             std::vector<EdgeReference> edges, double distanceMm,
             std::optional<FeatureId> editingFeatureId = std::nullopt);
  void setEdges(std::vector<EdgeReference> edges);
  void setDistanceFromPanel(double distanceMm);
  void setDistanceFromManipulator(double distanceMm);

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept;
  [[nodiscard]] const std::vector<EdgeReference>& edges() const noexcept;
  [[nodiscard]] double distanceMm() const noexcept;
  [[nodiscard]] std::optional<LinearToolManipulator> manipulator() const;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
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
  double distanceMm_{2.0};
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
};

}  // namespace solidar

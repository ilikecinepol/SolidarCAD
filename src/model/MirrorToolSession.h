#pragma once

#include <optional>

#include "model/MirrorFeature.h"
#include "model/ToolSession.h"

namespace solidar {

class MirrorToolSession final : public ToolSession {
 public:
  void begin(std::optional<FeatureId> editingFeatureId = std::nullopt);
  void setBody(BodyId bodyId, FeatureId sourceFeatureId,
               ShapeFeature::ShapePtr sourceShape);
  void clearBody();
  void setPlane(MirrorPlane plane);
  void clearPlane();

  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] const std::optional<MirrorPlane>& plane() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement>
  selectionRequirement() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  BodyId bodyId_{kInvalidBodyId};
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  ShapeFeature::ShapePtr sourceShape_;
  std::optional<MirrorPlane> plane_;
  std::optional<FeatureId> editingFeatureId_;
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
};

}  // namespace solidar

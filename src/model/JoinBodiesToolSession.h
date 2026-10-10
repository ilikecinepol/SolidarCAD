#pragma once

#include <vector>

#include "model/JoinBodiesFeature.h"
#include "model/ToolSession.h"

namespace solidar {

struct JoinBodyInput {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeFeature::ShapePtr shape;
};

class JoinBodiesToolSession final : public ToolSession {
 public:
  void begin(std::optional<FeatureId> editingFeatureId = std::nullopt,
             BodyId ownerBodyId = kInvalidBodyId);
  void setBodies(std::vector<JoinBodyInput> bodies);

  [[nodiscard]] const std::vector<JoinBodyInput>& bodies() const noexcept;
  [[nodiscard]] std::optional<FeatureId> editingFeatureId() const noexcept override;
  [[nodiscard]] ToolLifecycle lifecycle() const noexcept override;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept override;
  [[nodiscard]] std::optional<SelectionRequirement>
  selectionRequirement() const override;
  [[nodiscard]] std::shared_ptr<const TopoDS_Shape> previewShape() const override;
  [[nodiscard]] const std::string& error() const noexcept override;
  [[nodiscard]] OperationFailureCode errorCode() const noexcept override;
  bool updatePreview() override;
  void cancel() noexcept override;

 private:
  std::vector<JoinBodyInput> bodies_;
  ToolLifecycle lifecycle_{ToolLifecycle::Inactive};
  ShapeFeature::ShapePtr previewShape_;
  std::string error_;
  OperationFailureCode errorCode_{OperationFailureCode::None};
  std::optional<FeatureId> editingFeatureId_;
  BodyId ownerBodyId_{kInvalidBodyId};
};

}  // namespace solidar

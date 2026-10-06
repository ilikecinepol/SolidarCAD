#include "model/JoinBodiesToolSession.h"

#include <TopoDS_Shape.hxx>

#include <utility>

namespace solidar {

void JoinBodiesToolSession::begin() {
  bodies_.clear();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::SelectingInput;
}

void JoinBodiesToolSession::setBodies(std::vector<JoinBodyInput> bodies) {
  if (bodies.size() > 2) bodies.resize(2);
  bodies_ = std::move(bodies);
  updatePreview();
}

const std::vector<JoinBodyInput>& JoinBodiesToolSession::bodies() const noexcept {
  return bodies_;
}

std::optional<FeatureId> JoinBodiesToolSession::editingFeatureId() const noexcept {
  return std::nullopt;
}

ToolLifecycle JoinBodiesToolSession::lifecycle() const noexcept {
  return lifecycle_;
}

ToolSelectionStage JoinBodiesToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  return bodies_.size() < 2 ? ToolSelectionStage::SelectingInput
                            : ToolSelectionStage::EditingParameters;
}

std::optional<SelectionRequirement>
JoinBodiesToolSession::selectionRequirement() const {
  if (selectionStage() != ToolSelectionStage::SelectingInput) return std::nullopt;
  return SelectionRequirement{SelectionType::Body, "Select two bodies", 2, 2,
                              true};
}

std::shared_ptr<const TopoDS_Shape>
JoinBodiesToolSession::previewShape() const {
  return previewShape_;
}

const std::string& JoinBodiesToolSession::error() const noexcept {
  return error_;
}

bool JoinBodiesToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (bodies_.size() != 2 || !bodies_[0].shape || !bodies_[1].shape) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  previewShape_ =
      buildJoinedBodiesShape(*bodies_[0].shape, *bodies_[1].shape, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

void JoinBodiesToolSession::cancel() noexcept {
  bodies_.clear();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

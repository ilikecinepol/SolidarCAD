#include "model/MirrorToolSession.h"

#include <TopoDS_Shape.hxx>

#include <utility>

namespace solidar {

void MirrorToolSession::begin(std::optional<FeatureId> editingFeatureId) {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  plane_.reset();
  editingFeatureId_ = editingFeatureId;
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::SelectingInput;
}

void MirrorToolSession::setBody(BodyId bodyId, FeatureId sourceFeatureId,
                                ShapeFeature::ShapePtr sourceShape) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  sourceShape_ = std::move(sourceShape);
  plane_.reset();
  updatePreview();
}

void MirrorToolSession::clearBody() {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  plane_.reset();
  updatePreview();
}

void MirrorToolSession::setPlane(MirrorPlane plane) {
  plane_ = plane;
  updatePreview();
}

void MirrorToolSession::clearPlane() {
  plane_.reset();
  updatePreview();
}

BodyId MirrorToolSession::bodyId() const noexcept { return bodyId_; }

FeatureId MirrorToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}

const std::optional<MirrorPlane>& MirrorToolSession::plane() const noexcept {
  return plane_;
}

std::optional<FeatureId> MirrorToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}

ToolLifecycle MirrorToolSession::lifecycle() const noexcept {
  return lifecycle_;
}

ToolSelectionStage MirrorToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  if (bodyId_ == kInvalidBodyId) return ToolSelectionStage::SelectingInput;
  if (!plane_) return ToolSelectionStage::SelectingReference;
  return ToolSelectionStage::EditingParameters;
}

std::optional<SelectionRequirement>
MirrorToolSession::selectionRequirement() const {
  if (selectionStage() == ToolSelectionStage::SelectingInput)
    return SelectionRequirement{SelectionType::Body, "Select body", 1, 1,
                                false};
  if (selectionStage() == ToolSelectionStage::SelectingReference)
    return SelectionRequirement{SelectionType::Plane, "Select mirror plane", 1,
                                1, false};
  return std::nullopt;
}

std::shared_ptr<const TopoDS_Shape> MirrorToolSession::previewShape() const {
  return previewShape_;
}

const std::string& MirrorToolSession::error() const noexcept { return error_; }

bool MirrorToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (bodyId_ == kInvalidBodyId || sourceFeatureId_ == kInvalidFeatureId ||
      !sourceShape_) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  if (!plane_) {
    lifecycle_ = ToolLifecycle::SelectingReference;
    return false;
  }
  previewShape_ = buildMirrorShape(*sourceShape_, *plane_, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return bool(previewShape_);
}

void MirrorToolSession::cancel() noexcept {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  plane_.reset();
  editingFeatureId_.reset();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

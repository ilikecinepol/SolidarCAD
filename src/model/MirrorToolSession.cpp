#include "model/MirrorToolSession.h"

#include <TopoDS_Shape.hxx>

#include <utility>

#include "model/TopologyReferenceResolver.h"

namespace solidar {

void MirrorToolSession::begin(std::optional<FeatureId> editingFeatureId) {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  topologyIndex_.reset();
  topologyIndexError_.clear();
  plane_.reset();
  planeFace_.reset();
  editingFeatureId_ = editingFeatureId;
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::SelectingInput;
}

void MirrorToolSession::setBody(BodyId bodyId, FeatureId sourceFeatureId,
                                ShapeFeature::ShapePtr sourceShape,
                                std::shared_ptr<const TopologyIndex> topologyIndex) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  sourceShape_ = std::move(sourceShape);
  topologyIndexError_.clear();
  topologyIndex_ = std::move(topologyIndex);
  if (topologyIndex_ &&
      (!sourceShape_ || sourceShape_->IsNull() || !topologyIndex_->shape() ||
       topologyIndex_->shape().get() != sourceShape_.get()))
    topologyIndex_.reset();
  if (!topologyIndex_ && sourceShape_ && !sourceShape_->IsNull())
    topologyIndex_ = TopologyIndex::build(
        sourceShape_, kInvalidShapeRevision, &topologyIndexError_);
  plane_.reset();
  planeFace_.reset();
  updatePreview();
}

void MirrorToolSession::clearBody() {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  topologyIndex_.reset();
  topologyIndexError_.clear();
  plane_.reset();
  planeFace_.reset();
  updatePreview();
}

void MirrorToolSession::setPlane(MirrorPlane plane) {
  plane_ = plane;
  planeFace_.reset();
  updatePreview();
}

void MirrorToolSession::setPlaneFace(FaceReference planeFace) {
  plane_.reset();
  planeFace_ = std::move(planeFace);
  updatePreview();
}

void MirrorToolSession::clearPlane() {
  plane_.reset();
  planeFace_.reset();
  updatePreview();
}

BodyId MirrorToolSession::bodyId() const noexcept { return bodyId_; }

FeatureId MirrorToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}

const std::optional<MirrorPlane>& MirrorToolSession::plane() const noexcept {
  return plane_;
}

const std::optional<FaceReference>& MirrorToolSession::planeFace() const noexcept {
  return planeFace_;
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
  if (!plane_ && !planeFace_) return ToolSelectionStage::SelectingReference;
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
  if (!plane_ && !planeFace_) {
    lifecycle_ = ToolLifecycle::SelectingReference;
    return false;
  }
  if (planeFace_ &&
      (planeFace_->bodyId() != bodyId_ ||
       planeFace_->featureId() != sourceFeatureId_)) {
    error_ = "Mirror plane face must belong to the selected source Feature";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (planeFace_ && !topologyIndex_) {
    error_ = "Mirror topology could not be indexed";
    if (!topologyIndexError_.empty()) error_ += ": " + topologyIndexError_;
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  previewShape_ = planeFace_
                      ? buildMirrorShape(*sourceShape_, *topologyIndex_,
                                         *planeFace_, &error_)
                      : buildMirrorShape(*sourceShape_, *plane_, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return bool(previewShape_);
}

void MirrorToolSession::cancel() noexcept {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  topologyIndex_.reset();
  topologyIndexError_.clear();
  plane_.reset();
  planeFace_.reset();
  editingFeatureId_.reset();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

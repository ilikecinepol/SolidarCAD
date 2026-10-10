#include "model/ShellToolSession.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

#include "model/ShellBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

void ShellToolSession::begin(BodyId bodyId, FeatureId sourceFeatureId,
                             ShapeFeature::ShapePtr baseShape,
                             std::vector<FaceReference> faces,
                             double thickness, bool outside,
                             std::optional<FeatureId> editingFeatureId,
                             std::shared_ptr<const TopologyIndex> topologyIndex) {
  bodyId_ = bodyId; sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape); removedFaces_ = std::move(faces);
  topologyIndexError_.clear();
  topologyIndex_ = std::move(topologyIndex);
  if (topologyIndex_ &&
      (!baseShape_ || baseShape_->IsNull() || !topologyIndex_->shape() ||
       topologyIndex_->shape().get() != baseShape_.get()))
    topologyIndex_.reset();
  if (!topologyIndex_ && baseShape_ && !baseShape_->IsNull())
    topologyIndex_ = TopologyIndex::build(baseShape_, kInvalidShapeRevision,
                                          &topologyIndexError_);
  thicknessMm_ = thickness; outside_ = outside;
  editingFeatureId_ = editingFeatureId;
  maximumValidThicknessMm_.reset();
  pendingRequestedThicknessMm_.reset();
  limitReached_ = false;
  previewBuildAttemptCount_ = 0;
  lifecycle_ = ToolLifecycle::Editing;
  updatePreview();
}
void ShellToolSession::setRemovedFaces(std::vector<FaceReference> value) {
  removedFaces_ = std::move(value);
  maximumValidThicknessMm_.reset();
  pendingRequestedThicknessMm_.reset();
  limitReached_ = false;
  updatePreview();
}
void ShellToolSession::setThicknessFromPanel(double value) {
  trySetThickness(value, false);
}
void ShellToolSession::setThicknessFromManipulator(double value) {
  trySetThickness(std::max(0.01, value), false);
}
bool ShellToolSession::refineThicknessToBoundary(
    double requestedThicknessMm) {
  const double target = pendingRequestedThicknessMm_.value_or(
      std::max(0.01, requestedThicknessMm));
  pendingRequestedThicknessMm_.reset();
  return trySetThickness(target, true);
}
void ShellToolSession::setOutside(bool value) {
  outside_ = value;
  maximumValidThicknessMm_.reset();
  limitReached_ = false;
  updatePreview();
}
BodyId ShellToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId ShellToolSession::sourceFeatureId() const noexcept { return sourceFeatureId_; }
std::optional<FeatureId> ShellToolSession::editingFeatureId() const noexcept { return editingFeatureId_; }
const std::vector<FaceReference>& ShellToolSession::removedFaces() const noexcept { return removedFaces_; }
double ShellToolSession::thicknessMm() const noexcept { return thicknessMm_; }
bool ShellToolSession::outside() const noexcept { return outside_; }
std::optional<double> ShellToolSession::maximumValidThicknessMm() const noexcept {
  return maximumValidThicknessMm_;
}
bool ShellToolSession::limitReached() const noexcept { return limitReached_; }
std::uint64_t ShellToolSession::previewBuildAttemptCount() const noexcept {
  return previewBuildAttemptCount_;
}
ToolLifecycle ShellToolSession::lifecycle() const noexcept { return lifecycle_; }
ToolSelectionStage ShellToolSession::selectionStage() const noexcept { if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None; return removedFaces_.empty() ? ToolSelectionStage::SelectingInput : ToolSelectionStage::EditingParameters; }
std::optional<SelectionRequirement> ShellToolSession::selectionRequirement() const { if (!removedFaces_.empty()) return std::nullopt; return SelectionRequirement{SelectionType::Face, "Select faces to remove", 1, static_cast<std::size_t>(-1), true}; }
std::vector<ToolParameterDescriptor> ShellToolSession::parameters() const {
  return {{"thickness", "Thickness", ToolParameterType::Distance, thicknessMm_,
           0.01, maximumValidThicknessMm_.value_or(100000.0), 0.1, "mm", true,
           ToolManipulatorType::Linear},
          {"outside", "Direction", ToolParameterType::Boolean, outside_, 0.0, 1.0, 1.0, {}, false, ToolManipulatorType::None}};
}
std::shared_ptr<const TopoDS_Shape> ShellToolSession::previewShape() const { return previewShape_; }
const std::string& ShellToolSession::error() const noexcept { return error_; }
OperationFailureCode ShellToolSession::errorCode() const noexcept {
  return errorCode_;
}
bool ShellToolSession::updatePreview() {
  previewShape_.reset(); error_.clear(); errorCode_ = OperationFailureCode::None;
  if (!baseShape_ || baseShape_->IsNull()) { errorCode_ = OperationFailureCode::MissingSource; error_ = "Shell base shape is missing"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  if (removedFaces_.empty()) { lifecycle_ = ToolLifecycle::SelectingInput; return false; }
  if (!topologyIndex_) { errorCode_ = OperationFailureCode::TopologyIndexUnavailable; error_ = "Shell topology could not be indexed"; if (!topologyIndexError_.empty()) error_ += ": " + topologyIndexError_; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  std::vector<TopologyReference> references;
  references.reserve(removedFaces_.size());
  std::vector<std::size_t> indices;
  for (const auto& face : removedFaces_) {
    if (face.bodyId != bodyId_ || face.featureId != sourceFeatureId_) { errorCode_ = OperationFailureCode::TopologyReferenceMismatch; error_ = "Shell faces no longer match the active Body"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    references.push_back(face.topology());
  }
  for (const auto& resolved : topologyIndex_->resolveFaces(references)) {
    if (!resolved) { errorCode_ = operationFailureCode(resolved.failure); error_ = "Shell face could not be resolved: " + resolved.error; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    indices.push_back(resolved.index);
  }
  ++previewBuildAttemptCount_;
  previewShape_ = buildShellShape(*baseShape_, indices, thicknessMm_, outside_, &error_);
  if (!previewShape_) errorCode_ = OperationFailureCode::GeometryOperationFailed;
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}
std::optional<LinearToolManipulator> ShellToolSession::manipulator() const {
  if (!baseShape_ || removedFaces_.empty() || !topologyIndex_) return std::nullopt;
  try {
    const auto face = topologyIndex_->resolveFace(removedFaces_.front().topology());
    if (!face) return std::nullopt;
    double u0, u1, v0, v1; BRepTools::UVBounds(*face.subshape, u0, u1, v0, v1);
    BRepAdaptor_Surface surface(*face.subshape);
    gp_Pnt point; gp_Vec du, dv;
    surface.D1((u0 + u1) * 0.5, (v0 + v1) * 0.5, point, du, dv);
    gp_Vec normal = du.Crossed(dv);
    if (normal.SquareMagnitude() < 1e-16) return std::nullopt;
    normal.Normalize(); if (!outside_) normal.Reverse();
    return LinearToolManipulator{{point.X(), point.Y(), point.Z()},
                                 {normal.X(), normal.Y(), normal.Z()}, thicknessMm_,
                                 0.01,
                                 maximumValidThicknessMm_.value_or(100000.0)};
  } catch (const Standard_Failure&) { return std::nullopt; }
}

bool ShellToolSession::trySetThickness(double value, bool refineBoundary) {
  if (!std::isfinite(value) || value < 0.01 || !baseShape_ ||
      removedFaces_.empty())
    return false;
  if (!refineBoundary) {
    pendingRequestedThicknessMm_ = value;
    maximumValidThicknessMm_.reset();
    limitReached_ = false;
  }
  const double previous = thicknessMm_;
  if (std::abs(value - previous) <= 1e-12 &&
      lifecycle_ == ToolLifecycle::PreviewValid)
    return true;
  const auto previousPreview = previewShape_;
  thicknessMm_ = value;
  if (updatePreview()) {
    pendingRequestedThicknessMm_.reset();
    maximumValidThicknessMm_.reset();
    limitReached_ = false;
    return true;
  }
  const std::string failure = error_.empty()
                                  ? "Shell preview could not be built"
                                  : error_;
  const OperationFailureCode failureCode = errorCode_;
  if (refineBoundary && value > previous && previousPreview) {
    thicknessMm_ = previous;
    previewShape_ = previousPreview;
    lifecycle_ = ToolLifecycle::PreviewValid;
    if (recoverBelowInvalidThickness(value)) return true;
  }
  thicknessMm_ = previous;
  previewShape_ = previousPreview;
  lifecycle_ = ToolLifecycle::PreviewInvalid;
  error_ = failure;
  errorCode_ = failureCode;
  return false;
}

bool ShellToolSession::recoverBelowInvalidThickness(double upperInvalid) {
  if (!baseShape_ || removedFaces_.empty() || !std::isfinite(upperInvalid) ||
      upperInvalid <= 0.01)
    return false;

  std::vector<std::size_t> indices;
  indices.reserve(removedFaces_.size());
  if (!topologyIndex_) return false;
  std::vector<TopologyReference> references;
  references.reserve(removedFaces_.size());
  for (const auto& face : removedFaces_) {
    if (face.bodyId != bodyId_ || face.featureId != sourceFeatureId_)
      return false;
    references.push_back(face.topology());
  }
  for (const auto& resolved : topologyIndex_->resolveFaces(references)) {
    if (!resolved) return false;
    indices.push_back(resolved.index);
  }

  double lower = previewShape_ ? thicknessMm_ : 0.01;
  auto boundaryPreview = previewShape_;
  if (!boundaryPreview) {
    thicknessMm_ = lower;
    if (!updatePreview()) {
      thicknessMm_ = upperInvalid;
      return false;
    }
    boundaryPreview = previewShape_;
  }

  double upper = upperInvalid;
  for (int iteration = 0; iteration < 24 && upper - lower > 1e-4;
       ++iteration) {
    const double midpoint = lower + (upper - lower) * 0.5;
    std::string ignored;
    ++previewBuildAttemptCount_;
    auto preview = buildShellShape(*baseShape_, indices, midpoint, outside_,
                                   &ignored);
    if (preview) {
      lower = midpoint;
      boundaryPreview = std::move(preview);
    } else {
      upper = midpoint;
    }
  }

  double panelSafe = std::floor((lower + 1e-9) * 100.0) / 100.0;
  panelSafe = std::max(0.01, panelSafe);
  if (panelSafe < lower) {
    thicknessMm_ = panelSafe;
    if (updatePreview()) {
      lower = panelSafe;
      boundaryPreview = previewShape_;
    }
  }
  thicknessMm_ = lower;
  previewShape_ = std::move(boundaryPreview);
  maximumValidThicknessMm_ = lower;
  limitReached_ = true;
  lifecycle_ = ToolLifecycle::PreviewValid;
  error_.clear();
  errorCode_ = OperationFailureCode::None;
  return true;
}

void ShellToolSession::cancel() noexcept {
  previewShape_.reset();
  topologyIndex_.reset();
  topologyIndexError_.clear();
  removedFaces_.clear();
  error_.clear();
  errorCode_ = OperationFailureCode::None;
  maximumValidThicknessMm_.reset();
  pendingRequestedThicknessMm_.reset();
  limitReached_ = false;
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

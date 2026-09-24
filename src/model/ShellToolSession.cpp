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
                             std::optional<FeatureId> editingFeatureId) {
  bodyId_ = bodyId; sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape); removedFaces_ = std::move(faces);
  thicknessMm_ = thickness; outside_ = outside;
  editingFeatureId_ = editingFeatureId;
  maximumValidThicknessMm_.reset();
  limitReached_ = false;
  lifecycle_ = ToolLifecycle::Editing;
  if (!updatePreview() && !removedFaces_.empty())
    recoverBelowInvalidThickness(thicknessMm_);
}
void ShellToolSession::setRemovedFaces(std::vector<FaceReference> value) {
  removedFaces_ = std::move(value);
  maximumValidThicknessMm_.reset();
  limitReached_ = false;
  if (!updatePreview() && !removedFaces_.empty())
    recoverBelowInvalidThickness(thicknessMm_);
}
void ShellToolSession::setThicknessFromPanel(double value) {
  trySetThickness(value);
}
void ShellToolSession::setThicknessFromManipulator(double value) {
  trySetThickness(std::max(0.01, value));
}
void ShellToolSession::setOutside(bool value) {
  outside_ = value;
  maximumValidThicknessMm_.reset();
  limitReached_ = false;
  if (!updatePreview() && !removedFaces_.empty())
    recoverBelowInvalidThickness(thicknessMm_);
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
bool ShellToolSession::updatePreview() {
  previewShape_.reset(); error_.clear();
  if (!baseShape_ || baseShape_->IsNull()) { error_ = "Shell base shape is missing"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  if (removedFaces_.empty()) { lifecycle_ = ToolLifecycle::SelectingInput; return false; }
  std::vector<std::size_t> indices;
  for (const auto& face : removedFaces_) {
    if (face.bodyId != bodyId_ || face.featureId != sourceFeatureId_) { error_ = "Shell faces no longer match the active Body"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    const auto resolved = resolveFaceReference(*baseShape_, face.topology());
    if (!resolved) { error_ = "Shell face could not be resolved: " + resolved.error; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    indices.push_back(resolved.index);
  }
  previewShape_ = buildShellShape(*baseShape_, indices, thicknessMm_, outside_, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}
std::optional<LinearToolManipulator> ShellToolSession::manipulator() const {
  if (!baseShape_ || removedFaces_.empty()) return std::nullopt;
  try {
    const auto face = resolveFaceReference(*baseShape_, removedFaces_.front().topology());
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

bool ShellToolSession::trySetThickness(double value) {
  if (!std::isfinite(value) || value < 0.01 || !baseShape_ ||
      removedFaces_.empty())
    return false;
  const double previous = thicknessMm_;
  const auto previousPreview = previewShape_;
  thicknessMm_ = value;
  if (updatePreview()) {
    maximumValidThicknessMm_.reset();
    limitReached_ = false;
    return true;
  }
  const std::string failure = error_.empty()
                                  ? "Shell preview could not be built"
                                  : error_;
  if (value > previous && previousPreview) {
    thicknessMm_ = previous;
    previewShape_ = previousPreview;
    lifecycle_ = ToolLifecycle::PreviewValid;
    if (recoverBelowInvalidThickness(value)) return true;
  }
  thicknessMm_ = previous;
  previewShape_ = previousPreview;
  if (previousPreview) {
    if (value > previous) maximumValidThicknessMm_ = previous;
    limitReached_ = true;
    lifecycle_ = ToolLifecycle::PreviewValid;
    error_.clear();
    return false;
  }
  lifecycle_ = ToolLifecycle::PreviewInvalid;
  error_ = failure;
  return false;
}

bool ShellToolSession::recoverBelowInvalidThickness(double upperInvalid) {
  if (!baseShape_ || removedFaces_.empty() || !std::isfinite(upperInvalid) ||
      upperInvalid <= 0.01)
    return false;

  std::vector<std::size_t> indices;
  indices.reserve(removedFaces_.size());
  for (const auto& face : removedFaces_) {
    if (face.bodyId != bodyId_ || face.featureId != sourceFeatureId_)
      return false;
    const auto resolved = resolveFaceReference(*baseShape_, face.topology());
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
  return true;
}

void ShellToolSession::cancel() noexcept {
  previewShape_.reset();
  removedFaces_.clear();
  error_.clear();
  maximumValidThicknessMm_.reset();
  limitReached_ = false;
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

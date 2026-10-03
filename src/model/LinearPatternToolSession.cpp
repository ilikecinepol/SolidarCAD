#include "model/LinearPatternToolSession.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <utility>

namespace solidar {

void LinearPatternToolSession::begin(
    double spacingMm, int count,
    std::optional<FeatureId> editingFeatureId) {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  direction_.reset();
  spacing_.reset(spacingMm, 0.01, 100000.0);
  count_ = std::clamp(count, 2, 100);
  editingFeatureId_ = editingFeatureId;
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::SelectingInput;
}

void LinearPatternToolSession::setBody(BodyId bodyId, FeatureId sourceFeatureId,
                                       ShapeFeature::ShapePtr sourceShape) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  sourceShape_ = std::move(sourceShape);
  direction_.reset();
  updatePreview();
}

void LinearPatternToolSession::clearBody() {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  direction_.reset();
  updatePreview();
}

void LinearPatternToolSession::setDirection(PrincipalAxis direction) {
  direction_ = direction;
  updatePreview();
}

void LinearPatternToolSession::clearDirection() {
  direction_.reset();
  updatePreview();
}

void LinearPatternToolSession::setSpacingMm(double spacingMm) {
  const auto candidate = spacing_.candidate(spacingMm);
  if (!candidate) return;
  spacing_.accept(*candidate);
  updatePreview();
}

void LinearPatternToolSession::setCount(int count) {
  count_ = std::clamp(count, 2, 100);
  updatePreview();
}

BodyId LinearPatternToolSession::bodyId() const noexcept { return bodyId_; }

FeatureId LinearPatternToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}

const std::optional<PrincipalAxis>&
LinearPatternToolSession::direction() const noexcept {
  return direction_;
}

double LinearPatternToolSession::spacingMm() const noexcept {
  return spacing_.value();
}

int LinearPatternToolSession::count() const noexcept { return count_; }

std::optional<FeatureId>
LinearPatternToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}

ToolLifecycle LinearPatternToolSession::lifecycle() const noexcept {
  return lifecycle_;
}

ToolSelectionStage LinearPatternToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  if (bodyId_ == kInvalidBodyId) return ToolSelectionStage::SelectingInput;
  if (!direction_) return ToolSelectionStage::SelectingReference;
  return ToolSelectionStage::EditingParameters;
}

std::optional<SelectionRequirement>
LinearPatternToolSession::selectionRequirement() const {
  if (selectionStage() == ToolSelectionStage::SelectingInput)
    return SelectionRequirement{SelectionType::Body, "Select body", 1, 1,
                                false};
  if (selectionStage() == ToolSelectionStage::SelectingReference)
    return SelectionRequirement{SelectionType::Axis, "Select direction", 1, 1,
                                false};
  return std::nullopt;
}

std::vector<ToolParameterDescriptor>
LinearPatternToolSession::parameters() const {
  return {{"spacing", "Spacing", ToolParameterType::Distance,
           spacing_.value(), 0.01, 100000.0, 0.1, "mm", true,
           ToolManipulatorType::Linear},
          {"count", "Count", ToolParameterType::Integer, count_, 2.0, 100.0,
           1.0, {}, true, ToolManipulatorType::None}};
}

std::shared_ptr<const TopoDS_Shape>
LinearPatternToolSession::previewShape() const {
  return previewShape_;
}

const std::string& LinearPatternToolSession::error() const noexcept {
  return error_;
}

std::optional<LinearToolManipulator>
LinearPatternToolSession::manipulator() const {
  if (!sourceShape_ || sourceShape_->IsNull() || !direction_) return std::nullopt;
  Bnd_Box bounds;
  BRepBndLib::Add(*sourceShape_, bounds);
  if (bounds.IsVoid()) return std::nullopt;
  double minX = 0.0;
  double minY = 0.0;
  double minZ = 0.0;
  double maxX = 0.0;
  double maxY = 0.0;
  double maxZ = 0.0;
  bounds.Get(minX, minY, minZ, maxX, maxY, maxZ);
  const Point3d center{(minX + maxX) * 0.5, (minY + maxY) * 0.5,
                       (minZ + maxZ) * 0.5};
  Vector3d axis{0.0, 0.0, 1.0};
  if (*direction_ == PrincipalAxis::X)
    axis = {1.0, 0.0, 0.0};
  else if (*direction_ == PrincipalAxis::Y)
    axis = {0.0, 1.0, 0.0};
  return LinearToolManipulator{center, axis, spacing_.value(), 0.01, 100000.0};
}

bool LinearPatternToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (bodyId_ == kInvalidBodyId || sourceFeatureId_ == kInvalidFeatureId ||
      !sourceShape_) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  if (!direction_) {
    lifecycle_ = ToolLifecycle::SelectingReference;
    return false;
  }
  previewShape_ = buildLinearPatternShape(
      *sourceShape_, *direction_, count_, spacing_.value(), &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

void LinearPatternToolSession::cancel() noexcept {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  direction_.reset();
  editingFeatureId_.reset();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

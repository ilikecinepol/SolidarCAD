#include "model/LinearPatternToolSession.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <utility>

#include "model/GeometryOperation.h"

namespace solidar {

void LinearPatternToolSession::begin(
    double spacingMm, int count, PatternOperation operation,
    std::optional<FeatureId> editingFeatureId) {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  direction_.reset();
  spacing_.reset(spacingMm, kMinimumPatternParameter,
                 kMaximumPatternSpacingMm);
  count_ = clampPatternCountForUi(count);
  operation_ = operation;
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
  count_ = clampPatternCountForUi(count);
  updatePreview();
}

void LinearPatternToolSession::setOperation(PatternOperation operation) {
  operation_ = operation;
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

PatternOperation LinearPatternToolSession::operation() const noexcept {
  return operation_;
}

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
           spacing_.value(), kMinimumPatternParameter,
           kMaximumPatternSpacingMm, 0.1, "mm", true,
           ToolManipulatorType::Linear},
          {"count", "Count", ToolParameterType::Integer, count_,
           kMinimumPatternCount, kMaximumPatternCount,
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
  std::optional<LinearToolManipulator> result;
  runGeometryOperation([&] {
    if (!sourceShape_ || sourceShape_->IsNull() || !direction_ ||
        !validPrincipalAxis(*direction_))
      return;
    Bnd_Box bounds;
    BRepBndLib::Add(*sourceShape_, bounds);
    if (bounds.IsVoid()) return;
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
    result = LinearToolManipulator{center, axis, spacing_.value(),
                                   kMinimumPatternParameter,
                                   kMaximumPatternSpacingMm};
  });
  return result;
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
  if (!validPrincipalAxis(*direction_) ||
      !validPatternOperation(operation_)) {
    error_ = "Linear Pattern parameters are invalid";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
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
  operation_ = PatternOperation::NewBody;
  editingFeatureId_.reset();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

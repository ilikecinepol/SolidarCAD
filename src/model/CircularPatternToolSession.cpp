#include "model/CircularPatternToolSession.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <utility>

#include "model/GeometryOperation.h"

namespace solidar {

void CircularPatternToolSession::begin(
    double angleDeg, int count, PatternOperation operation,
    std::optional<FeatureId> editingFeatureId) {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  axis_.reset();
  angle_.reset(angleDeg, kMinimumPatternParameter,
               kMaximumPatternAngleDeg);
  count_ = clampPatternCountForUi(count);
  operation_ = operation;
  editingFeatureId_ = editingFeatureId;
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::SelectingInput;
}

void CircularPatternToolSession::setBody(BodyId bodyId,
                                         FeatureId sourceFeatureId,
                                         ShapeFeature::ShapePtr sourceShape) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  sourceShape_ = std::move(sourceShape);
  axis_.reset();
  updatePreview();
}

void CircularPatternToolSession::clearBody() {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  axis_.reset();
  updatePreview();
}

void CircularPatternToolSession::setAxis(PrincipalAxis axis) {
  axis_ = axis;
  updatePreview();
}

void CircularPatternToolSession::clearAxis() {
  axis_.reset();
  updatePreview();
}

void CircularPatternToolSession::setAngleDeg(double angleDeg) {
  const auto candidate = angle_.candidate(angleDeg);
  if (!candidate) return;
  angle_.accept(*candidate);
  updatePreview();
}

void CircularPatternToolSession::setCount(int count) {
  count_ = clampPatternCountForUi(count);
  updatePreview();
}

void CircularPatternToolSession::setOperation(PatternOperation operation) {
  operation_ = operation;
  updatePreview();
}

BodyId CircularPatternToolSession::bodyId() const noexcept { return bodyId_; }

FeatureId CircularPatternToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}

const std::optional<PrincipalAxis>&
CircularPatternToolSession::axis() const noexcept {
  return axis_;
}

double CircularPatternToolSession::angleDeg() const noexcept {
  return angle_.value();
}

int CircularPatternToolSession::count() const noexcept { return count_; }

PatternOperation CircularPatternToolSession::operation() const noexcept {
  return operation_;
}

std::optional<FeatureId>
CircularPatternToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}

ToolLifecycle CircularPatternToolSession::lifecycle() const noexcept {
  return lifecycle_;
}

ToolSelectionStage CircularPatternToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  if (bodyId_ == kInvalidBodyId) return ToolSelectionStage::SelectingInput;
  if (!axis_) return ToolSelectionStage::SelectingReference;
  return ToolSelectionStage::EditingParameters;
}

std::optional<SelectionRequirement>
CircularPatternToolSession::selectionRequirement() const {
  if (selectionStage() == ToolSelectionStage::SelectingInput)
    return SelectionRequirement{SelectionType::Body, "Select body", 1, 1,
                                false};
  if (selectionStage() == ToolSelectionStage::SelectingReference)
    return SelectionRequirement{SelectionType::Axis, "Select axis", 1, 1,
                                false};
  return std::nullopt;
}

std::vector<ToolParameterDescriptor>
CircularPatternToolSession::parameters() const {
  return {{"angle", "Angle", ToolParameterType::Angle, angle_.value(),
           kMinimumPatternParameter,
           kMaximumPatternAngleDeg, 1.0, "deg", true,
           ToolManipulatorType::Angular},
          {"count", "Count", ToolParameterType::Integer, count_,
           kMinimumPatternCount, kMaximumPatternCount,
           1.0, {}, true, ToolManipulatorType::None}};
}

std::shared_ptr<const TopoDS_Shape>
CircularPatternToolSession::previewShape() const {
  return previewShape_;
}

const std::string& CircularPatternToolSession::error() const noexcept {
  return error_;
}

std::optional<AngularToolManipulator>
CircularPatternToolSession::manipulator() const {
  std::optional<AngularToolManipulator> result;
  runGeometryOperation([&] {
    if (!sourceShape_ || sourceShape_->IsNull() || !axis_ ||
        !validPrincipalAxis(*axis_))
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
    const double radialX = std::max(std::abs(minX), std::abs(maxX));
    const double radialY = std::max(std::abs(minY), std::abs(maxY));
    const double radialZ = std::max(std::abs(minZ), std::abs(maxZ));
    double radius = 25.0;
    Vector3d direction{0.0, 0.0, 1.0};
    if (*axis_ == PrincipalAxis::X) {
      direction = {1.0, 0.0, 0.0};
      radius = std::hypot(radialY, radialZ);
    } else if (*axis_ == PrincipalAxis::Y) {
      direction = {0.0, 1.0, 0.0};
      radius = std::hypot(radialX, radialZ);
    } else {
      radius = std::hypot(radialX, radialY);
    }
    radius = std::max(10.0, radius * 1.15);
    result = AngularToolManipulator{{}, direction, radius, angle_.value()};
  });
  return result;
}

bool CircularPatternToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (bodyId_ == kInvalidBodyId || sourceFeatureId_ == kInvalidFeatureId ||
      !sourceShape_) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  if (!axis_) {
    lifecycle_ = ToolLifecycle::SelectingReference;
    return false;
  }
  if (!validPrincipalAxis(*axis_) || !validPatternOperation(operation_)) {
    error_ = "Circular Pattern parameters are invalid";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  previewShape_ = buildCircularPatternShape(
      *sourceShape_, *axis_, count_, angle_.value(), &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

void CircularPatternToolSession::cancel() noexcept {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  axis_.reset();
  operation_ = PatternOperation::NewBody;
  editingFeatureId_.reset();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

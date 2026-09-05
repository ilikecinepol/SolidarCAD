#include "model/RevolveToolSession.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

namespace solidar {

void RevolveToolSession::begin(const Document& document, BodyId bodyId,
                               FeatureId sourceFeatureId,
                               ShapeFeature::ShapePtr baseShape,
                               std::optional<FeatureId> editingFeatureId) {
  document_ = &document;
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  editingFeatureId_ = editingFeatureId;
  profileSketchId_ = kInvalidSketchId;
  axis_.reset();
  angleDeg_ = 360.0;
  operation_ = baseShape_ ? ExtrudeOperation::Join : ExtrudeOperation::NewBody;
  reversed_ = false;
  lifecycle_ = ToolLifecycle::SelectingInput;
  previewShape_.reset();
  error_.clear();
}
void RevolveToolSession::setProfile(SketchId value) { profileSketchId_ = value; updatePreview(); }
void RevolveToolSession::clearProfile() { profileSketchId_ = kInvalidSketchId; updatePreview(); }
void RevolveToolSession::setAxis(AxisReference value) { axis_ = value; updatePreview(); }
void RevolveToolSession::clearAxis() { axis_.reset(); updatePreview(); }
void RevolveToolSession::setAngleFromPanel(double value) { angleDeg_ = value; updatePreview(); }
void RevolveToolSession::setAngleFromManipulator(double value) { angleDeg_ = std::clamp(value, 0.01, 360.0); updatePreview(); }
void RevolveToolSession::setOperation(ExtrudeOperation value) { operation_ = value; updatePreview(); }
void RevolveToolSession::setReversed(bool value) { reversed_ = value; updatePreview(); }
SketchId RevolveToolSession::profileSketchId() const noexcept { return profileSketchId_; }
const std::optional<AxisReference>& RevolveToolSession::axis() const noexcept { return axis_; }
double RevolveToolSession::angleDeg() const noexcept { return angleDeg_; }
ExtrudeOperation RevolveToolSession::operation() const noexcept { return operation_; }
bool RevolveToolSession::reversed() const noexcept { return reversed_; }
BodyId RevolveToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId RevolveToolSession::sourceFeatureId() const noexcept { return sourceFeatureId_; }
std::optional<FeatureId> RevolveToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}
ToolLifecycle RevolveToolSession::lifecycle() const noexcept { return lifecycle_; }
ToolSelectionStage RevolveToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  if (profileSketchId_ == kInvalidSketchId) return ToolSelectionStage::SelectingInput;
  if (!axis_) return ToolSelectionStage::SelectingReference;
  return ToolSelectionStage::EditingParameters;
}
std::optional<SelectionRequirement> RevolveToolSession::selectionRequirement() const {
  if (selectionStage() == ToolSelectionStage::SelectingInput)
    return SelectionRequirement{SelectionType::Sketch, "Select profile", 1, 1, false};
  if (selectionStage() == ToolSelectionStage::SelectingReference)
    return SelectionRequirement{SelectionType::Axis, "Select revolution axis", 1, 1, false};
  return std::nullopt;
}
std::vector<ToolParameterDescriptor> RevolveToolSession::parameters() const {
  return {{"angle", "Angle", ToolParameterType::Angle, angleDeg_, 0.01, 360.0,
           1.0, "deg", true, ToolManipulatorType::Angular},
          {"reverse", "Reverse", ToolParameterType::Boolean, reversed_, 0.0, 1.0,
           1.0, {}, false, ToolManipulatorType::None}};
}
std::shared_ptr<const TopoDS_Shape> RevolveToolSession::previewShape() const { return previewShape_; }
const std::string& RevolveToolSession::error() const noexcept { return error_; }

bool RevolveToolSession::updatePreview() {
  previewShape_.reset(); error_.clear();
  if (!document_ || profileSketchId_ == kInvalidSketchId || !axis_) {
    lifecycle_ = profileSketchId_ == kInvalidSketchId
                     ? ToolLifecycle::SelectingInput
                     : ToolLifecycle::SelectingReference;
    return false;
  }
  RevolveFeature preview(profileSketchId_, *axis_, angleDeg_, "Revolve preview",
                         operation_, reversed_);
  const TopoDS_Shape* previous =
      operation_ == ExtrudeOperation::NewBody ? nullptr
                                               : (baseShape_ ? baseShape_.get() : nullptr);
  RebuildContext context{const_cast<Document&>(*document_),
                         document_->findBody(bodyId_), previous};
  if (!preview.rebuild(context)) {
    error_ = preview.error(); lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  previewShape_ = preview.shape(); lifecycle_ = ToolLifecycle::PreviewValid;
  return true;
}

std::optional<AngularToolManipulator> RevolveToolSession::manipulator() const {
  if (!document_ || !axis_ || profileSketchId_ == kInvalidSketchId) return std::nullopt;
  if (axis_->type == AxisReferenceType::GlobalX ||
      axis_->type == AxisReferenceType::GlobalY ||
      axis_->type == AxisReferenceType::GlobalZ) {
    const Vector3d direction = axis_->type == AxisReferenceType::GlobalX
                                   ? Vector3d{1.0, 0.0, 0.0}
                                   : axis_->type == AxisReferenceType::GlobalY
                                         ? Vector3d{0.0, 1.0, 0.0}
                                         : Vector3d{0.0, 0.0, 1.0};
    return AngularToolManipulator{{}, direction, 25.0, angleDeg_};
  }
  const auto* sketch = document_->findSketch(axis_->sketchId);
  if (!sketch) return std::nullopt;
  Point3d origin = sketch->placement.origin;
  Vector3d direction = axis_->type == AxisReferenceType::SketchVerticalAxis
                           ? sketch->placement.yDirection
                           : sketch->placement.xDirection;
  if (axis_->type == AxisReferenceType::SketchLine) {
    const auto index = sketch->geometry.lineIndex(axis_->lineId);
    if (!index) return std::nullopt;
    const auto& line = sketch->geometry.lines()[*index];
    origin = sketch->placement.toWorld(line.start.xMm, line.start.yMm);
    const auto end = sketch->placement.toWorld(line.end.xMm, line.end.yMm);
    direction = {end.x-origin.x, end.y-origin.y, end.z-origin.z};
  }
  return AngularToolManipulator{origin, direction, 25.0, angleDeg_};
}
void RevolveToolSession::cancel() noexcept {
  document_ = nullptr; baseShape_.reset(); previewShape_.reset(); axis_.reset();
  editingFeatureId_.reset();
  profileSketchId_ = kInvalidSketchId; error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}
}  // namespace solidar

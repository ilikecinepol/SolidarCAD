#include "model/RevolveToolSession.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

#include "model/GeometryOperation.h"

namespace solidar {

void RevolveToolSession::begin(const Document& document, BodyId bodyId,
                               FeatureId sourceFeatureId,
                               ShapeFeature::ShapePtr baseShape,
                               std::optional<FeatureId> editingFeatureId) {
  (void)document;
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  editingFeatureId_ = editingFeatureId;
  profileSketchId_ = kInvalidSketchId;
  profileOverride_.reset();
  axis_.reset();
  angleDeg_ = 360.0;
  operation_ = baseShape_ ? ExtrudeOperation::Join : ExtrudeOperation::NewBody;
  reversed_ = false;
  lifecycle_ = ToolLifecycle::SelectingInput;
  previewShape_.reset();
  error_.clear();
}
void RevolveToolSession::setProfile(
    const Document& document, SketchId value,
    std::optional<sketch::Sketch> profileOverride) {
  profileSketchId_ = value;
  profileOverride_ = std::move(profileOverride);
  updatePreview(document);
}
void RevolveToolSession::clearProfile(const Document& document) {
  profileSketchId_ = kInvalidSketchId;
  profileOverride_.reset();
  updatePreview(document);
}
void RevolveToolSession::setAxis(const Document& document, AxisReference value) { axis_ = value; updatePreview(document); }
void RevolveToolSession::clearAxis(const Document& document) { axis_.reset(); updatePreview(document); }
void RevolveToolSession::setAngleFromPanel(const Document& document, double value) { angleDeg_ = value; updatePreview(document); }
void RevolveToolSession::setAngleFromManipulator(const Document& document, double value) { angleDeg_ = std::clamp(value, 0.01, 360.0); updatePreview(document); }
void RevolveToolSession::setOperation(const Document& document, ExtrudeOperation value) { operation_ = value; updatePreview(document); }
void RevolveToolSession::setReversed(const Document& document, bool value) { reversed_ = value; updatePreview(document); }
SketchId RevolveToolSession::profileSketchId() const noexcept { return profileSketchId_; }
const std::optional<sketch::Sketch>&
RevolveToolSession::profileOverride() const noexcept {
  return profileOverride_;
}
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
    return SelectionRequirement{SelectionType::Sketch, "Select profiles", 1,
                                static_cast<std::size_t>(-1), true};
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

bool RevolveToolSession::updatePreview() { return false; }
bool RevolveToolSession::updatePreview(const Document& document) {
  error_.clear();
  if (profileSketchId_ == kInvalidSketchId || !axis_) {
    previewShape_.reset();
    lifecycle_ = profileSketchId_ == kInvalidSketchId
                     ? ToolLifecycle::SelectingInput
                     : ToolLifecycle::SelectingReference;
    return false;
  }
  RevolveFeature preview(profileSketchId_, *axis_, angleDeg_, "Revolve preview",
                         operation_, reversed_);
  preview.setProfileOverride(profileOverride_);
  const TopoDS_Shape* previous =
      operation_ == ExtrudeOperation::NewBody ? nullptr
                                               : (baseShape_ ? baseShape_.get() : nullptr);
  RebuildContext context{const_cast<Document&>(document),
                         document.findBody(bodyId_), previous};
  if (!preview.rebuild(context)) {
    error_ = preview.error(); lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  previewShape_ = preview.shape(); lifecycle_ = ToolLifecycle::PreviewValid;
  return true;
}

std::optional<AngularToolManipulator> RevolveToolSession::manipulator(
    const Document& document) const {
  std::optional<AngularToolManipulator> result;
  runGeometryOperation([&] {
    if (!axis_ || profileSketchId_ == kInvalidSketchId) return;
    if (axis_->type == AxisReferenceType::GlobalX ||
        axis_->type == AxisReferenceType::GlobalY ||
        axis_->type == AxisReferenceType::GlobalZ) {
      const Vector3d direction = axis_->type == AxisReferenceType::GlobalX
                                     ? Vector3d{1.0, 0.0, 0.0}
                                     : axis_->type == AxisReferenceType::GlobalY
                                           ? Vector3d{0.0, 1.0, 0.0}
                                           : Vector3d{0.0, 0.0, 1.0};
      result = AngularToolManipulator{{}, direction, 25.0, angleDeg_};
      return;
    }
    const auto* sketch = document.findSketch(axis_->sketchId);
    if (!sketch) return;
    Point3d origin = sketch->placement.origin;
    Vector3d direction = axis_->type == AxisReferenceType::SketchVerticalAxis
                             ? sketch->placement.yDirection
                             : sketch->placement.xDirection;
    if (axis_->type == AxisReferenceType::SketchLine) {
      const auto index = sketch->geometry.lineIndex(axis_->lineId);
      if (!index) return;
      const auto& line = sketch->geometry.lines()[*index];
      origin = sketch->placement.toWorld(line.start.xMm, line.start.yMm);
      const auto end = sketch->placement.toWorld(line.end.xMm, line.end.yMm);
      direction = {end.x - origin.x, end.y - origin.y, end.z - origin.z};
    }
    result = AngularToolManipulator{origin, direction, 25.0, angleDeg_};
  });
  return result;
}
void RevolveToolSession::cancel() noexcept {
  baseShape_.reset(); previewShape_.reset(); axis_.reset();
  editingFeatureId_.reset();
  profileSketchId_ = kInvalidSketchId; profileOverride_.reset(); error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}
}  // namespace solidar

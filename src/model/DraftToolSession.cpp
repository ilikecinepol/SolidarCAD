#include "model/DraftToolSession.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>

#include "model/DraftBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

void DraftToolSession::begin(Document& document, BodyId bodyId,
                             FeatureId sourceFeatureId,
                             ShapeFeature::ShapePtr baseShape,
                             std::vector<FaceReference> faces,
                             std::optional<PlaneReference> plane,
                             std::optional<AxisReference> direction,
                             double angle, bool reversed,
                             std::optional<FeatureId> editingFeatureId) {
  document_ = &document; bodyId_ = bodyId; sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape); faces_ = std::move(faces);
  neutralPlane_ = std::move(plane); pullDirection_ = std::move(direction);
  angleDeg_ = angle; reversed_ = reversed; editingFeatureId_ = editingFeatureId;
  lifecycle_ = ToolLifecycle::Editing; updatePreview();
}
void DraftToolSession::setFaces(std::vector<FaceReference> value) { faces_ = std::move(value); updatePreview(); }
void DraftToolSession::setNeutralPlane(PlaneReference value) { neutralPlane_ = std::move(value); updatePreview(); }
void DraftToolSession::clearNeutralPlane() { neutralPlane_.reset(); updatePreview(); }
void DraftToolSession::setPullDirection(AxisReference value) { pullDirection_ = value; updatePreview(); }
void DraftToolSession::clearPullDirection() { pullDirection_.reset(); updatePreview(); }
void DraftToolSession::setAngleFromPanel(double value) { angleDeg_ = value; updatePreview(); }
void DraftToolSession::setAngleFromManipulator(double value) { angleDeg_ = std::clamp(value, 0.01, 88.99); updatePreview(); }
void DraftToolSession::setReversed(bool value) { reversed_ = value; updatePreview(); }
BodyId DraftToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId DraftToolSession::sourceFeatureId() const noexcept { return sourceFeatureId_; }
std::optional<FeatureId> DraftToolSession::editingFeatureId() const noexcept { return editingFeatureId_; }
const std::vector<FaceReference>& DraftToolSession::faces() const noexcept { return faces_; }
const std::optional<PlaneReference>& DraftToolSession::neutralPlane() const noexcept { return neutralPlane_; }
const std::optional<AxisReference>& DraftToolSession::pullDirection() const noexcept { return pullDirection_; }
double DraftToolSession::angleDeg() const noexcept { return angleDeg_; }
bool DraftToolSession::reversed() const noexcept { return reversed_; }
ToolLifecycle DraftToolSession::lifecycle() const noexcept { return lifecycle_; }
ToolSelectionStage DraftToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  if (faces_.empty()) return ToolSelectionStage::SelectingInput;
  if (!neutralPlane_ || !pullDirection_) return ToolSelectionStage::SelectingReference;
  return ToolSelectionStage::EditingParameters;
}
std::optional<SelectionRequirement> DraftToolSession::selectionRequirement() const {
  if (faces_.empty()) return SelectionRequirement{SelectionType::Face, "Select faces", 1, static_cast<std::size_t>(-1), true};
  if (!neutralPlane_) return SelectionRequirement{SelectionType::Plane, "Select neutral plane", 1, 1, false};
  if (!pullDirection_) return SelectionRequirement{SelectionType::Axis, "Select pull direction", 1, 1, false};
  return std::nullopt;
}
std::vector<ToolParameterDescriptor> DraftToolSession::parameters() const {
  return {{"angle", "Angle", ToolParameterType::Angle, angleDeg_, 0.01, 88.99, 0.5, "deg", true, ToolManipulatorType::Angular},
          {"reversed", "Reverse", ToolParameterType::Boolean, reversed_, 0.0, 1.0, 1.0, {}, false, ToolManipulatorType::None}};
}
std::shared_ptr<const TopoDS_Shape> DraftToolSession::previewShape() const { return previewShape_; }
const std::string& DraftToolSession::error() const noexcept { return error_; }
bool DraftToolSession::updatePreview() {
  previewShape_.reset(); error_.clear();
  if (!document_ || !baseShape_ || baseShape_->IsNull()) { error_ = "Draft base shape is missing"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  if (faces_.empty()) { lifecycle_ = ToolLifecycle::SelectingInput; return false; }
  if (!neutralPlane_ || !pullDirection_) { lifecycle_ = ToolLifecycle::SelectingReference; return false; }
  std::vector<std::size_t> indices;
  for (const auto& face : faces_) {
    if (face.bodyId != bodyId_ || face.featureId != sourceFeatureId_) { error_ = "Draft faces no longer match the active Body"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    const auto resolved = resolveFaceReference(*baseShape_, face.topology());
    if (!resolved) { error_ = "Draft face could not be resolved: " + resolved.error; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    indices.push_back(resolved.index);
  }
  gp_Pln plane; gp_Dir direction;
  if (!resolveDraftReferences(*document_, *baseShape_, *neutralPlane_, *pullDirection_, &plane, &direction, &error_)) { lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  previewShape_ = buildDraftShape(*baseShape_, indices, plane, direction, angleDeg_, reversed_, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}
std::optional<AngularToolManipulator> DraftToolSession::manipulator() const {
  if (!baseShape_ || !neutralPlane_ || !pullDirection_ || !document_) return std::nullopt;
  gp_Pln plane; gp_Dir direction; std::string ignored;
  if (!resolveDraftReferences(*document_, *baseShape_, *neutralPlane_, *pullDirection_, &plane, &direction, &ignored)) return std::nullopt;
  Bnd_Box box; BRepBndLib::Add(*baseShape_, box);
  double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
  const double radius = std::max({x1 - x0, y1 - y0, z1 - z0, 10.0}) * 0.35;
  return AngularToolManipulator{{(x0+x1)*0.5, (y0+y1)*0.5, (z0+z1)*0.5},
                                {direction.X(), direction.Y(), direction.Z()},
                                radius, angleDeg_};
}
void DraftToolSession::cancel() noexcept { previewShape_.reset(); faces_.clear(); neutralPlane_.reset(); pullDirection_.reset(); error_.clear(); document_ = nullptr; lifecycle_ = ToolLifecycle::Inactive; }

}  // namespace solidar

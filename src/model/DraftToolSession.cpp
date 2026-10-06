#include "model/DraftToolSession.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

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
                             std::optional<FeatureId> editingFeatureId,
                             std::optional<EdgeReference> rotationEdge) {
  document_ = &document; bodyId_ = bodyId; sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape); faces_ = std::move(faces);
  neutralPlane_ = std::move(plane); pullDirection_ = std::move(direction);
  rotationEdge_ = std::move(rotationEdge);
  angleDeg_ = reversed ? -std::abs(angle) : angle;
  editingFeatureId_ = editingFeatureId;
  previewShape_.reset(); lastValidPreviewShape_.reset();
  lifecycle_ = ToolLifecycle::Editing; updatePreview();
}
void DraftToolSession::setFaces(std::vector<FaceReference> value) { faces_ = std::move(value); lastValidPreviewShape_.reset(); updatePreview(); }
void DraftToolSession::setNeutralPlane(PlaneReference value) { neutralPlane_ = std::move(value); lastValidPreviewShape_.reset(); updatePreview(); }
void DraftToolSession::clearNeutralPlane() { neutralPlane_.reset(); lastValidPreviewShape_.reset(); updatePreview(); }
void DraftToolSession::setPullDirection(AxisReference value) { pullDirection_ = value; lastValidPreviewShape_.reset(); updatePreview(); }
void DraftToolSession::clearPullDirection() { pullDirection_.reset(); lastValidPreviewShape_.reset(); updatePreview(); }
bool DraftToolSession::setPrincipalAxis(int axisIndex) {
  AxisReference direction;
  PlaneReference plane;
  if (axisIndex == 0) {
    direction.type = AxisReferenceType::GlobalX;
    plane.type = NeutralPlaneType::GlobalYZ;
  } else if (axisIndex == 1) {
    direction.type = AxisReferenceType::GlobalY;
    plane.type = NeutralPlaneType::GlobalXZ;
  } else if (axisIndex == 2) {
    direction.type = AxisReferenceType::GlobalZ;
    plane.type = NeutralPlaneType::GlobalXY;
  } else {
    return false;
  }
  neutralPlane_ = plane;
  pullDirection_ = direction;
  rotationEdge_.reset();
  lastValidPreviewShape_.reset();
  updatePreview();
  return true;
}
bool DraftToolSession::setRotationEdge(EdgeReference value) {
  if (!baseShape_ || faces_.size() != 1 || value.bodyId != bodyId_ ||
      value.featureId != sourceFeatureId_) {
    error_ = "Draft rotation edge no longer matches the selected surface";
    return false;
  }
  gp_Pln plane;
  gp_Dir direction;
  std::string structuralError;
  if (!resolveDraftEdgeAxis(*baseShape_, faces_.front(), value, &plane,
                            &direction, &structuralError)) {
    error_ = std::move(structuralError);
    previewShape_.reset();
    lastValidPreviewShape_.reset();
    lifecycle_ = ToolLifecycle::SelectingReference;
    return false;
  }
  rotationEdge_ = std::move(value);
  neutralPlane_.reset();
  pullDirection_.reset();
  lastValidPreviewShape_.reset();
  updatePreview();
  return true;
}
void DraftToolSession::clearPrincipalAxis() {
  neutralPlane_.reset();
  pullDirection_.reset();
  rotationEdge_.reset();
  lastValidPreviewShape_.reset();
  updatePreview();
}
std::optional<int> DraftToolSession::principalAxisIndex() const noexcept {
  if (rotationEdge_) return std::nullopt;
  if (!neutralPlane_ || !pullDirection_) return std::nullopt;
  if (pullDirection_->type == AxisReferenceType::GlobalX &&
      neutralPlane_->type == NeutralPlaneType::GlobalYZ)
    return 0;
  if (pullDirection_->type == AxisReferenceType::GlobalY &&
      neutralPlane_->type == NeutralPlaneType::GlobalXZ)
    return 1;
  if (pullDirection_->type == AxisReferenceType::GlobalZ &&
      neutralPlane_->type == NeutralPlaneType::GlobalXY)
    return 2;
  return std::nullopt;
}
void DraftToolSession::setAngleFromPanel(double value) {
  angleDeg_ = std::clamp(value, -89.99, 89.99);
  updatePreview();
}
void DraftToolSession::setAngleFromManipulator(double value) {
  angleDeg_ = std::clamp(value, -89.99, 89.99);
  updatePreview();
}
BodyId DraftToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId DraftToolSession::sourceFeatureId() const noexcept { return sourceFeatureId_; }
std::optional<FeatureId> DraftToolSession::editingFeatureId() const noexcept { return editingFeatureId_; }
const std::vector<FaceReference>& DraftToolSession::faces() const noexcept { return faces_; }
const std::optional<PlaneReference>& DraftToolSession::neutralPlane() const noexcept { return neutralPlane_; }
const std::optional<AxisReference>& DraftToolSession::pullDirection() const noexcept { return pullDirection_; }
const std::optional<EdgeReference>& DraftToolSession::rotationEdge() const noexcept { return rotationEdge_; }
double DraftToolSession::angleDeg() const noexcept { return angleDeg_; }
ToolLifecycle DraftToolSession::lifecycle() const noexcept { return lifecycle_; }
ToolSelectionStage DraftToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  if (faces_.empty()) return ToolSelectionStage::SelectingInput;
  if (!rotationEdge_ && (!neutralPlane_ || !pullDirection_))
    return ToolSelectionStage::SelectingReference;
  return ToolSelectionStage::EditingParameters;
}
std::optional<SelectionRequirement> DraftToolSession::selectionRequirement() const {
  if (faces_.empty())
    return SelectionRequirement{SelectionType::Face, "Select surface", 1, 1,
                                false};
  if (!rotationEdge_ && (!neutralPlane_ || !pullDirection_))
    return SelectionRequirement{SelectionType::Axis,
                                "Select X/Y/Z or adjacent edge", 1, 1,
                                false};
  return std::nullopt;
}
std::vector<ToolParameterDescriptor> DraftToolSession::parameters() const {
  return {{"angle", "Angle", ToolParameterType::Angle, angleDeg_, -89.99,
           89.99, 0.5, "deg", true, ToolManipulatorType::Angular}};
}
std::shared_ptr<const TopoDS_Shape> DraftToolSession::previewShape() const { return previewShape_; }
const std::string& DraftToolSession::error() const noexcept { return error_; }
bool DraftToolSession::updatePreview() {
  error_.clear();
  if (!document_ || !baseShape_ || baseShape_->IsNull()) { previewShape_.reset(); lastValidPreviewShape_.reset(); error_ = "Draft base shape is missing"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  if (faces_.empty()) { previewShape_.reset(); lifecycle_ = ToolLifecycle::SelectingInput; return false; }
  if (!rotationEdge_ && (!neutralPlane_ || !pullDirection_)) { previewShape_.reset(); lifecycle_ = ToolLifecycle::SelectingReference; return false; }
  std::vector<std::size_t> indices;
  for (const auto& face : faces_) {
    if (face.bodyId != bodyId_ || face.featureId != sourceFeatureId_) { previewShape_.reset(); error_ = "Draft faces no longer match the active Body"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    const auto resolved = resolveFaceReference(*baseShape_, face.topology());
    if (!resolved) { previewShape_.reset(); error_ = "Draft face could not be resolved: " + resolved.error; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    indices.push_back(resolved.index);
  }
  gp_Pln plane; gp_Dir direction;
  if (rotationEdge_) {
    if (faces_.size() != 1 || rotationEdge_->bodyId != bodyId_ ||
        rotationEdge_->featureId != sourceFeatureId_ ||
        !resolveDraftEdgeAxis(*baseShape_, faces_.front(), *rotationEdge_,
                              &plane, &direction, &error_)) {
      previewShape_.reset();
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
  } else if (!resolveDraftReferences(*document_, *baseShape_, *neutralPlane_,
                                     *pullDirection_, &plane, &direction,
                                     &error_)) { previewShape_.reset(); lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  auto candidate = buildDraftShape(*baseShape_, indices, plane, direction,
                                   std::abs(angleDeg_), angleDeg_ < 0.0,
                                   &error_);
  if (!candidate) {
    // Parameter-domain failure is recoverable. Keep displaying the last valid
    // trial, but remain PreviewInvalid so Apply cannot commit it as the newly
    // requested angle.
    previewShape_ = lastValidPreviewShape_;
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  previewShape_ = std::move(candidate);
  lastValidPreviewShape_ = previewShape_;
  lifecycle_ = ToolLifecycle::PreviewValid;
  return true;
}
std::optional<AngularToolManipulator> DraftToolSession::manipulator() const {
  // Do not advertise an angle handle until geometry/reference selection has
  // produced a valid Draft preview.
  if (lifecycle_ != ToolLifecycle::PreviewValid || faces_.empty() ||
      !baseShape_ || !document_ ||
      (!rotationEdge_ && (!neutralPlane_ || !pullDirection_)))
    return std::nullopt;
  gp_Pln plane; gp_Dir direction; std::string ignored;
  if (rotationEdge_) {
    if (faces_.size() != 1 ||
        !resolveDraftEdgeAxis(*baseShape_, faces_.front(), *rotationEdge_,
                              &plane, &direction, &ignored))
      return std::nullopt;
  } else if (!resolveDraftReferences(*document_, *baseShape_, *neutralPlane_,
                                     *pullDirection_, &plane, &direction,
                                     &ignored))
    return std::nullopt;
  Bnd_Box box;
  if (const auto selected = resolveFaceReference(
          *baseShape_, faces_.front().topology()))
    BRepBndLib::Add(*selected.subshape, box);
  else
    BRepBndLib::Add(*baseShape_, box);
  double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
  // The previous 0.35 factor produced a tiny, easy-to-miss arc.
  const double radius = std::max({x1 - x0, y1 - y0, z1 - z0, 10.0}) * 0.60;
  return AngularToolManipulator{{(x0+x1)*0.5, (y0+y1)*0.5, (z0+z1)*0.5},
                                {direction.X(), direction.Y(), direction.Z()},
                                radius, angleDeg_, -89.99, 89.99};
}
void DraftToolSession::cancel() noexcept { previewShape_.reset(); lastValidPreviewShape_.reset(); faces_.clear(); neutralPlane_.reset(); pullDirection_.reset(); rotationEdge_.reset(); error_.clear(); document_ = nullptr; lifecycle_ = ToolLifecycle::Inactive; }

}  // namespace solidar

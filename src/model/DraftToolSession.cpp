#include "model/DraftToolSession.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

#include "model/DraftBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

void DraftToolSession::begin(const Document& document, BodyId bodyId,
                             FeatureId sourceFeatureId,
                             ShapeFeature::ShapePtr baseShape,
                             std::vector<FaceReference> faces,
                             std::optional<PlaneReference> plane,
                             std::optional<AxisReference> direction,
                             double angle, bool reversed,
                             std::optional<FeatureId> editingFeatureId,
                             std::optional<EdgeReference> rotationEdge,
                             std::shared_ptr<const TopologyIndex> topologyIndex) {
  bodyId_ = bodyId; sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape); faces_ = std::move(faces);
  topologyIndexError_.clear();
  topologyIndex_ = std::move(topologyIndex);
  if (topologyIndex_ &&
      (!baseShape_ || baseShape_->IsNull() || !topologyIndex_->shape() ||
       topologyIndex_->shape().get() != baseShape_.get()))
    topologyIndex_.reset();
  if (!topologyIndex_ && baseShape_ && !baseShape_->IsNull())
    topologyIndex_ = TopologyIndex::build(baseShape_, kInvalidShapeRevision,
                                          &topologyIndexError_);
  neutralPlane_ = std::move(plane); pullDirection_ = std::move(direction);
  rotationEdge_ = std::move(rotationEdge);
  angleDeg_ = reversed ? -std::abs(angle) : angle;
  editingFeatureId_ = editingFeatureId;
  previewShape_.reset(); lastValidPreviewShape_.reset();
  lifecycle_ = ToolLifecycle::Editing; updatePreview(document);
}
void DraftToolSession::setFaces(const Document& document, std::vector<FaceReference> value) { faces_ = std::move(value); lastValidPreviewShape_.reset(); updatePreview(document); }
void DraftToolSession::setNeutralPlane(const Document& document, PlaneReference value) { neutralPlane_ = std::move(value); lastValidPreviewShape_.reset(); updatePreview(document); }
void DraftToolSession::clearNeutralPlane(const Document& document) { neutralPlane_.reset(); lastValidPreviewShape_.reset(); updatePreview(document); }
void DraftToolSession::setPullDirection(const Document& document, AxisReference value) { pullDirection_ = value; lastValidPreviewShape_.reset(); updatePreview(document); }
void DraftToolSession::clearPullDirection(const Document& document) { pullDirection_.reset(); lastValidPreviewShape_.reset(); updatePreview(document); }
bool DraftToolSession::setPrincipalAxis(const Document& document, int axisIndex) {
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
  updatePreview(document);
  return true;
}
bool DraftToolSession::setRotationEdge(const Document& document, EdgeReference value) {
  error_.clear();
  errorCode_ = OperationFailureCode::None;
  if (!baseShape_ || baseShape_->IsNull()) {
    errorCode_ = OperationFailureCode::MissingSource;
    error_ = "Draft base shape is missing";
    return false;
  }
  if (bodyId_ == kInvalidBodyId || sourceFeatureId_ == kInvalidFeatureId) {
    errorCode_ = OperationFailureCode::InvalidInput;
    error_ = "Draft Body or source Feature id is invalid";
    return false;
  }
  if (faces_.size() != 1) {
    errorCode_ = OperationFailureCode::InvalidInput;
    error_ = "Draft rotation edge requires exactly one selected face";
    return false;
  }
  if (faces_.front().bodyId != bodyId_ ||
      faces_.front().featureId != sourceFeatureId_ ||
      value.bodyId != bodyId_ || value.featureId != sourceFeatureId_) {
    errorCode_ = OperationFailureCode::TopologyReferenceMismatch;
    error_ = "Draft rotation edge no longer matches the selected surface";
    return false;
  }
  if (!topologyIndex_) {
    errorCode_ = OperationFailureCode::TopologyIndexUnavailable;
    error_ = "Draft topology could not be indexed";
    if (!topologyIndexError_.empty()) error_ += ": " + topologyIndexError_;
    return false;
  }
  gp_Pln plane;
  gp_Dir direction;
  const auto resolution = resolveDraftEdgeAxis(
      *baseShape_, *topologyIndex_, faces_.front(), value, &plane, &direction);
  if (!resolution) {
    errorCode_ = resolution.failure.code;
    error_ = resolution.failure.detail;
    previewShape_.reset();
    lastValidPreviewShape_.reset();
    lifecycle_ = ToolLifecycle::SelectingReference;
    return false;
  }
  rotationEdge_ = std::move(value);
  neutralPlane_.reset();
  pullDirection_.reset();
  lastValidPreviewShape_.reset();
  updatePreview(document);
  return true;
}
void DraftToolSession::clearPrincipalAxis(const Document& document) {
  neutralPlane_.reset();
  pullDirection_.reset();
  rotationEdge_.reset();
  lastValidPreviewShape_.reset();
  updatePreview(document);
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
void DraftToolSession::setAngleFromPanel(const Document& document, double value) {
  angleDeg_ = std::clamp(value, -89.99, 89.99);
  updatePreview(document);
}
void DraftToolSession::setAngleFromManipulator(const Document& document, double value) {
  angleDeg_ = std::clamp(value, -89.99, 89.99);
  updatePreview(document);
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
OperationFailureCode DraftToolSession::errorCode() const noexcept {
  return errorCode_;
}
bool DraftToolSession::updatePreview() { return false; }
bool DraftToolSession::updatePreview(const Document& document) {
  error_.clear(); errorCode_ = OperationFailureCode::None;
  if (!baseShape_ || baseShape_->IsNull()) { previewShape_.reset(); lastValidPreviewShape_.reset(); errorCode_ = OperationFailureCode::MissingSource; error_ = "Draft base shape is missing"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  if (bodyId_ == kInvalidBodyId || sourceFeatureId_ == kInvalidFeatureId) { previewShape_.reset(); lastValidPreviewShape_.reset(); errorCode_ = OperationFailureCode::InvalidInput; error_ = "Draft Body or source Feature id is invalid"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  if (faces_.empty()) { previewShape_.reset(); errorCode_ = OperationFailureCode::InvalidInput; error_ = "Draft requires at least one selected face"; lifecycle_ = ToolLifecycle::SelectingInput; return false; }
  if (rotationEdge_ && faces_.size() != 1) { previewShape_.reset(); errorCode_ = OperationFailureCode::InvalidInput; error_ = "Draft rotation edge requires exactly one selected face"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  if (!rotationEdge_ && (!neutralPlane_ || !pullDirection_)) { previewShape_.reset(); errorCode_ = OperationFailureCode::InvalidInput; error_ = "Draft reference selection is incomplete"; lifecycle_ = ToolLifecycle::SelectingReference; return false; }
  if (neutralPlane_ && !isKnownNeutralPlaneType(neutralPlane_->type)) {
    previewShape_.reset();
    errorCode_ = OperationFailureCode::InvalidInput;
    error_ = "Draft neutral plane type is unsupported";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (!rotationEdge_ && neutralPlane_->type == NeutralPlaneType::BodyFace &&
      !neutralPlane_->face) {
    previewShape_.reset();
    errorCode_ = OperationFailureCode::InvalidInput;
    error_ = "Draft neutral face is missing";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (!rotationEdge_ && neutralPlane_->type == NeutralPlaneType::BodyFace &&
      (neutralPlane_->face->bodyId != bodyId_ ||
       neutralPlane_->face->featureId != sourceFeatureId_)) {
    previewShape_.reset();
    errorCode_ = OperationFailureCode::TopologyReferenceMismatch;
    error_ = "Draft neutral face must belong to the active source Feature";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  std::vector<TopologyReference> references;
  references.reserve(faces_.size());
  std::vector<std::size_t> indices;
  for (const auto& face : faces_) {
    if (face.bodyId != bodyId_ || face.featureId != sourceFeatureId_) { previewShape_.reset(); errorCode_ = OperationFailureCode::TopologyReferenceMismatch; error_ = "Draft faces no longer match the active Body"; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    references.push_back(face.topology());
  }
  if (rotationEdge_ &&
      (rotationEdge_->bodyId != bodyId_ ||
       rotationEdge_->featureId != sourceFeatureId_)) {
    previewShape_.reset();
    errorCode_ = OperationFailureCode::TopologyReferenceMismatch;
    error_ = "Draft rotation edge no longer matches the selected surface";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (!topologyIndex_) { previewShape_.reset(); errorCode_ = OperationFailureCode::TopologyIndexUnavailable; error_ = "Draft topology could not be indexed"; if (!topologyIndexError_.empty()) error_ += ": " + topologyIndexError_; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
  for (const auto& resolved : topologyIndex_->resolveFaces(references)) {
    if (!resolved) { previewShape_.reset(); errorCode_ = operationFailureCode(resolved.failure); error_ = "Draft face could not be resolved: " + resolved.error; lifecycle_ = ToolLifecycle::PreviewInvalid; return false; }
    indices.push_back(resolved.index);
  }
  gp_Pln plane; gp_Dir direction;
  if (rotationEdge_) {
    const auto resolution = resolveDraftEdgeAxis(
        *baseShape_, *topologyIndex_, faces_.front(), *rotationEdge_, &plane,
        &direction);
    if (!resolution) {
      previewShape_.reset();
      errorCode_ = resolution.failure.code;
      error_ = resolution.failure.detail;
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
  } else {
    const auto resolution = resolveDraftReferences(
        document, *baseShape_, *topologyIndex_, *neutralPlane_,
        *pullDirection_, &plane, &direction);
    if (!resolution) {
      previewShape_.reset();
      errorCode_ = resolution.failure.code;
      error_ = resolution.failure.detail;
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
  }
  auto candidate = buildDraftShape(*baseShape_, indices, plane, direction,
                                   std::abs(angleDeg_), angleDeg_ < 0.0,
                                   &error_);
  if (!candidate) {
    errorCode_ = OperationFailureCode::GeometryOperationFailed;
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
std::optional<AngularToolManipulator> DraftToolSession::manipulator(
    const Document& document) const {
  // Do not advertise an angle handle until geometry/reference selection has
  // produced a valid Draft preview.
  if (lifecycle_ != ToolLifecycle::PreviewValid || faces_.empty() ||
      !baseShape_ ||
      !topologyIndex_ ||
      (!rotationEdge_ && (!neutralPlane_ || !pullDirection_)))
    return std::nullopt;
  gp_Pln plane; gp_Dir direction;
  if (rotationEdge_) {
    if (faces_.size() != 1 ||
        !resolveDraftEdgeAxis(*baseShape_, *topologyIndex_, faces_.front(),
                              *rotationEdge_, &plane, &direction))
      return std::nullopt;
  } else if (!resolveDraftReferences(document, *baseShape_, *topologyIndex_,
                                     *neutralPlane_, *pullDirection_, &plane,
                                     &direction))
    return std::nullopt;
  Bnd_Box box;
  if (const auto selected = topologyIndex_->resolveFace(
          faces_.front().topology()))
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
void DraftToolSession::cancel() noexcept { previewShape_.reset(); lastValidPreviewShape_.reset(); topologyIndex_.reset(); topologyIndexError_.clear(); faces_.clear(); neutralPlane_.reset(); pullDirection_.reset(); rotationEdge_.reset(); error_.clear(); errorCode_ = OperationFailureCode::None; lifecycle_ = ToolLifecycle::Inactive; }

}  // namespace solidar

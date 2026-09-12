#include "model/ExtrudeToolSession.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <utility>

#include "model/TopologyReferenceResolver.h"

namespace solidar {

void ExtrudeToolSession::begin(BodyId bodyId, FeatureId sourceFeatureId,
                               ShapeFeature::ShapePtr baseShape,
                               FaceReference face, double lengthMm,
                               ExtrudeOperation operation, bool reversed,
                               std::optional<FeatureId> editingFeatureId) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  face_ = std::move(face);
  operation_ = operation;
  reversed_ = reversed;
  editingFeatureId_ = editingFeatureId;

  minimumMm_ = 0.01;
  maximumMm_ = 100000.0;
  if (baseShape_ && !baseShape_->IsNull()) {
    Bnd_Box box;
    BRepBndLib::Add(*baseShape_, box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double dz = z1 - z0;
    const double diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (std::isfinite(diagonal) && diagonal > 0.0)
      maximumMm_ = std::clamp(diagonal * 20.0, 100.0, 100000.0);
  }
  length_.reset(lengthMm, minimumMm_, maximumMm_);

  lifecycle_ = ToolLifecycle::EditingParameters;
  updatePreview();
}

void ExtrudeToolSession::setFace(FaceReference face) {
  face_ = std::move(face);
  updatePreview();
}

void ExtrudeToolSession::setLengthFromPanel(double lengthMm) {
  trySetLength(lengthMm);
}

void ExtrudeToolSession::setLengthFromManipulator(double lengthMm) {
  trySetLength(lengthMm);
}

void ExtrudeToolSession::setOperation(ExtrudeOperation operation) {
  if (operation_ == operation) return;
  operation_ = operation;
  updatePreview();
}

void ExtrudeToolSession::setReversed(bool reversed) {
  if (reversed_ == reversed) return;
  reversed_ = reversed;
  updatePreview();
}

BodyId ExtrudeToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId ExtrudeToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}
std::optional<FeatureId> ExtrudeToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}
const FaceReference& ExtrudeToolSession::face() const noexcept { return face_; }
double ExtrudeToolSession::lengthMm() const noexcept { return length_.value(); }
ExtrudeOperation ExtrudeToolSession::operation() const noexcept {
  return operation_;
}
bool ExtrudeToolSession::reversed() const noexcept { return reversed_; }

ToolLifecycle ExtrudeToolSession::lifecycle() const noexcept {
  return lifecycle_;
}

ToolSelectionStage ExtrudeToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  return ToolSelectionStage::EditingParameters;
}

std::optional<SelectionRequirement> ExtrudeToolSession::selectionRequirement() const {
  if (lifecycle_ == ToolLifecycle::Inactive) return std::nullopt;
  return SelectionRequirement{SelectionType::Face, "Select face", 1, 1, false};
}

std::vector<ToolParameterDescriptor> ExtrudeToolSession::parameters() const {
  return {{"distance", "Distance", ToolParameterType::Distance, length_.value(),
           minimumMm_, maximumMm_, 0.1, "mm", true,
           ToolManipulatorType::Linear}};
}

std::shared_ptr<const TopoDS_Shape> ExtrudeToolSession::previewShape() const {
  return previewShape_;
}

const std::string& ExtrudeToolSession::error() const noexcept { return error_; }

bool ExtrudeToolSession::updatePreview() {
  previewShape_.reset();
  geometry_.reset();
  error_.clear();
  if (!baseShape_ || baseShape_->IsNull()) {
    error_ = "Extrude base shape is missing";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (operation_ == ExtrudeOperation::NewBody) {
    error_ = "Face extrusion cannot create a new body";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (face_.bodyId != bodyId_ || face_.featureId != sourceFeatureId_) {
    error_ = "Extrude face no longer matches the active Body";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }

  TopoDS_Shape result;
  FaceExtrudeGeometry geometry;
  if (!buildExtrusionFromFace(*baseShape_, face_, length_.value(), operation_,
                              reversed_, &result, &geometry, &error_)) {
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  previewShape_ = std::make_shared<TopoDS_Shape>(result);
  geometry_ = geometry;
  lifecycle_ = ToolLifecycle::PreviewValid;
  return true;
}

std::optional<LinearToolManipulator> ExtrudeToolSession::manipulator() const {
  if (lifecycle_ == ToolLifecycle::Inactive) return std::nullopt;

  FaceExtrudeGeometry geometry;
  if (geometry_) {
    geometry = *geometry_;
  } else {
    if (!baseShape_ || baseShape_->IsNull()) return std::nullopt;
    try {
      const auto resolved = resolveFaceReference(*baseShape_, face_.topology());
      if (!resolved) return std::nullopt;
      gp_Pln plane;
      gp_Dir normal;
      std::string ignored;
      if (!resolveFacePlaneAndNormal(*resolved.subshape, &plane, &normal,
                                     &ignored))
        return std::nullopt;
      GProp_GProps properties;
      BRepGProp::SurfaceProperties(*resolved.subshape, properties);
      geometry.centroid = properties.CentreOfMass();
      geometry.normal = normal;
    } catch (const Standard_Failure&) {
      return std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  Vector3d direction{geometry.normal.X(), geometry.normal.Y(),
                     geometry.normal.Z()};
  if (reversed_)
    direction = {-direction.x, -direction.y, -direction.z};

  return LinearToolManipulator{{geometry.centroid.X(), geometry.centroid.Y(),
                                geometry.centroid.Z()},
                               direction,
                               length_.value(),
                               minimumMm_,
                               maximumMm_};
}

bool ExtrudeToolSession::trySetLength(double lengthMm) {
  const auto candidate = length_.candidate(lengthMm);
  if (!candidate || !baseShape_ || baseShape_->IsNull()) return false;
  const double previous = length_.value();
  const auto previousPreview = previewShape_;
  const auto previousLifecycle = lifecycle_;
  const auto previousError = error_;
  const auto previousGeometry = geometry_;
  length_.accept(*candidate);
  if (updatePreview()) return true;
  length_.accept(previous);
  previewShape_ = previousPreview;
  lifecycle_ = previousLifecycle;
  error_ = previousError;
  geometry_ = previousGeometry;
  return false;
}

void ExtrudeToolSession::cancel() noexcept {
  previewShape_.reset();
  geometry_.reset();
  face_ = FaceReference{};
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

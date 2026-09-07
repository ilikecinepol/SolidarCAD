#include "model/ChamferToolSession.h"

#include <BRepAdaptor_Curve.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

#include "model/ChamferBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

void ChamferToolSession::begin(BodyId bodyId, FeatureId sourceFeatureId,
                               ShapeFeature::ShapePtr baseShape,
                               std::vector<EdgeReference> edges,
                               double distanceMm,
                               std::optional<FeatureId> editingFeatureId) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  edges_ = std::move(edges);
  distanceMm_ = distanceMm;
  editingFeatureId_ = editingFeatureId;
  lifecycle_ = ToolLifecycle::Editing;
  updatePreview();
}

void ChamferToolSession::setEdges(std::vector<EdgeReference> edges) {
  edges_ = std::move(edges);
  updatePreview();
}
void ChamferToolSession::setDistanceFromPanel(double distanceMm) {
  distanceMm_ = distanceMm;
  updatePreview();
}
void ChamferToolSession::setDistanceFromManipulator(double distanceMm) {
  distanceMm_ = std::max(0.01, distanceMm);
  updatePreview();
}

BodyId ChamferToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId ChamferToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}
std::optional<FeatureId> ChamferToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}
const std::vector<EdgeReference>& ChamferToolSession::edges() const noexcept {
  return edges_;
}
double ChamferToolSession::distanceMm() const noexcept { return distanceMm_; }
ToolLifecycle ChamferToolSession::lifecycle() const noexcept {
  return lifecycle_;
}
ToolSelectionStage ChamferToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  return edges_.empty() ? ToolSelectionStage::SelectingInput
                        : ToolSelectionStage::EditingParameters;
}
std::optional<SelectionRequirement> ChamferToolSession::selectionRequirement() const {
  if (!edges_.empty()) return std::nullopt;
  return SelectionRequirement{SelectionType::Edge, "Select edges", 1,
                              static_cast<std::size_t>(-1), true};
}
std::vector<ToolParameterDescriptor> ChamferToolSession::parameters() const {
  return {{"distance", "Distance", ToolParameterType::Distance, distanceMm_,
           0.01, 100000.0, 0.1, "mm", true, ToolManipulatorType::Linear}};
}
std::shared_ptr<const TopoDS_Shape> ChamferToolSession::previewShape() const {
  return previewShape_;
}
const std::string& ChamferToolSession::error() const noexcept { return error_; }

bool ChamferToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (!baseShape_ || baseShape_->IsNull()) {
    error_ = "Chamfer base shape is missing";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (edges_.empty()) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  std::vector<std::size_t> indices;
  indices.reserve(edges_.size());
  for (const auto& edge : edges_) {
    if (edge.bodyId != bodyId_ || edge.featureId != sourceFeatureId_) {
      error_ = "Chamfer edges no longer match the active Body";
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    const auto resolved = resolveEdgeReference(*baseShape_, edge.topology());
    if (!resolved) {
      error_ = "Chamfer edge could not be resolved: " + resolved.error;
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    indices.push_back(resolved.index);
  }
  previewShape_ = buildChamferShape(*baseShape_, indices, distanceMm_, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

std::optional<LinearToolManipulator> ChamferToolSession::manipulator() const {
  if (!baseShape_ || edges_.empty()) return std::nullopt;
  try {
    const auto edge =
        resolveEdgeReference(*baseShape_, edges_.front().topology());
    if (!edge) return std::nullopt;
    BRepAdaptor_Curve curve(*edge.subshape);
    const gp_Pnt point =
        curve.Value((curve.FirstParameter() + curve.LastParameter()) * 0.5);
    Vector3d direction{point.X(), point.Y(), 0.0};
    const double length = std::hypot(direction.x, direction.y);
    if (length < 1e-8) {
      direction = {0.0, 0.0, 1.0};
    } else {
      direction.x /= length;
      direction.y /= length;
    }
    return LinearToolManipulator{{point.X(), point.Y(), point.Z()}, direction,
                                 distanceMm_};
  } catch (const Standard_Failure&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

void ChamferToolSession::cancel() noexcept {
  previewShape_.reset();
  edges_.clear();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

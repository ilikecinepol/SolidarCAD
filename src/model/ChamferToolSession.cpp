#include "model/ChamferToolSession.h"

#include <BRepAdaptor_Curve.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

#include "model/ChamferBuilder.h"
#include "model/EdgeManipulatorGeometry.h"
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
  distance_.reset(distanceMm, 0.0, 100000.0);
  editingFeatureId_ = editingFeatureId;
  lifecycle_ = ToolLifecycle::Editing;
  updatePreview();
}

void ChamferToolSession::setEdges(std::vector<EdgeReference> edges) {
  edges_ = std::move(edges);
  updatePreview();
}
void ChamferToolSession::setDistanceFromPanel(double distanceMm) {
  trySetDistance(distanceMm);
}
void ChamferToolSession::setDistanceFromManipulator(double distanceMm) {
  trySetDistance(distanceMm);
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
double ChamferToolSession::distanceMm() const noexcept { return distance_.value(); }
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
  return {{"distance", "Distance", ToolParameterType::Distance, distance_.value(),
           0.0, 100000.0, 0.1, "mm", true, ToolManipulatorType::Linear}};
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
  if (distance_.value() <= 0.0) {
    previewShape_ = baseShape_;
    lifecycle_ = ToolLifecycle::EditingParameters;
    return true;
  }
  previewShape_ = buildChamferShape(*baseShape_, indices, distance_.value(), &error_);
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
    const auto geometry = localEdgeManipulatorGeometry(*baseShape_, *edge.subshape);
    if (!geometry) return std::nullopt;
    return LinearToolManipulator{geometry->midpoint,
                                 geometry->outwardDirection, distance_.value(),
                                 0.0, 100000.0};
  } catch (const Standard_Failure&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

bool ChamferToolSession::trySetDistance(double distanceMm) {
  const auto candidate = distance_.candidate(distanceMm);
  if (!candidate || !baseShape_ || edges_.empty()) return false;
  const double previous = distance_.value();
  const auto previousPreview = previewShape_;
  const auto previousLifecycle = lifecycle_;
  const auto previousError = error_;
  distance_.accept(*candidate);
  if (updatePreview()) return true;
  distance_.accept(previous);
  previewShape_ = previousPreview;
  lifecycle_ = previousLifecycle;
  error_ = previousError;
  return false;
}

void ChamferToolSession::cancel() noexcept {
  previewShape_.reset();
  edges_.clear();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

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
                               std::optional<FeatureId> editingFeatureId,
                               std::shared_ptr<const TopologyIndex> topologyIndex) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  topologyIndexError_.clear();
  topologyIndex_ = std::move(topologyIndex);
  if (topologyIndex_ &&
      (!baseShape_ || baseShape_->IsNull() || !topologyIndex_->shape() ||
       topologyIndex_->shape().get() != baseShape_.get()))
    topologyIndex_.reset();
  if (!topologyIndex_ && baseShape_ && !baseShape_->IsNull())
    topologyIndex_ = TopologyIndex::build(baseShape_, kInvalidShapeRevision,
                                          &topologyIndexError_);
  edges_ = std::move(edges);
  distance_.reset(distanceMm, 0.0, 100000.0);
  editingFeatureId_ = editingFeatureId;
  maximumValidDistanceMm_.reset();
  pendingRequestedDistanceMm_.reset();
  limitReached_ = false;
  previewBuildAttemptCount_ = 0;
  lifecycle_ = ToolLifecycle::Editing;
  updatePreview();
}

void ChamferToolSession::setEdges(std::vector<EdgeReference> edges) {
  edges_ = std::move(edges);
  maximumValidDistanceMm_.reset();
  pendingRequestedDistanceMm_.reset();
  limitReached_ = false;
  updatePreview();
}
void ChamferToolSession::setDistanceFromPanel(double distanceMm) {
  trySetDistance(distanceMm, false);
}
void ChamferToolSession::setDistanceFromManipulator(double distanceMm) {
  trySetDistance(distanceMm, false);
}
bool ChamferToolSession::refineDistanceToBoundary(double requestedDistanceMm) {
  const double target =
      pendingRequestedDistanceMm_.value_or(requestedDistanceMm);
  pendingRequestedDistanceMm_.reset();
  return trySetDistance(target, true);
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
std::optional<double> ChamferToolSession::maximumValidDistanceMm() const noexcept {
  return maximumValidDistanceMm_;
}
bool ChamferToolSession::limitReached() const noexcept { return limitReached_; }
std::uint64_t ChamferToolSession::previewBuildAttemptCount() const noexcept {
  return previewBuildAttemptCount_;
}
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
           0.0, maximumValidDistanceMm_.value_or(100000.0), 0.1, "mm", true,
           ToolManipulatorType::Linear}};
}
std::shared_ptr<const TopoDS_Shape> ChamferToolSession::previewShape() const {
  return previewShape_;
}
const std::string& ChamferToolSession::error() const noexcept { return error_; }
OperationFailureCode ChamferToolSession::errorCode() const noexcept {
  return errorCode_;
}

bool ChamferToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  errorCode_ = OperationFailureCode::None;
  if (!baseShape_ || baseShape_->IsNull()) {
    errorCode_ = OperationFailureCode::MissingSource;
    error_ = "Chamfer base shape is missing";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (edges_.empty()) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  if (!topologyIndex_) {
    errorCode_ = OperationFailureCode::TopologyIndexUnavailable;
    error_ = "Chamfer topology could not be indexed";
    if (!topologyIndexError_.empty()) error_ += ": " + topologyIndexError_;
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  std::vector<TopologyReference> references;
  references.reserve(edges_.size());
  std::vector<std::size_t> indices;
  indices.reserve(edges_.size());
  for (const auto& edge : edges_) {
    if (edge.bodyId != bodyId_ || edge.featureId != sourceFeatureId_) {
      errorCode_ = OperationFailureCode::TopologyReferenceMismatch;
      error_ = "Chamfer edges no longer match the active Body";
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    references.push_back(edge.topology());
  }
  for (const auto& resolved : topologyIndex_->resolveEdges(references)) {
    if (!resolved) {
      errorCode_ = operationFailureCode(resolved.failure);
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
  ++previewBuildAttemptCount_;
  previewShape_ = buildChamferShape(*baseShape_, indices, distance_.value(), &error_);
  if (!previewShape_) errorCode_ = OperationFailureCode::GeometryOperationFailed;
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

std::optional<LinearToolManipulator> ChamferToolSession::manipulator() const {
  if (!baseShape_ || edges_.empty() || !topologyIndex_) return std::nullopt;
  try {
    const auto edge = topologyIndex_->resolveEdge(edges_.front().topology());
    if (!edge) return std::nullopt;
    const auto geometry = localEdgeManipulatorGeometry(*baseShape_, *edge.subshape);
    if (!geometry) return std::nullopt;
    return LinearToolManipulator{geometry->midpoint,
                                 geometry->outwardDirection, distance_.value(),
                                 0.0,
                                 maximumValidDistanceMm_.value_or(100000.0)};
  } catch (const Standard_Failure&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

bool ChamferToolSession::trySetDistance(double distanceMm,
                                        bool clampToBoundary) {
  const auto candidate = distance_.candidate(distanceMm);
  if (!candidate || !baseShape_ || edges_.empty()) return false;
  if (!clampToBoundary) {
    pendingRequestedDistanceMm_ = *candidate;
    maximumValidDistanceMm_.reset();
    limitReached_ = false;
  }
  const double previous = distance_.value();
  if (std::abs(*candidate - previous) <= 1e-12 &&
      lifecycle_ == ToolLifecycle::PreviewValid)
    return true;
  const auto previousPreview = previewShape_;
  distance_.accept(*candidate);
  if (updatePreview()) {
    pendingRequestedDistanceMm_.reset();
    limitReached_ = false;
    return true;
  }
  const std::string failure = error_.empty()
                                  ? "Chamfer preview could not be built"
                                  : error_;
  const OperationFailureCode failureCode = errorCode_;

  if (clampToBoundary && *candidate > previous && previousPreview &&
      topologyIndex_) {
    std::vector<TopologyReference> references;
    references.reserve(edges_.size());
    for (const auto& edge : edges_) references.push_back(edge.topology());
    std::vector<std::size_t> indices;
    indices.reserve(edges_.size());
    for (const auto& resolved : topologyIndex_->resolveEdges(references)) {
      if (!resolved) {
        indices.clear();
        break;
      }
      indices.push_back(resolved.index);
    }
    if (!indices.empty()) {
      double lower = previous;
      double upper = *candidate;
      auto boundaryPreview = previousPreview;
      for (int iteration = 0;
           iteration < 24 && upper - lower > 1e-4; ++iteration) {
        const double midpoint = lower + (upper - lower) * 0.5;
        ++previewBuildAttemptCount_;
        auto preview = buildChamferShape(*baseShape_, indices, midpoint);
        if (preview) {
          lower = midpoint;
          boundaryPreview = std::move(preview);
        } else {
          upper = midpoint;
        }
      }
      const double panelSafe = std::floor((lower + 1e-9) * 100.0) / 100.0;
      if (panelSafe >= previous && panelSafe < lower) {
        ++previewBuildAttemptCount_;
        if (auto preview =
                buildChamferShape(*baseShape_, indices, panelSafe)) {
          lower = panelSafe;
          boundaryPreview = std::move(preview);
        }
      }
      maximumValidDistanceMm_ = lower;
      limitReached_ = true;
      if (clampToBoundary) {
        distance_.accept(lower);
        previewShape_ = std::move(boundaryPreview);
        lifecycle_ = ToolLifecycle::PreviewValid;
        error_.clear();
        errorCode_ = OperationFailureCode::None;
        return true;
      }
    }
  }

  distance_.accept(previous);
  previewShape_ = previousPreview;
  lifecycle_ = ToolLifecycle::PreviewInvalid;
  error_ = failure;
  errorCode_ = failureCode;
  return false;
}

void ChamferToolSession::cancel() noexcept {
  previewShape_.reset();
  topologyIndex_.reset();
  topologyIndexError_.clear();
  edges_.clear();
  error_.clear();
  errorCode_ = OperationFailureCode::None;
  maximumValidDistanceMm_.reset();
  pendingRequestedDistanceMm_.reset();
  limitReached_ = false;
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar

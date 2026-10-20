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
namespace {

struct ResolvedEdgeBatch {
  std::vector<std::size_t> indices;
  TopologyResolutionFailure failure{TopologyResolutionFailure::None};
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return failure == TopologyResolutionFailure::None;
  }
};

ResolvedEdgeBatch resolveEdgeBatch(const TopologyIndex& topology,
                                   const std::vector<EdgeReference>& edges) {
  std::vector<EdgeResolution> resolutions;
  resolutions.reserve(edges.size());
  for (const auto& edge : edges)
    resolutions.push_back(topology.resolveEdge(edge));

  ResolvedEdgeBatch result;
  for (const auto& resolved : resolutions) {
    if (!resolved && result.failure == TopologyResolutionFailure::None) {
      result.failure = resolved.failure;
      result.error = resolved.error;
    }
  }
  if (!result) return result;

  result.indices.reserve(resolutions.size());
  for (const auto& resolved : resolutions)
    if (std::find(result.indices.begin(), result.indices.end(),
                  resolved.index) == result.indices.end())
      result.indices.push_back(resolved.index);
  return result;
}

bool containsV2(const std::vector<EdgeReference>& edges) {
  return std::any_of(edges.begin(), edges.end(),
                     [](const EdgeReference& edge) { return edge.isV2(); });
}

}  // namespace

void ChamferToolSession::begin(FeatureTopologySnapshot source,
                               std::vector<EdgeReference> edges,
                               double distanceMm,
                               std::optional<FeatureId> editingFeatureId) {
  bodyId_ = source.bodyId();
  sourceFeatureId_ = source.featureId();
  baseShape_ = source.shape();
  topologyIndexError_.clear();
  topologyIndex_ = source.topologyIndex();
  topologyIndexAuthoritative_ = source.v2Authoritative();
  sourceSnapshot_ = std::move(source);
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

void ChamferToolSession::beginLegacyV1(
    BodyId bodyId, FeatureId sourceFeatureId, ShapeFeature::ShapePtr baseShape,
    std::vector<EdgeReference> edges, double distanceMm,
    std::optional<FeatureId> editingFeatureId) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  sourceSnapshot_.reset();
  topologyIndex_.reset();
  topologyIndexAuthoritative_ = false;
  topologyIndexError_.clear();
  if (!containsV2(edges) && baseShape_ && !baseShape_->IsNull())
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
  if (!topologyIndex_ && !containsV2(edges_) && baseShape_ &&
      !baseShape_->IsNull()) {
    topologyIndexError_.clear();
    topologyIndex_ = TopologyIndex::build(baseShape_, kInvalidShapeRevision,
                                          &topologyIndexError_);
    topologyIndexAuthoritative_ = false;
  }
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
const std::optional<FeatureTopologySnapshot>&
ChamferToolSession::sourceSnapshot() const noexcept {
  return sourceSnapshot_;
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
  if (containsV2(edges_) && !topologyIndexAuthoritative_) {
    errorCode_ = OperationFailureCode::TopologyIndexUnavailable;
    error_ = "Chamfer v2 edges require their owning topology index";
    if (!topologyIndexError_.empty()) error_ += ": " + topologyIndexError_;
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  for (const auto& edge : edges_) {
    if (edge.bodyId() != bodyId_ || edge.featureId() != sourceFeatureId_) {
      errorCode_ = OperationFailureCode::TopologyReferenceMismatch;
      error_ = "Chamfer edges no longer match the active Body";
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
  }
  const auto resolved = resolveEdgeBatch(*topologyIndex_, edges_);
  if (!resolved) {
    errorCode_ = operationFailureCode(resolved.failure);
    error_ = "Chamfer edge could not be resolved: " + resolved.error;
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (distance_.value() <= 0.0) {
    previewShape_ = baseShape_;
    lifecycle_ = ToolLifecycle::EditingParameters;
    return true;
  }
  ++previewBuildAttemptCount_;
  previewShape_ = buildChamferShape(*baseShape_, resolved.indices,
                                    distance_.value(), &error_);
  if (!previewShape_) errorCode_ = OperationFailureCode::GeometryOperationFailed;
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

std::optional<LinearToolManipulator> ChamferToolSession::manipulator() const {
  if ((lifecycle_ != ToolLifecycle::PreviewValid &&
       lifecycle_ != ToolLifecycle::EditingParameters) ||
      !error_.empty() || !previewShape_ || !baseShape_ || edges_.empty() ||
      !topologyIndex_)
    return std::nullopt;
  try {
    if (containsV2(edges_) && !topologyIndexAuthoritative_)
      return std::nullopt;
    if (!resolveEdgeBatch(*topologyIndex_, edges_)) return std::nullopt;
    const auto edge = topologyIndex_->resolveEdge(edges_.front());
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
    if (containsV2(edges_) && !topologyIndexAuthoritative_) return false;
    const auto resolved = resolveEdgeBatch(*topologyIndex_, edges_);
    if (resolved && !resolved.indices.empty()) {
      double lower = previous;
      double upper = *candidate;
      auto boundaryPreview = previousPreview;
      for (int iteration = 0;
           iteration < 24 && upper - lower > 1e-4; ++iteration) {
        const double midpoint = lower + (upper - lower) * 0.5;
        ++previewBuildAttemptCount_;
        auto preview =
            buildChamferShape(*baseShape_, resolved.indices, midpoint);
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
        if (auto preview = buildChamferShape(*baseShape_, resolved.indices,
                                             panelSafe)) {
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
  sourceSnapshot_.reset();
  topologyIndex_.reset();
  baseShape_.reset();
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  editingFeatureId_.reset();
  topologyIndexAuthoritative_ = false;
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

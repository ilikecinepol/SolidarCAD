#include "model/ChamferFeature.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "model/Body.h"
#include "model/ChamferBuilder.h"
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

}  // namespace

ChamferFeature::ChamferFeature(EdgeReference edge, double distanceMm,
                               std::string name)
    : ChamferFeature(std::vector<EdgeReference>{std::move(edge)}, distanceMm,
                     std::move(name)) {}

ChamferFeature::ChamferFeature(std::vector<EdgeReference> edges,
                               double distanceMm, std::string name)
    : ShapeFeature(name.empty() ? "Chamfer" : std::move(name)),
      edges_(std::move(edges)), distanceMm_(distanceMm) {}

ChamferFeature::ChamferFeature(FeatureId id,
                               std::vector<EdgeReference> edges,
                               double distanceMm, std::string name)
    : ShapeFeature(id, std::move(name)), edges_(std::move(edges)),
      distanceMm_(distanceMm) {}

const EdgeReference& ChamferFeature::edge() const noexcept {
  static const EdgeReference empty;
  return edges_.empty() ? empty : edges_.front();
}
const std::vector<EdgeReference>& ChamferFeature::edges() const noexcept {
  return edges_;
}
double ChamferFeature::distanceMm() const noexcept { return distanceMm_; }

void ChamferFeature::setEdges(std::vector<EdgeReference> edges) noexcept {
  if (edges_ == edges) return;
  edges_ = std::move(edges);
  setDirty();
}
void ChamferFeature::setDistanceMm(double value) noexcept {
  if (distanceMm_ == value) return;
  distanceMm_ = value;
  setDirty();
}


bool ChamferFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(distanceMm_) || distanceMm_ <= 0.0) {
    markError("Chamfer distance must be a finite positive value");
    return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Chamfer base shape is missing");
    return false;
  }
  if (edges_.empty()) {
    markError("Chamfer requires at least one edge");
    return false;
  }
  if (!context.body) {
    markError("Chamfer edge belongs to a different Body");
    return false;
  }
  const BodyId sourceBody = edges_.front().bodyId();
  const FeatureId sourceFeature = edges_.front().featureId();
  for (const auto& edge : edges_)
    if (edge.bodyId() != sourceBody || edge.featureId() != sourceFeature ||
        edge.bodyId() != context.body->id()) {
      markError("Chamfer edges must belong to one Body and source Feature");
      return false;
    }

  const auto& features = context.body->features();
  std::size_t ownIndex = features.size();
  for (std::size_t index = 0; index < features.size(); ++index)
    if (features[index].get() == this) {
      ownIndex = index;
      break;
    }
  if (ownIndex == 0 || ownIndex == features.size() ||
      features[ownIndex - 1]->id() != sourceFeature) {
    markError("Chamfer source Feature could not be resolved");
    return false;
  }
  const auto* immediatePredecessor = features[ownIndex - 1].get();
  if (!context.previousFeature ||
      context.previousFeature != immediatePredecessor ||
      immediatePredecessor->shape().get() != context.previousShape) {
    markError("Chamfer rebuild context does not match its immediate predecessor");
    return false;
  }

  std::string topologyError;
  const auto topology = immediatePredecessor->topologyIndex(&topologyError);
  if (!topology) {
    markError(OperationFailureCode::TopologyIndexUnavailable,
              "Chamfer topology could not be indexed: " + topologyError);
    return false;
  }
  if (!topology->shape() || topology->shape().get() != context.previousShape ||
      topology->revision() != immediatePredecessor->shapeRevision()) {
    markError("Chamfer topology index does not match its immediate predecessor");
    return false;
  }
  const auto resolved = resolveEdgeBatch(*topology, edges_);
  if (!resolved) {
    markError(operationFailureCode(resolved.failure),
              "Chamfer edge could not be resolved: " + resolved.error);
    return false;
  }
  std::string error;
  auto result = buildChamferShape(*context.previousShape, resolved.indices,
                                  distanceMm_, &error);
  if (!result) {
    markError(std::move(error));
    return false;
  }
  setShape(std::move(result));
  markValid();
  return true;
}

std::unique_ptr<Feature> ChamferFeature::clone() const {
  return std::make_unique<ChamferFeature>(*this);
}

}  // namespace solidar

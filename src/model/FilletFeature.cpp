#include "model/FilletFeature.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "model/Body.h"
#include "model/FilletBuilder.h"
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

FilletFeature::FilletFeature(EdgeReference edge, double radiusMm,
                             std::string name)
    : ShapeFeature(name.empty() ? "Fillet" : std::move(name)),
      edges_{edge},
      radiusMm_(radiusMm) {}

FilletFeature::FilletFeature(std::vector<EdgeReference> edges, double radiusMm,
                             std::string name)
    : ShapeFeature(name.empty() ? "Fillet" : std::move(name)),
      edges_(std::move(edges)), radiusMm_(radiusMm) {}

FilletFeature::FilletFeature(FeatureId id, EdgeReference edge,
                             double radiusMm, std::string name)
    : ShapeFeature(id, std::move(name)), edges_{edge}, radiusMm_(radiusMm) {}

FilletFeature::FilletFeature(FeatureId id, std::vector<EdgeReference> edges,
                             double radiusMm, std::string name)
    : ShapeFeature(id, std::move(name)), edges_(std::move(edges)),
      radiusMm_(radiusMm) {}

const EdgeReference& FilletFeature::edge() const noexcept {
  static const EdgeReference empty;
  return edges_.empty() ? empty : edges_.front();
}
const std::vector<EdgeReference>& FilletFeature::edges() const noexcept {
  return edges_;
}
double FilletFeature::radiusMm() const noexcept { return radiusMm_; }

void FilletFeature::setEdge(EdgeReference edge) noexcept {
  setEdges({edge});
}

void FilletFeature::setEdges(std::vector<EdgeReference> edges) noexcept {
  if (edges_ == edges) return;
  edges_ = std::move(edges);
  setDirty();
}

void FilletFeature::setRadiusMm(double value) noexcept {
  if (radiusMm_ == value) return;
  radiusMm_ = value;
  setDirty();
}


bool FilletFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(radiusMm_) || radiusMm_ <= 0.0) {
    markError("Fillet radius must be a finite positive value");
    return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Fillet base shape is missing");
    return false;
  }
  if (edges_.empty()) {
    markError("Fillet requires at least one edge");
    return false;
  }
  if (!context.body) {
    markError("Fillet edge belongs to a different Body");
    return false;
  }
  const BodyId sourceBody = edges_.front().bodyId();
  const FeatureId sourceFeature = edges_.front().featureId();
  for (const auto& edge : edges_)
    if (edge.bodyId() != sourceBody || edge.featureId() != sourceFeature ||
        edge.bodyId() != context.body->id()) {
      markError("Fillet edges must belong to one Body and source Feature");
      return false;
    }

  // The selected topology belongs to the result immediately preceding this
  // feature. Persistent data resolves the current edge; edgeIndex is only the
  // compatibility fallback for older project files.
  const auto& features = context.body->features();
  std::size_t ownIndex = features.size();
  for (std::size_t index = 0; index < features.size(); ++index)
    if (features[index].get() == this) {
      ownIndex = index;
      break;
    }
  if (ownIndex == 0 || ownIndex == features.size() ||
      features[ownIndex - 1]->id() != sourceFeature) {
    markError("Fillet source Feature could not be resolved");
    return false;
  }
  const auto* immediatePredecessor = features[ownIndex - 1].get();
  if (!context.previousFeature ||
      context.previousFeature != immediatePredecessor ||
      immediatePredecessor->shape().get() != context.previousShape) {
    markError("Fillet rebuild context does not match its immediate predecessor");
    return false;
  }

  std::string topologyError;
  const auto topology = immediatePredecessor->topologyIndex(&topologyError);
  if (!topology) {
    markError("Fillet topology could not be indexed: " + topologyError);
    return false;
  }
  if (!topology->shape() || topology->shape().get() != context.previousShape ||
      topology->revision() != immediatePredecessor->shapeRevision()) {
    markError("Fillet topology index does not match its immediate predecessor");
    return false;
  }
  const auto resolved = resolveEdgeBatch(*topology, edges_);
  if (!resolved) {
    markError("Fillet edge could not be resolved: " + resolved.error);
    return false;
  }
  std::string error;
  auto result = buildFilletShape(*context.previousShape, resolved.indices,
                                 radiusMm_, &error);
  if (!result) {
    markError(std::move(error));
    return false;
  }
  setShape(std::move(result));
  markValid();
  return true;
}

std::unique_ptr<Feature> FilletFeature::clone() const {
  return std::make_unique<FilletFeature>(*this);
}

}  // namespace solidar

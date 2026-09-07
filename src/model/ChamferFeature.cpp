#include "model/ChamferFeature.h"

#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/Body.h"
#include "model/ChamferBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

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

std::string ChamferFeature::typeName() const { return "Chamfer"; }

bool ChamferFeature::rebuild(const RebuildContext& context) {
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
  const BodyId sourceBody = edges_.front().bodyId;
  const FeatureId sourceFeature = edges_.front().featureId;
  for (const auto& edge : edges_)
    if (edge.bodyId != sourceBody || edge.featureId != sourceFeature ||
        edge.bodyId != context.body->id()) {
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

  std::vector<std::size_t> indices;
  indices.reserve(edges_.size());
  for (const auto& edge : edges_) {
    const auto resolved =
        resolveEdgeReference(*context.previousShape, edge.topology());
    if (!resolved) {
      markError("Chamfer edge could not be resolved: " + resolved.error);
      return false;
    }
    indices.push_back(resolved.index);
  }
  std::string error;
  auto result = buildChamferShape(*context.previousShape, indices, distanceMm_,
                                  &error);
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

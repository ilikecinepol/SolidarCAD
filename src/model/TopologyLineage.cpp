#include "model/TopologyLineage.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <tuple>
#include <utility>

namespace solidar {
namespace {

bool originLess(const TopologyProvenanceOrigin& left,
                const TopologyProvenanceOrigin& right) {
  return std::tie(left.featureId, left.kind, left.localName) <
         std::tie(right.featureId, right.kind, right.localName);
}

bool validKind(TopologyKind kind) noexcept {
  switch (kind) {
    case TopologyKind::Face:
    case TopologyKind::Edge:
      return true;
  }
  return false;
}

bool validOperation(TopologyProvenanceOperation operation) noexcept {
  switch (operation) {
    case TopologyProvenanceOperation::Seed:
    case TopologyProvenanceOperation::Generated:
    case TopologyProvenanceOperation::Modified:
    case TopologyProvenanceOperation::Merge:
      return true;
  }
  return false;
}

bool validRole(TopologySemanticRole role) noexcept {
  switch (role) {
    case TopologySemanticRole::SketchBoundary:
    case TopologySemanticRole::Cap:
    case TopologySemanticRole::Side:
    case TopologySemanticRole::Original:
    case TopologySemanticRole::Reflected:
    case TopologySemanticRole::PatternInstance:
    case TopologySemanticRole::BooleanResult:
    case TopologySemanticRole::Custom:
      return true;
  }
  return false;
}

}  // namespace

TopologyProvenanceKey::TopologyProvenanceKey(
    FeatureId producerFeatureId, TopologyKind topologyKind,
    TopologyProvenanceOperation provenanceOperation,
    TopologySemanticRole semanticRole, std::string authoredLocalName,
    std::vector<TopologyProvenanceOrigin> parentOrigins,
    std::optional<TopologyOccurrenceKey> authoredOccurrence)
    : featureId(producerFeatureId),
      kind(topologyKind),
      operation(provenanceOperation),
      role(semanticRole),
      localName(std::move(authoredLocalName)),
      origins(std::move(parentOrigins)),
      occurrence(authoredOccurrence) {
  canonicalizeOrigins();
}

void TopologyProvenanceKey::canonicalizeOrigins() {
  std::sort(origins.begin(), origins.end(), originLess);
  origins.erase(std::unique(origins.begin(), origins.end()), origins.end());
}

bool TopologyProvenanceKey::isCanonical(std::string* error) const {
  if (error) error->clear();
  if (featureId == kInvalidFeatureId) {
    if (error) *error = "Topology provenance producer identity is invalid";
    return false;
  }
  if (!validKind(kind)) {
    if (error) *error = "Topology provenance kind is invalid";
    return false;
  }
  if (!validOperation(operation)) {
    if (error) *error = "Topology provenance operation is invalid";
    return false;
  }
  if (!validRole(role)) {
    if (error) *error = "Topology provenance semantic role is invalid";
    return false;
  }
  if (localName.empty()) {
    if (error) *error = "Topology provenance local name is empty";
    return false;
  }
  if (occurrence && occurrence->featureId == kInvalidFeatureId) {
    if (error) *error = "Topology provenance occurrence identity is invalid";
    return false;
  }
  for (const auto& origin : origins) {
    if (origin.featureId == kInvalidFeatureId || !validKind(origin.kind) ||
        origin.localName.empty()) {
      if (error) *error = "Topology provenance origin is invalid";
      return false;
    }
  }
  if (!std::is_sorted(origins.begin(), origins.end(), originLess)) {
    if (error) *error = "Topology provenance origins are not canonical";
    return false;
  }
  if (std::adjacent_find(origins.begin(), origins.end()) != origins.end()) {
    if (error) *error = "Topology provenance origins contain duplicates";
    return false;
  }
  return true;
}

TopologyLineage::TopologyLineage(
    std::vector<TopologyLineageBinding> bindings)
    : bindings_(std::move(bindings)) {}

std::shared_ptr<const TopologyLineage> TopologyLineage::create(
    std::vector<TopologyLineageBinding> bindings, std::string* error) {
  if (error) error->clear();
  for (auto& binding : bindings) {
    binding.provenance.canonicalizeOrigins();
    std::string diagnostic;
    if (!binding.provenance.isCanonical(&diagnostic)) {
      if (error) *error = std::move(diagnostic);
      return {};
    }
    if (!binding.subshape || binding.subshape->IsNull()) {
      if (error) *error = "Topology lineage binding has no subshape";
      return {};
    }
  }
  return std::shared_ptr<const TopologyLineage>(
      new TopologyLineage(std::move(bindings)));
}

const std::vector<TopologyLineageBinding>& TopologyLineage::bindings()
    const noexcept {
  return bindings_;
}

bool TopologyLineage::empty() const noexcept { return bindings_.empty(); }

}  // namespace solidar

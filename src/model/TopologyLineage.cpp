#include "model/TopologyLineage.h"

#include "model/GeometryOperation.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <tuple>
#include <utility>

namespace solidar {
namespace {

bool originLess(const TopologyProvenanceOrigin& left,
                const TopologyProvenanceOrigin& right) {
  const auto occurrenceTuple = [](const TopologyProvenanceOrigin& origin) {
    return std::tuple{origin.occurrence.has_value(),
                      origin.occurrence ? origin.occurrence->featureId
                                        : kInvalidFeatureId,
                      origin.occurrence ? origin.occurrence->slot : 0};
  };
  const auto leftIdentity = std::tie(left.featureId, left.kind,
                                     left.operation, left.role,
                                     left.localName);
  const auto rightIdentity = std::tie(right.featureId, right.kind,
                                      right.operation, right.role,
                                      right.localName);
  if (leftIdentity != rightIdentity)
    return leftIdentity < rightIdentity;
  return occurrenceTuple(left) < occurrenceTuple(right);
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
    case TopologyProvenanceOperation::Retained:
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

TopologyProvenanceOrigin::TopologyProvenanceOrigin(
    FeatureId sourceFeatureId, TopologyKind topologyKind,
    TopologyProvenanceOperation provenanceOperation,
    TopologySemanticRole semanticRole, std::string authoredLocalName,
    std::optional<TopologyOccurrenceKey> authoredOccurrence)
    : featureId(sourceFeatureId),
      kind(topologyKind),
      operation(provenanceOperation),
      role(semanticRole),
      localName(std::move(authoredLocalName)),
      occurrence(authoredOccurrence) {}

bool TopologyProvenanceOrigin::isValid(std::string* error) const {
  if (error) error->clear();
  if (featureId == kInvalidFeatureId || !validKind(kind) ||
      !validOperation(operation) || !validRole(role) || localName.empty() ||
      (occurrence && occurrence->featureId != featureId)) {
    if (error) *error = "Topology provenance origin is invalid";
    return false;
  }
  return true;
}

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

TopologyProvenanceOrigin TopologyProvenanceKey::immediateOrigin() const {
  return {featureId, kind, operation, role, localName, occurrence};
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
  if (occurrence && occurrence->featureId != featureId) {
    if (error) *error = "Topology provenance occurrence owner is invalid";
    return false;
  }
  for (const auto& origin : origins) {
    std::string originError;
    if (!origin.isValid(&originError)) {
      if (error) *error = std::move(originError);
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

bool isValidTopologyLineageCoverage(
    TopologyLineageCoverage coverage) noexcept {
  switch (coverage) {
    case TopologyLineageCoverage::None:
    case TopologyLineageCoverage::PartialFaces:
    case TopologyLineageCoverage::CompleteFaces:
      return true;
  }
  return false;
}

TopologyLineage::TopologyLineage(
    std::vector<TopologyLineageBinding> bindings,
    TopologyLineageCoverage coverage)
    : bindings_(std::move(bindings)), coverage_(coverage) {}

std::shared_ptr<const TopologyLineage> TopologyLineage::create(
    std::vector<TopologyLineageBinding> bindings,
    TopologyLineageCoverage coverage, std::string* error) {
  if (error) error->clear();
  std::shared_ptr<const TopologyLineage> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        if (!isValidTopologyLineageCoverage(coverage)) {
          if (error) *error = "Topology lineage coverage is invalid";
          return;
        }
        bool hasFaceBinding = false;
        for (auto& binding : bindings) {
          binding.provenance.canonicalizeOrigins();
          std::string diagnostic;
          if (!binding.provenance.isCanonical(&diagnostic)) {
            if (error) *error = std::move(diagnostic);
            return;
          }
          if (!binding.subshape || binding.subshape->IsNull()) {
            if (error) *error = "Topology lineage binding has no subshape";
            return;
          }
          hasFaceBinding = hasFaceBinding ||
                           binding.provenance.kind == TopologyKind::Face;
        }
        if (coverage == TopologyLineageCoverage::None && !bindings.empty()) {
          if (error)
            *error = "No-coverage topology lineage cannot contain bindings";
          return;
        }
        if (coverage != TopologyLineageCoverage::None &&
            (bindings.empty() || !hasFaceBinding)) {
          if (error)
            *error = "Face coverage requires at least one face binding";
          return;
        }
        result = std::shared_ptr<const TopologyLineage>(
            new TopologyLineage(std::move(bindings), coverage));
      },
      &failure);
  if (!completed) {
    if (error)
      *error = failure.detail.empty()
                   ? "Topology lineage validation raised a geometry exception"
                   : "Topology lineage validation failed: " + failure.detail;
    return {};
  }
  return result;
}

const std::vector<TopologyLineageBinding>& TopologyLineage::bindings()
    const noexcept {
  return bindings_;
}

bool TopologyLineage::empty() const noexcept { return bindings_.empty(); }

TopologyLineageCoverage TopologyLineage::coverage() const noexcept {
  return coverage_;
}

bool TopologyLineage::validateForProducer(
    FeatureId producerFeatureId, std::string* error) const {
  if (error) error->clear();
  if (producerFeatureId == kInvalidFeatureId) {
    if (error) *error = "Topology lineage producer identity is invalid";
    return false;
  }
  if (bindings_.empty()) {
    if (error) *error = "Attempted topology lineage has no bindings";
    return false;
  }
  for (std::size_t index = 0; index < bindings_.size(); ++index) {
    const auto& binding = bindings_[index];
    if (binding.provenance.featureId != producerFeatureId) {
      if (error)
        *error = "Topology lineage binding producer does not match Feature";
      return false;
    }
    for (std::size_t otherIndex = 0; otherIndex < index; ++otherIndex) {
      const auto& other = bindings_[otherIndex];
      if (binding.provenance != other.provenance &&
          binding.provenance.immediateOrigin() ==
              other.provenance.immediateOrigin()) {
        if (error)
          *error = "Topology lineage immediate identity has conflicting ancestry";
        return false;
      }
    }
  }
  return true;
}

}  // namespace solidar

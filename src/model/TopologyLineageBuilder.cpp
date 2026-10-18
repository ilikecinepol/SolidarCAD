#include "model/TopologyLineageBuilder.h"

#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <utility>

#include "model/GeometryOperation.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {
namespace {

void setGeometryFailure(const GeometryFailure& failure, std::string* error,
                        const char* context) {
  if (!error) return;
  *error = context;
  if (!failure.detail.empty()) {
    *error += ": ";
    *error += failure.detail;
  }
}

bool validFactKind(ShapeHistoryFactKind kind) noexcept {
  switch (kind) {
    case ShapeHistoryFactKind::Seed:
    case ShapeHistoryFactKind::Generated:
    case ShapeHistoryFactKind::Modified:
    case ShapeHistoryFactKind::Retained:
    case ShapeHistoryFactKind::Deleted:
      return true;
  }
  return false;
}

bool validAuthoredToken(const AuthoredTopologyToken& token) noexcept {
  if (token.sketchId == kInvalidSketchId) return false;
  switch (token.kind) {
    case AuthoredTopologySourceKind::ProfileStartCap:
    case AuthoredTopologySourceKind::ProfileEndCap:
      return token.geometryId == sketch::kInvalidGeometryId;
    case AuthoredTopologySourceKind::GeometrySide:
      return token.geometryId != sketch::kInvalidGeometryId;
  }
  return false;
}

bool normalizeHistoryFact(HistoryFact& fact, FeatureId producerFeatureId,
                          std::string* error) {
  if (fact.kind != ShapeHistoryFactKind::Seed) return true;
  if (!fact.authoredSource) {
    if (error) *error = "Seed history fact authored source is missing";
    return false;
  }
  const auto projected = projectAuthoredTopologyToken(
      *fact.authoredSource, producerFeatureId, error);
  if (!projected) return false;
  if (fact.outputProvenance && *fact.outputProvenance != *projected) {
    if (error)
      *error = "Seed history fact output does not match authored source";
    return false;
  }
  fact.outputProvenance = *projected;
  return true;
}

bool normalizeShapeHistoryArtifact(ShapeHistoryArtifact& artifact,
                                   std::string* error) {
  for (auto& fact : artifact.facts) {
    if (!normalizeHistoryFact(fact, artifact.producerFeatureId, error))
      return false;
  }
  return true;
}

bool outputOperationMatches(ShapeHistoryFactKind fact,
                            TopologyProvenanceOperation operation) noexcept {
  switch (fact) {
    case ShapeHistoryFactKind::Seed:
      return operation == TopologyProvenanceOperation::Seed;
    case ShapeHistoryFactKind::Generated:
      return operation == TopologyProvenanceOperation::Generated ||
             operation == TopologyProvenanceOperation::Merge;
    case ShapeHistoryFactKind::Modified:
      return operation == TopologyProvenanceOperation::Modified ||
             operation == TopologyProvenanceOperation::Merge;
    case ShapeHistoryFactKind::Retained:
      return operation == TopologyProvenanceOperation::Retained ||
             operation == TopologyProvenanceOperation::Merge;
    case ShapeHistoryFactKind::Deleted:
      return false;
  }
  return false;
}

bool subshapeKindMatches(TopologyKind kind,
                         const TopoDS_Shape& subshape) noexcept {
  switch (kind) {
    case TopologyKind::Face:
      return subshape.ShapeType() == TopAbs_FACE;
    case TopologyKind::Edge:
      return subshape.ShapeType() == TopAbs_EDGE;
  }
  return false;
}

bool outputContains(const TopoDS_Shape& output,
                    const TopoDS_Shape& subshape,
                    TopologyKind kind) {
  TopAbs_ShapeEnum shapeKind;
  switch (kind) {
    case TopologyKind::Face:
      shapeKind = TopAbs_FACE;
      break;
    case TopologyKind::Edge:
      shapeKind = TopAbs_EDGE;
      break;
    default:
      return false;
  }
  for (TopExp_Explorer explorer(output, shapeKind); explorer.More();
       explorer.Next()) {
    if (explorer.Current().IsSame(subshape)) return true;
  }
  return false;
}

bool validateTopologySourceSnapshotImpl(const TopologySourceSnapshot& snapshot,
                                        std::string* error) {
  if (snapshot.featureId == kInvalidFeatureId || !snapshot.shape ||
      snapshot.shape->IsNull() || !snapshot.lineage) {
    if (error) *error = "Topology source snapshot is invalid";
    return false;
  }
  if (!snapshot.lineage->validateForProducer(snapshot.featureId, error))
    return false;
  return static_cast<bool>(
      TopologyIndex::build(snapshot.shape, 1, snapshot.lineage, error));
}

bool validateHistoryFactImpl(
    const HistoryFact& fact, FeatureId producerFeatureId,
    std::span<const TopologySourceSnapshot> sources, std::string* error) {
  if (!validFactKind(fact.kind) || producerFeatureId == kInvalidFeatureId) {
    if (error) *error = "Shape history fact is invalid";
    return false;
  }

  if (fact.kind == ShapeHistoryFactKind::Seed) {
    if (!fact.authoredSource || !validAuthoredToken(*fact.authoredSource) ||
        fact.source || fact.sourceSubshape) {
      if (error) *error = "Seed history fact authored source is invalid";
      return false;
    }
  } else {
    if (fact.authoredSource || !fact.source || !fact.sourceSubshape ||
        fact.sourceSubshape->IsNull() || !fact.source->isValid(error)) {
      if (error && error->empty())
        *error = "Shape history fact source evidence is invalid";
      return false;
    }
    const auto source = std::find_if(
        sources.begin(), sources.end(), [&](const auto& candidate) {
          return candidate.featureId == fact.source->featureId;
        });
    if (source == sources.end() ||
        !validateTopologySourceSnapshotImpl(*source, error)) {
      if (error && error->empty())
        *error = "Shape history fact source snapshot is missing";
      return false;
    }
    if (!subshapeKindMatches(fact.source->kind, *fact.sourceSubshape)) {
      if (error) *error = "Shape history fact source kind is invalid";
      return false;
    }

    bool sourceIdentityExists = false;
    for (const auto& binding : source->lineage->bindings()) {
      if (binding.provenance.immediateOrigin() != *fact.source) continue;
      if (!binding.subshape->IsSame(*fact.sourceSubshape)) {
        if (error)
          *error = "Shape history source identity is not canonical";
        return false;
      }
      sourceIdentityExists = true;
    }
    if (!sourceIdentityExists) {
      if (error) *error = "Shape history fact source identity is missing";
      return false;
    }
  }

  if (fact.kind == ShapeHistoryFactKind::Deleted) {
    if (fact.outputProvenance || fact.outputSubshape) {
      if (error) *error = "Deleted history fact cannot have an output";
      return false;
    }
    return true;
  }
  if (!fact.outputProvenance || !fact.outputSubshape ||
      fact.outputSubshape->IsNull()) {
    if (error) *error = "Shape history fact output is missing";
    return false;
  }
  std::string provenanceError;
  if (!fact.outputProvenance->isCanonical(&provenanceError) ||
      fact.outputProvenance->featureId != producerFeatureId ||
      !outputOperationMatches(fact.kind,
                              fact.outputProvenance->operation) ||
      !subshapeKindMatches(fact.outputProvenance->kind,
                           *fact.outputSubshape)) {
    if (error)
      *error = provenanceError.empty()
                   ? "Shape history fact output identity is invalid"
                   : std::move(provenanceError);
    return false;
  }
  if (fact.kind == ShapeHistoryFactKind::Retained &&
      !fact.outputSubshape->IsSame(*fact.sourceSubshape)) {
    if (error)
      *error = "Retained history output is not the same source subshape";
    return false;
  }
  if (fact.kind == ShapeHistoryFactKind::Seed) {
    const auto projected = projectAuthoredTopologyToken(
        *fact.authoredSource, producerFeatureId, &provenanceError);
    if (!projected || *projected != *fact.outputProvenance) {
      if (error)
        *error = provenanceError.empty()
                     ? "Seed history fact output does not match authored source"
                     : std::move(provenanceError);
      return false;
    }
  } else if (std::find(fact.outputProvenance->origins.begin(),
                       fact.outputProvenance->origins.end(), *fact.source) ==
             fact.outputProvenance->origins.end()) {
    if (error)
      *error = "Shape history fact output does not retain its source identity";
    return false;
  }
  return true;
}

struct OutputFactGroup {
  TopologyProvenanceKey provenance;
  std::shared_ptr<const TopoDS_Shape> subshape;
  std::vector<const HistoryFact*> facts;
};

bool validateShapeHistoryArtifactImpl(const ShapeHistoryArtifact& artifact,
                                      std::string* error) {
  if (artifact.producerFeatureId == kInvalidFeatureId ||
      !artifact.outputShape || artifact.outputShape->IsNull() ||
      !isValidTopologyLineageCoverage(artifact.coverage)) {
    if (error) *error = "Shape history artifact header is invalid";
    return false;
  }
  for (std::size_t index = 0; index < artifact.sources.size(); ++index) {
    if (!validateTopologySourceSnapshotImpl(artifact.sources[index], error))
      return false;
    for (std::size_t other = 0; other < index; ++other) {
      if (artifact.sources[index].featureId ==
          artifact.sources[other].featureId) {
        if (error) *error = "Shape history source identity is duplicated";
        return false;
      }
    }
  }

  std::vector<OutputFactGroup> groups;
  bool hasFaceOutput = false;
  for (std::size_t factIndex = 0; factIndex < artifact.facts.size();
       ++factIndex) {
    const auto& fact = artifact.facts[factIndex];
    if (!validateHistoryFactImpl(fact, artifact.producerFeatureId,
                                 artifact.sources, error))
      return false;
    if (fact.source && fact.sourceSubshape) {
      for (std::size_t otherIndex = 0; otherIndex < factIndex; ++otherIndex) {
        const auto& other = artifact.facts[otherIndex];
        if (!other.source || !other.sourceSubshape ||
            *other.source != *fact.source ||
            !other.sourceSubshape->IsSame(*fact.sourceSubshape))
          continue;
        const bool factDeleted = fact.kind == ShapeHistoryFactKind::Deleted;
        const bool otherDeleted = other.kind == ShapeHistoryFactKind::Deleted;
        const auto liveKind = factDeleted ? other.kind : fact.kind;
        if (factDeleted != otherDeleted &&
            (liveKind == ShapeHistoryFactKind::Modified ||
             liveKind == ShapeHistoryFactKind::Retained)) {
          if (error)
            *error = "Shape history source has contradictory deletion evidence";
          return false;
        }
      }
    }
    if (fact.kind == ShapeHistoryFactKind::Deleted) continue;
    hasFaceOutput = hasFaceOutput ||
                    fact.outputProvenance->kind == TopologyKind::Face;
    if (!outputContains(*artifact.outputShape, *fact.outputSubshape,
                        fact.outputProvenance->kind)) {
      if (error)
        *error = "Shape history fact output is not part of output shape";
      return false;
    }

    auto group = std::find_if(groups.begin(), groups.end(), [&](auto& item) {
      return item.subshape->IsSame(*fact.outputSubshape);
    });
    if (group == groups.end()) {
      const auto keyCollision =
          std::find_if(groups.begin(), groups.end(), [&](auto& item) {
            return item.provenance == *fact.outputProvenance;
          });
      if (keyCollision != groups.end()) {
        if (error)
          *error = "Shape history output identity names distinct subshapes";
        return false;
      }
      groups.push_back(
          {*fact.outputProvenance, fact.outputSubshape, {&fact}});
    } else {
      if (group->provenance != *fact.outputProvenance) {
        if (error)
          *error = "Canonical history output has multiple provenance identities";
        return false;
      }
      group->facts.push_back(&fact);
    }
  }

  if (groups.empty()) {
    if (error) *error = "Shape history artifact has no live output facts";
    return false;
  }
  if (artifact.coverage == TopologyLineageCoverage::None) {
    if (error) *error = "No-coverage history artifact has output facts";
    return false;
  }
  if (!hasFaceOutput) {
    if (error) *error = "Face coverage requires a face output fact";
    return false;
  }

  for (const auto& group : groups) {
    std::vector<TopologyProvenanceOrigin> evidenceOrigins;
    for (const auto* fact : group.facts) {
      if (fact->source) evidenceOrigins.push_back(*fact->source);
    }
    const auto evidenceCount = evidenceOrigins.size();
    TopologyProvenanceKey evidenceKey{
        group.provenance.featureId, group.provenance.kind,
        group.provenance.operation, group.provenance.role,
        group.provenance.localName, std::move(evidenceOrigins),
        group.provenance.occurrence};
    if (evidenceKey.origins.size() != evidenceCount) {
      if (error) *error = "Shape history output has duplicate source facts";
      return false;
    }
    if (evidenceKey.origins != group.provenance.origins) {
      if (error)
        *error = "Shape history output origins do not match source facts";
      return false;
    }

    if (group.provenance.operation == TopologyProvenanceOperation::Merge) {
      if (group.facts.size() < 2 || evidenceKey.origins.size() < 2) {
        if (error) *error = "Merge history requires distinct source facts";
        return false;
      }
    } else if (group.facts.size() != 1) {
      if (error)
        *error = "Non-merge history output has multiple source facts";
      return false;
    }
  }

  if (artifact.coverage == TopologyLineageCoverage::CompleteFaces) {
    for (TopExp_Explorer explorer(*artifact.outputShape, TopAbs_FACE);
         explorer.More(); explorer.Next()) {
      const bool named = std::any_of(
          groups.begin(), groups.end(), [&](const auto& group) {
            return group.provenance.kind == TopologyKind::Face &&
                   group.subshape->IsSame(explorer.Current());
          });
      if (!named) {
        if (error)
          *error = "Complete face history does not name every output face";
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::optional<TopologyProvenanceKey> projectAuthoredTopologyToken(
    const AuthoredTopologyToken& token, FeatureId producerFeatureId,
    std::string* error) {
  if (error) error->clear();
  if (producerFeatureId == kInvalidFeatureId || !validAuthoredToken(token)) {
    if (error) *error = "Authored topology token is invalid";
    return std::nullopt;
  }

  TopologySemanticRole role;
  std::string localName = "sketch:" + std::to_string(token.sketchId) + ":";
  switch (token.kind) {
    case AuthoredTopologySourceKind::ProfileStartCap:
      role = TopologySemanticRole::Cap;
      localName += "profile-start-cap";
      break;
    case AuthoredTopologySourceKind::ProfileEndCap:
      role = TopologySemanticRole::Cap;
      localName += "profile-end-cap";
      break;
    case AuthoredTopologySourceKind::GeometrySide:
      role = TopologySemanticRole::Side;
      localName += "geometry-side:" + std::to_string(token.geometryId);
      break;
    default:
      if (error) *error = "Authored topology token kind is invalid";
      return std::nullopt;
  }
  return TopologyProvenanceKey{
      producerFeatureId, TopologyKind::Face,
      TopologyProvenanceOperation::Seed, role, std::move(localName)};
}

bool validateTopologySourceSnapshot(const TopologySourceSnapshot& snapshot,
                                    std::string* error) {
  if (error) error->clear();
  bool valid = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] { valid = validateTopologySourceSnapshotImpl(snapshot, error); },
      &failure);
  if (!completed)
    setGeometryFailure(failure, error,
                       "Topology source validation raised a geometry exception");
  return completed && valid;
}

bool validateHistoryFact(
    const HistoryFact& fact, FeatureId producerFeatureId,
    std::span<const TopologySourceSnapshot> sources, std::string* error) {
  if (error) error->clear();
  bool valid = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        auto normalized = fact;
        valid = normalizeHistoryFact(normalized, producerFeatureId, error) &&
                validateHistoryFactImpl(normalized, producerFeatureId,
                                        sources, error);
      },
      &failure);
  if (!completed)
    setGeometryFailure(failure, error,
                       "Shape history validation raised a geometry exception");
  return completed && valid;
}

bool validateShapeHistoryArtifact(const ShapeHistoryArtifact& artifact,
                                  std::string* error) {
  if (error) error->clear();
  bool valid = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        auto normalized = artifact;
        valid = normalizeShapeHistoryArtifact(normalized, error) &&
                validateShapeHistoryArtifactImpl(normalized, error);
      },
      &failure);
  if (!completed)
    setGeometryFailure(failure, error,
                       "Shape history validation raised a geometry exception");
  return completed && valid;
}

TopologyLineageBuilder::TopologyLineageBuilder(
    ShapeHistoryArtifact artifact)
    : artifact_(std::move(artifact)) {}

std::optional<TopologyLineageBuilder> TopologyLineageBuilder::create(
    ShapeHistoryArtifact artifact, std::string* error) {
  if (error) error->clear();
  std::optional<TopologyLineageBuilder> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        if (normalizeShapeHistoryArtifact(artifact, error) &&
            validateShapeHistoryArtifactImpl(artifact, error))
          result = TopologyLineageBuilder(std::move(artifact));
      },
      &failure);
  if (!completed)
    setGeometryFailure(failure, error,
                       "Shape history builder creation raised a geometry exception");
  return result;
}

const ShapeHistoryArtifact& TopologyLineageBuilder::artifact() const noexcept {
  return artifact_;
}

std::shared_ptr<const TopologyLineage> TopologyLineageBuilder::build(
    std::string* error) const {
  if (error) error->clear();
  std::shared_ptr<const TopologyLineage> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        auto normalized = artifact_;
        if (!normalizeShapeHistoryArtifact(normalized, error) ||
            !validateShapeHistoryArtifactImpl(normalized, error))
          return;
        std::vector<TopologyLineageBinding> bindings;
        bindings.reserve(normalized.facts.size());
        for (const auto& fact : normalized.facts) {
          if (fact.kind == ShapeHistoryFactKind::Deleted) continue;
          const bool alreadyBound = std::any_of(
              bindings.begin(), bindings.end(), [&](const auto& binding) {
                return binding.provenance == *fact.outputProvenance &&
                       binding.subshape->IsSame(*fact.outputSubshape);
              });
          if (!alreadyBound)
            bindings.push_back(
                {*fact.outputProvenance, fact.outputSubshape});
        }
        if (bindings.empty()) {
          if (error) *error = "Shape history artifact has no live bindings";
          return;
        }
        auto lineage = TopologyLineage::create(
            std::move(bindings), normalized.coverage, error);
        if (!lineage || !lineage->validateForProducer(
                            normalized.producerFeatureId, error))
          return;
        result = std::move(lineage);
      },
      &failure);
  if (!completed)
    setGeometryFailure(failure, error,
                       "Shape history build raised a geometry exception");
  return result;
}

}  // namespace solidar

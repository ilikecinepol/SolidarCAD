#include "model/FaceBoundaryEdgeProvenance.h"

#include <BRep_Tool.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_ShapeMapHasher.hxx>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

#include "model/GeometryOperation.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {
namespace {

constexpr std::size_t kMaximumLocalNameLength = 16384;
using ShapeIndexedMap =
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

void setFailure(FaceBoundaryEdgeProvenanceFailure* failure,
                FaceBoundaryEdgeProvenanceFailureCode code,
                std::string detail) {
  if (!failure) return;
  failure->code = code;
  failure->detail = std::move(detail);
}

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
  if (leftIdentity != rightIdentity) return leftIdentity < rightIdentity;
  return occurrenceTuple(left) < occurrenceTuple(right);
}

void appendLengthPrefixed(std::string& output, std::string_view value) {
  output += std::to_string(value.size());
  output.push_back(':');
  output.append(value);
}

std::string encodedOrigin(const TopologyProvenanceOrigin& origin) {
  std::string result;
  appendLengthPrefixed(result, std::to_string(origin.featureId));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.kind)));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.operation)));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.role)));
  appendLengthPrefixed(result, origin.localName);
  appendLengthPrefixed(result, origin.occurrence ? "1" : "0");
  if (origin.occurrence) {
    appendLengthPrefixed(result,
                         std::to_string(origin.occurrence->featureId));
    appendLengthPrefixed(result, std::to_string(origin.occurrence->slot));
  }
  return result;
}

std::optional<std::string> edgeLocalName(
    const std::array<TopologyProvenanceOrigin, 2>& origins) {
  std::string result = "face-boundary-edge:v1;";
  for (const auto& origin : origins)
    appendLengthPrefixed(result, encodedOrigin(origin));
  if (result.size() > kMaximumLocalNameLength) return std::nullopt;
  return result;
}

std::size_t canonicalIndex(const ShapeIndexedMap& map,
                           const TopoDS_Shape& shape) {
  const int index = map.FindIndex(shape);
  return index > 0 ? static_cast<std::size_t>(index - 1)
                   : std::numeric_limits<std::size_t>::max();
}

struct FaceIdentity {
  TopoDS_Face face;
  TopologyProvenanceKey key;
};

struct BoundaryCandidate {
  TopoDS_Edge edge;
  TopologyProvenanceKey key;
  TopologyProvenanceOrigin origin;
};

std::optional<FaceBoundaryEdgeProvenanceResult> captureImpl(
    const TopologySourceSnapshot& source, const FaceReference& selectedFace,
    FaceBoundaryEdgeProvenanceFailure* failure) {
  const auto fail = [&](FaceBoundaryEdgeProvenanceFailureCode code,
                        std::string detail)
      -> std::optional<FaceBoundaryEdgeProvenanceResult> {
    setFailure(failure, code, std::move(detail));
    return std::nullopt;
  };

  if (source.featureId == kInvalidFeatureId || !source.shape ||
      source.shape->IsNull() || !source.lineage)
    return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidSource,
                "Face-boundary source snapshot is invalid");
  if (source.lineage->coverage() !=
      TopologyLineageCoverage::CompleteFaces)
    return fail(
        FaceBoundaryEdgeProvenanceFailureCode::IncompleteFaceCoverage,
        "Face-boundary source must have CompleteFaces coverage");

  const auto* selectedV2 = selectedFace.v2();
  if (!selectedV2 || !selectedV2->hasValidOwner() ||
      selectedV2->provenance.kind != TopologyKind::Face) {
    return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidSelectedFace,
                "Selected face must be an exact v2 face reference");
  }
  if (selectedV2->provenance.featureId != source.featureId)
    return fail(FaceBoundaryEdgeProvenanceFailureCode::ForeignSelectedFace,
                "Selected face belongs to another source Feature");

  ShapeIndexedMap faceMap;
  TopExp::MapShapes(*source.shape, TopAbs_FACE, faceMap);
  std::vector<FaceIdentity> faces;
  faces.reserve(static_cast<std::size_t>(faceMap.Extent()));
  std::vector<TopologyProvenanceKey> globallySeenFaceKeys;
  for (int faceIndex = 1; faceIndex <= faceMap.Extent(); ++faceIndex) {
    const TopoDS_Face face = TopoDS::Face(faceMap(faceIndex));
    std::vector<const TopologyLineageBinding*> matches;
    for (const auto& binding : source.lineage->bindings()) {
      if (binding.provenance.kind == TopologyKind::Face &&
          binding.subshape && binding.subshape->IsSame(face))
        matches.push_back(&binding);
    }
    if (matches.empty())
      return fail(FaceBoundaryEdgeProvenanceFailureCode::MissingFaceIdentity,
                  "A canonical source face has no provenance key");
    if (matches.size() != 1)
      return fail(
          FaceBoundaryEdgeProvenanceFailureCode::AmbiguousFaceIdentity,
          "A canonical source face has multiple provenance keys");
    if (matches.front()->provenance.featureId != source.featureId)
      return fail(FaceBoundaryEdgeProvenanceFailureCode::ForeignFaceIdentity,
                  "A canonical source face has a foreign producer");
    if (std::find(globallySeenFaceKeys.begin(), globallySeenFaceKeys.end(),
                  matches.front()->provenance) !=
        globallySeenFaceKeys.end())
      return fail(
          FaceBoundaryEdgeProvenanceFailureCode::AmbiguousFaceIdentity,
          "A face provenance key maps to multiple canonical faces");
    globallySeenFaceKeys.push_back(matches.front()->provenance);
    faces.push_back({face, matches.front()->provenance});
  }

  std::string diagnostic;
  if (!source.lineage->validateForProducer(source.featureId, &diagnostic))
    return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidSource,
                diagnostic.empty() ? "Face-boundary lineage is invalid"
                                   : std::move(diagnostic));
  auto index = TopologyIndex::build(source.shape, 1, source.lineage,
                                    &diagnostic);
  if (!index)
    return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidSource,
                diagnostic.empty() ? "Face-boundary source cannot be indexed"
                                   : std::move(diagnostic));
  const auto selectedResolution = index->resolveFace(selectedFace);
  if (!selectedResolution) {
    switch (selectedResolution.failure) {
      case TopologyResolutionFailure::InvalidReference:
        return fail(
            FaceBoundaryEdgeProvenanceFailureCode::InvalidSelectedFace,
            selectedResolution.error);
      case TopologyResolutionFailure::Missing:
        return fail(
            FaceBoundaryEdgeProvenanceFailureCode::MissingSelectedFace,
            selectedResolution.error);
      case TopologyResolutionFailure::Ambiguous:
        return fail(
            FaceBoundaryEdgeProvenanceFailureCode::AmbiguousSelectedFace,
            selectedResolution.error);
      default:
        return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidSource,
                    selectedResolution.error);
    }
  }

  const std::size_t selectedFaceIndex =
      canonicalIndex(faceMap, *selectedResolution.subshape);
  if (selectedFaceIndex >= faces.size())
    return fail(FaceBoundaryEdgeProvenanceFailureCode::MissingSelectedFace,
                "Selected face is not a canonical source face");

  ShapeIndexedMap selectedEdgeMap;
  std::vector<TopoDS_Edge> selectedOrientedEdges;
  std::vector<std::size_t> selectedOccurrenceCounts;
  for (TopExp_Explorer explorer(faces[selectedFaceIndex].face, TopAbs_EDGE);
       explorer.More(); explorer.Next()) {
    const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
    if (edge.IsNull() || BRep_Tool::Degenerated(edge) ||
        (edge.Orientation() != TopAbs_FORWARD &&
         edge.Orientation() != TopAbs_REVERSED))
      return fail(
          FaceBoundaryEdgeProvenanceFailureCode::InvalidBoundaryEdge,
          "Selected face contains a null, degenerate or invalidly oriented edge");
    int mapped = selectedEdgeMap.FindIndex(edge);
    if (mapped == 0) {
      selectedEdgeMap.Add(edge);
      selectedOrientedEdges.push_back(edge);
      selectedOccurrenceCounts.push_back(1);
    } else {
      ++selectedOccurrenceCounts[static_cast<std::size_t>(mapped - 1)];
    }
  }
  if (selectedOrientedEdges.empty())
    return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidBoundaryEdge,
                "Selected face has no boundary edges");
  if (std::any_of(selectedOccurrenceCounts.begin(),
                  selectedOccurrenceCounts.end(),
                  [](std::size_t count) { return count != 1; }))
    return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidBoundaryEdge,
                "Selected face contains a seam or repeated boundary edge");

  std::vector<BoundaryCandidate> candidates;
  candidates.reserve(selectedOrientedEdges.size());
  std::vector<std::array<TopologyProvenanceOrigin, 2>> seenPairs;
  for (const TopoDS_Edge& selectedEdge : selectedOrientedEdges) {
    std::size_t totalOccurrences = 0;
    std::vector<std::size_t> ancestorIndices;
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
      std::size_t faceOccurrences = 0;
      for (TopExp_Explorer explorer(faces[faceIndex].face, TopAbs_EDGE);
           explorer.More(); explorer.Next()) {
        if (!explorer.Current().IsSame(selectedEdge)) continue;
        if (explorer.Current().Orientation() != TopAbs_FORWARD &&
            explorer.Current().Orientation() != TopAbs_REVERSED)
          return fail(
              FaceBoundaryEdgeProvenanceFailureCode::InvalidBoundaryEdge,
              "Boundary edge has an INTERNAL or EXTERNAL face occurrence");
        ++faceOccurrences;
      }
      totalOccurrences += faceOccurrences;
      if (faceOccurrences != 0) ancestorIndices.push_back(faceIndex);
    }
    if (totalOccurrences < 2 || ancestorIndices.size() < 2)
      return fail(FaceBoundaryEdgeProvenanceFailureCode::OpenBoundaryEdge,
                  "Selected boundary edge is open");
    if (totalOccurrences != 2 || ancestorIndices.size() != 2 ||
        std::find(ancestorIndices.begin(), ancestorIndices.end(),
                  selectedFaceIndex) == ancestorIndices.end())
      return fail(
          FaceBoundaryEdgeProvenanceFailureCode::NonManifoldBoundaryEdge,
          "Selected boundary edge is not a two-face manifold edge");

    std::array<TopologyProvenanceOrigin, 2> origins{
        faces[ancestorIndices[0]].key.immediateOrigin(),
        faces[ancestorIndices[1]].key.immediateOrigin()};
    if (originLess(origins[1], origins[0])) std::swap(origins[0], origins[1]);
    if (std::find(seenPairs.begin(), seenPairs.end(), origins) !=
        seenPairs.end())
      return fail(FaceBoundaryEdgeProvenanceFailureCode::RepeatedFacePair,
                  "A canonical face pair maps to multiple boundary edges");
    seenPairs.push_back(origins);

    const auto localName = edgeLocalName(origins);
    if (!localName)
      return fail(FaceBoundaryEdgeProvenanceFailureCode::ResourceLimit,
                  "Derived edge local name exceeds 16384 bytes");
    TopologyProvenanceKey key{
        source.featureId, TopologyKind::Edge,
        TopologyProvenanceOperation::Generated, TopologySemanticRole::Custom,
        *localName, {origins[0], origins[1]}};
    if (!key.isCanonical(&diagnostic))
      return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidSource,
                  diagnostic);
    candidates.push_back(
        {selectedEdge, key, key.immediateOrigin()});
  }

  std::vector<TopologyLineageBinding> augmentedBindings =
      source.lineage->bindings();
  for (const auto& candidate : candidates) {
    std::vector<const TopologyLineageBinding*> onEdge;
    std::vector<const TopologyLineageBinding*> withKey;
    for (const auto& binding : source.lineage->bindings()) {
      if (binding.provenance.kind != TopologyKind::Edge) continue;
      if (binding.subshape && binding.subshape->IsSame(candidate.edge))
        onEdge.push_back(&binding);
      if (binding.provenance == candidate.key) withKey.push_back(&binding);
    }
    if (onEdge.empty() && withKey.empty()) {
      augmentedBindings.push_back(
          {candidate.key, std::make_shared<TopoDS_Edge>(candidate.edge)});
      continue;
    }
    if (onEdge.size() != 1 || withKey.size() != 1 ||
        onEdge.front() != withKey.front() ||
        onEdge.front()->provenance != candidate.key ||
        !onEdge.front()->subshape ||
        !onEdge.front()->subshape->IsSame(candidate.edge))
      return fail(
          FaceBoundaryEdgeProvenanceFailureCode::ConflictingEdgeIdentity,
          "Existing edge provenance conflicts with derived boundary identity");
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
              return left.key.localName < right.key.localName;
            });
  auto augmentedLineage = TopologyLineage::create(
      std::move(augmentedBindings), TopologyLineageCoverage::CompleteFaces,
      &diagnostic);
  if (!augmentedLineage)
    return fail(FaceBoundaryEdgeProvenanceFailureCode::InvalidSource,
                diagnostic.empty() ? "Augmented lineage is invalid"
                                   : std::move(diagnostic));

  FaceBoundaryEdgeProvenanceResult result;
  result.augmentedSource =
      TopologySourceSnapshot{source.featureId, source.shape,
                             std::move(augmentedLineage)};
  result.boundarySeeds.reserve(candidates.size());
  for (auto& candidate : candidates) {
    result.boundarySeeds.push_back(
        {std::make_shared<TopoDS_Edge>(candidate.edge),
         std::move(candidate.origin), std::move(candidate.key)});
  }
  return result;
}

}  // namespace

std::optional<FaceBoundaryEdgeProvenanceResult>
captureUniqueManifoldFaceBoundary(
    const TopologySourceSnapshot& source, const FaceReference& selectedFace,
    FaceBoundaryEdgeProvenanceFailure* failure) {
  if (failure) *failure = {};
  std::optional<FaceBoundaryEdgeProvenanceResult> result;
  GeometryFailure geometryFailure;
  const bool completed = runGeometryOperation(
      [&] { result = captureImpl(source, selectedFace, failure); },
      &geometryFailure);
  if (!completed) {
    std::string detail =
        "Face-boundary provenance capture raised a geometry exception";
    if (!geometryFailure.detail.empty()) detail += ": " + geometryFailure.detail;
    setFailure(failure,
               FaceBoundaryEdgeProvenanceFailureCode::GeometryException,
               std::move(detail));
    return std::nullopt;
  }
  return result;
}

}  // namespace solidar

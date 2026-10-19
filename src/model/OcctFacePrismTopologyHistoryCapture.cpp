#include "model/OcctFacePrismTopologyHistoryCapture.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <NCollection_List.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "model/GeometryOperation.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {
namespace {

constexpr std::size_t kMaximumLocalNameLength = 16384;

void setFailure(FacePrismTopologyHistoryFailure* failure,
                FacePrismTopologyHistoryFailureCode code,
                std::string detail) {
  if (!failure) return;
  failure->code = code;
  failure->detail = std::move(detail);
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
    appendLengthPrefixed(result,
                         std::to_string(origin.occurrence->slot));
  }
  return result;
}

std::optional<std::string> facePrismLocalName(
    std::string_view branch, const TopologyProvenanceOrigin& origin) {
  std::string result = "face-prism:v1;";
  appendLengthPrefixed(result, branch);
  appendLengthPrefixed(result, encodedOrigin(origin));
  if (result.size() > kMaximumLocalNameLength) return std::nullopt;
  return result;
}

bool contains(const TopoDS_Shape& owner, const TopoDS_Shape& candidate,
              TopAbs_ShapeEnum kind) {
  for (TopExp_Explorer explorer(owner, kind); explorer.More();
       explorer.Next()) {
    if (explorer.Current().IsSame(candidate)) return true;
  }
  return false;
}

std::vector<TopoDS_Shape> canonicalSubshapes(const TopoDS_Shape& shape,
                                              TopAbs_ShapeEnum kind) {
  std::vector<TopoDS_Shape> result;
  for (TopExp_Explorer explorer(shape, kind); explorer.More();
       explorer.Next()) {
    const TopoDS_Shape candidate = explorer.Current();
    if (std::none_of(result.begin(), result.end(), [&](const auto& existing) {
          return existing.IsSame(candidate);
        }))
      result.push_back(candidate);
  }
  return result;
}

bool sameOrientedShape(const TopoDS_Shape& left,
                       const TopoDS_Shape& right) {
  return left.IsSame(right) && left.Orientation() == right.Orientation();
}

bool sameLineageBindings(const TopologyLineage& left,
                         const TopologyLineage& right) {
  if (left.coverage() != right.coverage() ||
      left.bindings().size() != right.bindings().size())
    return false;
  std::vector<bool> matched(right.bindings().size(), false);
  for (const auto& leftBinding : left.bindings()) {
    std::optional<std::size_t> match;
    for (std::size_t index = 0; index < right.bindings().size(); ++index) {
      const auto& rightBinding = right.bindings()[index];
      if (matched[index] ||
          leftBinding.provenance != rightBinding.provenance ||
          !leftBinding.subshape || !rightBinding.subshape ||
          !sameOrientedShape(*leftBinding.subshape,
                             *rightBinding.subshape))
        continue;
      if (match) return false;
      match = index;
    }
    if (!match) return false;
    matched[*match] = true;
  }
  return true;
}

bool sameBoundarySeeds(
    std::span<const FaceBoundaryEdgeSeed> supplied,
    std::span<const FaceBoundaryEdgeSeed> canonical) {
  if (supplied.size() != canonical.size()) return false;
  std::vector<bool> matched(canonical.size(), false);
  for (const auto& suppliedSeed : supplied) {
    std::optional<std::size_t> match;
    for (std::size_t index = 0; index < canonical.size(); ++index) {
      const auto& canonicalSeed = canonical[index];
      if (matched[index] ||
          suppliedSeed.provenance != canonicalSeed.provenance ||
          suppliedSeed.origin != canonicalSeed.origin ||
          !suppliedSeed.edge || !canonicalSeed.edge ||
          !sameOrientedShape(*suppliedSeed.edge, *canonicalSeed.edge))
        continue;
      if (match) return false;
      match = index;
    }
    if (!match) return false;
    matched[*match] = true;
  }
  return true;
}

std::optional<TopologyLineageBuilder> captureImpl(
    BRepPrimAPI_MakePrism& maker, FeatureId producerFeatureId,
    const FaceReference& selectedFace,
    const FaceBoundaryEdgeProvenanceResult& boundary,
    FacePrismTopologyHistoryFailure* failure) {
  const auto fail = [&](FacePrismTopologyHistoryFailureCode code,
                        std::string detail)
      -> std::optional<TopologyLineageBuilder> {
    setFailure(failure, code, std::move(detail));
    return std::nullopt;
  };

  if (producerFeatureId == kInvalidFeatureId)
    return fail(FacePrismTopologyHistoryFailureCode::InvalidProducer,
                "Face-prism producer identity is invalid");

  const auto& suppliedSource = boundary.augmentedSource;
  if (suppliedSource.featureId == kInvalidFeatureId ||
      !suppliedSource.shape || suppliedSource.shape->IsNull() ||
      !suppliedSource.lineage)
    return fail(FacePrismTopologyHistoryFailureCode::InvalidSource,
                "Face-prism source snapshot is invalid");
  if (producerFeatureId == suppliedSource.featureId)
    return fail(FacePrismTopologyHistoryFailureCode::ProducerMatchesSource,
                "Face-prism producer must differ from its source");
  if (suppliedSource.lineage->coverage() !=
      TopologyLineageCoverage::CompleteFaces)
    return fail(FacePrismTopologyHistoryFailureCode::IncompleteFaceCoverage,
                "Face-prism source must have CompleteFaces coverage");

  const auto* selectedV2 = selectedFace.v2();
  std::string diagnostic;
  if (!selectedV2 || !selectedV2->hasValidOwner() ||
      selectedV2->provenance.kind != TopologyKind::Face ||
      !selectedV2->provenance.isCanonical(&diagnostic))
    return fail(FacePrismTopologyHistoryFailureCode::InvalidSelectedFace,
                diagnostic.empty()
                    ? "Face-prism selection must be an exact v2 face"
                    : std::move(diagnostic));
  if (selectedV2->provenance.featureId != suppliedSource.featureId)
    return fail(FacePrismTopologyHistoryFailureCode::ForeignSelectedFace,
                "Face-prism selection belongs to another source Feature");

  if (!validateTopologySourceSnapshot(suppliedSource, &diagnostic))
    return fail(FacePrismTopologyHistoryFailureCode::InvalidSource,
                diagnostic.empty() ? "Face-prism source is invalid"
                                   : std::move(diagnostic));

  // FaceBoundaryEdgeProvenanceResult is intentionally a public value type;
  // callers can copy or alter it between the boundary and prism stages.  Do
  // not trust the earlier capture: re-establish exact, producer-owned,
  // one-key-per-canonical-face coverage here.
  const auto sourceFaces =
      canonicalSubshapes(*suppliedSource.shape, TopAbs_FACE);
  if (sourceFaces.empty())
    return fail(FacePrismTopologyHistoryFailureCode::InvalidSource,
                "Face-prism source has no canonical faces");
  std::vector<TopologyProvenanceKey> sourceFaceKeys;
  sourceFaceKeys.reserve(sourceFaces.size());
  for (const auto& face : sourceFaces) {
    std::vector<const TopologyLineageBinding*> matches;
    for (const auto& binding : suppliedSource.lineage->bindings()) {
      if (binding.provenance.kind == TopologyKind::Face &&
          binding.subshape && binding.subshape->IsSame(face))
        matches.push_back(&binding);
    }
    if (matches.size() != 1 ||
        matches.front()->provenance.featureId != suppliedSource.featureId)
      return fail(FacePrismTopologyHistoryFailureCode::InvalidSource,
                  "A canonical source face lacks one exact producer key");
    if (std::find(sourceFaceKeys.begin(), sourceFaceKeys.end(),
                  matches.front()->provenance) != sourceFaceKeys.end())
      return fail(FacePrismTopologyHistoryFailureCode::InvalidSource,
                  "A source face key names multiple canonical faces");
    sourceFaceKeys.push_back(matches.front()->provenance);
  }

  auto index = TopologyIndex::build(suppliedSource.shape, 1,
                                    suppliedSource.lineage, &diagnostic);
  if (!index)
    return fail(FacePrismTopologyHistoryFailureCode::InvalidSource,
                diagnostic.empty() ? "Face-prism source cannot be indexed"
                                   : std::move(diagnostic));
  const auto selectedResolution = index->resolveFace(selectedFace);
  if (!selectedResolution) {
    if (selectedResolution.failure == TopologyResolutionFailure::Missing)
      return fail(FacePrismTopologyHistoryFailureCode::MissingSelectedFace,
                  selectedResolution.error);
    if (selectedResolution.failure == TopologyResolutionFailure::Ambiguous)
      return fail(FacePrismTopologyHistoryFailureCode::AmbiguousSelectedFace,
                  selectedResolution.error);
    return fail(FacePrismTopologyHistoryFailureCode::InvalidSelectedFace,
                selectedResolution.error);
  }
  const TopoDS_Face selected = *selectedResolution.subshape;

  FaceBoundaryEdgeProvenanceFailure boundaryFailure;
  auto canonicalBoundary = captureUniqueManifoldFaceBoundary(
      suppliedSource, selectedFace, &boundaryFailure);
  if (!canonicalBoundary)
    return fail(
        FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence,
        boundaryFailure.detail.empty()
            ? "Face-prism boundary evidence cannot be canonically recaptured"
            : "Face-prism boundary evidence is non-canonical: " +
                  boundaryFailure.detail);
  if (!sameBoundarySeeds(boundary.boundarySeeds,
                         canonicalBoundary->boundarySeeds) ||
      !sameLineageBindings(*suppliedSource.lineage,
                           *canonicalBoundary->augmentedSource.lineage))
    return fail(
        FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence,
        "Supplied face-prism boundary evidence differs from canonical recapture");

  // Only canonical re-derived values are used below.  This keeps the public
  // evidence carrier useful without turning it into a trust boundary.
  const auto& source = suppliedSource;
  const auto& boundarySeeds = canonicalBoundary->boundarySeeds;

  const auto selectedBoundaryEdges = canonicalSubshapes(selected, TopAbs_EDGE);
  if (selectedBoundaryEdges.empty() ||
      selectedBoundaryEdges.size() != boundarySeeds.size())
    return fail(FacePrismTopologyHistoryFailureCode::MissingBoundarySeed,
                "Face-prism boundary seed coverage is incomplete");

  for (std::size_t seedIndex = 0;
       seedIndex < boundarySeeds.size(); ++seedIndex) {
    const auto& seed = boundarySeeds[seedIndex];
    if (!seed.edge || seed.edge->IsNull() ||
        seed.edge->ShapeType() != TopAbs_EDGE ||
        !seed.origin.isValid(&diagnostic) ||
        !seed.provenance.isCanonical(&diagnostic) ||
        seed.provenance.kind != TopologyKind::Edge ||
        seed.provenance.featureId != source.featureId ||
        seed.origin != seed.provenance.immediateOrigin())
      return fail(FacePrismTopologyHistoryFailureCode::InvalidBoundarySeed,
                  diagnostic.empty() ? "Face-prism boundary seed is invalid"
                                     : std::move(diagnostic));
    if (!contains(selected, *seed.edge, TopAbs_EDGE))
      return fail(FacePrismTopologyHistoryFailureCode::ForeignBoundarySeed,
                  "Face-prism boundary seed is not on the selected face");
    for (std::size_t other = 0; other < seedIndex; ++other) {
      if (seed.edge->IsSame(*boundarySeeds[other].edge) ||
          seed.provenance == boundarySeeds[other].provenance)
        return fail(FacePrismTopologyHistoryFailureCode::AmbiguousBoundarySeed,
                    "Face-prism boundary seed identity is duplicated");
    }

    std::size_t edgeMatches = 0;
    std::size_t keyMatches = 0;
    bool sameBinding = false;
    for (const auto& binding : source.lineage->bindings()) {
      if (binding.provenance.kind != TopologyKind::Edge) continue;
      const bool edgeMatch = binding.subshape &&
                             binding.subshape->IsSame(*seed.edge);
      const bool keyMatch = binding.provenance == seed.provenance;
      edgeMatches += edgeMatch ? 1U : 0U;
      keyMatches += keyMatch ? 1U : 0U;
      sameBinding = sameBinding || (edgeMatch && keyMatch);
    }
    if (edgeMatches == 0 || keyMatches == 0)
      return fail(FacePrismTopologyHistoryFailureCode::MissingBoundarySeed,
                  "Face-prism boundary seed is absent from current lineage");
    if (edgeMatches != 1 || keyMatches != 1 || !sameBinding)
      return fail(FacePrismTopologyHistoryFailureCode::AmbiguousBoundarySeed,
                  "Face-prism boundary seed is not bijective");
  }
  for (const auto& edge : selectedBoundaryEdges) {
    if (std::count_if(boundarySeeds.begin(), boundarySeeds.end(),
                      [&](const auto& seed) {
          return seed.edge && seed.edge->IsSame(edge);
        }) != 1)
      return fail(FacePrismTopologyHistoryFailureCode::MissingBoundarySeed,
                  "A selected boundary edge has no unique seed");
  }

  if (!maker.IsDone())
    return fail(FacePrismTopologyHistoryFailureCode::InvalidMaker,
                "Face-prism builder is not done");

  // Copy every maker-owned result while the live history object is in scope.
  const auto outputShape = std::make_shared<TopoDS_Shape>(maker.Shape());
  const TopoDS_Shape first = maker.FirstShape();
  const TopoDS_Shape last = maker.LastShape();
  if (outputShape->IsNull() || !BRepCheck_Analyzer(*outputShape).IsValid())
    return fail(FacePrismTopologyHistoryFailureCode::InvalidOutput,
                "Face-prism output B-Rep is invalid");
  const auto outputSolids = canonicalSubshapes(*outputShape, TopAbs_SOLID);
  if (outputSolids.size() != 1)
    return fail(FacePrismTopologyHistoryFailureCode::InvalidOutput,
                "Face-prism output must contain exactly one solid");
  if (first.IsNull() || last.IsNull() ||
      first.ShapeType() != TopAbs_FACE || last.ShapeType() != TopAbs_FACE ||
      !first.IsSame(selected) || first.IsSame(last) ||
      !contains(*outputShape, first, TopAbs_FACE) ||
      !contains(*outputShape, last, TopAbs_FACE))
    return fail(FacePrismTopologyHistoryFailureCode::InvalidCapHistory,
                "Face-prism cap history is invalid");

  // Only query subshape history after the safe no-argument cap identity has
  // proven that this maker was built from the exact selected face.  OCCT does
  // not define Generated(foreignSubshape) as a safe probe.
  std::vector<std::vector<TopoDS_Shape>> generatedBySeed;
  generatedBySeed.reserve(boundarySeeds.size());
  for (const auto& seed : boundarySeeds) {
    std::vector<TopoDS_Shape> copied;
    const auto& generated = maker.Generated(*seed.edge);
    copied.reserve(static_cast<std::size_t>(generated.Size()));
    for (NCollection_List<TopoDS_Shape>::Iterator iterator(generated);
         iterator.More(); iterator.Next())
      copied.push_back(iterator.Value());
    generatedBySeed.push_back(std::move(copied));
  }

  const TopologyProvenanceOrigin selectedOrigin =
      selectedV2->provenance.immediateOrigin();
  const auto startName = facePrismLocalName("start-cap", selectedOrigin);
  const auto endName = facePrismLocalName("end-cap", selectedOrigin);
  if (!startName || !endName)
    return fail(FacePrismTopologyHistoryFailureCode::ResourceLimit,
                "Face-prism cap identity exceeds 16384 bytes");

  ShapeHistoryArtifact artifact;
  artifact.producerFeatureId = producerFeatureId;
  artifact.outputShape = outputShape;
  artifact.coverage = TopologyLineageCoverage::CompleteFaces;
  artifact.sources.push_back(source);
  std::vector<std::shared_ptr<const TopoDS_Shape>> namedFaces;
  const auto appendFact = [&](const TopoDS_Shape& output,
                              TopologySemanticRole role,
                              const TopologyProvenanceOrigin& origin,
                              const std::string& localName,
                              const TopoDS_Shape& sourceSubshape) -> bool {
    if (std::any_of(namedFaces.begin(), namedFaces.end(), [&](const auto& item) {
          return item->IsSame(output);
        }))
      return false;
    const auto ownedOutput = std::make_shared<TopoDS_Shape>(output);
    namedFaces.push_back(ownedOutput);
    artifact.facts.push_back(
        {ShapeHistoryFactKind::Generated, std::nullopt, origin,
         std::make_shared<TopoDS_Shape>(sourceSubshape),
         TopologyProvenanceKey{producerFeatureId, TopologyKind::Face,
                               TopologyProvenanceOperation::Generated, role,
                               localName, {origin}},
         std::move(ownedOutput)});
    return true;
  };
  if (!appendFact(first, TopologySemanticRole::Cap, selectedOrigin,
                  *startName, selected) ||
      !appendFact(last, TopologySemanticRole::Cap, selectedOrigin,
                  *endName, selected))
    return fail(FacePrismTopologyHistoryFailureCode::IdentityCollision,
                "Face-prism cap output collides with another face");

  for (std::size_t sideIndex = 0;
       sideIndex < boundarySeeds.size(); ++sideIndex) {
    const auto& generated = generatedBySeed[sideIndex];
    if (generated.size() != 1 || generated.front().IsNull() ||
        generated.front().ShapeType() != TopAbs_FACE ||
        !contains(*outputShape, generated.front(), TopAbs_FACE))
      return fail(FacePrismTopologyHistoryFailureCode::InvalidSideHistory,
                  "Face-prism edge history is not exactly one contained face");
    const auto& seed = boundarySeeds[sideIndex];
    const auto sideName = facePrismLocalName("side", seed.origin);
    if (!sideName)
      return fail(FacePrismTopologyHistoryFailureCode::ResourceLimit,
                  "Face-prism side identity exceeds 16384 bytes");
    if (!appendFact(generated.front(), TopologySemanticRole::Side,
                    seed.origin, *sideName, *seed.edge))
      return fail(FacePrismTopologyHistoryFailureCode::IdentityCollision,
                  "Face-prism side output is not globally unique");
  }

  const auto outputFaces = canonicalSubshapes(*outputShape, TopAbs_FACE);
  if (outputFaces.size() != boundarySeeds.size() + 2 ||
      outputFaces.size() != namedFaces.size())
    return fail(FacePrismTopologyHistoryFailureCode::IncompleteOutputCoverage,
                "Face-prism history does not cover the canonical output faces");
  for (const auto& face : outputFaces) {
    if (std::count_if(namedFaces.begin(), namedFaces.end(),
                      [&](const auto& item) { return item->IsSame(face); }) != 1)
      return fail(FacePrismTopologyHistoryFailureCode::IncompleteOutputCoverage,
                  "Face-prism canonical face coverage is not bijective");
  }

  auto builder = TopologyLineageBuilder::create(std::move(artifact),
                                                &diagnostic);
  if (!builder)
    return fail(FacePrismTopologyHistoryFailureCode::HistoryBuildFailure,
                diagnostic.empty() ? "Face-prism history is invalid"
                                   : std::move(diagnostic));
  return builder;
}

}  // namespace

std::optional<TopologyLineageBuilder> captureFacePrismTopologyHistory(
    BRepPrimAPI_MakePrism& maker, FeatureId producerFeatureId,
    const FaceReference& selectedFace,
    const FaceBoundaryEdgeProvenanceResult& boundary,
    FacePrismTopologyHistoryFailure* failure) {
  if (failure) *failure = {};
  std::optional<TopologyLineageBuilder> result;
  GeometryFailure geometryFailure;
  const bool completed = runGeometryOperation(
      [&] {
        result = captureImpl(maker, producerFeatureId, selectedFace, boundary,
                             failure);
      },
      &geometryFailure);
  if (!completed) {
    std::string detail =
        "Face-prism topology capture raised a geometry exception";
    if (!geometryFailure.detail.empty())
      detail += ": " + geometryFailure.detail;
    setFailure(failure,
               FacePrismTopologyHistoryFailureCode::GeometryException,
               std::move(detail));
    return std::nullopt;
  }
  return result;
}

}  // namespace solidar

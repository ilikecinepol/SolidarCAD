#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/TopologyLineageBuilder.h"

class BRepPrimAPI_MakePrism;

namespace solidar {

enum class FacePrismTopologyHistoryFailureCode : std::uint8_t {
  None,
  InvalidProducer,
  ProducerMatchesSource,
  InvalidSource,
  IncompleteFaceCoverage,
  InvalidSelectedFace,
  ForeignSelectedFace,
  MissingSelectedFace,
  AmbiguousSelectedFace,
  InvalidBoundarySeed,
  MissingBoundarySeed,
  AmbiguousBoundarySeed,
  ForeignBoundarySeed,
  NonCanonicalBoundaryEvidence,
  InvalidMaker,
  InvalidOutput,
  InvalidCapHistory,
  InvalidSideHistory,
  IncompleteOutputCoverage,
  ResourceLimit,
  IdentityCollision,
  HistoryBuildFailure,
  GeometryException,
};

struct FacePrismTopologyHistoryFailure {
  FacePrismTopologyHistoryFailureCode code{
      FacePrismTopologyHistoryFailureCode::None};
  std::string detail;
};

// Captures the face history of a live, completed prism synchronously.  The
// maker must have been built with Copy=false from the exact current face
// selected by selectedFace in boundary.augmentedSource. Copy=true is
// deliberately unsupported because its FirstShape is not IsSame the selected
// input and therefore cannot prove exact source identity. Every OCCT result is copied
// before this function returns; no maker-owned list or transient handle
// escapes.  TopologySourceSnapshot deliberately carries no BodyId, so proving
// that selectedFace.bodyId() owns boundary.augmentedSource is a caller
// precondition.  The function still requires a valid BodyId and exact v2
// producer identity.
//
// On success the returned builder owns one immutable output root, its source
// snapshot, and CompleteFaces generated history for the two caps and every
// boundary side.  On failure no partial artifact is published.
[[nodiscard]] std::optional<TopologyLineageBuilder>
captureFacePrismTopologyHistory(
    BRepPrimAPI_MakePrism& maker, FeatureId producerFeatureId,
    const FaceReference& selectedFace,
    const FaceBoundaryEdgeProvenanceResult& boundary,
    FacePrismTopologyHistoryFailure* failure = nullptr);

}  // namespace solidar

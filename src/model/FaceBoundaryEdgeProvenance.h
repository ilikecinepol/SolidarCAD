#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/FaceReference.h"
#include "model/TopologyLineageBuilder.h"

class TopoDS_Edge;

namespace solidar {

// Failure taxonomy for the strict UniqueManifoldFaceBoundary contract.  The
// capture is topology-only: it either returns a complete augmented snapshot
// and all boundary seeds, or leaves the caller with no partial result.
enum class FaceBoundaryEdgeProvenanceFailureCode : std::uint8_t {
  None,
  InvalidSource,
  IncompleteFaceCoverage,
  InvalidSelectedFace,
  MissingSelectedFace,
  AmbiguousSelectedFace,
  ForeignSelectedFace,
  MissingFaceIdentity,
  AmbiguousFaceIdentity,
  ForeignFaceIdentity,
  InvalidBoundaryEdge,
  OpenBoundaryEdge,
  NonManifoldBoundaryEdge,
  RepeatedFacePair,
  ConflictingEdgeIdentity,
  ResourceLimit,
  GeometryException,
};

struct FaceBoundaryEdgeProvenanceFailure {
  FaceBoundaryEdgeProvenanceFailureCode code{
      FaceBoundaryEdgeProvenanceFailureCode::None};
  std::string detail;
};

// The edge keeps the orientation of its single occurrence on the selected
// face.  provenance is the persistent Edge key inserted into the augmented
// lineage; origin is the exact immediate projection consumed by later stages.
struct FaceBoundaryEdgeSeed {
  std::shared_ptr<const TopoDS_Edge> edge;
  TopologyProvenanceOrigin origin;
  TopologyProvenanceKey provenance;
};

struct FaceBoundaryEdgeProvenanceResult {
  TopologySourceSnapshot augmentedSource;
  std::vector<FaceBoundaryEdgeSeed> boundarySeeds;
};

// Publication-side augmentation used only by ExtrudeFeature's simple NewBody
// sketch path. Given a coherent CompleteFaces result, every uniquely
// identified closed two-face manifold edge receives the same
// face-boundary-edge:v1 identity used by the selected-face capture above.
// Unsupported edges (open, seam, non-manifold, degenerate, or sharing a face
// pair) are deliberately omitted. Invalid source evidence and conflicts with
// existing edge evidence fail atomically.
struct CompleteFaceEdgeLineageAugmentationResult {
  TopologySourceSnapshot augmentedSource;
  std::size_t addedBindingCount{};
};

[[nodiscard]] std::optional<CompleteFaceEdgeLineageAugmentationResult>
augmentCompleteFaceLineageWithUniqueManifoldEdges(
    const TopologySourceSnapshot& source,
    FaceBoundaryEdgeProvenanceFailure* failure = nullptr);

// Derives persistent Edge provenance for every edge of one exact v2-selected
// face.  The source must be CompleteFaces and every canonical source face must
// have exactly one producer-owned key.  Only closed two-face manifold edges
// are accepted.  Existing exact edge bindings are reused; the input snapshot
// and lineage are never mutated. TopologySourceSnapshot intentionally has no
// BodyId; validating that selectedFace.bodyId() belongs to this snapshot is a
// caller precondition. This function still rejects invalid BodyIds and foreign
// producer FeatureIds.
[[nodiscard]] std::optional<FaceBoundaryEdgeProvenanceResult>
captureUniqueManifoldFaceBoundary(
    const TopologySourceSnapshot& source,
    const FaceReference& selectedFace,
    FaceBoundaryEdgeProvenanceFailure* failure = nullptr);

}  // namespace solidar

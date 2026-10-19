#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "model/TopologyLineageBuilder.h"

class BRepAlgoAPI_Cut;
class BRepAlgoAPI_Fuse;
class TopoDS_Face;

namespace solidar {

enum class BooleanOperandRole : std::uint8_t {
  Object,
  Tool,
};

struct BooleanTopologySource {
  BooleanOperandRole role{BooleanOperandRole::Object};
  TopologySourceSnapshot snapshot;
};

// A stable authored discriminator for an otherwise ambiguous output branch.
// outputFace is used only to validate the seed against live OCCT history; the
// persistent identity is stableBranchId plus the exact canonical origins.
struct BooleanFaceBranchSeed {
  std::vector<TopologyProvenanceOrigin> origins;
  std::string stableBranchId;
  std::shared_ptr<const TopoDS_Face> outputFace;
};

struct BooleanTopologyCaptureOptions {
  bool simplifyResult{false};
  bool unifyEdges{true};
  bool unifyFaces{true};
};

enum class BooleanTopologyCaptureFailureCode : std::uint8_t {
  None,
  InvalidProducer,
  InvalidSource,
  DuplicateSourceFeature,
  SourceMismatch,
  BuilderNotDone,
  HistoryUnavailable,
  InvalidOutput,
  MissingSourceIdentity,
  DuplicateSourceIdentity,
  InvalidHistory,
  MissingHistory,
  UncoveredOutput,
  SplitSeedRequired,
  InvalidSplitSeed,
  SplitSeedCollision,
  LocalNameTooLong,
  ResourceLimit,
  GeometryException,
};

struct BooleanTopologyCaptureFailure {
  BooleanTopologyCaptureFailureCode code{
      BooleanTopologyCaptureFailureCode::None};
  std::string detail;
};

struct BooleanTopologyCaptureResult {
  TopologyLineageBuilder builder;
  std::shared_ptr<const TopoDS_Shape> outputShape;
};

// Captures Fuse/Cut face history synchronously. The result owns every shape
// copied from the maker and retains no maker-owned list or scratch reference.
[[nodiscard]] std::optional<BooleanTopologyCaptureResult>
captureBooleanFaceHistory(
    BRepAlgoAPI_Fuse& maker, FeatureId producerFeatureId,
    std::span<const BooleanTopologySource> sources,
    std::span<const BooleanFaceBranchSeed> splitSeeds = {},
    BooleanTopologyCaptureOptions options = {},
    BooleanTopologyCaptureFailure* failure = nullptr);

[[nodiscard]] std::optional<BooleanTopologyCaptureResult>
captureBooleanFaceHistory(
    BRepAlgoAPI_Cut& maker, FeatureId producerFeatureId,
    std::span<const BooleanTopologySource> sources,
    std::span<const BooleanFaceBranchSeed> splitSeeds = {},
    BooleanTopologyCaptureOptions options = {},
    BooleanTopologyCaptureFailure* failure = nullptr);

}  // namespace solidar

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "model/TopologyLineageBuilder.h"

class BRepPrimAPI_MakeRevol;
class TopoDS_Edge;
class TopoDS_Face;

namespace solidar {

struct RevolveProfileEdgeSeed {
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  std::shared_ptr<const TopoDS_Edge> edge;
};

enum class RevolveTopologyHistoryFailureCode : std::uint8_t {
  None,
  InvalidProducer,
  InvalidSketch,
  InvalidProfile,
  InvalidProfileSeed,
  MissingProfileSeed,
  AmbiguousProfileSeed,
  ForeignProfileSeed,
  InvalidMaker,
  MakerInputMismatch,
  InvalidOutput,
  InvalidCapHistory,
  InvalidSideHistory,
  IncompleteOutputCoverage,
  ResourceLimit,
  IdentityCollision,
  HistoryBuildFailure,
  GeometryException,
};

struct RevolveTopologyHistoryFailure {
  RevolveTopologyHistoryFailureCode code{
      RevolveTopologyHistoryFailureCode::None};
  std::string detail;
};

// Captures the face history of a live, completed BRepPrimAPI_MakeRevol.
// The maker must have been built with Copy=false from the exact profileFace.
// Every maker-owned shape/list entry is copied synchronously; no transient
// history handle escapes. A partial revolution is recognized only by two
// distinct contained cap faces. A full revolution is recognized only when
// neither reported cap is contained and generated sides cover every output
// face. Classification therefore never depends on a caller-supplied angle.
//
// Each canonical profile edge must have exactly one GeometryId seed. An edge
// on the revolution axis may generate no face; every other accepted history
// result is exactly one contained output face. On success, the returned
// builder owns a valid one-solid output and CompleteFaces authored-root
// lineage. On failure no partial artifact is published.
[[nodiscard]] std::optional<TopologyLineageBuilder>
captureRevolveTopologyHistory(
    BRepPrimAPI_MakeRevol& maker, FeatureId producerFeatureId,
    SketchId sketchId, std::shared_ptr<const TopoDS_Face> profileFace,
    std::span<const RevolveProfileEdgeSeed> profileEdges,
    RevolveTopologyHistoryFailure* failure = nullptr);

}  // namespace solidar

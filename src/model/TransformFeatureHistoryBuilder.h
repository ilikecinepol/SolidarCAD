#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <gp_Trsf.hxx>

#include "model/TopologyLineageBuilder.h"

namespace solidar {

enum class TransformFeatureInstanceRelation : std::uint8_t {
  RetainedSource,
  ModifiedCopy,
};

// One authored transform branch. RetainedSource ignores transform and emits
// the exact source shape; ModifiedCopy is built with a live OCCT
// BRepBuilderAPI_Transform using copyGeom=true.
struct TransformFeatureInstanceRequest {
  TransformFeatureInstanceRelation relation{
      TransformFeatureInstanceRelation::ModifiedCopy};
  gp_Trsf transform;
  TopologySemanticRole role{TopologySemanticRole::Custom};
  std::optional<TopologyOccurrenceKey> occurrence;
};

struct TransformFeatureHistoryBuild {
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const TopologyLineage> lineage;
};

// Injective, length-safe encoding of the complete immediate source origin.
// Instance identity remains typed in role/occurrence and is not embedded in
// this source-derived name.
[[nodiscard]] std::optional<std::string> transformDerivedLocalName(
    const TopologyProvenanceOrigin& source,
    std::string* error = nullptr);

[[nodiscard]] std::optional<TopologyProvenanceKey>
deriveTransformFaceProvenance(
    const TopologyProvenanceOrigin& source, FeatureId producerFeatureId,
    TopologyProvenanceOperation operation, TopologySemanticRole role,
    std::optional<TopologyOccurrenceKey> occurrence = {},
    std::string* error = nullptr);

// Aggregates exactly one coherent CompleteFaces source snapshot into either a
// direct one-instance output or a compound of retained/copied instances. The
// result is assigned only after RT0 artifact creation and lineage build pass.
[[nodiscard]] bool buildTransformFeatureHistory(
    TopologySourceSnapshot source, FeatureId producerFeatureId,
    std::span<const TransformFeatureInstanceRequest> instances,
    bool directSingleOutput, TransformFeatureHistoryBuild* result,
    std::string* error = nullptr);

}  // namespace solidar

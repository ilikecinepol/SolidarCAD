#pragma once

#include <cstddef>
#include <optional>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

// Native UI authoring prefers operation provenance. Only an exact absence of
// provenance permits the legacy geometric carrier; malformed or ambiguous
// provenance must fail closed.
[[nodiscard]] std::optional<FaceReference> createViewportFaceReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t rawFaceIndex);
[[nodiscard]] std::optional<EdgeReference> createViewportEdgeReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t rawEdgeIndex);

}  // namespace solidar

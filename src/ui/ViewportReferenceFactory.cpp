#include "ui/ViewportReferenceFactory.h"

namespace solidar {

std::optional<FaceReference> createViewportFaceReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t rawFaceIndex) {
  const auto createdV2 =
      index.createFaceReferenceV2(bodyId, featureId, rawFaceIndex);
  if (createdV2.status == FaceReferenceV2CreationStatus::Created)
    return createdV2.reference;
  if (createdV2.status != FaceReferenceV2CreationStatus::Missing)
    return std::nullopt;
  const auto createdV1 =
      index.createFaceReference(bodyId, featureId, rawFaceIndex);
  return createdV1 ? std::optional<FaceReference>{createdV1.reference}
                   : std::nullopt;
}

std::optional<EdgeReference> createViewportEdgeReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t rawEdgeIndex) {
  const auto createdV2 =
      index.createEdgeReferenceV2(bodyId, featureId, rawEdgeIndex);
  if (createdV2.status == EdgeReferenceV2CreationStatus::Created)
    return createdV2.reference;
  if (createdV2.status != EdgeReferenceV2CreationStatus::Missing)
    return std::nullopt;
  const auto createdV1 =
      index.createEdgeReference(bodyId, featureId, rawEdgeIndex);
  return createdV1 ? std::optional<EdgeReference>{createdV1.reference}
                   : std::nullopt;
}

}  // namespace solidar

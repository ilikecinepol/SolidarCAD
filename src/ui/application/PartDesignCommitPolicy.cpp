#include "ui/application/PartDesignCommitPolicy.h"

#include <algorithm>

namespace solidar {

bool containsV2TopologyReference(
    const PartDesignToolSnapshot& snapshot) noexcept {
  if (snapshot.face.isV2() ||
      std::any_of(snapshot.edges.begin(), snapshot.edges.end(),
                  [](const EdgeReference& edge) { return edge.isV2(); }) ||
      std::any_of(snapshot.faces.begin(), snapshot.faces.end(),
                  [](const FaceReference& face) { return face.isV2(); }))
    return true;
  if (snapshot.mirrorPlaneFace && snapshot.mirrorPlaneFace->isV2())
    return true;
  if (snapshot.neutralPlane && snapshot.neutralPlane->face &&
      snapshot.neutralPlane->face->isV2())
    return true;
  return snapshot.rotationEdge && snapshot.rotationEdge->isV2();
}

}  // namespace solidar

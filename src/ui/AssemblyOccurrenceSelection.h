#pragma once

#include <utility>

#include "presentation/AssemblyScenePublicationCoordinator.h"

namespace solidar {

// An occurrence identity is only meaningful inside the exact immutable
// publication that produced it. Numeric epochs/generations are deliberately
// insufficient because separate coordinators may reuse those values.
struct AssemblyOccurrenceSelection final {
  AssemblyScenePublicationHandle publication;
  AssemblyOccurrenceIdentity occurrence;

  [[nodiscard]] bool isCurrentFor(
      const AssemblyScenePublicationHandle& activePublication) const noexcept {
    return publication.isSameCurrentPublication(activePublication);
  }

  friend bool operator==(const AssemblyOccurrenceSelection& left,
                         const AssemblyOccurrenceSelection& right) noexcept {
    return left.occurrence == right.occurrence &&
           left.publication.isSameCurrentPublication(right.publication);
  }
};

}  // namespace solidar

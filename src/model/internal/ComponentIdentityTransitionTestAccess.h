#pragma once

#include "model/ProductDocument.h"

namespace solidar {

// Narrow characterization seam for proving every scalar in the opaque seal
// is validated. Production code cannot manufacture or edit a transition.
class ComponentIdentityTransitionTestAccess final {
 public:
  [[nodiscard]] static std::uint64_t validationCount() noexcept {
    return ComponentIdentityTransition::validationCountForTesting();
  }

  static void resetValidationCount() noexcept {
    ComponentIdentityTransition::resetValidationCountForTesting();
  }

  static void replaceSourceIncarnation(
      ComponentIdentityTransition& transition,
      ProductIncarnationId incarnation) noexcept {
    transition.sourceIncarnation_ = incarnation;
  }

  static void replaceSourceRoot(ComponentIdentityTransition& transition,
                                DefinitionId root) noexcept {
    transition.sourceRoot_ = root;
  }

  static void replaceFirstDefinitionState(
      ComponentIdentityTransition& transition,
      DefinitionStateId state) noexcept {
    if (!transition.definitionStates_.empty())
      transition.definitionStates_.front().second = state;
  }
};

}  // namespace solidar

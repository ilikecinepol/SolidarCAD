#pragma once

#include "presentation/ComponentPresentation.h"

namespace solidar {

class ProductDocument;

// Pure synchronous projection of retained component model state.  It never
// recomputes Documents or creates transient topology indexes.
class ComponentPresentationBuilder final {
 public:
  [[nodiscard]] static ComponentTreeProjectionResult buildTreeProjection(
      const ProductDocument& product) noexcept;
  [[nodiscard]] static ComponentTreeProjectionResult buildTreeProjection(
      const ProductDocument& product,
      const ComponentIdentityTransition& transition) noexcept;
  [[nodiscard]] static ComponentPresentationBuildResult build(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile) noexcept;
  [[nodiscard]] static ComponentPresentationBuildResult build(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentIdentityTransition& transition) noexcept;

 private:
  [[nodiscard]] static ComponentTreeProjectionResult buildTreeProjectionImpl(
      const ProductDocument& product,
      const ComponentIdentityTransition* transition) noexcept;
  [[nodiscard]] static ComponentPresentationBuildResult buildImpl(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentIdentityTransition* transition) noexcept;
};

}  // namespace solidar

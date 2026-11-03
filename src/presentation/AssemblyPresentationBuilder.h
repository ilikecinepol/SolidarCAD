#pragma once

#include "presentation/AssemblyPresentation.h"

namespace solidar {

class ProductDocument;

// Pure synchronous builder.  It reads retained model presentation state and
// either returns one complete immutable snapshot or no snapshot at all.
class AssemblyPresentationBuilder final {
 public:
  [[nodiscard]] static AssemblyPresentationBuildResult build(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile) noexcept;
};

}  // namespace solidar

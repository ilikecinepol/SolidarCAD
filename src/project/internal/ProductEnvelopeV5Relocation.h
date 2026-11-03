#pragma once

#include "project/ProductEnvelopeV5Relocation.h"
#include "project/internal/ProjectFileRelocation.h"

namespace solidar::project::detail {

// Deterministic transaction seam. Production callers use the path overload.
[[nodiscard]] ProductEnvelopeV5RelocationResult relocateProductEnvelopeV5(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& productDocument,
    ProjectFileRelocationTransaction& transaction) noexcept;

}  // namespace solidar::project::detail

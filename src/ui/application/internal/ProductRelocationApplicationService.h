#pragma once

#include "project/internal/ProductEnvelopeV5Relocation.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar::application::detail {

// Deterministic filesystem seam for application-level relocation tests.
[[nodiscard]] ProjectApplicationStatus relocateProduct(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& source,
    const ProductRelocationPrecondition& precondition,
    project::detail::ProjectFileRelocationTransaction& transaction) noexcept;

}  // namespace solidar::application::detail

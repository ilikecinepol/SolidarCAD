#pragma once

#include "project/internal/ProjectAtomicWriting.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar::application::detail {

// Deterministic transaction seam for the headless conversion orchestration.
// Normal application callers use ProjectApplicationService's path overload.
[[nodiscard]] ProjectApplicationStatus convertStandaloneToProduct(
    ProductDocument& source,
    const StandaloneProductConversionRequest& request,
    const StandaloneProductConversionPrecondition& precondition,
    project::detail::ProjectAtomicWriteTransaction& transaction) noexcept;

}  // namespace solidar::application::detail

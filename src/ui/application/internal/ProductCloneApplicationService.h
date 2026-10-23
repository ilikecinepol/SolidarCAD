#pragma once

#include "project/internal/ProjectAtomicWriting.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar::application::detail {

// Deterministic transaction seam for identified product cloning. Production
// callers use ProjectApplicationService's path overload.
[[nodiscard]] ProjectApplicationStatus saveProductAsClone(
    ProductDocument& source, const ProductCloneRequest& request,
    project::detail::ProjectAtomicWriteTransaction& transaction) noexcept;

}  // namespace solidar::application::detail

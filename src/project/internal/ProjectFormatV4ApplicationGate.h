#pragma once

#include "model/Document.h"

namespace solidar::project::detail {

// Pure policy seam for the public v4 load boundary. Runtime topology misses
// are recoverable authored state; every other recompute failure rejects the
// staged document before it can reach application ownership.
[[nodiscard]] bool acceptsProjectFormatV4RecomputeResult(
    const RecomputeResult& result) noexcept;

}  // namespace solidar::project::detail

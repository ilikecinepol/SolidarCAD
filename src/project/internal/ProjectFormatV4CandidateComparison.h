#pragma once

#include "project/ProjectFormatV4ModelValidator.h"

namespace solidar::project::detail {

[[nodiscard]] bool sameProjectFormatV4Candidate(
    const ProjectFormatV4ModelCandidate& left,
    const ProjectFormatV4ModelCandidate& right);

}  // namespace solidar::project::detail

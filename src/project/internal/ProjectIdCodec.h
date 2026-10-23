#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "model/ProjectId.h"

namespace solidar::project::detail {

// Strict persisted representation: lowercase RFC-4122-style hex with
// hyphens, and with the all-zero sentinel excluded.  This codec deliberately
// has no Qt dependency so every persistence boundary uses the same rules.
[[nodiscard]] std::optional<ProjectId> parseCanonicalProjectId(
    std::string_view text) noexcept;

[[nodiscard]] std::optional<std::string> formatCanonicalProjectId(
    ProjectId id);

}  // namespace solidar::project::detail

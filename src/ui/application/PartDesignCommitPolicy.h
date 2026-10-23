#pragma once

#include "ui/application/PartDesignCoordinator.h"

namespace solidar {

// Pure inspection used by the application persistence gate. The coordinator
// and its sessions remain deliberately unaware of the selected file format.
[[nodiscard]] bool containsV2TopologyReference(
    const PartDesignToolSnapshot& snapshot) noexcept;

}  // namespace solidar

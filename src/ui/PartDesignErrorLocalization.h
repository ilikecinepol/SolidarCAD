#pragma once

#include <QString>

#include "model/OperationFailure.h"
#include "model/PartDesignToolFramework.h"

namespace solidar {

[[nodiscard]] bool requiresPartDesignReselection(
    OperationFailureCode code) noexcept;
[[nodiscard]] QString localizedFaceToolError(
    const OperationFailure& failure);
[[nodiscard]] QString localizedPartDesignError(
    PartDesignToolKind kind, const OperationFailure& failure);

}  // namespace solidar

#pragma once

#include <cstddef>
#include <memory>

#include <QString>

#include "project/LinkedPartSourceResolver.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar::application::detail {

// Deterministic boundary seam for the application-owned linked diagnostic
// policy. It performs no filesystem work; production supplies the same status
// from the native read-only batch resolver.
[[nodiscard]] StagedApplicationDocumentResult
stageAssemblyOpenWithResolutionForTesting(
    std::unique_ptr<ProductDocument> product,
    project::ProjectPersistenceFormat format,
    project::LinkedPartSourceResolveStatus status,
    std::size_t failureIndex = 0, QString detail = {});

[[nodiscard]] ProjectApplicationStatus
preflightFullProductExportWithResolutionForTesting(
    const ProductDocument& product,
    project::LinkedPartSourceResolveStatus status,
    std::size_t failureIndex = 0, QString detail = {});

}  // namespace solidar::application::detail

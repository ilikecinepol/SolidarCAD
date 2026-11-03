#pragma once

#include <QString>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/internal/ProjectAtomicWriting.h"

namespace solidar::project::detail {

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV7Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction);

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV7Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project::detail

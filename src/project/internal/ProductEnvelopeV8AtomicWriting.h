#pragma once

#include <QString>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/internal/ProjectAtomicWriting.h"

namespace solidar::project::detail {

enum class ProductEnvelopeV8AtomicWritePreparationFailurePoint {
  None,
  Encode,
  Scan,
  Materialize,
};

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV8Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction,
    ProductEnvelopeV8AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV8AtomicWritePreparationFailurePoint::None);

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV8Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata,
    ProductEnvelopeV8AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV8AtomicWritePreparationFailurePoint::None);

}  // namespace solidar::project::detail

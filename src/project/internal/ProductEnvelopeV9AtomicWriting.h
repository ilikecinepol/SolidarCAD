#pragma once

#include <QString>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/internal/ProjectAtomicWriting.h"

namespace solidar::project::detail {

enum class ProductEnvelopeV9AtomicWritePreparationFailurePoint {
  None,
  EncodeInitial,
  ScanInitial,
  MaterializeInitial,
  EncodeFixedPoint,
  FixedPointMismatch,
  ScanFixedPoint,
  MaterializeFixedPoint,
  EncodeStable,
  StableMismatch,
};

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV9Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction,
    ProductEnvelopeV9AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV9AtomicWritePreparationFailurePoint::None);

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV9Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata,
    ProductEnvelopeV9AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV9AtomicWritePreparationFailurePoint::None);

}  // namespace solidar::project::detail

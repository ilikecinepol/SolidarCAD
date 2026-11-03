#pragma once

#include <QString>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/internal/ProjectAtomicWriting.h"

namespace solidar::project::detail {

enum class ProductEnvelopeV12AtomicWritePreparationFailurePoint {
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

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV12Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction,
    ProductEnvelopeV12AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV12AtomicWritePreparationFailurePoint::None);

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV12Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata,
    ProductEnvelopeV12AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV12AtomicWritePreparationFailurePoint::None);

}  // namespace solidar::project::detail

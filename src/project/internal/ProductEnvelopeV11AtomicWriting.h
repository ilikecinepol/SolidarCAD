#pragma once

#include <QString>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/internal/ProjectAtomicWriting.h"

namespace solidar::project::detail {

enum class ProductEnvelopeV11AtomicWritePreparationFailurePoint {
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

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV11Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction,
    ProductEnvelopeV11AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV11AtomicWritePreparationFailurePoint::None);

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV11Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata,
    ProductEnvelopeV11AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV11AtomicWritePreparationFailurePoint::None);

}  // namespace solidar::project::detail

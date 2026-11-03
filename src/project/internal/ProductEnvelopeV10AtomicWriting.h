#pragma once

#include <QString>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/internal/ProjectAtomicWriting.h"

namespace solidar::project::detail {

enum class ProductEnvelopeV10AtomicWritePreparationFailurePoint {
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

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV10Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction,
    ProductEnvelopeV10AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV10AtomicWritePreparationFailurePoint::None);

[[nodiscard]] ProjectAtomicWriteResult writeProductEnvelopeV10Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata,
    ProductEnvelopeV10AtomicWritePreparationFailurePoint failurePoint =
        ProductEnvelopeV10AtomicWritePreparationFailurePoint::None);

}  // namespace solidar::project::detail

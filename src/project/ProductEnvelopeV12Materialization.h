#pragma once

#include <QString>

#include <memory>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV12Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV12MaterializationStatus {
  Materialized,
  Invalid,
};

enum class ProductEnvelopeV12MaterializationMode {
  Publication,
  ValidationOnly,
};

enum class ProductEnvelopeV12MaterializationFailurePoint {
  None,
  BeforeLastDocument,
  BeforeProductConstruction,
  Finalization,
};

struct ProductEnvelopeV12MaterializationResult final {
  ProductEnvelopeV12MaterializationStatus status{
      ProductEnvelopeV12MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV12MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

[[nodiscard]] ProductEnvelopeV12MaterializationResult
materializeProductEnvelopeV12(
    const ProductEnvelopeV12Candidate& candidate,
    ProductEnvelopeV12MaterializationFailurePoint failurePoint =
        ProductEnvelopeV12MaterializationFailurePoint::None,
    ProductEnvelopeV12MaterializationMode mode =
        ProductEnvelopeV12MaterializationMode::Publication);

}  // namespace solidar::project

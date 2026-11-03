#pragma once

#include <QString>

#include <memory>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV11Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV11MaterializationStatus {
  Materialized,
  Invalid,
};

enum class ProductEnvelopeV11MaterializationMode {
  Publication,
  ValidationOnly,
};

enum class ProductEnvelopeV11MaterializationFailurePoint {
  None,
  BeforeLastDocument,
  BeforeProductConstruction,
  Finalization,
};

struct ProductEnvelopeV11MaterializationResult final {
  ProductEnvelopeV11MaterializationStatus status{
      ProductEnvelopeV11MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV11MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

[[nodiscard]] ProductEnvelopeV11MaterializationResult
materializeProductEnvelopeV11(
    const ProductEnvelopeV11Candidate& candidate,
    ProductEnvelopeV11MaterializationFailurePoint failurePoint =
        ProductEnvelopeV11MaterializationFailurePoint::None,
    ProductEnvelopeV11MaterializationMode mode =
        ProductEnvelopeV11MaterializationMode::Publication);

}  // namespace solidar::project

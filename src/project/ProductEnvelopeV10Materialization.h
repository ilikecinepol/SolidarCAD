#pragma once

#include <QString>

#include <memory>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV10Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV10MaterializationStatus {
  Materialized,
  Invalid,
};

enum class ProductEnvelopeV10MaterializationMode {
  Publication,
  ValidationOnly,
};

enum class ProductEnvelopeV10MaterializationFailurePoint {
  None,
  BeforeLastDocument,
  BeforeProductConstruction,
  Finalization,
};

struct ProductEnvelopeV10MaterializationResult final {
  ProductEnvelopeV10MaterializationStatus status{
      ProductEnvelopeV10MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV10MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

[[nodiscard]] ProductEnvelopeV10MaterializationResult
materializeProductEnvelopeV10(
    const ProductEnvelopeV10Candidate& candidate,
    ProductEnvelopeV10MaterializationFailurePoint failurePoint =
        ProductEnvelopeV10MaterializationFailurePoint::None,
    ProductEnvelopeV10MaterializationMode mode =
        ProductEnvelopeV10MaterializationMode::Publication);

}  // namespace solidar::project

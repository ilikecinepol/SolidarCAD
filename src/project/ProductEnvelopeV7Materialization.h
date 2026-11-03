#pragma once

#include <QString>

#include <memory>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV7Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV7MaterializationStatus {
  Materialized,
  Unsupported,
  Invalid,
};

// Deterministic test seam for the staged, pre-publication path.
enum class ProductEnvelopeV7MaterializationFailurePoint {
  None,
  SecondPartRecompute,
};

struct ProductEnvelopeV7MaterializationResult {
  ProductEnvelopeV7MaterializationStatus status{
      ProductEnvelopeV7MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV7MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

[[nodiscard]] ProductEnvelopeV7MaterializationResult
materializeProductEnvelopeV7(
    const ProductEnvelopeV7Candidate& candidate,
    ProductEnvelopeV7MaterializationFailurePoint failurePoint =
        ProductEnvelopeV7MaterializationFailurePoint::None);

}  // namespace solidar::project

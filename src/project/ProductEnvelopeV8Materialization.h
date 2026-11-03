#pragma once

#include <QString>

#include <memory>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV8Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV8MaterializationStatus {
  Materialized,
  Invalid,
};

enum class ProductEnvelopeV8MaterializationFailurePoint {
  None,
  DeepestPartRecompute,
  BeforeProductConstruction,
};

struct ProductEnvelopeV8MaterializationResult final {
  ProductEnvelopeV8MaterializationStatus status{
      ProductEnvelopeV8MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  QString error;
  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV8MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

[[nodiscard]] ProductEnvelopeV8MaterializationResult
materializeProductEnvelopeV8(
    const ProductEnvelopeV8Candidate& candidate,
    ProductEnvelopeV8MaterializationFailurePoint failurePoint =
        ProductEnvelopeV8MaterializationFailurePoint::None);

}  // namespace solidar::project

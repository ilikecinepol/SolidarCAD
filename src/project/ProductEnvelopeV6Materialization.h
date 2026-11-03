#pragma once

#include <QString>

#include <memory>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV6Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV6MaterializationStatus {
  Materialized,
  Unsupported,
  Invalid,
};

struct ProductEnvelopeV6MaterializationResult {
  ProductEnvelopeV6MaterializationStatus status{
      ProductEnvelopeV6MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV6MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

[[nodiscard]] ProductEnvelopeV6MaterializationResult
materializeProductEnvelopeV6(const ProductEnvelopeV6Candidate& candidate);

}  // namespace solidar::project

#pragma once

#include <QString>

#include <memory>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV5MaterializationStatus {
  Materialized,
  Unsupported,
  Invalid,
};

struct ProductEnvelopeV5MaterializationResult {
  ProductEnvelopeV5MaterializationStatus status{
      ProductEnvelopeV5MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV5MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

// Disconnected construction boundary for a fully validated v5 Candidate.
// It defensively rebuilds the carrier from authoredRoot, materializes and
// recomputes the embedded v4 part without reserving process-global edit IDs,
// and publishes no application or persistence state.
[[nodiscard]] ProductEnvelopeV5MaterializationResult
materializeProductEnvelopeV5(const ProductEnvelopeV5Candidate& candidate);

}  // namespace solidar::project

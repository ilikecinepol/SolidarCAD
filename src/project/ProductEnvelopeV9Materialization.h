#pragma once

#include <QString>

#include <memory>
#include <vector>

#include "model/PlanarMateSolver.h"
#include "project/ProductEnvelopeV9Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV9MaterializationStatus { Materialized, Invalid };
enum class ProductEnvelopeV9MaterializationMode {
  Publication,
  ValidationOnly,
};
enum class ProductEnvelopeV9MaterializationFailurePoint {
  None,
  BeforeLastPart,
  BeforeProduct,
  BeforeLastSolve,
  BeforeLastApply,
  ApplyFailure,
  AfterFirstApply,
  Finalization,
};

struct MateRuntimeDiagnostics final {
  DefinitionId ownerAssemblyId;
  std::vector<MateId> componentMateIds;
  MateSolveStatus status{MateSolveStatus::InvalidInput};
  MateSolveDiagnostics diagnostics;
};

struct ProductEnvelopeV9MaterializationResult final {
  ProductEnvelopeV9MaterializationStatus status{
      ProductEnvelopeV9MaterializationStatus::Invalid};
  std::unique_ptr<ProductDocument> product;
  std::vector<MateRuntimeDiagnostics> mateDiagnostics;
  QString error;
  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV9MaterializationStatus::Materialized &&
           product != nullptr;
  }
};

[[nodiscard]] ProductEnvelopeV9MaterializationResult
materializeProductEnvelopeV9(
    const ProductEnvelopeV9Candidate& candidate,
    ProductEnvelopeV9MaterializationFailurePoint failurePoint =
        ProductEnvelopeV9MaterializationFailurePoint::None,
    ProductEnvelopeV9MaterializationMode mode =
        ProductEnvelopeV9MaterializationMode::Publication);

}  // namespace solidar::project

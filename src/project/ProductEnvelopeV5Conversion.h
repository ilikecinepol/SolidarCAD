#pragma once

#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Encoder.h"

namespace solidar::project {

enum class ProductEnvelopeV5ConversionStatus {
  Prepared,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct PreparedProductEnvelopeV5Conversion {
  ProductDocumentIdentity identity;
  DefinitionId rootDefinitionId;
  ProductEnvelopeV5EncodedPayload payload;
};

struct ProductEnvelopeV5ConversionResult {
  ProductEnvelopeV5ConversionStatus status{
      ProductEnvelopeV5ConversionStatus::Invalid};
  std::optional<PreparedProductEnvelopeV5Conversion> prepared;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV5ConversionStatus::Prepared &&
           prepared.has_value();
  }
};

// Pure, non-mutating preparation for an explicit future conversion commit.
// All persistent identities are injected by the caller; this function does
// not generate IDs, reserve edit cursors, perform I/O, or mutate source state.
[[nodiscard]] ProductEnvelopeV5ConversionResult
prepareStandaloneProductEnvelopeV5Conversion(
    const ProductDocument& source, ProjectId outputProjectId,
    DefinitionId outputRootDefinitionId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

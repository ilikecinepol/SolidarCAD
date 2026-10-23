#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV5EncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct ProductEnvelopeV5EncodedPayload {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV5EncodeResult {
  ProductEnvelopeV5EncodeStatus status{ProductEnvelopeV5EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV5EncodedPayload> payload;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV5EncodeStatus::Valid &&
           payload.has_value();
  }
};

// Disconnected A2b1 encoder. It never mutates source identity/model state,
// recomputes geometry, performs I/O, or participates in ProjectFile dispatch.
[[nodiscard]] ProductEnvelopeV5EncodeResult encodeProductEnvelopeV5Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

// Explicit-root variant for standalone-to-product preparation. The persisted
// output definition identity is independent of a transient source carrier ID.
[[nodiscard]] ProductEnvelopeV5EncodeResult encodeProductEnvelopeV5Root(
    const ProductDocument& source, ProjectId outputProjectId,
    DefinitionId outputRootDefinitionId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

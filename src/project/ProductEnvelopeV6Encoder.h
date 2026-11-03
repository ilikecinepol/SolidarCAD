#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV6EncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
  UnsupportedAuthoredOccurrenceName,
};

struct ProductEnvelopeV6EncodedPayload {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV6EncodeResult {
  ProductEnvelopeV6EncodeStatus status{ProductEnvelopeV6EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV6EncodedPayload> payload;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV6EncodeStatus::Valid &&
           payload.has_value();
  }
};

[[nodiscard]] ProductEnvelopeV6EncodeResult encodeProductEnvelopeV6Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

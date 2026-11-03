#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV8EncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct ProductEnvelopeV8EncodedPayload final {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV8EncodeResult final {
  ProductEnvelopeV8EncodeStatus status{ProductEnvelopeV8EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV8EncodedPayload> payload;
  QString error;
  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV8EncodeStatus::Valid && payload.has_value();
  }
};

[[nodiscard]] ProductEnvelopeV8EncodeResult encodeProductEnvelopeV8Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

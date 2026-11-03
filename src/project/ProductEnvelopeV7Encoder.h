#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV7EncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct ProductEnvelopeV7EncodedPayload {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV7EncodeResult {
  ProductEnvelopeV7EncodeStatus status{ProductEnvelopeV7EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV7EncodedPayload> payload;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV7EncodeStatus::Valid &&
           payload.has_value();
  }
};

[[nodiscard]] ProductEnvelopeV7EncodeResult encodeProductEnvelopeV7Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

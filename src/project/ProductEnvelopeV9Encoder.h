#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV9EncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct ProductEnvelopeV9EncodedPayload final {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV9EncodeResult final {
  ProductEnvelopeV9EncodeStatus status{ProductEnvelopeV9EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV9EncodedPayload> payload;
  QString error;
  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV9EncodeStatus::Valid && payload;
  }
};

[[nodiscard]] ProductEnvelopeV9EncodeResult encodeProductEnvelopeV9Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

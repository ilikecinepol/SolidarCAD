#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV11EncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct ProductEnvelopeV11EncodedPayload final {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV11EncodeResult final {
  ProductEnvelopeV11EncodeStatus status{ProductEnvelopeV11EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV11EncodedPayload> payload;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV11EncodeStatus::Valid && payload;
  }
};

[[nodiscard]] ProductEnvelopeV11EncodeResult encodeProductEnvelopeV11Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

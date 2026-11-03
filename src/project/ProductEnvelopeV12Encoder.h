#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV12EncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct ProductEnvelopeV12EncodedPayload final {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV12EncodeResult final {
  ProductEnvelopeV12EncodeStatus status{ProductEnvelopeV12EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV12EncodedPayload> payload;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV12EncodeStatus::Valid && payload;
  }
};

[[nodiscard]] ProductEnvelopeV12EncodeResult encodeProductEnvelopeV12Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV10EncodeStatus {
  Valid,
  Invalid,
  UnsupportedComponentDrawings,
  PayloadTooLarge,
  SerializedPayloadValidation,
};

struct ProductEnvelopeV10EncodedPayload final {
  QJsonObject root;
  QByteArray bytes;
};

struct ProductEnvelopeV10EncodeResult final {
  ProductEnvelopeV10EncodeStatus status{ProductEnvelopeV10EncodeStatus::Invalid};
  std::optional<ProductEnvelopeV10EncodedPayload> payload;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV10EncodeStatus::Valid && payload;
  }
};

[[nodiscard]] ProductEnvelopeV10EncodeResult encodeProductEnvelopeV10Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata);

}  // namespace solidar::project

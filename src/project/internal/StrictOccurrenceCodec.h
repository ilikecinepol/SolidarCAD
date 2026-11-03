#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

#include "project/ProductEnvelopeV7Decoder.h"

namespace solidar::project::detail {

enum class StrictOccurrenceDecodeStatus {
  Valid,
  Invalid,
};

struct StrictOccurrenceDecodeResult final {
  StrictOccurrenceDecodeStatus status{StrictOccurrenceDecodeStatus::Invalid};
  std::vector<ProductEnvelopeV7OccurrenceCandidate> occurrences;
  std::size_t aggregateAuthoredOccurrences{};
  QString error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == StrictOccurrenceDecodeStatus::Valid;
  }
};

// Raw JSON-token policy shared by strict product envelopes. This intentionally
// operates before DOM semantic decoding so authored quaternion negative zero
// cannot be normalized away by QJsonDocument.
[[nodiscard]] bool validateStrictOccurrenceRawTokens(
    QByteArrayView bytes, QString* error) noexcept;

// V12 keeps the legacy quaternion policy and additionally requires canonical
// positive zero for occurrence translation arrays. Older readers continue to
// call validateStrictOccurrenceRawTokens() and retain their exact behavior.
[[nodiscard]] bool validateProductEnvelopeV12TransformRawTokens(
    QByteArrayView bytes, QString* error) noexcept;

// Decodes one exact, sorted occurrence array. Allocation exceptions propagate
// to the enclosing envelope scanner so its established resource-failure text
// and status remain unchanged.
[[nodiscard]] StrictOccurrenceDecodeResult decodeStrictOccurrences(
    const QJsonValue& value, const QString& definitionPath,
    std::size_t currentAuthoredOccurrences,
    std::size_t maximumAuthoredOccurrences);

enum class StrictOccurrenceEncodeStatus {
  Valid,
  InvalidId,
};

struct StrictOccurrenceEncodeResult final {
  StrictOccurrenceEncodeStatus status{StrictOccurrenceEncodeStatus::InvalidId};
  std::optional<QJsonObject> occurrence;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == StrictOccurrenceEncodeStatus::Valid &&
           occurrence.has_value();
  }
};

// Mirrors the V8 authored occurrence object exactly. It deliberately performs
// only the ID validation performed by the original V8 encoder.
[[nodiscard]] StrictOccurrenceEncodeResult encodeStrictOccurrence(
    OccurrenceId authoredOccurrenceId,
    const ComponentOccurrence& occurrence);

}  // namespace solidar::project::detail

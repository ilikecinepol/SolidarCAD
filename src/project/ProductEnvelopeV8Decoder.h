#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "model/AssemblyGraphAnalysis.h"
#include "project/ProductEnvelopeV7Decoder.h"

namespace solidar::project {

enum class ProductEnvelopeV8ScanStatus {
  Candidate,
  Unsupported,
  Invalid,
  NotApplicable,
};

struct ProductEnvelopeV8Candidate final {
  QJsonObject authoredRoot;
  ProjectId projectId;
  DefinitionId rootDefinitionId;
  ProductEnvelopeV5Metadata metadata;
  QStringList capabilities;
  std::vector<ProductEnvelopeV7PartCandidate> parts;
  std::vector<ProductEnvelopeV7AssemblyCandidate> assemblies;
  AssemblyGraphAnalysis graph;
  qsizetype jsonStringUtf8Bytes{};
  qsizetype partPayloadBytes{};
  qsizetype relativePathUtf8Bytes{};
  qsizetype rebuildWork{};
};

struct ProductEnvelopeV8ScanResult final {
  ProductEnvelopeV8ScanStatus status{ProductEnvelopeV8ScanStatus::Invalid};
  std::optional<ProductEnvelopeV8Candidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return status == ProductEnvelopeV8ScanStatus::Candidate &&
           candidate.has_value();
  }
};

// Disconnected only. No Document/OCCT materialization and no I/O.
[[nodiscard]] ProductEnvelopeV8ScanResult scanProductEnvelopeV8(
    QByteArrayView bytes);

}  // namespace solidar::project

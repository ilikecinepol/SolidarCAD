#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <optional>
#include <vector>

#include "model/ComponentGraphAnalysis.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProjectFormatV4ModelValidator.h"

namespace solidar::project {

inline constexpr qsizetype kProductEnvelopeV10MaximumJsonStringUtf8Bytes =
    1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV10MaximumEmbeddedPayloadBytes =
    48 * 1024 * 1024;
inline constexpr std::size_t kProductEnvelopeV10MaximumTopologyReferences =
    4096;
inline constexpr std::size_t kProductEnvelopeV10MaximumProvenanceRecords =
    65536;
inline constexpr std::size_t kProductEnvelopeV10MaximumLocalNameUtf8Bytes =
    8 * 1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV10MaximumRebuildWork = 10000;

enum class ProductEnvelopeV10ScanStatus {
  Candidate,
  Unsupported,
  Invalid,
  NotApplicable,
};

struct ProductEnvelopeV10DefinitionCandidate final {
  DefinitionId definitionId;
  QJsonObject authoredDefinition;
  QJsonObject authoredPayload;
  ProjectFormatV4ModelCandidate document;
  std::vector<ProductEnvelopeV7OccurrenceCandidate> occurrences;
};

struct ProductEnvelopeV10Candidate final {
  QJsonObject authoredRoot;
  ProjectId projectId;
  DefinitionId rootDefinitionId;
  ProductEnvelopeV5Metadata metadata;
  QStringList capabilities;
  std::vector<ProductEnvelopeV10DefinitionCandidate> definitions;
  ComponentGraphView graph;
  qsizetype jsonStringUtf8Bytes{};
  qsizetype embeddedPayloadBytes{};
  std::size_t topologyReferenceCount{};
  std::size_t provenanceRecordCount{};
  std::size_t localNameUtf8Bytes{};
  qsizetype rebuildWork{};
};

struct ProductEnvelopeV10ScanResult final {
  ProductEnvelopeV10ScanStatus status{ProductEnvelopeV10ScanStatus::Invalid};
  std::optional<ProductEnvelopeV10Candidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return status == ProductEnvelopeV10ScanStatus::Candidate && candidate;
  }
};

// Disconnected bounded scanner only. It performs no Document materialization,
// recompute, global ID reservation, publication or file I/O.
[[nodiscard]] ProductEnvelopeV10ScanResult scanProductEnvelopeV10(
    QByteArrayView bytes);

}  // namespace solidar::project

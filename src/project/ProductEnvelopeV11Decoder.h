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

inline constexpr qsizetype kProductEnvelopeV11MaximumJsonStringUtf8Bytes =
    1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV11MaximumEmbeddedPayloadBytes =
    48 * 1024 * 1024;
inline constexpr std::size_t kProductEnvelopeV11MaximumTopologyReferences =
    4096;
inline constexpr std::size_t kProductEnvelopeV11MaximumProvenanceRecords =
    65536;
inline constexpr std::size_t kProductEnvelopeV11MaximumLocalNameUtf8Bytes =
    8 * 1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV11MaximumRebuildWork = 10000;
inline constexpr std::size_t kProductEnvelopeV11MaximumDrawingNameUtf8Bytes =
    kComponentGraphMaximumDrawings *
    kComponentDrawingMaximumNameUtf8Bytes;
inline constexpr std::size_t kProductEnvelopeV11MaximumDrawingWork =
    kComponentGraphMaximumDrawings +
    kComponentGraphMaximumDrawingBodyReferences;

enum class ProductEnvelopeV11ScanStatus {
  Candidate,
  Unsupported,
  Invalid,
  NotApplicable,
};

struct ProductEnvelopeV11DefinitionCandidate final {
  DefinitionId definitionId;
  QJsonObject authoredDefinition;
  QJsonObject authoredPayload;
  ProjectFormatV4ModelCandidate document;
  std::vector<ComponentDrawing> drawings;
  std::vector<ProductEnvelopeV7OccurrenceCandidate> occurrences;
};

struct ProductEnvelopeV11Candidate final {
  QJsonObject authoredRoot;
  ProjectId projectId;
  DefinitionId rootDefinitionId;
  ProductEnvelopeV5Metadata metadata;
  QStringList capabilities;
  std::vector<ProductEnvelopeV11DefinitionCandidate> definitions;
  ComponentGraphView graph;
  qsizetype jsonStringUtf8Bytes{};
  qsizetype embeddedPayloadBytes{};
  std::size_t topologyReferenceCount{};
  std::size_t provenanceRecordCount{};
  std::size_t localNameUtf8Bytes{};
  qsizetype rebuildWork{};
  std::size_t drawingCount{};
  std::size_t drawingSourceReferenceCount{};
  std::size_t drawingNameUtf8Bytes{};
  std::size_t drawingWork{};
};

struct ProductEnvelopeV11ScanResult final {
  ProductEnvelopeV11ScanStatus status{ProductEnvelopeV11ScanStatus::Invalid};
  std::optional<ProductEnvelopeV11Candidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return status == ProductEnvelopeV11ScanStatus::Candidate && candidate;
  }
};

// Disconnected bounded scanner only. It performs no Document materialization,
// recompute, global ID reservation, publication or file I/O.
[[nodiscard]] ProductEnvelopeV11ScanResult scanProductEnvelopeV11(
    QByteArrayView bytes);

}  // namespace solidar::project

#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "model/ComponentGraphAnalysis.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProjectFormatV4ModelValidator.h"

namespace solidar::project {

inline constexpr qsizetype kProductEnvelopeV12MaximumJsonStringUtf8Bytes =
    1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV12MaximumEmbeddedPayloadBytes =
    48 * 1024 * 1024;
inline constexpr std::size_t kProductEnvelopeV12MaximumTopologyReferences =
    4096;
inline constexpr std::size_t kProductEnvelopeV12MaximumProvenanceRecords =
    65536;
inline constexpr std::size_t kProductEnvelopeV12MaximumLocalNameUtf8Bytes =
    8 * 1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV12MaximumRebuildWork = 10000;
inline constexpr std::size_t kProductEnvelopeV12MaximumDrawingNameUtf8Bytes =
    kComponentGraphMaximumDrawings *
    kComponentDrawingMaximumNameUtf8Bytes;
inline constexpr std::size_t kProductEnvelopeV12MaximumDrawingWork =
    kComponentGraphMaximumDrawings +
    kComponentGraphMaximumDrawingBodyReferences;

enum class ProductEnvelopeV12ScanStatus {
  Candidate,
  Unsupported,
  Invalid,
  NotApplicable,
};

struct ProductEnvelopeV12OccurrenceCandidate final {
  OccurrenceId occurrenceId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  RigidTransform3d homePlacement{RigidTransform3d::identity()};
  OccurrenceFlags flags;
  std::string displayNameUtf8;
  QJsonObject authoredOccurrence;

  friend bool operator==(const ProductEnvelopeV12OccurrenceCandidate&,
                         const ProductEnvelopeV12OccurrenceCandidate&) =
      default;
};

struct ProductEnvelopeV12DefinitionCandidate final {
  DefinitionId definitionId;
  QJsonObject authoredDefinition;
  QJsonObject authoredPayload;
  ProjectFormatV4ModelCandidate document;
  std::vector<ComponentDrawing> drawings;
  std::vector<ProductEnvelopeV12OccurrenceCandidate> occurrences;
};

struct ProductEnvelopeV12Candidate final {
  QJsonObject authoredRoot;
  ProjectId projectId;
  DefinitionId rootDefinitionId;
  ProductEnvelopeV5Metadata metadata;
  QStringList capabilities;
  std::vector<ProductEnvelopeV12DefinitionCandidate> definitions;
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

struct ProductEnvelopeV12ScanResult final {
  ProductEnvelopeV12ScanStatus status{ProductEnvelopeV12ScanStatus::Invalid};
  std::optional<ProductEnvelopeV12Candidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return status == ProductEnvelopeV12ScanStatus::Candidate && candidate;
  }
};

// Disconnected bounded scanner only. It performs no Document materialization,
// recompute, global ID reservation, publication or file I/O.
[[nodiscard]] ProductEnvelopeV12ScanResult scanProductEnvelopeV12(
    QByteArrayView bytes);

}  // namespace solidar::project

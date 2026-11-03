#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <optional>
#include <vector>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProjectFormatV4ModelValidator.h"

namespace solidar::project {

inline constexpr qsizetype kProductEnvelopeV6MaximumDefinitions = 256;
inline constexpr qsizetype kProductEnvelopeV6MaximumOccurrences = 4096;
inline constexpr qsizetype kProductEnvelopeV6MaximumJsonStringUtf8Bytes =
    1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV6MaximumEmbeddedPartBytes =
    48 * 1024 * 1024;

enum class ProductEnvelopeV6ScanStatus {
  Candidate,
  Unsupported,
  Invalid,
  NotApplicable,
};

enum class ProductEnvelopeV6DeferredUnknownKind {
  Capability,
  DefinitionKind,
  PayloadKind,
  EmbeddedPartSemantic,
};

struct ProductEnvelopeV6DeferredUnknown {
  ProductEnvelopeV6DeferredUnknownKind kind{
      ProductEnvelopeV6DeferredUnknownKind::Capability};
  QString path;
  QString token;

  friend bool operator==(const ProductEnvelopeV6DeferredUnknown&,
                         const ProductEnvelopeV6DeferredUnknown&) = default;
};

struct ProductEnvelopeV6PartCandidate {
  DefinitionId definitionId;
  QJsonObject authoredDefinition;
  QJsonObject authoredPayload;
  std::optional<ProjectFormatV4ModelCandidate> embeddedPart;

  friend bool operator==(const ProductEnvelopeV6PartCandidate&,
                         const ProductEnvelopeV6PartCandidate&) = default;
};

struct ProductEnvelopeV6OccurrenceCandidate {
  OccurrenceId occurrenceId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags;
  QJsonObject authoredOccurrence;

  friend bool operator==(const ProductEnvelopeV6OccurrenceCandidate&,
                         const ProductEnvelopeV6OccurrenceCandidate&) =
      default;
};

struct ProductEnvelopeV6AssemblyCandidate {
  DefinitionId definitionId;
  QJsonObject authoredDefinition;
  std::vector<ProductEnvelopeV6OccurrenceCandidate> occurrences;

  friend bool operator==(const ProductEnvelopeV6AssemblyCandidate&,
                         const ProductEnvelopeV6AssemblyCandidate&) = default;
};

struct ProductEnvelopeV6Candidate {
  QJsonObject authoredRoot;
  ProjectId projectId;
  DefinitionId rootDefinitionId;
  ProductEnvelopeV5Metadata metadata;
  QStringList capabilities;
  std::vector<ProductEnvelopeV6PartCandidate> parts;
  std::optional<ProductEnvelopeV6AssemblyCandidate> assembly;
  std::vector<ProductEnvelopeV6DeferredUnknown> deferredUnknowns;
  qsizetype jsonStringUtf8Bytes{};
  qsizetype embeddedPartBytes{};
  qsizetype rebuildWork{};
};

struct ProductEnvelopeV6ScanResult {
  ProductEnvelopeV6ScanStatus status{ProductEnvelopeV6ScanStatus::Invalid};
  std::optional<ProductEnvelopeV6Candidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return (status == ProductEnvelopeV6ScanStatus::Candidate ||
            status == ProductEnvelopeV6ScanStatus::Unsupported) &&
           candidate.has_value();
  }
};

// Disconnected bounded scan only: no Document materialization, OCCT work,
// recompute, application mutation or file I/O.
[[nodiscard]] ProductEnvelopeV6ScanResult scanProductEnvelopeV6(
    QByteArrayView bytes);

}  // namespace solidar::project

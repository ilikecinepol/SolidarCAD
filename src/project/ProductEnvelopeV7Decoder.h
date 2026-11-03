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

inline constexpr qsizetype kProductEnvelopeV7MaximumDefinitions = 256;
inline constexpr qsizetype kProductEnvelopeV7MaximumOccurrences = 4096;
inline constexpr qsizetype kProductEnvelopeV7MaximumJsonStringUtf8Bytes =
    1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV7MaximumPartPayloadBytes =
    48 * 1024 * 1024;
inline constexpr qsizetype kProductEnvelopeV7MaximumRelativePathUtf8Bytes =
    16 * 1024;

enum class ProductEnvelopeV7ScanStatus {
  Candidate,
  Unsupported,
  Invalid,
  NotApplicable,
};

enum class ProductEnvelopeV7PartKind { Embedded, Linked };

struct ProductEnvelopeV7PartCandidate {
  DefinitionId definitionId;
  ProductEnvelopeV7PartKind kind{ProductEnvelopeV7PartKind::Embedded};
  QJsonObject authoredDefinition;
  QJsonObject authoredPayload;
  std::optional<LinkedPartBinding> binding;
  std::optional<ProjectFormatV4ModelCandidate> lastValid;

  friend bool operator==(const ProductEnvelopeV7PartCandidate&,
                         const ProductEnvelopeV7PartCandidate&) = default;
};

struct ProductEnvelopeV7OccurrenceCandidate {
  OccurrenceId occurrenceId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags;
  std::string displayNameUtf8;
  QJsonObject authoredOccurrence;

  friend bool operator==(const ProductEnvelopeV7OccurrenceCandidate&,
                         const ProductEnvelopeV7OccurrenceCandidate&) =
      default;
};

struct ProductEnvelopeV7AssemblyCandidate {
  DefinitionId definitionId;
  QJsonObject authoredDefinition;
  std::vector<ProductEnvelopeV7OccurrenceCandidate> occurrences;

  friend bool operator==(const ProductEnvelopeV7AssemblyCandidate&,
                         const ProductEnvelopeV7AssemblyCandidate&) = default;
};

struct ProductEnvelopeV7Candidate {
  QJsonObject authoredRoot;
  ProjectId projectId;
  DefinitionId rootDefinitionId;
  ProductEnvelopeV5Metadata metadata;
  QStringList capabilities;
  std::vector<ProductEnvelopeV7PartCandidate> parts;
  std::optional<ProductEnvelopeV7AssemblyCandidate> assembly;
  qsizetype jsonStringUtf8Bytes{};
  qsizetype partPayloadBytes{};
  qsizetype relativePathUtf8Bytes{};
  qsizetype rebuildWork{};
};

struct ProductEnvelopeV7ScanResult {
  ProductEnvelopeV7ScanStatus status{ProductEnvelopeV7ScanStatus::Invalid};
  std::optional<ProductEnvelopeV7Candidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return (status == ProductEnvelopeV7ScanStatus::Candidate ||
            status == ProductEnvelopeV7ScanStatus::Unsupported) &&
           candidate.has_value();
  }
};

// Disconnected bounded scan only: no Document materialization, OCCT work,
// recompute, application mutation or file I/O.
[[nodiscard]] ProductEnvelopeV7ScanResult scanProductEnvelopeV7(
    QByteArrayView bytes);

}  // namespace solidar::project

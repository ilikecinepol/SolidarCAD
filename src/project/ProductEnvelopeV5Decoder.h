#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "model/ProductDocument.h"
#include "project/ProjectFormatV4ModelValidator.h"

namespace solidar::project {

enum class ProductEnvelopeV5ScanStatus {
  Candidate,
  Unsupported,
  Invalid,
  NotApplicable,
};

enum class ProductEnvelopeV5DeferredUnknownKind {
  Capability,
  DefinitionKind,
  PayloadKind,
  EmbeddedPartSemantic,
};

struct ProductEnvelopeV5DeferredUnknown {
  ProductEnvelopeV5DeferredUnknownKind kind{
      ProductEnvelopeV5DeferredUnknownKind::Capability};
  QString path;
  QString token;

  friend bool operator==(const ProductEnvelopeV5DeferredUnknown&,
                         const ProductEnvelopeV5DeferredUnknown&) = default;
};

struct ProductEnvelopeV5Metadata {
  QString name;
  QString createdAt;

  friend bool operator==(const ProductEnvelopeV5Metadata&,
                         const ProductEnvelopeV5Metadata&) = default;
};

// A bounded and fully validated A2a persistence candidate.  authoredRoot,
// authoredDefinition and authoredPayload retain the exact authored JSON.  The
// nested v4 candidate comes from the existing v4 validation pipeline; this
// layer deliberately has no second feature/topology codec.
struct ProductEnvelopeV5Candidate {
  QJsonObject authoredRoot;
  ProjectId projectId;
  DefinitionId rootDefinitionId;
  DefinitionId definitionId;
  ProductEnvelopeV5Metadata metadata;
  QStringList capabilities;
  QJsonObject authoredDefinition;
  QJsonObject authoredPayload;
  std::optional<ProjectFormatV4ModelCandidate> embeddedPart;
  std::vector<ProductEnvelopeV5DeferredUnknown> deferredUnknowns;
};

struct ProductEnvelopeV5ScanResult {
  ProductEnvelopeV5ScanStatus status{ProductEnvelopeV5ScanStatus::Invalid};
  std::optional<ProductEnvelopeV5Candidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return (status == ProductEnvelopeV5ScanStatus::Candidate ||
            status == ProductEnvelopeV5ScanStatus::Unsupported) &&
           candidate.has_value();
  }
};

// Disconnected A2a scanner.  It does not participate in ProjectFile dispatch,
// materialize a Document, recompute geometry, or perform file I/O.
[[nodiscard]] ProductEnvelopeV5ScanResult scanProductEnvelopeV5(
    QByteArrayView bytes);

}  // namespace solidar::project

#pragma once

#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace solidar::project {

enum class ProjectFormatV4EnvelopeScanStatus {
  Candidate,
  Invalid,
  NotApplicable,
};

enum class ProjectFormatV4DeferredUnknownKind {
  Capability,
  ConstraintTypeEncoding,
};

struct ProjectFormatV4DeferredUnknown {
  ProjectFormatV4DeferredUnknownKind kind{
      ProjectFormatV4DeferredUnknownKind::Capability};
  QString path;
  QString token;

  friend bool operator==(const ProjectFormatV4DeferredUnknown&,
                         const ProjectFormatV4DeferredUnknown&) = default;
};

// A bounded, structurally valid outer v4 envelope. Nested v3-compatible
// payloads deliberately remain authored JSON for later schema validation.
struct ProjectFormatV4EnvelopeCandidate {
  QJsonObject authoredRoot;
  QJsonObject document;
  QJsonArray legacySketches;
  QJsonObject legacyExtrusion;
  QJsonObject model;
  QStringList capabilities;
  std::vector<ProjectFormatV4DeferredUnknown> deferredUnknowns;
};

struct ProjectFormatV4EnvelopeScanResult {
  ProjectFormatV4EnvelopeScanStatus status{
      ProjectFormatV4EnvelopeScanStatus::Invalid};
  std::optional<ProjectFormatV4EnvelopeCandidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return status == ProjectFormatV4EnvelopeScanStatus::Candidate &&
           candidate.has_value();
  }
};

// Scanner-only P2b boundary. It parses bytes once, validates bounded v4
// envelope fields, and never materializes a Document or participates in the
// production ProjectFile dispatcher/writer.
[[nodiscard]] ProjectFormatV4EnvelopeScanResult
scanProjectFormatV4Envelope(QByteArrayView bytes);

}  // namespace solidar::project

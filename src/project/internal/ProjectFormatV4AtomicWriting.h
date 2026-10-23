#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/Document.h"
#include "project/internal/ProjectAtomicWriting.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"

namespace solidar::project::detail {

using ProjectFormatV4AtomicWriteFailure = ProjectAtomicWriteFailure;

struct ProjectFormatV4PreparedPayload {
  QJsonObject root;
  QByteArray bytes;
};

struct ProjectFormatV4PayloadPreparationResult {
  ProjectFormatV4AtomicWriteFailure failure{
      ProjectFormatV4AtomicWriteFailure::Encode};
  std::optional<ProjectFormatV4PreparedPayload> payload;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return failure == ProjectFormatV4AtomicWriteFailure::None &&
           payload.has_value();
  }
};

using ProjectFormatV4AtomicWriteResult = ProjectAtomicWriteResult;
using ProjectFormatV4AtomicWriteTransaction = ProjectAtomicWriteTransaction;

// Encodes, serializes, applies the exact-byte size limit and validates those
// exact indented bytes through the P2 Candidate pipeline before any sink opens.
[[nodiscard]] ProjectFormatV4PayloadPreparationResult
prepareProjectFormatV4DocumentPayload(
    const Document &document, const ProjectFormatV4DocumentMetadata &metadata);

// Atomic publication backend. The caller supplies already prepared bytes;
// the exact-byte size gate is repeated here so an oversized payload cannot
// open a transaction even through this lower-level seam.
[[nodiscard]] ProjectFormatV4AtomicWriteResult
publishProjectFormatV4PayloadAtomically(
    QByteArrayView bytes, ProjectFormatV4AtomicWriteTransaction &transaction);

// Injectable, deterministic writer used by tests and later integration work.
[[nodiscard]] ProjectFormatV4AtomicWriteResult
writeProjectFormatV4DocumentAtomically(
    const Document &document, const ProjectFormatV4DocumentMetadata &metadata,
    ProjectFormatV4AtomicWriteTransaction &transaction);

// Disconnected production adapter. Metadata comes from the target path and
// the current UTC clock, matching the existing standalone project convention.
[[nodiscard]] ProjectFormatV4AtomicWriteResult
writeProjectFormatV4DocumentAtomically(const QString &path,
                                       const Document &document);

} // namespace solidar::project::detail

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QString>

#include <optional>

#include "model/Document.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"

namespace solidar::project::detail {

enum class ProjectFormatV4AtomicWriteFailure {
  None,
  Encode,
  SerializedPayloadValidation,
  PayloadTooLarge,
  OpenTemporary,
  WriteTemporary,
  FlushTemporary,
  SyncTemporary,
  ReplaceDestination,
};

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

struct ProjectFormatV4AtomicWriteResult {
  ProjectFormatV4AtomicWriteFailure failure{
      ProjectFormatV4AtomicWriteFailure::Encode};
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return failure == ProjectFormatV4AtomicWriteFailure::None;
  }
};

// Internal transaction seam. Implementations stage bytes privately until
// commit() publishes them. cancel() must be safe after every failed operation.
class ProjectFormatV4AtomicWriteTransaction {
public:
  virtual ~ProjectFormatV4AtomicWriteTransaction() = default;

  virtual bool open(QString *error) = 0;
  virtual qint64 write(QByteArrayView bytes, QString *error) = 0;
  virtual bool flush(QString *error) = 0;
  virtual bool sync(QString *error) = 0;
  virtual bool commit(QString *error) = 0;
  virtual void cancel() noexcept = 0;
};

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

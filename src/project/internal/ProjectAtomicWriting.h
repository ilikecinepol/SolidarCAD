#pragma once

#include <QByteArrayView>
#include <QString>

namespace solidar::project::detail {

enum class ProjectAtomicWriteFailure {
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

struct ProjectAtomicWriteResult {
  ProjectAtomicWriteFailure failure{ProjectAtomicWriteFailure::Encode};
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return failure == ProjectAtomicWriteFailure::None;
  }
};

// Format-neutral transaction seam. Implementations stage bytes privately until
// commit() publishes them. cancel() must be safe after every failed operation.
class ProjectAtomicWriteTransaction {
public:
  virtual ~ProjectAtomicWriteTransaction() = default;

  virtual bool open(QString *error) = 0;
  virtual qint64 write(QByteArrayView bytes, QString *error) = 0;
  virtual bool flush(QString *error) = 0;
  virtual bool sync(QString *error) = 0;
  virtual bool commit(QString *error) = 0;
  virtual void cancel() noexcept = 0;
};

// Publishes already encoded bytes. The exact-byte size gate runs before a
// transaction is opened.
[[nodiscard]] ProjectAtomicWriteResult publishProjectPayloadAtomically(
    QByteArrayView bytes, ProjectAtomicWriteTransaction &transaction);

// Production QSaveFile adapter for any validated project payload.
[[nodiscard]] ProjectAtomicWriteResult
publishProjectPayloadAtomically(const QString &path, QByteArrayView bytes);

} // namespace solidar::project::detail

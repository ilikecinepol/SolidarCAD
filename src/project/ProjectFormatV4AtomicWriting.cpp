#include "project/internal/ProjectFormatV4AtomicWriting.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <cerrno>
#include <exception>
#include <system_error>
#include <utility>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4ModelValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"

namespace solidar::project::detail {
namespace {

QString fallbackError(QString error, const char *fallback) {
  return error.isEmpty() ? QString::fromLatin1(fallback) : std::move(error);
}

QString exceptionError(const std::exception &exception) {
  return QString::fromUtf8(exception.what());
}

class QSaveFileTransaction final
    : public ProjectFormatV4AtomicWriteTransaction {
public:
  explicit QSaveFileTransaction(QString path) : file_(std::move(path)) {
    file_.setDirectWriteFallback(false);
  }

  bool open(QString *error) override {
    if (file_.open(QIODevice::WriteOnly))
      return true;
    if (error)
      *error = file_.errorString();
    return false;
  }

  qint64 write(QByteArrayView bytes, QString *error) override {
    const qint64 written = file_.write(bytes.data(), bytes.size());
    if (written != bytes.size() && error)
      *error = file_.errorString();
    return written;
  }

  bool flush(QString *error) override {
    if (file_.flush())
      return true;
    if (error)
      *error = file_.errorString();
    return false;
  }

  bool sync(QString *error) override {
    const int descriptor = file_.handle();
    if (descriptor < 0) {
      if (error) {
        *error = QStringLiteral(
                     "temporary file has no valid descriptor for sync: %1")
                     .arg(descriptor);
      }
      return false;
    }
#ifdef Q_OS_WIN
    const int status = ::_commit(descriptor);
#else
    const int status = ::fsync(descriptor);
#endif
    if (status == 0)
      return true;

    const int savedErrno = errno;
    if (error) {
      const std::system_error systemError(savedErrno, std::generic_category());
      *error =
          QStringLiteral("temporary file sync failed for descriptor %1: %2")
              .arg(descriptor)
              .arg(QString::fromLocal8Bit(systemError.what()));
    }
    return false;
  }

  bool commit(QString *error) override {
    if (file_.commit())
      return true;
    if (error)
      *error = file_.errorString();
    return false;
  }

  void cancel() noexcept override { file_.cancelWriting(); }

private:
  QSaveFile file_;
};

ProjectFormatV4AtomicWriteResult
writeFailure(ProjectFormatV4AtomicWriteFailure failure, QString error) {
  return {failure, std::move(error)};
}

} // namespace

ProjectFormatV4PayloadPreparationResult prepareProjectFormatV4DocumentPayload(
    const Document &document, const ProjectFormatV4DocumentMetadata &metadata) {
  ProjectFormatV4PayloadPreparationResult result;
  try {
    auto encoded = encodeProjectFormatV4DocumentRoot(document, metadata);
    if (!encoded.succeeded()) {
      result.failure = ProjectFormatV4AtomicWriteFailure::Encode;
      result.error = fallbackError(std::move(encoded.error),
                                   "v4 document encoding failed");
      return result;
    }

    QJsonObject root = std::move(*encoded.root);
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > ProjectFile::kMaximumFileBytes) {
      result.failure = ProjectFormatV4AtomicWriteFailure::PayloadTooLarge;
      result.error = QStringLiteral("serialized v4 payload exceeds %1 bytes")
                         .arg(ProjectFile::kMaximumFileBytes);
      return result;
    }

    auto envelope = scanProjectFormatV4Envelope(QByteArrayView(bytes));
    if (!envelope.hasCandidate() || envelope.candidate->authoredRoot != root) {
      result.failure =
          ProjectFormatV4AtomicWriteFailure::SerializedPayloadValidation;
      result.error = fallbackError(
          std::move(envelope.error),
          "serialized v4 envelope does not match the encoded root");
      return result;
    }
    auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
    if (!payload.hasCandidate() ||
        payload.candidate->envelope.authoredRoot != root) {
      result.failure =
          ProjectFormatV4AtomicWriteFailure::SerializedPayloadValidation;
      result.error = fallbackError(
          std::move(payload.error),
          "serialized v4 payload does not match the encoded root");
      return result;
    }
    auto model = validateProjectFormatV4Model(std::move(*payload.candidate));
    if (model.status != ProjectFormatV4ModelValidationStatus::Candidate ||
        !model.candidate ||
        model.candidate->legacySketch.topology.payload.envelope.authoredRoot !=
            root) {
      result.failure =
          ProjectFormatV4AtomicWriteFailure::SerializedPayloadValidation;
      result.error =
          fallbackError(std::move(model.error),
                        "serialized v4 model does not match the encoded root");
      return result;
    }

    result.failure = ProjectFormatV4AtomicWriteFailure::None;
    result.payload.emplace(
        ProjectFormatV4PreparedPayload{std::move(root), std::move(bytes)});
    return result;
  } catch (const std::exception &exception) {
    result.failure = ProjectFormatV4AtomicWriteFailure::Encode;
    result.error = exceptionError(exception);
    return result;
  } catch (...) {
    result.failure = ProjectFormatV4AtomicWriteFailure::Encode;
    result.error =
        QStringLiteral("unknown exception while preparing v4 payload");
    return result;
  }
}

ProjectFormatV4AtomicWriteResult publishProjectFormatV4PayloadAtomically(
    QByteArrayView bytes, ProjectFormatV4AtomicWriteTransaction &transaction) {
  if (bytes.size() > ProjectFile::kMaximumFileBytes) {
    return writeFailure(ProjectFormatV4AtomicWriteFailure::PayloadTooLarge,
                        QStringLiteral("serialized v4 payload exceeds %1 bytes")
                            .arg(ProjectFile::kMaximumFileBytes));
  }

  ProjectFormatV4AtomicWriteFailure currentFailure =
      ProjectFormatV4AtomicWriteFailure::OpenTemporary;
  try {
    QString error;
    if (!transaction.open(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "failed to open temporary file");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectFormatV4AtomicWriteFailure::WriteTemporary;
    const qint64 written = transaction.write(bytes, &error);
    if (written != bytes.size()) {
      const QString preserved =
          fallbackError(std::move(error), "short temporary-file write");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectFormatV4AtomicWriteFailure::FlushTemporary;
    error.clear();
    if (!transaction.flush(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "temporary-file flush failed");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectFormatV4AtomicWriteFailure::SyncTemporary;
    error.clear();
    if (!transaction.sync(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "temporary-file sync failed");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectFormatV4AtomicWriteFailure::ReplaceDestination;
    error.clear();
    if (!transaction.commit(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "destination replacement failed");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }
    return {ProjectFormatV4AtomicWriteFailure::None, {}};
  } catch (const std::exception &exception) {
    const QString preserved = exceptionError(exception);
    transaction.cancel();
    return writeFailure(currentFailure, preserved);
  } catch (...) {
    const QString preserved =
        QStringLiteral("unknown exception during atomic v4 publication");
    transaction.cancel();
    return writeFailure(currentFailure, preserved);
  }
}

ProjectFormatV4AtomicWriteResult writeProjectFormatV4DocumentAtomically(
    const Document &document, const ProjectFormatV4DocumentMetadata &metadata,
    ProjectFormatV4AtomicWriteTransaction &transaction) {
  auto prepared = prepareProjectFormatV4DocumentPayload(document, metadata);
  if (!prepared.succeeded())
    return {prepared.failure, std::move(prepared.error)};
  return publishProjectFormatV4PayloadAtomically(
      QByteArrayView(prepared.payload->bytes), transaction);
}

ProjectFormatV4AtomicWriteResult
writeProjectFormatV4DocumentAtomically(const QString &path,
                                       const Document &document) {
  const ProjectFormatV4DocumentMetadata metadata{
      QFileInfo(path).completeBaseName(),
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate)};
  auto prepared = prepareProjectFormatV4DocumentPayload(document, metadata);
  if (!prepared.succeeded())
    return {prepared.failure, std::move(prepared.error)};

  QSaveFileTransaction transaction(path);
  return publishProjectFormatV4PayloadAtomically(
      QByteArrayView(prepared.payload->bytes), transaction);
}

} // namespace solidar::project::detail

#include "project/internal/ProjectAtomicWriting.h"

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

namespace solidar::project::detail {
namespace {

QString fallbackError(QString error, const char *fallback) {
  return error.isEmpty() ? QString::fromLatin1(fallback) : std::move(error);
}

QString exceptionError(const std::exception &exception) {
  return QString::fromUtf8(exception.what());
}

class QSaveFileTransaction final : public ProjectAtomicWriteTransaction {
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

ProjectAtomicWriteResult writeFailure(ProjectAtomicWriteFailure failure,
                                      QString error) {
  return {failure, std::move(error)};
}

} // namespace

ProjectAtomicWriteResult publishProjectPayloadAtomically(
    QByteArrayView bytes, ProjectAtomicWriteTransaction &transaction) {
  if (bytes.size() > ProjectFile::kMaximumFileBytes) {
    return writeFailure(ProjectAtomicWriteFailure::PayloadTooLarge,
                        QStringLiteral("serialized project payload exceeds %1 bytes")
                            .arg(ProjectFile::kMaximumFileBytes));
  }

  ProjectAtomicWriteFailure currentFailure =
      ProjectAtomicWriteFailure::OpenTemporary;
  try {
    QString error;
    if (!transaction.open(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "failed to open temporary file");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectAtomicWriteFailure::WriteTemporary;
    const qint64 written = transaction.write(bytes, &error);
    if (written != bytes.size()) {
      const QString preserved =
          fallbackError(std::move(error), "short temporary-file write");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectAtomicWriteFailure::FlushTemporary;
    error.clear();
    if (!transaction.flush(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "temporary-file flush failed");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectAtomicWriteFailure::SyncTemporary;
    error.clear();
    if (!transaction.sync(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "temporary-file sync failed");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }

    currentFailure = ProjectAtomicWriteFailure::ReplaceDestination;
    error.clear();
    if (!transaction.commit(&error)) {
      const QString preserved =
          fallbackError(std::move(error), "destination replacement failed");
      transaction.cancel();
      return writeFailure(currentFailure, preserved);
    }
    return {ProjectAtomicWriteFailure::None, {}};
  } catch (const std::exception &exception) {
    const QString preserved = exceptionError(exception);
    transaction.cancel();
    return writeFailure(currentFailure, preserved);
  } catch (...) {
    const QString preserved =
        QStringLiteral("unknown exception during atomic project publication");
    transaction.cancel();
    return writeFailure(currentFailure, preserved);
  }
}

ProjectAtomicWriteResult
publishProjectPayloadAtomically(const QString &path, QByteArrayView bytes) {
  QSaveFileTransaction transaction(path);
  return publishProjectPayloadAtomically(bytes, transaction);
}

} // namespace solidar::project::detail

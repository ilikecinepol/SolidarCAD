#include "TestAssertions.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <stdexcept>
#include <utility>
#include <vector>

#include "project/ProjectFile.h"
#include "project/internal/ProjectAtomicWriting.h"

namespace {

using namespace solidar::project;
using namespace solidar::project::detail;

enum class Operation { None, Open, Write, Flush, Sync, Commit };
enum class Behavior { Succeed, ReturnFailure, ThrowStandard, ThrowUnknown };

class FakeTransaction final : public ProjectAtomicWriteTransaction {
public:
  FakeTransaction(Operation operation = Operation::None,
                  Behavior behavior = Behavior::Succeed)
      : operation_(operation), behavior_(behavior) {}

  bool open(QString *error) override {
    calls << QStringLiteral("open");
    return complete(Operation::Open, error);
  }

  qint64 write(QByteArrayView bytes, QString *error) override {
    calls << QStringLiteral("write");
    CHECK(destination == QByteArray("destination-sentinel"));
    if (operation_ == Operation::Write) {
      if (behavior_ == Behavior::ReturnFailure) {
        rememberError(error, QStringLiteral("write-error"));
        return shortWrite_ ? bytes.size() - 1 : -1;
      }
      throwIfRequested();
    }
    staged = QByteArray(bytes.data(), bytes.size());
    return bytes.size();
  }

  bool flush(QString *error) override {
    calls << QStringLiteral("flush");
    CHECK(destination == QByteArray("destination-sentinel"));
    return complete(Operation::Flush, error);
  }

  bool sync(QString *error) override {
    calls << QStringLiteral("sync");
    CHECK(destination == QByteArray("destination-sentinel"));
    return complete(Operation::Sync, error);
  }

  bool commit(QString *error) override {
    calls << QStringLiteral("commit");
    CHECK(destination == QByteArray("destination-sentinel"));
    if (!complete(Operation::Commit, error))
      return false;
    destination = staged;
    return true;
  }

  void cancel() noexcept override {
    calls << QStringLiteral("cancel");
    ++cancelCount;
    if (operationError_)
      *operationError_ = QStringLiteral("cancel-clobber");
    operationError_ = nullptr;
    staged.clear();
  }

  void useShortWrite() noexcept { shortWrite_ = true; }

  QStringList calls;
  QByteArray destination{"destination-sentinel"};
  QByteArray staged;
  int cancelCount{};

private:
  bool complete(Operation operation, QString *error) {
    if (operation_ != operation)
      return true;
    if (behavior_ == Behavior::ReturnFailure) {
      rememberError(error,
                    QStringLiteral("%1-error").arg(operationName(operation)));
      return false;
    }
    throwIfRequested();
    return true;
  }

  void rememberError(QString *error, QString message) {
    if (error)
      *error = std::move(message);
    operationError_ = error;
  }

  void throwIfRequested() const {
    if (behavior_ == Behavior::ThrowStandard)
      throw std::runtime_error("operation-exception");
    if (behavior_ == Behavior::ThrowUnknown)
      throw 42;
  }

  static QString operationName(Operation operation) {
    switch (operation) {
    case Operation::Open:
      return QStringLiteral("open");
    case Operation::Write:
      return QStringLiteral("write");
    case Operation::Flush:
      return QStringLiteral("flush");
    case Operation::Sync:
      return QStringLiteral("sync");
    case Operation::Commit:
      return QStringLiteral("commit");
    case Operation::None:
      break;
    }
    return {};
  }

  Operation operation_;
  Behavior behavior_;
  QString *operationError_{};
  bool shortWrite_{};
};

struct Expectation {
  Operation operation;
  ProjectAtomicWriteFailure failure;
  QStringList calls;
};

const std::vector<Expectation> kExpectations{
    {Operation::Open, ProjectAtomicWriteFailure::OpenTemporary,
     {"open", "cancel"}},
    {Operation::Write, ProjectAtomicWriteFailure::WriteTemporary,
     {"open", "write", "cancel"}},
    {Operation::Flush, ProjectAtomicWriteFailure::FlushTemporary,
     {"open", "write", "flush", "cancel"}},
    {Operation::Sync, ProjectAtomicWriteFailure::SyncTemporary,
     {"open", "write", "flush", "sync", "cancel"}},
    {Operation::Commit, ProjectAtomicWriteFailure::ReplaceDestination,
     {"open", "write", "flush", "sync", "commit", "cancel"}},
};

QByteArray readAll(const QString &path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

} // namespace

int main() {
  const QByteArray payload("format-neutral-payload");

  // Success performs the complete transaction once and never cancels.
  {
    FakeTransaction transaction;
    const auto result = publishProjectPayloadAtomically(
        QByteArrayView(payload), transaction);
    CHECK(result.succeeded());
    CHECK(result.error.isEmpty());
    CHECK(transaction.destination == payload);
    CHECK(transaction.cancelCount == 0);
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
  }

  // Every false-return path preserves its operation error across cancel,
  // cancels exactly once, keeps the destination and makes no later call.
  for (const auto &expectation : kExpectations) {
    FakeTransaction transaction(expectation.operation,
                                Behavior::ReturnFailure);
    const auto result = publishProjectPayloadAtomically(
        QByteArrayView(payload), transaction);
    CHECK(result.failure == expectation.failure);
    CHECK(result.error.endsWith(QStringLiteral("-error")));
    CHECK(result.error != QStringLiteral("cancel-clobber"));
    CHECK(transaction.destination == QByteArray("destination-sentinel"));
    CHECK(transaction.cancelCount == 1);
    CHECK(transaction.calls == expectation.calls);
  }

  // Both short and negative write results are rejected as write failures.
  for (const bool shortWrite : {false, true}) {
    FakeTransaction transaction(Operation::Write, Behavior::ReturnFailure);
    if (shortWrite)
      transaction.useShortWrite();
    const auto result = publishProjectPayloadAtomically(
        QByteArrayView(payload), transaction);
    CHECK(result.failure == ProjectAtomicWriteFailure::WriteTemporary);
    CHECK(transaction.cancelCount == 1);
    CHECK((transaction.calls == QStringList{"open", "write", "cancel"}));
  }

  // Standard exceptions at every operation retain stage classification.
  for (const auto &expectation : kExpectations) {
    FakeTransaction transaction(expectation.operation,
                                Behavior::ThrowStandard);
    const auto result = publishProjectPayloadAtomically(
        QByteArrayView(payload), transaction);
    CHECK(result.failure == expectation.failure);
    CHECK(result.error == QStringLiteral("operation-exception"));
    CHECK(transaction.destination == QByteArray("destination-sentinel"));
    CHECK(transaction.cancelCount == 1);
    CHECK(transaction.calls == expectation.calls);
  }

  // Non-standard exceptions are also contained and cancel exactly once.
  for (const auto &expectation : kExpectations) {
    FakeTransaction transaction(expectation.operation,
                                Behavior::ThrowUnknown);
    const auto result = publishProjectPayloadAtomically(
        QByteArrayView(payload), transaction);
    CHECK(result.failure == expectation.failure);
    CHECK(result.error == QStringLiteral(
                              "unknown exception during atomic project publication"));
    CHECK(transaction.cancelCount == 1);
    CHECK(transaction.calls == expectation.calls);
  }

  // Oversized bytes fail before open and therefore do not need cancellation.
  {
    const QByteArray oversized(
        static_cast<qsizetype>(ProjectFile::kMaximumFileBytes + 1), 'x');
    FakeTransaction transaction;
    const auto result = publishProjectPayloadAtomically(
        QByteArrayView(oversized), transaction);
    CHECK(result.failure == ProjectAtomicWriteFailure::PayloadTooLarge);
    CHECK(result.error.contains(QStringLiteral("project payload")));
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.cancelCount == 0);
    CHECK(transaction.destination == QByteArray("destination-sentinel"));
  }

  // The production adapter atomically replaces an existing destination with
  // the exact caller-supplied bytes.
  {
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("atomic-project-XXXXXX")));
    CHECK(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("payload.solidar"));
    {
      QFile file(path);
      CHECK(file.open(QIODevice::WriteOnly));
      CHECK(file.write("existing-sentinel") == 17);
    }
    const auto result =
        publishProjectPayloadAtomically(path, QByteArrayView(payload));
    CHECK(result.succeeded());
    CHECK(readAll(path) == payload);
  }

  return 0;
}

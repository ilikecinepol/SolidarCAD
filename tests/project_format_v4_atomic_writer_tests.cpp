#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "model/ChamferFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/MirrorFeature.h"
#include "model/ShellFeature.h"
#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4ModelValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/ProjectPersistenceState.h"
#include "project/internal/ProjectFormatV4AtomicWriting.h"

namespace {

using namespace solidar;
using namespace solidar::project;
using namespace solidar::project::detail;

constexpr BodyId kBody = 10;
constexpr FeatureId kProducer = 1;
constexpr SketchId kSketch = 100;

ShapeFeature::ShapePtr box() {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape());
}

FaceReference faceV2(const char *localName) {
  return FaceReference::fromV2(
      {kBody, TopologyProvenanceKey{kProducer, TopologyKind::Face,
                                    TopologyProvenanceOperation::Generated,
                                    TopologySemanticRole::Side, localName}});
}

EdgeReference edgeV2(const char *localName) {
  return EdgeReference::fromV2(
      {kBody, TopologyProvenanceKey{kProducer, TopologyKind::Edge,
                                    TopologyProvenanceOperation::Generated,
                                    TopologySemanticRole::Custom, localName}});
}

Document v1Document() {
  Document document;
  auto &body = document.addBody(kBody, "v1 body");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
  body.addFeature(std::make_unique<FilletFeature>(
      2, std::vector<EdgeReference>{EdgeReference(kBody, kProducer, 0)}, 0.25,
      "v1 fillet"));
  return document;
}

Document mixedAllSiteDocument() {
  Document document;
  auto &sketch = document.addSketch(kSketch, "profile");
  sketch.geometry.addRectangle({0.0, 0.0}, {10.0, 8.0});
  sketch.support.type = SketchSupportType::Face;
  sketch.support.face = FaceReference(kBody, kProducer, 0, "support-v1");

  auto &body = document.addBody(kBody, "mixed body");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
  body.addFeature(std::make_unique<FilletFeature>(
      2,
      std::vector<EdgeReference>{EdgeReference(kBody, kProducer, 0),
                                 edgeV2("fillet-v2")},
      0.25, "fillet"));
  body.addFeature(std::make_unique<ChamferFeature>(
      3,
      std::vector<EdgeReference>{edgeV2("chamfer-v2"),
                                 EdgeReference(kBody, kProducer, 1)},
      0.15, "chamfer"));
  body.addFeature(std::make_unique<MirrorFeature>(
      4, kProducer, FaceReference(kBody, kProducer, 1), "mirror"));
  body.addFeature(std::make_unique<ShellFeature>(
      5, kProducer,
      std::vector<FaceReference>{FaceReference(kBody, kProducer, 2),
                                 faceV2("shell-v2")},
      0.1, false, "shell"));
  body.addFeature(std::make_unique<DraftFeature>(
      6, kProducer,
      std::vector<FaceReference>{faceV2("drafted-v2"),
                                 FaceReference(kBody, kProducer, 3)},
      PlaneReference{NeutralPlaneType::BodyFace, faceV2("neutral-v2")},
      AxisReference{AxisReferenceType::GlobalZ}, 2.0, false, "draft",
      edgeV2("rotation-v2")));
  body.addFeature(std::make_unique<ExtrudeFeature>(
      7, faceV2("extrude-v2"), 1.0, "face extrude", ExtrudeOperation::Join,
      false));
  return document;
}

struct ValidatedBytes {
  ProjectFormatV4PayloadCandidate payload;
  ProjectFormatV4ModelCandidate model;
};

ValidatedBytes validateBytes(const QByteArray &bytes) {
  auto envelope = scanProjectFormatV4Envelope(QByteArrayView(bytes));
  CHECK(envelope.status == ProjectFormatV4EnvelopeScanStatus::Candidate);
  CHECK(envelope.candidate.has_value());
  auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
  CHECK(payload.status == ProjectFormatV4PayloadScanStatus::Candidate);
  CHECK(payload.candidate.has_value());
  ProjectFormatV4PayloadCandidate payloadCopy = *payload.candidate;
  auto model = validateProjectFormatV4Model(std::move(*payload.candidate));
  CHECK(model.status == ProjectFormatV4ModelValidationStatus::Candidate);
  CHECK(model.candidate.has_value());
  return {std::move(payloadCopy), std::move(*model.candidate)};
}

QByteArray readAll(const QString &path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

void writeAll(const QString &path, QByteArrayView bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cerr << "Cannot open test file " << path.toStdString() << ": "
              << file.errorString().toStdString() << '\n';
    CHECK(false);
  }
  CHECK(file.write(bytes.data(), bytes.size()) == bytes.size());
  CHECK(file.flush());
}

enum class FailurePoint { None, Open, Write, Flush, Sync, Commit };

class FakeTransaction final : public ProjectFormatV4AtomicWriteTransaction {
public:
  explicit FakeTransaction(FailurePoint failure = FailurePoint::None)
      : failure_(failure) {}

  bool open(QString *error) override {
    calls << QStringLiteral("open");
    return complete(FailurePoint::Open, error, "open-error");
  }

  qint64 write(QByteArrayView bytes, QString *error) override {
    calls << QStringLiteral("write");
    CHECK(destination == QByteArray("published-sentinel"));
    if (!complete(FailurePoint::Write, error, "write-error"))
      return bytes.isEmpty() ? -1 : bytes.size() - 1;
    staged = QByteArray(bytes.data(), bytes.size());
    return bytes.size();
  }

  bool flush(QString *error) override {
    calls << QStringLiteral("flush");
    CHECK(destination == QByteArray("published-sentinel"));
    return complete(FailurePoint::Flush, error, "flush-error");
  }

  bool sync(QString *error) override {
    calls << QStringLiteral("sync");
    CHECK(destination == QByteArray("published-sentinel"));
    return complete(FailurePoint::Sync, error, "sync-error");
  }

  bool commit(QString *error) override {
    calls << QStringLiteral("commit");
    destinationObservedAtCommit = destination;
    if (!complete(FailurePoint::Commit, error, "commit-error"))
      return false;
    destination = staged;
    return true;
  }

  void cancel() noexcept override {
    calls << QStringLiteral("cancel");
    if (operationError_)
      *operationError_ = QStringLiteral("cancel-clobber");
    operationError_ = nullptr;
    staged.clear();
  }

  QStringList calls;
  QByteArray destination{"published-sentinel"};
  QByteArray staged;
  QByteArray destinationObservedAtCommit;

private:
  bool complete(FailurePoint point, QString *error, const char *message) {
    if (failure_ != point)
      return true;
    if (error)
      *error = QString::fromLatin1(message);
    operationError_ = error;
    return false;
  }

  FailurePoint failure_;
  QString *operationError_{};
};

ProjectFormatV4DocumentMetadata fixedMetadata() {
  return {QString::fromUtf8("Деталь Ω"),
          QStringLiteral("2026-10-21T12:34:56Z")};
}

}  // namespace

int main() {
  // Persistence choice is a neutral value, independent from Document state.
  {
    const ProjectPersistenceState defaults;
    CHECK(defaults.format == ProjectPersistenceFormat::V3);
    const ProjectPersistenceState explicitV4{ProjectPersistenceFormat::V4};
    CHECK(explicitV4.format == ProjectPersistenceFormat::V4);
    CHECK(defaults != explicitV4);
  }

  // The disconnected v4 writer emits v4 even when every topology carrier is
  // v1. State-selected dispatch remains an integration-stage responsibility.
  ProjectFormatV4PayloadPreparationResult v1Prepared;
  {
    const Document document = v1Document();
    v1Prepared =
        prepareProjectFormatV4DocumentPayload(document, fixedMetadata());
    CHECK(v1Prepared.succeeded());
    CHECK(v1Prepared.payload->root.value("version").toInt() == 4);
    const auto validated = validateBytes(v1Prepared.payload->bytes);
    CHECK(validated.payload.envelope.authoredRoot == v1Prepared.payload->root);
    CHECK(validated.payload.topologySites.size() == 1);
    CHECK(validated.payload.topologySites[0].authoredReference.toObject().value(
              "referenceVersion") == QStringLiteral("V1"));
  }

  // Mixed v1/v2 carriers cover every authored topology site family and remain
  // a strict P2 Candidate after exact-byte serialization.
  {
    const auto prepared = prepareProjectFormatV4DocumentPayload(
        mixedAllSiteDocument(), fixedMetadata());
    CHECK(prepared.succeeded());
    const auto validated = validateBytes(prepared.payload->bytes);
    CHECK(validated.payload.topologySites.size() == 13);
    QSet<int> families;
    for (const auto &site : validated.payload.topologySites)
      families.insert(static_cast<int>(site.family));
    CHECK(families.size() == 9);
    CHECK(validated.payload.envelope.authoredRoot == prepared.payload->root);
  }

  // Explicit Unicode metadata and the exact indented bytes pass unchanged
  // through the injectable transaction.
  {
    Document document;
    const auto prepared =
        prepareProjectFormatV4DocumentPayload(document, fixedMetadata());
    CHECK(prepared.succeeded());
    CHECK(prepared.payload->root.value("name") == fixedMetadata().name);
    CHECK(prepared.payload->root.value("createdAt") ==
          fixedMetadata().createdAt);
    CHECK(
        prepared.payload->bytes ==
        QJsonDocument(prepared.payload->root).toJson(QJsonDocument::Indented));
    FakeTransaction transaction;
    const auto result = writeProjectFormatV4DocumentAtomically(
        document, fixedMetadata(), transaction);
    CHECK(result.succeeded());
    CHECK(transaction.destination == prepared.payload->bytes);
    CHECK(transaction.destinationObservedAtCommit ==
          QByteArray("published-sentinel"));
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
  }

  // Every deterministic transaction failure keeps the published sentinel,
  // cancels staging and never advances to a later operation.
  {
    struct FailureExpectation {
      FailurePoint point;
      ProjectFormatV4AtomicWriteFailure failure;
      QString error;
      QStringList calls;
    };
    const std::vector<FailureExpectation> expectations{
        {FailurePoint::Open,
         ProjectFormatV4AtomicWriteFailure::OpenTemporary,
         "open-error",
         {"open", "cancel"}},
        {FailurePoint::Write,
         ProjectFormatV4AtomicWriteFailure::WriteTemporary,
         "write-error",
         {"open", "write", "cancel"}},
        {FailurePoint::Flush,
         ProjectFormatV4AtomicWriteFailure::FlushTemporary,
         "flush-error",
         {"open", "write", "flush", "cancel"}},
        {FailurePoint::Sync,
         ProjectFormatV4AtomicWriteFailure::SyncTemporary,
         "sync-error",
         {"open", "write", "flush", "sync", "cancel"}},
        {FailurePoint::Commit,
         ProjectFormatV4AtomicWriteFailure::ReplaceDestination,
         "commit-error",
         {"open", "write", "flush", "sync", "commit", "cancel"}},
    };
    for (const auto &expectation : expectations) {
      FakeTransaction transaction(expectation.point);
      const auto result = publishProjectFormatV4PayloadAtomically(
          QByteArrayView(v1Prepared.payload->bytes), transaction);
      CHECK(result.failure == expectation.failure);
      CHECK(result.error == expectation.error);
      CHECK(transaction.destination == QByteArray("published-sentinel"));
      CHECK(transaction.calls == expectation.calls);
    }
  }

  // Encode and exact serialized-byte size failures happen before sink open.
  {
    Document invalid;
    auto &sketch = invalid.addSketch(kSketch, "invalid support");
    sketch.support.type = SketchSupportType::Face;
    FakeTransaction transaction;
    const auto result = writeProjectFormatV4DocumentAtomically(
        invalid, fixedMetadata(), transaction);
    CHECK(result.failure == ProjectFormatV4AtomicWriteFailure::Encode);
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("published-sentinel"));
  }
  {
    const QByteArray oversized(
        static_cast<qsizetype>(ProjectFile::kMaximumFileBytes + 1), 'x');
    FakeTransaction transaction;
    const auto result = publishProjectFormatV4PayloadAtomically(
        QByteArrayView(oversized), transaction);
    CHECK(result.failure == ProjectFormatV4AtomicWriteFailure::PayloadTooLarge);
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("published-sentinel"));
  }

  // The production QSaveFile adapter replaces existing files only after a
  // flushed and platform-synced temporary payload has passed validation.
  {
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("atomic-v4-XXXXXX")));
    CHECK(directory.isValid());
    const QString firstPath =
        directory.filePath(QString::fromUtf8("Первая.solidar"));
    const QString secondPath =
        directory.filePath(QStringLiteral("second.solidar"));
    writeAll(firstPath, QByteArrayView("first-sentinel"));
    writeAll(secondPath, QByteArrayView("second-sentinel"));

    Document document;
    const auto first =
        writeProjectFormatV4DocumentAtomically(firstPath, document);
    CHECK(first.succeeded());
    const QByteArray firstBytes = readAll(firstPath);
    const auto firstValidated = validateBytes(firstBytes);
    const QJsonObject firstRoot = firstValidated.payload.envelope.authoredRoot;
    CHECK(firstRoot.value("version").toInt() == 4);
    CHECK(firstRoot.value("name") == QString::fromUtf8("Первая"));
    const QDateTime createdAt = QDateTime::fromString(
        firstRoot.value("createdAt").toString(), Qt::ISODate);
    CHECK(createdAt.isValid());
    CHECK(createdAt.timeSpec() == Qt::UTC);

    const auto second =
        writeProjectFormatV4DocumentAtomically(secondPath, document);
    CHECK(second.succeeded());
    const auto secondValidated = validateBytes(readAll(secondPath));
    CHECK(secondValidated.payload.envelope.authoredRoot.value("version")
              .toInt() == 4);
    CHECK(secondValidated.payload.envelope.authoredRoot.value("name") ==
          QStringLiteral("second"));
  }

  // Production preparation failures occur before QSaveFile is opened: an
  // existing destination stays byte-identical and an absent one stays absent.
  {
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("atomic-v4-encode-XXXXXX")));
    CHECK(directory.isValid());
    const QString existingPath =
        directory.filePath(QStringLiteral("existing.solidar"));
    const QString absentPath =
        directory.filePath(QStringLiteral("absent.solidar"));
    const QByteArray sentinel("encode-failure-sentinel");
    writeAll(existingPath, QByteArrayView(sentinel));

    Document invalid;
    auto &sketch = invalid.addSketch(kSketch, "invalid support");
    sketch.support.type = SketchSupportType::Face;

    const auto existing =
        writeProjectFormatV4DocumentAtomically(existingPath, invalid);
    CHECK(existing.failure == ProjectFormatV4AtomicWriteFailure::Encode);
    CHECK(readAll(existingPath) == sentinel);

    const auto absent =
        writeProjectFormatV4DocumentAtomically(absentPath, invalid);
    CHECK(absent.failure == ProjectFormatV4AtomicWriteFailure::Encode);
    CHECK(!QFileInfo::exists(absentPath));
  }

  // The existing public v3 writer remains the dispatcher and still rejects a
  // v2 carrier without touching an existing destination.
  {
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("legacy-v3-XXXXXX")));
    CHECK(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("legacy-v3.solidar"));
    const QByteArray sentinel("legacy-v3-sentinel");
    writeAll(path, QByteArrayView(sentinel));
    QString error;
    CHECK(!ProjectFile::saveDocument(path, mixedAllSiteDocument(), &error));
    CHECK(!error.isEmpty());
    CHECK(readAll(path) == sentinel);
  }

  return 0;
}

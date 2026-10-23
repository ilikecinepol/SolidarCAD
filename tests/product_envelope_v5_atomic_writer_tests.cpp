#include "TestAssertions.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <memory>
#include <utility>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/internal/ProductEnvelopeV5AtomicWriting.h"
#include "project/internal/ProjectFormatV4AtomicWriting.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"

namespace {

using namespace solidar;
using namespace solidar::project;
using namespace solidar::project::detail;

constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};

ProductEnvelopeV5Metadata metadata() {
  return {QStringLiteral("atomic"),
          QStringLiteral("2026-10-23T12:34:56Z")};
}

std::unique_ptr<ProductDocument> product(Document document = {}) {
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(
            DefinitionId{7},
            std::make_unique<Document>(std::move(document)))) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(registry), DefinitionId{7});
  CHECK(created);
  return std::move(created.product);
}

Document oversizedButStructurallyValidDocument() {
  Document document;
  // Three individually valid bounded strings per item cross the 64 MiB
  // whole-file limit without expensive geometry or a test-only codec seam.
  constexpr int kImageCount = 1500;
  const std::string field(15000, 'x');
  for (int index = 0; index < kImageCount; ++index) {
    document.addReferenceImage(field, field, field, SketchPlacement::xy(), 1,
                               1);
  }
  return document;
}

class RecordingTransaction : public ProjectAtomicWriteTransaction {
 public:
  bool open(QString*) override {
    calls << QStringLiteral("open");
    return true;
  }
  qint64 write(QByteArrayView bytes, QString*) override {
    calls << QStringLiteral("write");
    staged = QByteArray(bytes.data(), bytes.size());
    return bytes.size();
  }
  bool flush(QString*) override {
    calls << QStringLiteral("flush");
    return true;
  }
  bool sync(QString*) override {
    calls << QStringLiteral("sync");
    return true;
  }
  bool commit(QString*) override {
    calls << QStringLiteral("commit");
    destination = staged;
    return true;
  }
  void cancel() noexcept override { calls << QStringLiteral("cancel"); }

  QStringList calls;
  QByteArray staged;
  QByteArray destination{"sentinel"};
};

QByteArray readAll(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

void writeAll(const QString& path, QByteArrayView bytes) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  CHECK(file.write(bytes.data(), bytes.size()) == bytes.size());
  CHECK(file.flush());
}

}  // namespace

int main() {
  // The injectable writer publishes exactly the bytes produced by the pure
  // encoder, via the shared generic transaction sequence.
  {
    auto source = product();
    const auto encoded =
        encodeProductEnvelopeV5Root(*source, kProject, metadata());
    CHECK(encoded.succeeded());
    RecordingTransaction transaction;
    const auto result = writeProductEnvelopeV5Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(result.succeeded());
    CHECK(transaction.destination == encoded.payload->bytes);
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    const auto scanned =
        scanProductEnvelopeV5(QByteArrayView(transaction.destination));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
  }

  // Preparation failures precede open and preserve the published sentinel.
  {
    auto source = product();
    RecordingTransaction transaction;
    const auto result = writeProductEnvelopeV5Atomically(
        *source, ProjectId{}, metadata(), transaction);
    CHECK(result.failure == ProjectAtomicWriteFailure::Encode);
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
  }
  {
    Document invalid;
    auto& sketch = invalid.addSketch("invalid support");
    sketch.support.type = SketchSupportType::Face;
    auto source = product(std::move(invalid));
    RecordingTransaction transaction;
    const auto result = writeProductEnvelopeV5Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(result.failure == ProjectAtomicWriteFailure::Encode);
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
  }

  // The nested v4 encoder reports the whole-file size condition distinctly;
  // v5 preserves that classification and never opens the publication sink.
  {
    auto source = product(oversizedButStructurallyValidDocument());
    const ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata().name, metadata().createdAt};
    const auto nested = encodeProjectFormatV4DocumentRoot(
        source->rootDocument(), nestedMetadata);
    CHECK(nested.status ==
          ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge);
    CHECK(!nested.succeeded());
    CHECK(!nested.root);
    CHECK(!nested.error.isEmpty());

    RecordingTransaction v4Transaction;
    const auto v4Written = writeProjectFormatV4DocumentAtomically(
        source->rootDocument(), nestedMetadata, v4Transaction);
    CHECK(v4Written.failure == ProjectAtomicWriteFailure::PayloadTooLarge);
    CHECK(!v4Written.error.isEmpty());
    CHECK(v4Transaction.calls.isEmpty());
    CHECK(v4Transaction.destination == QByteArray("sentinel"));

    const auto encoded =
        encodeProductEnvelopeV5Root(*source, kProject, metadata());
    CHECK(encoded.status == ProductEnvelopeV5EncodeStatus::PayloadTooLarge);
    CHECK(!encoded.succeeded());
    CHECK(!encoded.payload);
    CHECK(!encoded.error.isEmpty());

    RecordingTransaction transaction;
    const auto written = writeProductEnvelopeV5Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(written.failure == ProjectAtomicWriteFailure::PayloadTooLarge);
    CHECK(!written.error.isEmpty());
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
  }

  // Generic failure handling is integrated: a short write cancels without
  // replacing the destination and preserves the operation diagnostic.
  {
    class ShortWrite final : public RecordingTransaction {
     public:
      qint64 write(QByteArrayView bytes, QString* error) override {
        calls << QStringLiteral("write");
        if (error) *error = QStringLiteral("short-write");
        return bytes.size() - 1;
      }
    } transaction;
    auto source = product();
    const auto result = writeProductEnvelopeV5Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(result.failure == ProjectAtomicWriteFailure::WriteTemporary);
    CHECK(result.error == QStringLiteral("short-write"));
    CHECK(transaction.destination == QByteArray("sentinel"));
    CHECK((transaction.calls == QStringList{"open", "write", "cancel"}));
  }

  // The real QSaveFile adapter replaces a sentinel only for valid bytes and
  // leaves it byte-identical when preparation fails.
  {
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("atomic-v5-XXXXXX")));
    CHECK(directory.isValid());
    const QString validPath =
        directory.filePath(QStringLiteral("valid.solidar"));
    const QString invalidPath =
        directory.filePath(QStringLiteral("invalid.solidar"));
    const QByteArray sentinel("published-sentinel");
    writeAll(validPath, QByteArrayView(sentinel));
    writeAll(invalidPath, QByteArrayView(sentinel));

    auto source = product();
    const auto expected =
        encodeProductEnvelopeV5Root(*source, kProject, metadata());
    CHECK(expected.succeeded());
    const auto written = writeProductEnvelopeV5Atomically(
        validPath, *source, kProject, metadata());
    CHECK(written.succeeded());
    CHECK(readAll(validPath) == expected.payload->bytes);

    const auto rejected = writeProductEnvelopeV5Atomically(
        invalidPath, *source, ProjectId{}, metadata());
    CHECK(rejected.failure == ProjectAtomicWriteFailure::Encode);
    CHECK(readAll(invalidPath) == sentinel);
  }

  return EXIT_SUCCESS;
}

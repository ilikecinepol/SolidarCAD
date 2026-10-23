#include "TestAssertions.h"

#include "ui/application/ProjectApplicationService.h"
#include "ui/application/internal/ProductConversionApplicationService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStringList>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/MoveFeature.h"
#include "project/ProductEnvelopeV5Conversion.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV5Materialization.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr DefinitionId kTransientRoot{1};
constexpr DefinitionId kPersistentRoot{4242};
constexpr SketchId kSketch{830001};
constexpr ReferenceImageId kImage{830002};
constexpr BodyId kBody{830003};
constexpr FeatureId kImported{830004};
constexpr FeatureId kMoved{830005};
constexpr ProjectId kProject{
    {0x9a, 0x2b, 0x44, 0x6e, 0x18, 0x37, 0x4b, 0xa1,
     0x91, 0xd0, 0x55, 0x60, 0x73, 0x84, 0x95, 0xa6}};

StandaloneProductConversionRequest request() {
  return {kProject, kPersistentRoot,
          QString::fromUtf8("Изделие Ω/雪"),
          QString::fromUtf8("2026-10-23T21:00:00+03:00/точно")};
}

ProductEnvelopeV5Metadata metadata(
    const StandaloneProductConversionRequest& value) {
  return {value.name, value.createdAt};
}

ShapeFeature::ShapePtr box() {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape());
}

Document modelDocument() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document document;
  auto& sketch = document.addSketch(kSketch, "profile");
  sketch.geometry.addRectangle({-4.0, -2.0}, {4.0, 2.0});
  ReferenceImage image;
  image.id = kImage;
  image.name = "reference";
  image.sourcePath = "reference.png";
  image.supportName = "XY";
  image.pixelWidth = 640;
  image.pixelHeight = 480;
  document.addReferenceImage(std::move(image));
  auto& body = document.addBody(kBody, "body");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kImported, box(), "archive"));
  body.addFeature(std::make_unique<MoveFeature>(
      kMoved, kImported, Vector3d{1.0, 2.0, 3.0}, "move"));
  CHECK(document.recompute());
  return document;
}

std::unique_ptr<ProductDocument> product(
    std::optional<ProductDocumentIdentity> identity = std::nullopt) {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            kTransientRoot,
            std::make_unique<Document>(modelDocument()))) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(
      std::move(definitions), kTransientRoot, std::move(identity));
  CHECK(created);
  return std::move(created.product);
}

QByteArray modelFingerprint(const Document& document) {
  const auto encoded = project::detail::encodeProjectFormatV4DocumentRoot(
      document, {QStringLiteral("fingerprint"),
                 QStringLiteral("2026-10-23T00:00:00Z")});
  CHECK(encoded.succeeded());
  return QJsonDocument(*encoded.root).toJson(QJsonDocument::Compact);
}

struct FeatureSnapshot {
  const Feature* address{};
  FeatureId id{};
  const TopoDS_Shape* shape{};
  const TopoDS_Shape* lastValidShape{};
  ShapeRevision revision{};
};

struct SourceSnapshot {
  std::optional<ProductDocumentIdentity> identity;
  DefinitionId rootId{};
  std::size_t definitionCount{};
  const Document* document{};
  const DocumentSketch* sketch{};
  const ReferenceImage* image{};
  const Body* body{};
  std::vector<FeatureSnapshot> features;
  QByteArray fingerprint;
};

SourceSnapshot snapshot(const ProductDocument& source) {
  const Document& document = source.rootDocument();
  CHECK(document.sketches().size() == 1);
  CHECK(document.referenceImages().size() == 1);
  CHECK(document.bodies().size() == 1);
  const Body& body = document.bodies().front();
  CHECK(body.features().size() == 2);
  SourceSnapshot result{source.identity(), source.rootDefinitionId(),
                        source.definitions().size(), &document,
                        &document.sketches().front(),
                        &document.referenceImages().front(), &body};
  for (const auto& feature : body.features()) {
    result.features.push_back(
        {feature.get(), feature->id(), feature->shape().get(),
         feature->lastValidShape().get(), feature->shapeRevision()});
  }
  result.fingerprint = modelFingerprint(document);
  return result;
}

void checkLiveGraph(const ProductDocument& source,
                    const SourceSnapshot& before) {
  const Document& document = source.rootDocument();
  CHECK(&document == before.document);
  CHECK(document.sketches().size() == 1);
  CHECK(&document.sketches().front() == before.sketch);
  CHECK(document.referenceImages().size() == 1);
  CHECK(&document.referenceImages().front() == before.image);
  CHECK(document.bodies().size() == 1);
  CHECK(&document.bodies().front() == before.body);
  CHECK(document.bodies().front().features().size() == before.features.size());
  for (std::size_t index = 0; index < before.features.size(); ++index) {
    const auto& feature = document.bodies().front().features().at(index);
    const auto& expected = before.features.at(index);
    CHECK(feature.get() == expected.address);
    CHECK(feature->id() == expected.id);
    CHECK(feature->shape().get() == expected.shape);
    CHECK(feature->lastValidShape().get() == expected.lastValidShape);
    CHECK(feature->shapeRevision() == expected.revision);
  }
  CHECK(modelFingerprint(document) == before.fingerprint);
}

void checkUnchanged(const ProductDocument& source,
                    const SourceSnapshot& before) {
  CHECK(source.identity() == before.identity);
  CHECK(source.rootDefinitionId() == before.rootId);
  CHECK(source.definitions().size() == before.definitionCount);
  checkLiveGraph(source, before);
}

struct AutoIds {
  SketchId sketch{};
  ReferenceImageId image{};
  BodyId body{};
  FeatureId feature{};
};

AutoIds consumeAutoIds(const char* suffix) {
  Document document;
  AutoIds ids;
  ids.sketch = document.addSketch(std::string("sketch-") + suffix).id;
  ids.image = document
                  .addReferenceImage(std::string("image-") + suffix,
                                     "image.png", "XY",
                                     SketchPlacement::xy(), 10, 10)
                  .id;
  auto& body = document.addBody(std::string("body-") + suffix);
  ids.body = body.id();
  auto feature =
      std::make_unique<ImportedShapeFeature>(box(), "auto-feature");
  ids.feature = feature->id();
  body.addFeature(std::move(feature));
  return ids;
}

void checkOnlyOneAutoAdvance(const AutoIds& before, const AutoIds& after) {
  CHECK(after.sketch == before.sketch + 1);
  CHECK(after.image == before.image + 1);
  CHECK(after.body == before.body + 1);
  CHECK(after.feature == before.feature + 1);
}

QByteArray readAll(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

enum class FailurePhase { None, Open, Write, Flush, Sync, Replace };

class RecordingTransaction final
    : public project::detail::ProjectAtomicWriteTransaction {
 public:
  explicit RecordingTransaction(FailurePhase failure = FailurePhase::None)
      : failure_(failure) {}

  bool open(QString* error) override {
    calls << QStringLiteral("open");
    if (failure_ != FailurePhase::Open) return true;
    if (error) *error = QStringLiteral("injected open failure");
    return false;
  }
  qint64 write(QByteArrayView bytes, QString* error) override {
    calls << QStringLiteral("write");
    staged = QByteArray(bytes.data(), bytes.size());
    if (failure_ != FailurePhase::Write) return bytes.size();
    if (error) *error = QStringLiteral("injected write failure");
    return bytes.size() - 1;
  }
  bool flush(QString* error) override {
    calls << QStringLiteral("flush");
    if (failure_ != FailurePhase::Flush) return true;
    if (error) *error = QStringLiteral("injected flush failure");
    return false;
  }
  bool sync(QString* error) override {
    calls << QStringLiteral("sync");
    if (failure_ != FailurePhase::Sync) return true;
    if (error) *error = QStringLiteral("injected sync failure");
    return false;
  }
  bool commit(QString* error) override {
    calls << QStringLiteral("commit");
    if (failure_ == FailurePhase::Replace) {
      if (error) *error = QStringLiteral("injected replace failure");
      return false;
    }
    destination = staged;
    return true;
  }
  void cancel() noexcept override { calls << QStringLiteral("cancel"); }

  QStringList calls;
  QByteArray staged;
  QByteArray destination{"sentinel-destination"};

 private:
  FailurePhase failure_;
};

void checkRejectedBeforePublisher(
    ProductDocument& source, const StandaloneProductConversionRequest& value,
    ProjectApplicationErrorCode expectedCode) {
  const auto before = snapshot(source);
  RecordingTransaction transaction;
  const auto result = application::detail::convertStandaloneToProduct(
      source, value, transaction);
  CHECK(!result.succeeded());
  CHECK(result.code == expectedCode);
  CHECK(!result.detail.isEmpty());
  CHECK(transaction.calls.isEmpty());
  CHECK(transaction.destination == QByteArray("sentinel-destination"));
  checkUnchanged(source, before);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication coreApplication(argc, argv);
  static_assert(noexcept(std::declval<const ProjectApplicationService&>()
                             .convertStandaloneToProduct(
                                 std::declval<const QString&>(),
                                 std::declval<ProductDocument&>(),
                                 std::declval<const
                                     StandaloneProductConversionRequest&>())));
  static_assert(noexcept(application::detail::convertStandaloneToProduct(
      std::declval<ProductDocument&>(),
      std::declval<const StandaloneProductConversionRequest&>(),
      std::declval<project::detail::ProjectAtomicWriteTransaction&>())));

  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("product-conversion-service-XXXXXX")));
  CHECK(directory.isValid());
  ProjectApplicationService service;

  // The path API publishes the exact bytes prepared before the call, then
  // adopts identity/root while retaining the complete live model graph.
  {
    auto source = product();
    const auto sourceBefore = snapshot(*source);
    const auto fixedRequest = request();
    const auto expected = prepareStandaloneProductEnvelopeV5Conversion(
        *source, fixedRequest.outputProjectId,
        fixedRequest.outputRootDefinitionId, metadata(fixedRequest));
    CHECK(expected.succeeded());
    const QByteArray expectedBytes = expected.prepared->payload.bytes;
    const AutoIds beforeIds = consumeAutoIds("before-production");
    const QString path =
        directory.filePath(QStringLiteral("converted.solidar"));
    const auto converted =
        service.convertStandaloneToProduct(path, *source, fixedRequest);
    CHECK(converted.succeeded());
    CHECK(readAll(path) == expectedBytes);
    CHECK(source->rootDefinitionId() == kPersistentRoot);
    CHECK(source->identity());
    CHECK(source->identity()->projectId == kProject);
    const QByteArray expectedName = fixedRequest.name.toUtf8();
    const QByteArray expectedCreatedAt = fixedRequest.createdAt.toUtf8();
    CHECK(source->identity()->metadata.nameUtf8 ==
          std::string(expectedName.constData(), expectedName.size()));
    CHECK(source->identity()->metadata.createdAtUtf8 ==
          std::string(expectedCreatedAt.constData(), expectedCreatedAt.size()));
    checkLiveGraph(*source, sourceBefore);

    const auto scanned = scanProductEnvelopeV5(QByteArrayView(expectedBytes));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(scanned.candidate);
    auto restored = materializeProductEnvelopeV5(*scanned.candidate);
    CHECK(restored.succeeded());
    CHECK(restored.product->identity() == source->identity());
    CHECK(restored.product->rootDefinitionId() == kPersistentRoot);
    CHECK(modelFingerprint(restored.product->rootDocument()) ==
          sourceBefore.fingerprint);
    CHECK(&restored.product->rootDocument() != sourceBefore.document);
    const auto* restoredFeature = dynamic_cast<const ShapeFeature*>(
        restored.product->rootDocument()
            .bodies()
            .front()
            .features()
            .front()
            .get());
    CHECK(restoredFeature);
    std::weak_ptr<const TopoDS_Shape> reconstructedShape =
        restoredFeature->lastValidShape();
    restored.product.reset();
    CHECK(reconstructedShape.expired());
    const AutoIds afterIds = consumeAutoIds("after-production");
    checkOnlyOneAutoAdvance(beforeIds, afterIds);
  }

  // Invalid inputs stop at preparation: the transaction never opens and the
  // source carrier is an exact no-op.
  {
    auto nilSource = product();
    auto nil = request();
    nil.outputProjectId = ProjectId{};
    checkRejectedBeforePublisher(*nilSource, nil,
                                 ProjectApplicationErrorCode::InvalidProject);

    auto zeroRootSource = product();
    auto zeroRoot = request();
    zeroRoot.outputRootDefinitionId = DefinitionId{};
    checkRejectedBeforePublisher(*zeroRootSource, zeroRoot,
                                 ProjectApplicationErrorCode::InvalidProject);

    auto highRootSource = product();
    auto highRoot = request();
    highRoot.outputRootDefinitionId = DefinitionId{
        static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId) + 1};
    checkRejectedBeforePublisher(*highRootSource, highRoot,
                                 ProjectApplicationErrorCode::InvalidProject);

    const auto identifiedIdentity = ProductDocumentIdentity{
        kProject, {"identified", "2026-10-23T00:00:00Z"}};
    auto identified = product(identifiedIdentity);
    checkRejectedBeforePublisher(*identified, request(),
                                 ProjectApplicationErrorCode::InvalidProject);

    auto multiple = product();
    CHECK(multiple->insert(PartDefinition(
              DefinitionId{2}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    checkRejectedBeforePublisher(*multiple, request(),
                                 ProjectApplicationErrorCode::InvalidProject);

    auto pathRejected = product();
    auto pathInvalid = request();
    pathInvalid.outputProjectId = ProjectId{};
    const QString absent =
        directory.filePath(QStringLiteral("invalid-no-destination.solidar"));
    CHECK(!service.convertStandaloneToProduct(absent, *pathRejected,
                                               pathInvalid)
               .succeeded());
    CHECK(!QFile::exists(absent));
  }

  // Every atomic publication phase can fail without changing the sentinel or
  // source. The same source then succeeds, and a repeat is rejected before a
  // fresh transaction can open.
  {
    auto source = product();
    const auto sourceBefore = snapshot(*source);
    const auto fixedRequest = request();
    const auto expected = prepareStandaloneProductEnvelopeV5Conversion(
        *source, fixedRequest.outputProjectId,
        fixedRequest.outputRootDefinitionId, metadata(fixedRequest));
    CHECK(expected.succeeded());
    for (const auto phase : {FailurePhase::Open, FailurePhase::Write,
                             FailurePhase::Flush, FailurePhase::Sync,
                             FailurePhase::Replace}) {
      RecordingTransaction transaction(phase);
      const auto failed = application::detail::convertStandaloneToProduct(
          *source, fixedRequest, transaction);
      CHECK(!failed.succeeded());
      CHECK(failed.code == ProjectApplicationErrorCode::SaveFailed);
      CHECK(!failed.detail.isEmpty());
      CHECK(!transaction.calls.isEmpty());
      CHECK(transaction.calls.back() == QStringLiteral("cancel"));
      CHECK(transaction.destination == QByteArray("sentinel-destination"));
      checkUnchanged(*source, sourceBefore);
    }

    RecordingTransaction successful;
    const auto converted = application::detail::convertStandaloneToProduct(
        *source, fixedRequest, successful);
    CHECK(converted.succeeded());
    CHECK(successful.destination == expected.prepared->payload.bytes);
    CHECK((successful.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    CHECK(source->identity());
    CHECK(source->rootDefinitionId() == kPersistentRoot);
    checkLiveGraph(*source, sourceBefore);

    const auto convertedSnapshot = snapshot(*source);
    RecordingTransaction repeated;
    const auto repeat = application::detail::convertStandaloneToProduct(
        *source, fixedRequest, repeated);
    CHECK(!repeat.succeeded());
    CHECK(repeat.code == ProjectApplicationErrorCode::InvalidProject);
    CHECK(repeated.calls.isEmpty());
    CHECK(repeated.destination == QByteArray("sentinel-destination"));
    checkUnchanged(*source, convertedSnapshot);
  }

  // A real missing-parent publication failure is atomic and leaves the live
  // standalone carrier unchanged.
  {
    auto source = product();
    const auto before = snapshot(*source);
    const QString missing = directory.filePath(
        QStringLiteral("missing-parent/converted.solidar"));
    const auto failed =
        service.convertStandaloneToProduct(missing, *source, request());
    CHECK(!failed.succeeded());
    CHECK(failed.code == ProjectApplicationErrorCode::SaveFailed);
    CHECK(!failed.detail.isEmpty());
    CHECK(!QFile::exists(missing));
    checkUnchanged(*source, before);
  }

  return EXIT_SUCCESS;
}

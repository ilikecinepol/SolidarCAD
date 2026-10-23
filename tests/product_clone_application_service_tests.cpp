#include "TestAssertions.h"

#include "ui/application/ProjectApplicationService.h"
#include "ui/application/internal/ProductCloneApplicationService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/MoveFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/ProductEnvelopeV5Materialization.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr DefinitionId kRoot{44001};
constexpr SketchId kSketch{840001};
constexpr ReferenceImageId kImage{840002};
constexpr BodyId kBody{840003};
constexpr FeatureId kImported{840004};
constexpr FeatureId kMoved{840005};
constexpr ProjectId kProjectA{
    {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x46, 0x77,
     0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01}};
constexpr ProjectId kProjectB{
    {0x20, 0x31, 0x42, 0x53, 0x64, 0x75, 0x48, 0x87,
     0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x02}};
constexpr ProjectId kProjectC{
    {0x30, 0x41, 0x52, 0x63, 0x74, 0x85, 0x4a, 0x97,
     0xa8, 0xb9, 0xca, 0xdb, 0xec, 0xfd, 0x0e, 0x03}};

ProductDocumentIdentity identityA() {
  return {kProjectA,
          {"Source \xCE\xA9/\xE9\x9B\xAA",
           "2026-10-23T20:00:00Z/\xD0\xB8\xD1\x81\xD1\x85\xD0\xBE\xD0\xB4"}};
}

ProductCloneRequest requestB() {
  return {kProjectB, QString::fromUtf8("Копия β/東"),
          QString::fromUtf8("2026-10-24T01:02:03+03:00/точно")};
}

ProductCloneRequest requestC() {
  return {kProjectC, QString::fromUtf8("Копия C"),
          QStringLiteral("2026-10-25T00:00:00Z")};
}

ProductEnvelopeV5Metadata metadata(const ProductCloneRequest& request) {
  return {request.name, request.createdAt};
}

ShapeFeature::ShapePtr box() {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape());
}

Document richDocument() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document document;
  auto& sketch = document.addSketch(kSketch, "clone profile");
  sketch.geometry.addRectangle({-5.0, -3.0}, {5.0, 3.0});
  ReferenceImage image;
  image.id = kImage;
  image.name = "clone reference";
  image.sourcePath = "reference.png";
  image.supportName = "XY";
  image.pixelWidth = 800;
  image.pixelHeight = 600;
  document.addReferenceImage(std::move(image));
  auto& body = document.addBody(kBody, "clone body");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kImported, box(), "source"));
  body.addFeature(std::make_unique<MoveFeature>(
      kMoved, kImported, Vector3d{2.0, 3.0, 4.0}, "moved"));
  CHECK(document.recompute());
  return document;
}

std::unique_ptr<ProductDocument> product(
    std::optional<ProductDocumentIdentity> identity = identityA(),
    DefinitionId root = kRoot) {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            root, std::make_unique<Document>(richDocument()))) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(definitions), root,
                                         std::move(identity));
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
  const TopologyIndex* topology{};
  ShapeRevision revision{};
  std::size_t faceCount{};
  std::size_t edgeCount{};
};

struct SourceSnapshot {
  std::optional<ProductDocumentIdentity> identity;
  DefinitionId root{};
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
    const auto topology = feature->lastValidTopologyIndex();
    CHECK(topology);
    result.features.push_back(
        {feature.get(), feature->id(), feature->shape().get(),
         feature->lastValidShape().get(), topology.get(),
         feature->shapeRevision(), topology->rawFaceCount(),
         topology->rawEdgeCount()});
  }
  result.fingerprint = modelFingerprint(document);
  return result;
}

void checkGraph(const ProductDocument& source, const SourceSnapshot& before) {
  CHECK(source.rootDefinitionId() == before.root);
  CHECK(source.definitions().size() == before.definitionCount);
  const Document& document = source.rootDocument();
  CHECK(&document == before.document);
  CHECK(&document.sketches().front() == before.sketch);
  CHECK(&document.referenceImages().front() == before.image);
  CHECK(&document.bodies().front() == before.body);
  CHECK(document.bodies().front().features().size() == before.features.size());
  for (std::size_t index = 0; index < before.features.size(); ++index) {
    const auto& actual = document.bodies().front().features().at(index);
    const auto& expected = before.features.at(index);
    CHECK(actual.get() == expected.address);
    CHECK(actual->id() == expected.id);
    CHECK(actual->shape().get() == expected.shape);
    CHECK(actual->lastValidShape().get() == expected.lastValidShape);
    CHECK(actual->shapeRevision() == expected.revision);
    const auto topology = actual->lastValidTopologyIndex();
    CHECK(topology.get() == expected.topology);
    CHECK(topology->rawFaceCount() == expected.faceCount);
    CHECK(topology->rawEdgeCount() == expected.edgeCount);
  }
  CHECK(modelFingerprint(document) == before.fingerprint);
}

void checkUnchanged(const ProductDocument& source,
                    const SourceSnapshot& before) {
  CHECK(source.identity() == before.identity);
  checkGraph(source, before);
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
  auto feature = std::make_unique<ImportedShapeFeature>(box(), "auto");
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

bool writeAll(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(bytes) == bytes.size() && file.flush();
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
    if (error) *error = QStringLiteral("injected short write");
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

void checkRejectedBeforeOpen(ProductDocument& source,
                             const ProductCloneRequest& request) {
  const auto before = snapshot(source);
  RecordingTransaction transaction;
  const auto result = application::detail::saveProductAsClone(
      source, request, transaction);
  CHECK(!result.succeeded());
  CHECK(result.code == ProjectApplicationErrorCode::InvalidProject);
  CHECK(!result.detail.isEmpty());
  CHECK(transaction.calls.isEmpty());
  CHECK(transaction.destination == QByteArray("sentinel-destination"));
  checkUnchanged(source, before);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication coreApplication(argc, argv);
  static_assert(noexcept(std::declval<const ProjectApplicationService&>()
                             .saveProductAsClone(
                                 std::declval<const QString&>(),
                                 std::declval<ProductDocument&>(),
                                 std::declval<const ProductCloneRequest&>())));
  static_assert(noexcept(application::detail::saveProductAsClone(
      std::declval<ProductDocument&>(),
      std::declval<const ProductCloneRequest&>(),
      std::declval<project::detail::ProjectAtomicWriteTransaction&>())));

  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("product-clone-service-XXXXXX")));
  CHECK(directory.isValid());
  ProjectApplicationService service;

  // Production Save As writes exactly the existing encoder's bytes to a
  // distinct target, leaves the original file untouched, then changes only
  // the live carrier identity from A to B.
  {
    auto source = product();
    const auto sourceBefore = snapshot(*source);
    const auto cloneRequest = requestB();
    const auto expected = encodeProductEnvelopeV5Root(
        *source, cloneRequest.outputProjectId, metadata(cloneRequest));
    CHECK(expected.succeeded());
    const QString originalPath =
        directory.filePath(QStringLiteral("original-a.solidar"));
    CHECK(service.save(originalPath, *source).succeeded());
    const QByteArray originalBytes = readAll(originalPath);
    const QString clonePath =
        directory.filePath(QStringLiteral("clone-b.solidar"));
    CHECK(writeAll(clonePath, QByteArray("old clone sentinel")));
    const AutoIds beforeIds = consumeAutoIds("before-clone");

    const auto cloned =
        service.saveProductAsClone(clonePath, *source, cloneRequest);
    CHECK(cloned.succeeded());
    CHECK(readAll(clonePath) == expected.payload->bytes);
    CHECK(readAll(originalPath) == originalBytes);
    CHECK(source->identity());
    CHECK(source->identity()->projectId == kProjectB);
    const QByteArray expectedName = cloneRequest.name.toUtf8();
    const QByteArray expectedCreatedAt = cloneRequest.createdAt.toUtf8();
    CHECK(source->identity()->metadata.nameUtf8 ==
          std::string(expectedName.constData(), expectedName.size()));
    CHECK(source->identity()->metadata.createdAtUtf8 ==
          std::string(expectedCreatedAt.constData(), expectedCreatedAt.size()));
    checkGraph(*source, sourceBefore);

    const auto scanned =
        scanProductEnvelopeV5(QByteArrayView(expected.payload->bytes));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(scanned.candidate);
    CHECK(scanned.candidate->projectId == kProjectB);
    CHECK(scanned.candidate->rootDefinitionId == kRoot);
    auto materialized = materializeProductEnvelopeV5(*scanned.candidate);
    CHECK(materialized.succeeded());
    CHECK(materialized.product->identity() == source->identity());
    CHECK(materialized.product->rootDefinitionId() == kRoot);
    CHECK(modelFingerprint(materialized.product->rootDocument()) ==
          sourceBefore.fingerprint);
    const auto& restoredDocument = materialized.product->rootDocument();
    CHECK(restoredDocument.sketches().front().id == kSketch);
    CHECK(restoredDocument.referenceImages().front().id == kImage);
    CHECK(restoredDocument.bodies().front().id() == kBody);
    CHECK(restoredDocument.bodies().front().features().at(0)->id() ==
          kImported);
    CHECK(restoredDocument.bodies().front().features().at(1)->id() == kMoved);
    for (std::size_t index = 0; index < sourceBefore.features.size(); ++index) {
      const auto topology = restoredDocument.bodies()
                                .front()
                                .features()
                                .at(index)
                                ->lastValidTopologyIndex();
      CHECK(topology);
      CHECK(topology->rawFaceCount() ==
            sourceBefore.features.at(index).faceCount);
      CHECK(topology->rawEdgeCount() ==
            sourceBefore.features.at(index).edgeCount);
    }
    const AutoIds afterIds = consumeAutoIds("after-clone");
    checkOnlyOneAutoAdvance(beforeIds, afterIds);
  }

  // Every invalid source/request is rejected before the writer can open.
  {
    auto identityless = product(std::nullopt);
    checkRejectedBeforeOpen(*identityless, requestB());

    auto nilSource = product();
    auto nil = requestB();
    nil.outputProjectId = ProjectId{};
    checkRejectedBeforeOpen(*nilSource, nil);

    auto sameSource = product();
    auto same = requestB();
    same.outputProjectId = kProjectA;
    checkRejectedBeforeOpen(*sameSource, same);

    auto multiple = product();
    CHECK(multiple->insert(PartDefinition(
              DefinitionId{44002}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    checkRejectedBeforeOpen(*multiple, requestB());

    auto outOfRangeRoot = product(
        identityA(), DefinitionId{
                         static_cast<std::uint64_t>(
                             ProjectFile::kMaximumPersistedId) +
                         1});
    checkRejectedBeforeOpen(*outOfRangeRoot, requestB());

    auto invalidMetadataSource = product();
    auto invalidMetadata = requestB();
    invalidMetadata.name = QString(1, QChar{char16_t{0xD800}});
    checkRejectedBeforeOpen(*invalidMetadataSource, invalidMetadata);

    auto oversizeMetadataSource = product();
    auto oversizeMetadata = requestB();
    oversizeMetadata.createdAt =
        QString(ProjectFile::kMaximumStringCharacters + 1, QLatin1Char('x'));
    checkRejectedBeforeOpen(*oversizeMetadataSource, oversizeMetadata);
  }

  // Every atomic phase preserves destination sentinel and the complete A
  // identity/graph. Retry B succeeds, B->B rejects before open, then B->C
  // succeeds without rebuilding or reallocating the model.
  {
    auto source = product();
    const auto sourceA = snapshot(*source);
    const auto cloneB = requestB();
    const auto expectedB = encodeProductEnvelopeV5Root(
        *source, cloneB.outputProjectId, metadata(cloneB));
    CHECK(expectedB.succeeded());
    for (const auto phase : {FailurePhase::Open, FailurePhase::Write,
                             FailurePhase::Flush, FailurePhase::Sync,
                             FailurePhase::Replace}) {
      RecordingTransaction transaction(phase);
      const auto failed = application::detail::saveProductAsClone(
          *source, cloneB, transaction);
      CHECK(!failed.succeeded());
      CHECK(failed.code == ProjectApplicationErrorCode::SaveFailed);
      CHECK(!failed.detail.isEmpty());
      CHECK(!transaction.calls.isEmpty());
      CHECK(transaction.calls.back() == QStringLiteral("cancel"));
      CHECK(transaction.destination == QByteArray("sentinel-destination"));
      checkUnchanged(*source, sourceA);
    }

    RecordingTransaction retryB;
    CHECK(application::detail::saveProductAsClone(*source, cloneB, retryB)
              .succeeded());
    CHECK(retryB.destination == expectedB.payload->bytes);
    CHECK((retryB.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    CHECK(source->identity()->projectId == kProjectB);
    checkGraph(*source, sourceA);

    const auto sourceB = snapshot(*source);
    auto sameB = cloneB;
    sameB.name = QStringLiteral("same B with changed metadata");
    RecordingTransaction rejectedSameB;
    const auto sameResult = application::detail::saveProductAsClone(
        *source, sameB, rejectedSameB);
    CHECK(!sameResult.succeeded());
    CHECK(sameResult.code == ProjectApplicationErrorCode::InvalidProject);
    CHECK(rejectedSameB.calls.isEmpty());
    CHECK(rejectedSameB.destination == QByteArray("sentinel-destination"));
    checkUnchanged(*source, sourceB);

    const auto cloneC = requestC();
    const auto expectedC = encodeProductEnvelopeV5Root(
        *source, cloneC.outputProjectId, metadata(cloneC));
    CHECK(expectedC.succeeded());
    RecordingTransaction subsequentC;
    CHECK(application::detail::saveProductAsClone(
              *source, cloneC, subsequentC)
              .succeeded());
    CHECK(subsequentC.destination == expectedC.payload->bytes);
    CHECK(source->identity()->projectId == kProjectC);
    checkGraph(*source, sourceA);
  }

  return EXIT_SUCCESS;
}

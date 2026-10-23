#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <QByteArray>
#include <QJsonDocument>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/MoveFeature.h"
#include "project/ProductEnvelopeV5Conversion.h"
#include "project/ProductEnvelopeV5Materialization.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr DefinitionId kTransientRoot{7};
constexpr DefinitionId kOutputRoot{4242};
constexpr SketchId kSketch{810001};
constexpr ReferenceImageId kImage{810002};
constexpr BodyId kBody{810003};
constexpr FeatureId kImported{810004};
constexpr FeatureId kMoved{810005};
constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};

ProductEnvelopeV5Metadata metadata() {
  return {QString::fromUtf8("Изделие Ω/雪"),
          QString::fromUtf8("2026-10-23T12:34:56Z/точно")};
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
    Document document,
    std::optional<ProductDocumentIdentity> identity = std::nullopt) {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            kTransientRoot,
            std::make_unique<Document>(std::move(document)))) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(
      std::move(definitions), kTransientRoot, std::move(identity));
  CHECK(created);
  return std::move(created.product);
}

QByteArray documentFingerprint(
    const Document& document,
    const ProductEnvelopeV5Metadata& authoredMetadata = metadata()) {
  const auto encoded =
      project::detail::encodeProjectFormatV4DocumentRoot(
          document, {authoredMetadata.name, authoredMetadata.createdAt});
  CHECK(encoded.succeeded());
  return QJsonDocument(*encoded.root).toJson(QJsonDocument::Compact);
}

struct FeatureSnapshot {
  const ShapeFeature* address{};
  FeatureId id{};
  FeatureState state{FeatureState::Dirty};
  const TopoDS_Shape* shape{};
  const TopoDS_Shape* lastValidShape{};
  ShapeRevision revision{};
};

struct SourceSnapshot {
  std::optional<ProductDocumentIdentity> identity;
  DefinitionId rootDefinitionId{};
  std::size_t definitionCount{};
  const Document* documentAddress{};
  const DocumentSketch* sketchAddress{};
  std::uint64_t sketchFingerprint{};
  const ReferenceImage* imageAddress{};
  std::size_t imageCount{};
  const Body* bodyAddress{};
  std::vector<FeatureSnapshot> features;
  const TopoDS_Shape* importedShape{};
  std::string importedHistory;
  std::optional<QByteArray> serializedModel;
};

SourceSnapshot snapshot(const ProductDocument& source,
                        bool includeSerializedModel = true) {
  const Document& document = source.rootDocument();
  CHECK(document.sketches().size() == 1);
  CHECK(!document.referenceImages().empty());
  CHECK(document.bodies().size() == 1);
  const Body& body = document.bodies().front();
  CHECK(body.features().size() == 2);
  SourceSnapshot result;
  result.identity = source.identity();
  result.rootDefinitionId = source.rootDefinitionId();
  result.definitionCount = source.definitions().size();
  result.documentAddress = &document;
  result.sketchAddress = &document.sketches().front();
  result.sketchFingerprint =
      document.sketches().front().geometry.semanticFingerprint();
  result.imageAddress = &document.referenceImages().front();
  result.imageCount = document.referenceImages().size();
  result.bodyAddress = &body;
  for (const auto& feature : body.features()) {
    result.features.push_back(
        {feature.get(), feature->id(), feature->state(), feature->shape().get(),
         feature->lastValidShape().get(), feature->shapeRevision()});
  }
  const auto* imported =
      dynamic_cast<const ImportedShapeFeature*>(body.features().front().get());
  CHECK(imported);
  result.importedShape = imported->importedShape().get();
  result.importedHistory = imported->historyArchive();
  if (includeSerializedModel)
    result.serializedModel = documentFingerprint(document);
  return result;
}

void checkUnchanged(const ProductDocument& source,
                    const SourceSnapshot& before) {
  CHECK(source.identity() == before.identity);
  CHECK(source.rootDefinitionId() == before.rootDefinitionId);
  CHECK(source.definitions().size() == before.definitionCount);
  const Document& document = source.rootDocument();
  CHECK(&document == before.documentAddress);
  CHECK(document.sketches().size() == 1);
  CHECK(&document.sketches().front() == before.sketchAddress);
  CHECK(document.sketches().front().geometry.semanticFingerprint() ==
        before.sketchFingerprint);
  CHECK(document.referenceImages().size() == before.imageCount);
  CHECK(&document.referenceImages().front() == before.imageAddress);
  CHECK(document.bodies().size() == 1);
  const Body& body = document.bodies().front();
  CHECK(&body == before.bodyAddress);
  CHECK(body.features().size() == before.features.size());
  for (std::size_t index = 0; index < before.features.size(); ++index) {
    const ShapeFeature& feature = *body.features().at(index);
    const FeatureSnapshot& expected = before.features.at(index);
    CHECK(&feature == expected.address);
    CHECK(feature.id() == expected.id);
    CHECK(feature.state() == expected.state);
    CHECK(feature.shape().get() == expected.shape);
    CHECK(feature.lastValidShape().get() == expected.lastValidShape);
    CHECK(feature.shapeRevision() == expected.revision);
  }
  const auto* imported =
      dynamic_cast<const ImportedShapeFeature*>(body.features().front().get());
  CHECK(imported);
  CHECK(imported->importedShape().get() == before.importedShape);
  CHECK(imported->historyArchive() == before.importedHistory);
  if (before.serializedModel)
    CHECK(documentFingerprint(document) == *before.serializedModel);
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

void checkInvalid(const ProductEnvelopeV5ConversionResult& result) {
  CHECK(result.status == ProductEnvelopeV5ConversionStatus::Invalid);
  CHECK(!result.succeeded());
  CHECK(!result.prepared);
  CHECK(!result.error.isEmpty());
}

Document oversizedDocument() {
  Document document = modelDocument();
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  constexpr int kImageCount = 1500;
  const std::string field(15000, 'x');
  for (int index = 0; index < kImageCount; ++index) {
    ReferenceImage image;
    image.id = static_cast<ReferenceImageId>(900000 + index);
    image.name = field;
    image.sourcePath = field;
    image.supportName = field;
    image.pixelWidth = 1;
    image.pixelHeight = 1;
    document.addReferenceImage(std::move(image));
  }
  return document;
}

}  // namespace

int main() {
  // Preparation injects only caller-owned persistent identities. The source
  // remains a standalone document with the same ownership, model, history,
  // geometry caches, fingerprints and addresses.
  {
    auto source = product(modelDocument());
    const SourceSnapshot before = snapshot(*source);
    const AutoIds autoBefore = consumeAutoIds("before-success");
    const auto converted = prepareStandaloneProductEnvelopeV5Conversion(
        *source, kProject, kOutputRoot, metadata());
    const AutoIds autoAfter = consumeAutoIds("after-success");
    checkOnlyOneAutoAdvance(autoBefore, autoAfter);
    CHECK(converted.succeeded());
    CHECK(converted.prepared);
    CHECK(converted.prepared->identity.projectId == kProject);
    const QByteArray expectedName = QByteArray::fromHex(
        "d098d0b7d0b4d0b5d0bbd0b8d0b520cea92fe99baa");
    const QByteArray expectedCreatedAt = QByteArray::fromHex(
        "323032362d31302d32335431323a33343a35365a2fd182d0bed187d0bdd0be");
    CHECK(converted.prepared->identity.metadata.nameUtf8 ==
          std::string(expectedName.constData(), expectedName.size()));
    CHECK(converted.prepared->identity.metadata.createdAtUtf8 ==
          std::string(expectedCreatedAt.constData(), expectedCreatedAt.size()));
    CHECK(converted.prepared->rootDefinitionId == kOutputRoot);
    CHECK(!source->identity());
    checkUnchanged(*source, before);

    const auto scanned = scanProductEnvelopeV5(
        QByteArrayView(converted.prepared->payload.bytes));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(scanned.candidate);
    CHECK(scanned.candidate->projectId == kProject);
    CHECK(scanned.candidate->rootDefinitionId == kOutputRoot);
    CHECK(scanned.candidate->definitionId == kOutputRoot);
    CHECK(scanned.candidate->metadata == metadata());

    const auto materialized = materializeProductEnvelopeV5(*scanned.candidate);
    CHECK(materialized.succeeded());
    CHECK(materialized.product);
    CHECK(materialized.product->identity() == converted.prepared->identity);
    CHECK(materialized.product->rootDefinitionId() == kOutputRoot);
    CHECK(materialized.product->definitions().size() == 1);
    CHECK(&materialized.product->rootDocument() != &source->rootDocument());
    CHECK(documentFingerprint(materialized.product->rootDocument()) ==
          *before.serializedModel);
    checkUnchanged(*source, before);
  }

  // Validation failures preserve the same source snapshot and do not advance
  // any global edit-ID generator.
  {
    auto source = product(modelDocument());
    const SourceSnapshot before = snapshot(*source);
    const AutoIds autoBefore = consumeAutoIds("before-invalid");
    checkInvalid(prepareStandaloneProductEnvelopeV5Conversion(
        *source, ProjectId{}, kOutputRoot, metadata()));
    checkUnchanged(*source, before);
    checkInvalid(prepareStandaloneProductEnvelopeV5Conversion(
        *source, kProject, DefinitionId{}, metadata()));
    checkUnchanged(*source, before);
    const DefinitionId aboveMaximum{
        static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId) + 1};
    checkInvalid(prepareStandaloneProductEnvelopeV5Conversion(
        *source, kProject, aboveMaximum, metadata()));
    checkUnchanged(*source, before);
    const AutoIds autoAfter = consumeAutoIds("after-invalid");
    checkOnlyOneAutoAdvance(autoBefore, autoAfter);
  }

  // The maximum persisted root is accepted exactly, without replacing the
  // transient source root.
  {
    auto source = product(modelDocument());
    const SourceSnapshot before = snapshot(*source);
    const DefinitionId maximum{
        static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId)};
    const auto converted = prepareStandaloneProductEnvelopeV5Conversion(
        *source, kProject, maximum, metadata());
    CHECK(converted.succeeded());
    CHECK(converted.prepared->rootDefinitionId == maximum);
    const auto scanned = scanProductEnvelopeV5(
        QByteArrayView(converted.prepared->payload.bytes));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(scanned.candidate);
    CHECK(scanned.candidate->rootDefinitionId == maximum);
    CHECK(scanned.candidate->definitionId == maximum);
    checkUnchanged(*source, before);
  }

  // Identity-bearing and multi-definition inputs are rejected by conversion
  // preconditions, before the encoder can inspect the nested model.
  {
    ProductDocumentIdentity identity{
        kProject, {"existing", "2026-10-23T00:00:00Z"}};
    auto identified = product(modelDocument(), identity);
    const SourceSnapshot before = snapshot(*identified);
    const auto converted = prepareStandaloneProductEnvelopeV5Conversion(
        *identified, kProject, kOutputRoot, metadata());
    checkInvalid(converted);
    CHECK(converted.error.contains(QStringLiteral("already")));
    checkUnchanged(*identified, before);
  }
  {
    auto multiple = product(modelDocument());
    CHECK(multiple->insert(PartDefinition(
              DefinitionId{8}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    const SourceSnapshot before = snapshot(*multiple);
    const auto converted = prepareStandaloneProductEnvelopeV5Conversion(
        *multiple, kProject, kOutputRoot, metadata());
    checkInvalid(converted);
    CHECK(converted.error.contains(QStringLiteral("one definition")));
    checkUnchanged(*multiple, before);
  }

  // Nested model validation remains an Invalid conversion result, with the
  // malformed source untouched.
  {
    Document invalid = modelDocument();
    auto& sketch = *invalid.findSketch(kSketch);
    sketch.support.type = SketchSupportType::Face;
    auto source = product(std::move(invalid));
    const SourceSnapshot before = snapshot(*source, false);
    const auto converted = prepareStandaloneProductEnvelopeV5Conversion(
        *source, kProject, kOutputRoot, metadata());
    checkInvalid(converted);
    checkUnchanged(*source, before);
  }

  // Whole-payload overflow keeps its encoder classification and the large
  // source carrier remains byte-for-byte/address stable at sampled bounds.
  {
    auto source = product(oversizedDocument());
    const SourceSnapshot before = snapshot(*source, false);
    const auto* const last = &source->rootDocument().referenceImages().back();
    const std::string lastSupport = last->supportName;
    const auto converted = prepareStandaloneProductEnvelopeV5Conversion(
        *source, kProject, kOutputRoot, metadata());
    CHECK(converted.status ==
          ProductEnvelopeV5ConversionStatus::PayloadTooLarge);
    CHECK(!converted.succeeded());
    CHECK(!converted.prepared);
    CHECK(!converted.error.isEmpty());
    checkUnchanged(*source, before);
    CHECK(source->rootDocument().referenceImages().size() == 1501);
    CHECK(&source->rootDocument().referenceImages().back() == last);
    CHECK(last->supportName == lastSupport);
  }

  return EXIT_SUCCESS;
}

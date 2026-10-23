#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectIdCodec.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr DefinitionId kRootDefinition{17};
constexpr BodyId kBody{10};
constexpr FeatureId kImported{1};
constexpr ProjectId kProjectA{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
constexpr ProjectId kProjectB{
    {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x49, 0x88,
     0x87, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00}};

ProductEnvelopeV5Metadata metadata() {
  return {QString::fromUtf8("Изделие Ω"),
          QStringLiteral("2026-10-23T12:34:56Z")};
}

ShapeFeature::ShapePtr box() {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape());
}

std::unique_ptr<ProductDocument> product(
    Document document = {}, std::optional<ProjectId> id = std::nullopt,
    DefinitionId root = kRootDefinition) {
  DefinitionRegistry registry;
  CHECK(registry.insert(
            PartDefinition(root,
                           std::make_unique<Document>(std::move(document)))) ==
        DefinitionInsertStatus::Inserted);
  std::optional<ProductDocumentIdentity> identity;
  if (id) {
    identity.emplace(ProductDocumentIdentity{
        *id, {"source product", "2026-10-23T00:00:00Z"}});
  }
  auto created =
      ProductDocument::create(std::move(registry), root, std::move(identity));
  CHECK(created);
  return std::move(created.product);
}

Document topologyDocument() {
  Document document;
  auto& body = document.addBody(kBody, "root");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kImported, box(), "archive"));
  EdgeReference reference = EdgeReference::fromV2(
      {kBody,
       TopologyProvenanceKey{kImported, TopologyKind::Edge,
                             TopologyProvenanceOperation::Generated,
                             TopologySemanticRole::Custom, "v2-edge",
                             {{kImported, TopologyKind::Edge,
                               TopologyProvenanceOperation::Seed,
                               TopologySemanticRole::Custom, "origin"}}}});
  body.addFeature(std::make_unique<FilletFeature>(
      2, std::vector<EdgeReference>{std::move(reference)}, 0.25, "fillet"));
  return document;
}

QJsonObject nestedRoot(const QJsonObject& root) {
  return root.value("definitions").toArray().at(0).toObject()
      .value("payload").toObject().value("data").toObject();
}

QJsonArray nestedFeatures(const QJsonObject& root) {
  return nestedRoot(root).value("model").toObject().value("bodies")
      .toArray().at(0).toObject().value("features").toArray();
}

void checkInvalid(const ProductEnvelopeV5EncodeResult& result) {
  CHECK(!result.succeeded());
  CHECK(!result.payload);
  CHECK(!result.error.isEmpty());
}

void writeAll(const QString& path, QByteArrayView bytes) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  CHECK(file.write(bytes.data(), bytes.size()) == bytes.size());
  CHECK(file.flush());
}

}  // namespace

int main() {
  using solidar::project::detail::formatCanonicalProjectId;
  using solidar::project::detail::parseCanonicalProjectId;

  // The shared non-Qt codec is strict and round-trips only canonical,
  // lowercase, non-nil UUIDs.
  {
    const auto formatted = formatCanonicalProjectId(kProjectA);
    CHECK(formatted ==
          std::optional<std::string>{"123e4567-e89b-42d3-a456-426614174000"});
    CHECK(parseCanonicalProjectId(*formatted) == kProjectA);
    CHECK(!formatCanonicalProjectId(ProjectId{}));
    for (const std::string_view invalid : {
             "123E4567-e89b-42d3-a456-426614174000",
             "{123e4567-e89b-42d3-a456-426614174000}",
             "123e4567e89b42d3a456426614174000",
             "00000000-0000-0000-0000-000000000000",
             "123e4567-e89b-42d3-a456-42661417400g"}) {
      CHECK(!parseCanonicalProjectId(invalid));
    }
  }

  // Explicit output identity is independent of the optional source identity.
  // Encoding is deterministic and leaves all ownership/B-Rep addresses intact.
  {
    auto standalone = product(topologyDocument());
    const Document* const documentAddress = &standalone->rootDocument();
    const auto* const feature =
        standalone->rootDocument().findFeature(kBody, kImported);
    CHECK(feature);
    const auto* const importedFeature =
        dynamic_cast<const ImportedShapeFeature*>(feature);
    CHECK(importedFeature);
    const auto importedShapeAddress = importedFeature->importedShape().get();
    const auto shapeAddress = feature->lastValidShape().get();

    const auto first =
        encodeProductEnvelopeV5Root(*standalone, kProjectA, metadata());
    const auto second =
        encodeProductEnvelopeV5Root(*standalone, kProjectA, metadata());
    CHECK(first.succeeded());
    CHECK(second.succeeded());
    CHECK(first.payload->root == second.payload->root);
    CHECK(first.payload->bytes == second.payload->bytes);
    CHECK(first.payload->bytes == QJsonDocument(first.payload->root)
                                       .toJson(QJsonDocument::Indented));
    CHECK(!standalone->projectId());
    CHECK(&standalone->rootDocument() == documentAddress);
    CHECK(standalone->rootDocument().findFeature(kBody, kImported) == feature);
    CHECK(importedFeature->importedShape().get() == importedShapeAddress);
    CHECK(feature->lastValidShape().get() == shapeAddress);

    const QJsonObject& root = first.payload->root;
    CHECK(root.size() == 7);
    CHECK(root.value("format") == QStringLiteral("solidar-project"));
    CHECK(root.value("version") == 5);
    CHECK(root.value("capabilities").toArray() ==
          QJsonArray{QStringLiteral("product-envelope-v1")});
    CHECK(root.value("projectId") ==
          QStringLiteral("123e4567-e89b-42d3-a456-426614174000"));
    CHECK(root.value("rootDefinitionId").toInteger() == 17);
    const QJsonObject expectedMetadata{{"name", metadata().name},
                                       {"createdAt", metadata().createdAt}};
    CHECK(root.value("metadata").toObject() == expectedMetadata);
    const QJsonArray definitions = root.value("definitions").toArray();
    CHECK(definitions.size() == 1);
    const QJsonObject definition = definitions.at(0).toObject();
    CHECK(definition.size() == 3);
    CHECK(definition.value("definitionId").toInteger() == 17);
    CHECK(definition.value("definitionKind") == QStringLiteral("part"));
    CHECK(definition.value("payload").toObject().value("payloadKind") ==
          QStringLiteral("embedded-part-v4"));
    const QJsonObject embedded = nestedRoot(root);
    CHECK(embedded.value("version") == 4);
    CHECK(embedded.value("name") == metadata().name);
    CHECK(embedded.value("createdAt") == metadata().createdAt);

    const auto scanned =
        scanProductEnvelopeV5(QByteArrayView(first.payload->bytes));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(scanned.candidate);
    CHECK(scanned.candidate->authoredRoot == root);
    CHECK(scanned.candidate->projectId == kProjectA);
    CHECK(scanned.candidate->rootDefinitionId == kRootDefinition);
    CHECK(scanned.candidate->definitionId == kRootDefinition);
    CHECK(scanned.candidate->metadata == metadata());
    CHECK(scanned.candidate->embeddedPart);
    CHECK(scanned.candidate->deferredUnknowns.empty());

    // V2 reference/provenance survives through the existing v4 codec. Only
    // ImportedShape carries a B-Rep archive; derived feature JSON does not.
    const QJsonArray features = nestedFeatures(root);
    CHECK(features.size() == 2);
    const QJsonObject imported = features.at(0).toObject();
    const QJsonObject fillet = features.at(1).toObject();
    CHECK(imported.value("id").toInteger() == kImported);
    CHECK(fillet.value("id").toInteger() == 2);
    CHECK(imported.contains("brep"));
    CHECK(!fillet.contains("brep"));
    CHECK(!fillet.contains("lastValidShape"));
    const QJsonObject edge = fillet.value("edges")
                                 .toArray()
                                 .at(0)
                                 .toObject()
                                 .value("reference")
                                 .toObject();
    CHECK(edge.value("referenceVersion") == QStringLiteral("V2"));
    CHECK(edge.value("bodyId").toInteger() == kBody);
    CHECK(edge.value("provenance")
              .toObject()
              .value("featureId")
              .toInteger() == kImported);
    CHECK(edge.value("provenance").toObject().value("origins")
              .toArray().size() == 1);
  }

  // The compatibility overload preserves the source carrier root, while the
  // explicit-root overload injects a distinct persisted output identity and
  // validates its self-scan against that output value.
  {
    auto source = product();
    constexpr DefinitionId kOutputRoot{4242};
    const auto compatible =
        encodeProductEnvelopeV5Root(*source, kProjectA, metadata());
    const auto explicitRoot = encodeProductEnvelopeV5Root(
        *source, kProjectA, kOutputRoot, metadata());
    CHECK(compatible.succeeded());
    CHECK(explicitRoot.succeeded());
    CHECK(source->rootDefinitionId() == kRootDefinition);
    CHECK(compatible.payload->root.value("rootDefinitionId").toInteger() ==
          static_cast<qint64>(kRootDefinition.value()));
    CHECK(compatible.payload->root.value("definitions")
              .toArray()
              .at(0)
              .toObject()
              .value("definitionId")
              .toInteger() == static_cast<qint64>(kRootDefinition.value()));
    CHECK(explicitRoot.payload->root.value("rootDefinitionId").toInteger() ==
          static_cast<qint64>(kOutputRoot.value()));
    CHECK(explicitRoot.payload->root.value("definitions")
              .toArray()
              .at(0)
              .toObject()
              .value("definitionId")
              .toInteger() == static_cast<qint64>(kOutputRoot.value()));
    const auto scanned =
        scanProductEnvelopeV5(QByteArrayView(explicitRoot.payload->bytes));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(scanned.candidate);
    CHECK(scanned.candidate->rootDefinitionId == kOutputRoot);
    CHECK(scanned.candidate->definitionId == kOutputRoot);
  }

  {
    auto identified = product(Document{}, kProjectA);
    CHECK(identified->identity());
    const auto sourceIdentity = *identified->identity();
    const auto encoded =
        encodeProductEnvelopeV5Root(*identified, kProjectB, metadata());
    CHECK(encoded.succeeded());
    CHECK(identified->identity() == sourceIdentity);
    CHECK(identified->projectId() == kProjectA);
    CHECK(encoded.payload->root.value("projectId") ==
          QStringLiteral("ffeeddcc-bbaa-4988-8766-554433221100"));
  }

  // Invalid identity, registry cardinality, persisted ID range and nested
  // v4 model state all fail before any external effect.
  {
    auto valid = product();
    checkInvalid(encodeProductEnvelopeV5Root(*valid, ProjectId{}, metadata()));

    CHECK(valid->insert(PartDefinition(
              DefinitionId{18}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    checkInvalid(encodeProductEnvelopeV5Root(*valid, kProjectA, metadata()));
  }
  {
    auto tooLarge = product(
        Document{}, std::nullopt,
        DefinitionId{static_cast<std::uint64_t>(
                         ProjectFile::kMaximumPersistedId) +
                     1});
    checkInvalid(
        encodeProductEnvelopeV5Root(*tooLarge, kProjectA, metadata()));
  }
  {
    const DefinitionId maximumId{
        static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId)};
    auto boundary = product(Document{}, std::nullopt, maximumId);
    const auto encoded =
        encodeProductEnvelopeV5Root(*boundary, kProjectA, metadata());
    CHECK(encoded.succeeded());
    CHECK(encoded.payload->root.value("rootDefinitionId").toInteger() ==
          ProjectFile::kMaximumPersistedId);
    CHECK(encoded.payload->root.value("definitions")
              .toArray()
              .at(0)
              .toObject()
              .value("definitionId")
              .toInteger() == ProjectFile::kMaximumPersistedId);
  }
  {
    Document invalid;
    auto& sketch = invalid.addSketch("invalid support");
    sketch.support.type = SketchSupportType::Face;
    auto source = product(std::move(invalid));
    checkInvalid(encodeProductEnvelopeV5Root(*source, kProjectA, metadata()));
  }

  // Product v5 stays disconnected from the production part-only dispatcher.
  {
    auto source = product();
    const auto encoded =
        encodeProductEnvelopeV5Root(*source, kProjectA, metadata());
    CHECK(encoded.succeeded());
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("v5-dispatch-XXXXXX")));
    CHECK(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("product.solidar"));
    writeAll(path, QByteArrayView(encoded.payload->bytes));
    const auto staged = ProjectFile::stageLoad(path);
    CHECK(staged.status == ProjectLoadStatus::Unsupported);
    CHECK(staged.sourceVersion == 5);
    CHECK(staged.payloadKind == ProjectPayloadKind::None);
    CHECK(!staged.document);
  }

  return EXIT_SUCCESS;
}

#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/FilletFeature.h"
#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/MoveFeature.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/ProductEnvelopeV5Materialization.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr DefinitionId kRootDefinition{17};
constexpr BodyId kBody{900005};
constexpr FeatureId kImported{900006};
constexpr FeatureId kDerived{900007};
constexpr FeatureId kFillet{900008};
constexpr BodyId kOtherBody{900009};
constexpr FeatureId kOtherImported{900010};
constexpr SketchId kSketch{900003};
constexpr ReferenceImageId kImage{900004};
constexpr ProjectId kProjectA{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
constexpr ProjectId kProjectB{
    {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x49, 0x88,
     0x87, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00}};

ShapeFeature::ShapePtr box() {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape());
}

ShapeFeature::ShapePtr boxAt(double x) {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(gp_Pnt{x, 0.0, 0.0}, 20.0, 10.0, 5.0).Shape());
}

ProductEnvelopeV5Metadata metadata() {
  return {QString::fromUtf8("Изделие Ω"),
          QStringLiteral("2026-10-23T12:34:56Z")};
}

std::unique_ptr<ProductDocument> sourceProduct(Document document) {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            kRootDefinition,
            std::make_unique<Document>(std::move(document)))) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), kRootDefinition);
  CHECK(created);
  return std::move(created.product);
}

ProductEnvelopeV5Candidate candidateFor(
    Document document, const ProductEnvelopeV5Metadata& authoredMetadata =
                           metadata()) {
  auto source = sourceProduct(std::move(document));
  const auto encoded =
      encodeProductEnvelopeV5Root(*source, kProjectA, authoredMetadata);
  CHECK(encoded.succeeded());
  auto scanned = scanProductEnvelopeV5(QByteArrayView(encoded.payload->bytes));
  CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
  CHECK(scanned.candidate);
  return std::move(*scanned.candidate);
}

ProductEnvelopeV5Candidate candidateForRoot(const QJsonObject& root) {
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  auto scanned = scanProductEnvelopeV5(QByteArrayView(bytes));
  CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
  CHECK(scanned.candidate);
  return std::move(*scanned.candidate);
}

Document importedDocument(bool includeDerived = false) {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document document;
  document.addSketch(kSketch, "persisted sketch");
  ReferenceImage image;
  image.id = kImage;
  image.name = "persisted image";
  image.sourcePath = "image.png";
  image.supportName = "XY";
  image.pixelWidth = 10;
  image.pixelHeight = 10;
  document.addReferenceImage(std::move(image));
  auto& body = document.addBody(kBody, "root");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kImported, box(), "archive"));
  if (includeDerived) {
    body.addFeature(std::make_unique<MoveFeature>(
        kDerived, kImported, Vector3d{1.0, 2.0, 3.0}, "move"));
  }
  return document;
}

Document missingV2Document() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document document;
  auto& body = document.addBody(kBody, "root");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kImported, box(), "archive"));
  const EdgeReference missing = EdgeReference::fromV2(
      {kBody,
       TopologyProvenanceKey{kImported, TopologyKind::Edge,
                             TopologyProvenanceOperation::Generated,
                             TopologySemanticRole::Custom,
                             "definitely-missing-v2",
                             {{kImported, TopologyKind::Face,
                               TopologyProvenanceOperation::Seed,
                               TopologySemanticRole::Cap,
                               "authored-origin"}}}});
  body.addFeature(std::make_unique<FilletFeature>(
      kFillet, std::vector<EdgeReference>{missing}, 0.5, "missing v2"));
  return document;
}

Document invalidRecomputeDocument() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document document;
  auto& first = document.addBody(kBody, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      kImported, boxAt(0.0), "first archive"));
  auto& second = document.addBody(kOtherBody, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      kOtherImported, boxAt(100.0), "second archive"));
  document.findBody(kBody)->addFeature(std::make_unique<JoinBodiesFeature>(
      kDerived, kBody, kImported, kOtherBody, kOtherImported,
      "disconnected join"));
  return document;
}

bool equivalentV4Candidate(const ProjectFormatV4ModelCandidate& lhs,
                           const ProjectFormatV4ModelCandidate& rhs) {
  const auto& ll = lhs.legacySketch;
  const auto& rl = rhs.legacySketch;
  const auto& lt = ll.topology;
  const auto& rt = rl.topology;
  const auto& lp = lt.payload;
  const auto& rp = rt.payload;
  const auto& le = lp.envelope;
  const auto& re = rp.envelope;
  return ll.deferredLegacySketch == rl.deferredLegacySketch &&
         le.authoredRoot == re.authoredRoot && le.document == re.document &&
         le.legacySketches == re.legacySketches &&
         le.legacyExtrusion == re.legacyExtrusion && le.model == re.model &&
         le.capabilities == re.capabilities &&
         le.deferredUnknowns == re.deferredUnknowns &&
         lp.topologySites == rp.topologySites &&
         lp.featureOwners == rp.featureOwners &&
         lt.topologyReferences == rt.topologyReferences &&
         lt.topologyInspections == rt.topologyInspections &&
         lt.deferredUnsupported == rt.deferredUnsupported &&
         lt.provenanceRecordCount == rt.provenanceRecordCount &&
         lt.localNameUtf8Bytes == rt.localNameUtf8Bytes;
}

bool equivalentCarrier(const ProductEnvelopeV5Candidate& lhs,
                       const ProductEnvelopeV5Candidate& rhs) {
  return lhs.authoredRoot == rhs.authoredRoot &&
         lhs.projectId == rhs.projectId &&
         lhs.rootDefinitionId == rhs.rootDefinitionId &&
         lhs.definitionId == rhs.definitionId &&
         lhs.metadata == rhs.metadata &&
         lhs.capabilities == rhs.capabilities &&
         lhs.authoredDefinition == rhs.authoredDefinition &&
         lhs.authoredPayload == rhs.authoredPayload &&
         lhs.deferredUnknowns == rhs.deferredUnknowns &&
         lhs.embeddedPart.has_value() == rhs.embeddedPart.has_value() &&
         (!lhs.embeddedPart ||
          equivalentV4Candidate(*lhs.embeddedPart, *rhs.embeddedPart));
}

void checkInvalid(const ProductEnvelopeV5MaterializationResult& result) {
  CHECK(result.status == ProductEnvelopeV5MaterializationStatus::Invalid);
  CHECK(!result.succeeded());
  CHECK(!result.product);
  CHECK(!result.error.isEmpty());
}

void checkUnsupported(const ProductEnvelopeV5MaterializationResult& result) {
  CHECK(result.status == ProductEnvelopeV5MaterializationStatus::Unsupported);
  CHECK(!result.succeeded());
  CHECK(!result.product);
  CHECK(!result.error.isEmpty());
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
  auto feature = std::make_unique<ImportedShapeFeature>(box(), "feature");
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

QJsonObject withInvalidBRep(QJsonObject root) {
  auto definitions = root.value("definitions").toArray();
  auto definition = definitions[0].toObject();
  auto payload = definition.value("payload").toObject();
  auto nested = payload.value("data").toObject();
  auto model = nested.value("model").toObject();
  auto bodies = model.value("bodies").toArray();
  auto body = bodies[0].toObject();
  auto features = body.value("features").toArray();
  auto imported = features[0].toObject();
  imported["brep"] = QStringLiteral("TQ==");
  features[0] = imported;
  body["features"] = features;
  bodies[0] = body;
  model["bodies"] = bodies;
  nested["model"] = model;
  payload["data"] = nested;
  definition["payload"] = payload;
  definitions[0] = definition;
  root["definitions"] = definitions;
  return root;
}

QJsonObject withV1FilletReference(QJsonObject root) {
  auto definitions = root.value("definitions").toArray();
  auto definition = definitions[0].toObject();
  auto payload = definition.value("payload").toObject();
  auto nested = payload.value("data").toObject();
  auto model = nested.value("model").toObject();
  auto bodies = model.value("bodies").toArray();
  auto body = bodies[0].toObject();
  auto features = body.value("features").toArray();
  auto fillet = features[1].toObject();
  fillet["edges"] = QJsonArray{QJsonObject{
      {"reference",
       QJsonObject{{"referenceVersion", "V1"},
                   {"bodyId", static_cast<qint64>(kBody)},
                   {"featureId", static_cast<qint64>(kImported)},
                   {"kind", "Edge"},
                   {"legacyIndex", 0}}}}};
  features[1] = fillet;
  body["features"] = features;
  bodies[0] = body;
  model["bodies"] = bodies;
  nested["model"] = model;
  payload["data"] = nested;
  definition["payload"] = payload;
  definitions[0] = definition;
  root["definitions"] = definitions;
  return root;
}

}  // namespace

int main() {
  // Successful staging owns one recomputed root definition and the complete
  // identity bundle, while leaving both the carrier and global cursors alone.
  {
    auto candidate = candidateFor(importedDocument(true));
    const auto candidateBefore = candidate;
    const AutoIds before = consumeAutoIds("before-success");
    auto result = materializeProductEnvelopeV5(candidate);
    const AutoIds after = consumeAutoIds("after-success");
    checkOnlyOneAutoAdvance(before, after);
    CHECK(result.succeeded());
    CHECK(equivalentCarrier(candidate, candidateBefore));
    CHECK(result.product->definitions().size() == 1);
    CHECK(result.product->rootDefinitionId() == kRootDefinition);
    CHECK(result.product->projectId() == kProjectA);
    CHECK(result.product->identity());
    const std::string expectedName =
        "\xD0\x98\xD0\xB7\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8\xD0\xB5"
        " \xCE\xA9";
    CHECK(result.product->identity()->metadata.nameUtf8 == expectedName);
    CHECK(result.product->identity()->metadata.createdAtUtf8 ==
          "2026-10-23T12:34:56Z");

    const Document& document = result.product->rootDocument();
    CHECK(document.sketches().size() == 1);
    CHECK(document.sketches()[0].id == kSketch);
    CHECK(document.referenceImages().size() == 1);
    CHECK(document.referenceImages()[0].id == kImage);
    CHECK(document.bodies().size() == 1);
    CHECK(document.bodies()[0].id() == kBody);
    CHECK(document.bodies()[0].features().size() == 2);
    const auto* imported = dynamic_cast<const ImportedShapeFeature*>(
        document.bodies()[0].features()[0].get());
    const auto* moved = dynamic_cast<const MoveFeature*>(
        document.bodies()[0].features()[1].get());
    CHECK(imported && moved);
    CHECK(imported->id() == kImported);
    CHECK(moved->id() == kDerived);
    CHECK(imported->importedShape());
    CHECK(imported->shape());
    CHECK(moved->shape());
    CHECK(moved->sourceFeatureId() == kImported);
    CHECK(moved->offsetMm().x == 1.0);
    CHECK(moved->offsetMm().y == 2.0);
    CHECK(moved->offsetMm().z == 3.0);

    // Staging never reserves global generators. The application can do so
    // explicitly after it commits ownership.
    result.product->rootDocument().reserveIdsForEditing();
    const AutoIds reserved = consumeAutoIds("after-explicit-reserve");
    CHECK(reserved.sketch > kSketch);
    CHECK(reserved.image > kImage);
    CHECK(reserved.body > kBody);
    CHECK(reserved.feature > kDerived);
  }

  // Legacy V1 references remain supported by materialization even though the
  // current writer authors V2 references only.
  {
    const auto v2 = candidateFor(missingV2Document());
    auto v1 = candidateForRoot(withV1FilletReference(v2.authoredRoot));
    auto result = materializeProductEnvelopeV5(v1);
    CHECK(result.succeeded());
    const auto* fillet = dynamic_cast<const FilletFeature*>(
        result.product->rootDocument().bodies()[0].features()[1].get());
    CHECK(fillet);
    CHECK(fillet->edges().size() == 1);
    CHECK(fillet->edges()[0].isV1());
    CHECK(fillet->edges()[0].bodyId() == kBody);
    CHECK(fillet->edges()[0].featureId() == kImported);
    CHECK(fillet->edges()[0].edgeIndex() == 0);
    CHECK(fillet->shape());
  }

  // A canonical carrier with unknown required semantics retains the scanner's
  // forward-compatible Unsupported verdict and never reaches materialization.
  {
    const auto valid = candidateFor(importedDocument());
    QJsonObject root = valid.authoredRoot;
    root["capabilities"] = QJsonArray{
        QStringLiteral("product-envelope-v1"),
        QStringLiteral("z-future-product-capability")};
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    auto scanned = scanProductEnvelopeV5(QByteArrayView(bytes));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Unsupported);
    CHECK(scanned.candidate);
    checkUnsupported(materializeProductEnvelopeV5(*scanned.candidate));
  }

  // A valid V2 carrier with an unresolved authored topology reference is an
  // accepted staged product; the reference and provenance remain exact.
  {
    auto source = missingV2Document();
    const auto* sourceFillet =
        dynamic_cast<const FilletFeature*>(source.bodies()[0].features()[1].get());
    CHECK(sourceFillet);
    const auto expected = sourceFillet->edges();
    auto candidate = candidateFor(std::move(source));
    auto result = materializeProductEnvelopeV5(candidate);
    CHECK(result.succeeded());
    const auto* fillet = dynamic_cast<const FilletFeature*>(
        result.product->rootDocument().bodies()[0].features()[1].get());
    CHECK(fillet);
    CHECK(fillet->edges() == expected);
    CHECK(fillet->edges()[0].v2());
    CHECK(fillet->edges()[0].v2()->provenance.origins.size() == 1);
    CHECK(fillet->errorCode() ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(!fillet->shape());
  }

  // Every public carrier projection is untrusted and must still match the
  // Candidate rebuilt from authoredRoot.
  {
    const auto pristine = candidateFor(importedDocument());
    const std::vector<std::function<void(ProductEnvelopeV5Candidate&)>>
        tamperers{
            [](auto& value) {
              value.authoredRoot["projectId"] =
                  QStringLiteral("ffeeddcc-bbaa-4988-8766-554433221100");
            },
            [](auto& value) { value.projectId = kProjectB; },
            [](auto& value) { value.rootDefinitionId = DefinitionId{18}; },
            [](auto& value) { value.definitionId = DefinitionId{18}; },
            [](auto& value) { value.metadata.name += QStringLiteral("x"); },
            [](auto& value) {
              value.capabilities.push_back(QStringLiteral("future"));
            },
            [](auto& value) {
              value.authoredDefinition["definitionKind"] =
                  QStringLiteral("assembly");
            },
            [](auto& value) {
              value.authoredPayload["payloadKind"] =
                  QStringLiteral("future");
            },
            [](auto& value) {
              ++value.embeddedPart->legacySketch.topology.provenanceRecordCount;
            },
            [](auto& value) {
              value.deferredUnknowns.push_back(
                  {ProductEnvelopeV5DeferredUnknownKind::Capability,
                   QStringLiteral("$.capabilities[1]"),
                   QStringLiteral("future")});
            },
            [](auto& value) { value.embeddedPart.reset(); }};
    for (const auto& tamper : tamperers) {
      auto candidate = pristine;
      tamper(candidate);
      checkInvalid(materializeProductEnvelopeV5(candidate));
    }
  }

  // Structurally valid Base64 can survive scanning but fail late OCCT B-Rep
  // materialization. No partial ProductDocument escapes.
  {
    const auto valid = candidateFor(importedDocument());
    auto invalid = candidateForRoot(withInvalidBRep(valid.authoredRoot));
    const AutoIds before = consumeAutoIds("before-brep-failure");
    const auto result = materializeProductEnvelopeV5(invalid);
    const AutoIds after = consumeAutoIds("after-brep-failure");
    checkOnlyOneAutoAdvance(before, after);
    checkInvalid(result);
    CHECK(result.error.contains(QStringLiteral("materialization failed")));
  }

  // A non-topology recompute failure is not one of the explicitly permitted
  // staged misses and rejects the complete product.
  {
    auto candidate = candidateFor(invalidRecomputeDocument());
    const AutoIds before = consumeAutoIds("before-recompute-failure");
    const auto result = materializeProductEnvelopeV5(candidate);
    const AutoIds after = consumeAutoIds("after-recompute-failure");
    checkOnlyOneAutoAdvance(before, after);
    checkInvalid(result);
    CHECK(result.error.contains(QStringLiteral("recompute was rejected")));
  }

  return EXIT_SUCCESS;
}

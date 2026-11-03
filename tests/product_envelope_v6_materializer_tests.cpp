#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "project/ProductEnvelopeV6Encoder.h"
#include "project/ProductEnvelopeV6Materialization.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};

ProductEnvelopeV5Metadata metadata() {
  return {QStringLiteral("assembly"),
          QStringLiteral("2026-10-23T00:00:00Z")};
}

QByteArray fixtureBytes() {
  QFile file(QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH));
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

ShapeFeature::ShapePtr boxAt(double x) {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(gp_Pnt{x, 0.0, 0.0}, 20.0, 10.0, 5.0).Shape());
}

Document invalidRecomputeDocument() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document document;
  auto& first = document.addBody(900001, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      900002, boxAt(0.0), "first archive"));
  auto& second = document.addBody(900003, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      900004, boxAt(100.0), "second archive"));
  document.findBody(900001)->addFeature(std::make_unique<JoinBodiesFeature>(
      900005, 900001, 900002, 900003, 900004, "disconnected join"));
  return document;
}

std::unique_ptr<ProductDocument> productWithParts(Document firstPart,
                                                  Document secondPart) {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{1},
            std::make_unique<Document>(std::move(firstPart)))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{2},
            std::make_unique<Document>(std::move(secondPart)))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition assembly(DefinitionId{100});
  CHECK(assembly.insert(ComponentOccurrence(
            OccurrenceId{11}, DefinitionId{1},
            RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(assembly)) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(definitions),
                                         DefinitionId{100});
  CHECK(created);
  return std::move(created.product);
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
  auto feature = std::make_unique<ImportedShapeFeature>(boxAt(0.0), "shape");
  ids.feature = feature->id();
  body.addFeature(std::move(feature));
  return ids;
}

void checkOnlyOneAdvance(const AutoIds& before, const AutoIds& after) {
  CHECK(after.sketch == before.sketch + 1);
  CHECK(after.image == before.image + 1);
  CHECK(after.body == before.body + 1);
  CHECK(after.feature == before.feature + 1);
}

}  // namespace

int main() {
  auto scanned = scanProductEnvelopeV6(QByteArrayView(fixtureBytes()));
  CHECK(scanned.status == ProductEnvelopeV6ScanStatus::Candidate);
  CHECK(scanned.candidate);
  auto materialized = materializeProductEnvelopeV6(*scanned.candidate);
  CHECK(materialized.succeeded());
  CHECK(materialized.product->rootDefinitionId() == DefinitionId{100});
  CHECK(materialized.product->projectId() == kProject);
  CHECK(materialized.product->definitions().definitionIds() ==
        std::vector<DefinitionId>({DefinitionId{1}, DefinitionId{2},
                                   DefinitionId{100}}));
  CHECK(materialized.product->definitions().findPart(DefinitionId{2}) !=
        nullptr);  // orphan is retained
  const auto* assembly = materialized.product->rootAssemblyDefinition();
  CHECK(assembly);
  CHECK(assembly->size() == 2);
  const auto* first = assembly->find(OccurrenceId{11});
  const auto* second = assembly->find(OccurrenceId{12});
  CHECK(first && second);
  CHECK(first->targetDefinitionId() == second->targetDefinitionId());
  CHECK(first->targetDefinitionId() == DefinitionId{1});
  CHECK(first->flags() == OccurrenceFlags({true, false, true}));
  CHECK(second->flags() == OccurrenceFlags({false, true, false}));
  const std::array<double, 3> expectedTranslation{10.0, 20.0, 30.0};
  CHECK(second->placement().translationMm() == expectedTranslation);
  const auto reencoded =
      encodeProductEnvelopeV6Root(*materialized.product, kProject, metadata());
  CHECK(reencoded.succeeded());
  CHECK(reencoded.payload->root == scanned.candidate->authoredRoot);

  // Every public projection, including nested V4 work accounting, is
  // untrusted at the materialization boundary.
  {
    auto tampered = *scanned.candidate;
    ++tampered.rebuildWork;
    const auto rejected = materializeProductEnvelopeV6(tampered);
    CHECK(rejected.status == ProductEnvelopeV6MaterializationStatus::Invalid);
    CHECK(!rejected.product);
  }
  {
    auto tampered = *scanned.candidate;
    ++tampered.parts[0]
          .embeddedPart->legacySketch.topology.payload.rebuildWork;
    const auto rejected = materializeProductEnvelopeV6(tampered);
    CHECK(rejected.status == ProductEnvelopeV6MaterializationStatus::Invalid);
    CHECK(!rejected.product);
  }
  {
    auto root = scanned.candidate->authoredRoot;
    root[QStringLiteral("capabilities")] =
        QJsonArray{QStringLiteral("flat-assembly-v1"),
                   QStringLiteral("product-envelope-v1"),
                   QStringLiteral("z-future")};
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    auto unsupported = scanProductEnvelopeV6(QByteArrayView(bytes));
    CHECK(unsupported.status == ProductEnvelopeV6ScanStatus::Unsupported);
    CHECK(unsupported.candidate);
    const auto rejected = materializeProductEnvelopeV6(*unsupported.candidate);
    CHECK(rejected.status ==
          ProductEnvelopeV6MaterializationStatus::Unsupported);
    CHECK(!rejected.product);
  }

  // A failure after all structural scanning and OCCT materialization work
  // occurs in definition 2 after the same definition-1 payload has succeeded
  // in a control product. It publishes no partial product and advances none
  // of the edit-ID cursors.
  {
    auto validControl = productWithParts(Document{}, Document{});
    const auto controlEncoded =
        encodeProductEnvelopeV6Root(*validControl, kProject, metadata());
    CHECK(controlEncoded.succeeded());
    auto controlCandidate =
        scanProductEnvelopeV6(QByteArrayView(controlEncoded.payload->bytes));
    CHECK(controlCandidate.status == ProductEnvelopeV6ScanStatus::Candidate);
    CHECK(materializeProductEnvelopeV6(*controlCandidate.candidate).succeeded());

    auto invalidSource =
        productWithParts(Document{}, invalidRecomputeDocument());
    const auto encoded =
        encodeProductEnvelopeV6Root(*invalidSource, kProject, metadata());
    CHECK(encoded.succeeded());
    auto invalidCandidate =
        scanProductEnvelopeV6(QByteArrayView(encoded.payload->bytes));
    CHECK(invalidCandidate.status == ProductEnvelopeV6ScanStatus::Candidate);
    const AutoIds before = consumeAutoIds("before-v6-failure");
    const auto rejected =
        materializeProductEnvelopeV6(*invalidCandidate.candidate);
    const AutoIds after = consumeAutoIds("after-v6-failure");
    checkOnlyOneAdvance(before, after);
    CHECK(rejected.status == ProductEnvelopeV6MaterializationStatus::Invalid);
    CHECK(!rejected.product);
    CHECK(rejected.error.contains(QStringLiteral("recompute was rejected")));
  }

  return EXIT_SUCCESS;
}

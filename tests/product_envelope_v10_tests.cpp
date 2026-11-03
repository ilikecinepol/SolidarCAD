#include "TestAssertions.h"

#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>

#include <TopoDS_Shape.hxx>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "model/ExtrudeFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/MirrorFeature.h"
#include "project/ProductEnvelopeV10Decoder.h"
#include "project/ProductEnvelopeV10Encoder.h"
#include "project/ProductEnvelopeV10Materialization.h"
#include "project/ProductEnvelopeV9Decoder.h"
#include "project/internal/ProductEnvelopeV10AtomicWriting.h"
#include "sketch/Sketch.h"

namespace {

using namespace solidar;
using namespace solidar::project;
using namespace solidar::project::detail;

constexpr ProjectId kProject{{0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                              0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x10}};

ProductEnvelopeV5Metadata metadata() {
  return {QStringLiteral("component fixture"),
          QStringLiteral("2026-10-31T00:00:00Z")};
}

QJsonObject minimalV4() {
  static constexpr char json[] = R"JSON({
    "format":"solidar-project","version":4,
    "capabilities":["persistent-topology-v2"],
    "constraintTypeEncoding":"stable-name-v1",
    "name":"component fixture","createdAt":"2026-10-31T00:00:00Z",
    "document":{"widthMm":60,"heightMm":40,"extrusionMm":25},
    "sketches":[],"extrusion":{"enabled":false},
    "model":{"sketches":[],"bodies":[],"referenceImages":[]}
  })JSON";
  QJsonParseError error;
  const auto parsed = QJsonDocument::fromJson(QByteArray(json), &error);
  CHECK(error.error == QJsonParseError::NoError);
  CHECK(parsed.isObject());
  return parsed.object();
}

QJsonObject occurrence(int id, int target, QString name = {}) {
  return {{QStringLiteral("occurrenceId"), id},
          {QStringLiteral("targetDefinitionId"), target},
          {QStringLiteral("placement"),
           QJsonObject{{QStringLiteral("translationMm"), QJsonArray{1, 2, 3}},
                       {QStringLiteral("quaternionWxyz"),
                        QJsonArray{1, 0, 0, 0}}}},
          {QStringLiteral("name"), std::move(name)},
          {QStringLiteral("visible"), true},
          {QStringLiteral("suppressed"), false},
          {QStringLiteral("grounded"), false}};
}

QJsonObject definition(int id, QJsonArray occurrences = {},
                       QJsonObject data = minimalV4()) {
  return {{QStringLiteral("definitionId"), id},
          {QStringLiteral("definitionKind"), QStringLiteral("component")},
          {QStringLiteral("payload"),
           QJsonObject{{QStringLiteral("payloadKind"),
                        QStringLiteral("embedded-component-v4")},
                       {QStringLiteral("data"), std::move(data)}}},
          {QStringLiteral("occurrences"), std::move(occurrences)}};
}

QJsonObject root(int rootId, QJsonArray definitions) {
  return {{QStringLiteral("format"), QStringLiteral("solidar-project")},
          {QStringLiteral("version"), 10},
          {QStringLiteral("capabilities"),
           QJsonArray{QStringLiteral("component-definitions-v1"),
                      QStringLiteral("nested-components-v1"),
                      QStringLiteral("product-envelope-v1")}},
          {QStringLiteral("projectId"),
           QStringLiteral("123e4567-e89b-42d3-a456-426614174010")},
          {QStringLiteral("rootDefinitionId"), rootId},
          {QStringLiteral("metadata"),
           QJsonObject{{QStringLiteral("name"), metadata().name},
                       {QStringLiteral("createdAt"), metadata().createdAt}}},
          {QStringLiteral("definitions"), std::move(definitions)}};
}

ProductEnvelopeV10ScanResult scan(const QJsonObject& value) {
  const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
  return scanProductEnvelopeV10(QByteArrayView(bytes));
}

void expectInvalid(const QJsonObject& value) {
  const auto result = scan(value);
  CHECK(result.status == ProductEnvelopeV10ScanStatus::Invalid);
  CHECK(!result.candidate);
}

QJsonObject v4WithPatternWork(QJsonObject data, int finalPatternCount) {
  QJsonArray features{QJsonObject{{QStringLiteral("id"), 1},
                                  {QStringLiteral("name"), QStringLiteral("imported")},
                                  {QStringLiteral("type"), QStringLiteral("ImportedShape")},
                                  {QStringLiteral("brep"), QStringLiteral("TQ==")}}};
  for (int index = 0; index < 50; ++index) {
    features.push_back(QJsonObject{
        {QStringLiteral("id"), index + 2},
        {QStringLiteral("name"), QStringLiteral("pattern")},
        {QStringLiteral("type"), QStringLiteral("LinearPattern")},
        {QStringLiteral("sourceBodyId"), 0},
        {QStringLiteral("sourceFeatureId"), index + 1},
        {QStringLiteral("direction"), 0},
        {QStringLiteral("count"), index == 49 ? finalPatternCount : 100},
        {QStringLiteral("spacingMm"), 5},
        {QStringLiteral("operation"), 1}});
  }
  auto model = data.value(QStringLiteral("model")).toObject();
  model[QStringLiteral("bodies")] = QJsonArray{QJsonObject{
      {QStringLiteral("id"), 10}, {QStringLiteral("name"), QStringLiteral("body")},
      {QStringLiteral("visible"), true}, {QStringLiteral("features"), features}}};
  data[QStringLiteral("model")] = model;
  return data;
}

std::unique_ptr<Document> authoredDocument(std::uint64_t base,
                                           bool withTopology) {
  auto document = std::make_unique<Document>();
  auto& sketch = document->addSketch(SketchId{base + 1}, "profile");
  sketch.geometry.addRectangle({0, 0}, {12, 8});
  auto& body = document->addBody(BodyId{base + 2}, "body");
  auto extrude = std::make_unique<ExtrudeFeature>(
      FeatureId{base + 3}, sketch.id, 5.0, "extrude");
  auto* extrudePtr = extrude.get();
  body.addFeature(std::move(extrude));
  CHECK(document->recomputeDetailed().success);
  if (withTopology) {
    const auto face = extrudePtr->createFaceReferenceV2(body.id(), 0);
    CHECK(face);
    CHECK(face.reference.isV2());
    body.addFeature(std::make_unique<MirrorFeature>(
        FeatureId{base + 4}, extrudePtr->id(), face.reference, "mirror"));
    document->addReferenceImage("reference", "image.png", "XY",
                                SketchPlacement::xy(), 640, 480);
    CHECK(document->recomputeDetailed().success);
  }
  return document;
}

std::unique_ptr<ProductDocument> componentProduct() {
  std::vector<ComponentDefinition> components;
  components.emplace_back(DefinitionId{1}, authoredDocument(1000, false));
  components.emplace_back(DefinitionId{2}, authoredDocument(2000, false));
  components.emplace_back(DefinitionId{3}, authoredDocument(3000, true));
  CHECK(components[1].insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{1}, RigidTransform3d::identity(),
            {true, false, true}, "leaf")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  const double half = 0.25 * 3.14159265358979323846;
  const auto moved = RigidTransform3d::create(
      {4, 5, 6}, {std::cos(half), 0, 0, std::sin(half)});
  CHECK(moved);
  CHECK(components[2].insert(ComponentOccurrence(
            OccurrenceId{5}, DefinitionId{2}, *moved.transform,
            {true, false, false}, "left")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(components[2].insert(ComponentOccurrence(
            OccurrenceId{6}, DefinitionId{2}, RigidTransform3d::identity(),
            {false, true, false}, "right")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  auto made = ProductDocument::createComponentGraph(
      std::move(components), DefinitionId{3});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> componentProductWithUnreachableDrawing() {
  std::vector<ComponentDefinition> components;
  components.emplace_back(DefinitionId{1}, std::make_unique<Document>());
  std::vector<ComponentDrawing> drawings;
  drawings.emplace_back(
      DrawingId{91}, "unsupported drawing", ComponentDrawingPaper::A4,
      ComponentDrawingOrientation::Landscape, 2.0,
      std::vector<BodyId>{BodyId{9002}});
  components.emplace_back(DefinitionId{2}, authoredDocument(9000, false),
                          std::move(drawings));
  auto made = ProductDocument::createComponentGraph(
      std::move(components), DefinitionId{1});
  CHECK(made);
  CHECK(made.product->definitions()
            .findComponent(DefinitionId{2})
            ->drawings()
            .size() == 1);
  return std::move(made.product);
}

struct Cursors final {
  BodyId body;
  SketchId sketch;
  FeatureId feature;
  ProductIncarnationId product;
};

Cursors consumeCursors() {
  Document document;
  const auto sketch = document.addSketch("cursor").id;
  auto& body = document.addBody("cursor");
  auto feature = std::make_unique<ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(), "cursor");
  const auto featureId = feature->id();
  body.addFeature(std::move(feature));
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{1}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(made);
  return {body.id(), sketch, featureId, made.product->incarnation()};
}

void checkCursorDelta(const Cursors& before, const Cursors& after,
                      std::uint64_t productDelta) {
  CHECK(after.body == before.body + 1);
  CHECK(after.sketch == before.sketch + 1);
  CHECK(after.feature == before.feature + 1);
  CHECK(after.product.value() == before.product.value() + productDelta);
}

class Transaction final : public ProjectAtomicWriteTransaction {
 public:
  bool open(QString*) override { calls << "open"; return fail != ProjectAtomicWriteFailure::OpenTemporary; }
  qint64 write(QByteArrayView bytes, QString*) override {
    calls << "write";
    staged = QByteArray(bytes.data(), bytes.size());
    return fail == ProjectAtomicWriteFailure::WriteTemporary ? bytes.size() - 1 : bytes.size();
  }
  bool flush(QString*) override { calls << "flush"; return fail != ProjectAtomicWriteFailure::FlushTemporary; }
  bool sync(QString*) override { calls << "sync"; return fail != ProjectAtomicWriteFailure::SyncTemporary; }
  bool commit(QString*) override {
    calls << "commit";
    if (fail == ProjectAtomicWriteFailure::ReplaceDestination) return false;
    destination = staged;
    return true;
  }
  void cancel() noexcept override { calls << "cancel"; }

  ProjectAtomicWriteFailure fail{ProjectAtomicWriteFailure::None};
  QStringList calls;
  QByteArray staged;
  QByteArray destination{"sentinel"};
};

}  // namespace

int main() {
  const auto minimal = root(1, QJsonArray{definition(1)});
  const auto minimalScan = scan(minimal);
  CHECK(minimalScan.hasCandidate());
  CHECK(minimalScan.candidate->graph.definitions.size() == 1);
  auto minimalProduct = materializeProductEnvelopeV10(*minimalScan.candidate);
  CHECK(minimalProduct.succeeded());
  const auto minimalFixed = encodeProductEnvelopeV10Root(
      *minimalProduct.product, kProject, metadata());
  CHECK(minimalFixed.succeeded());
  CHECK(minimalFixed.payload->bytes ==
        QJsonDocument(minimal).toJson(QJsonDocument::Indented));

  const auto nested = root(
      3, QJsonArray{definition(1),
                    definition(2, QJsonArray{occurrence(7, 1, "leaf")}),
                    definition(3, QJsonArray{occurrence(5, 2, "left"),
                                             occurrence(6, 2, "right")})});
  const auto nestedScan = scan(nested);
  CHECK(nestedScan.hasCandidate());
  CHECK(nestedScan.candidate->definitions.size() == 3);
  CHECK(nestedScan.candidate->definitions[2].occurrences[0].displayNameUtf8 ==
        "left");
  CHECK(materializeProductEnvelopeV10(*nestedScan.candidate).succeeded());

  for (const QString& field :
       {QStringLiteral("format"), QStringLiteral("version"),
        QStringLiteral("capabilities"), QStringLiteral("projectId"),
        QStringLiteral("rootDefinitionId"), QStringLiteral("metadata"),
        QStringLiteral("definitions")}) {
    auto invalid = minimal;
    invalid.remove(field);
    expectInvalid(invalid);
  }
  {
    auto invalid = minimal;
    invalid[QStringLiteral("unexpected")] = true;
    expectInvalid(invalid);
    auto definitions = minimal.value("definitions").toArray();
    auto item = definitions[0].toObject();
    item[QStringLiteral("mates")] = QJsonArray{};
    definitions[0] = item;
    invalid = minimal;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
    item = minimal.value("definitions").toArray()[0].toObject();
    auto payload = item.value("payload").toObject();
    payload[QStringLiteral("unexpected")] = true;
    item[QStringLiteral("payload")] = payload;
    definitions[0] = item;
    invalid = minimal;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = minimal;
    invalid[QStringLiteral("capabilities")] =
        QJsonArray{QStringLiteral("nested-components-v1"),
                   QStringLiteral("component-definitions-v1"),
                   QStringLiteral("product-envelope-v1")};
    expectInvalid(invalid);
  }
  {
    auto invalid = nested;
    auto definitions = invalid.value("definitions").toArray();
    const auto first = definitions[0];
    definitions[0] = definitions[1];
    definitions[1] = first;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
    definitions = nested.value("definitions").toArray();
    auto duplicate = definitions[1].toObject();
    duplicate[QStringLiteral("definitionId")] = 1;
    definitions[1] = duplicate;
    invalid = nested;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = nested;
    auto definitions = invalid.value("definitions").toArray();
    auto rootDefinition = definitions[2].toObject();
    auto occurrences = rootDefinition.value("occurrences").toArray();
    const auto first = occurrences[0];
    occurrences[0] = occurrences[1];
    occurrences[1] = first;
    rootDefinition[QStringLiteral("occurrences")] = occurrences;
    definitions[2] = rootDefinition;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = minimal;
    auto definitions = invalid.value("definitions").toArray();
    auto item = definitions[0].toObject();
    item[QStringLiteral("definitionKind")] = QStringLiteral("part");
    definitions[0] = item;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
    item[QStringLiteral("definitionKind")] = QStringLiteral("component");
    auto payload = item.value("payload").toObject();
    payload[QStringLiteral("payloadKind")] = QStringLiteral("embedded-part-v4");
    item[QStringLiteral("payload")] = payload;
    definitions[0] = item;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = nested;
    auto definitions = invalid.value("definitions").toArray();
    auto rootDefinition = definitions[2].toObject();
    auto occurrences = rootDefinition.value("occurrences").toArray();
    auto first = occurrences[0].toObject();
    first[QStringLiteral("targetDefinitionId")] = 999;
    occurrences[0] = first;
    rootDefinition[QStringLiteral("occurrences")] = occurrences;
    definitions[2] = rootDefinition;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
    first[QStringLiteral("targetDefinitionId")] = 3;
    occurrences[0] = first;
    rootDefinition[QStringLiteral("occurrences")] = occurrences;
    definitions[2] = rootDefinition;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = minimal;
    auto definitions = invalid.value("definitions").toArray();
    auto item = definitions[0].toObject();
    auto payload = item.value("payload").toObject();
    auto data = payload.value("data").toObject();
    data.remove(QStringLiteral("model"));
    payload[QStringLiteral("data")] = data;
    item[QStringLiteral("payload")] = payload;
    definitions[0] = item;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    QByteArray bytes = QJsonDocument(nested).toJson(QJsonDocument::Compact);
    CHECK(bytes.contains(QByteArrayLiteral("\"quaternionWxyz\":[1,0,0,0]")));
    bytes.replace(QByteArrayLiteral("\"quaternionWxyz\":[1,0,0,0]"),
                  QByteArrayLiteral("\"quaternionWxyz\":[1,-0,0,0]"));
    const auto rejected = scanProductEnvelopeV10(QByteArrayView(bytes));
    CHECK(rejected.status == ProductEnvelopeV10ScanStatus::Invalid);
    CHECK(rejected.error.contains(QStringLiteral("negative zero")));
  }
  {
    auto invalid = nested;
    auto definitions = invalid.value("definitions").toArray();
    auto branch = definitions[1].toObject();
    auto occurrences = branch.value("occurrences").toArray();
    auto item = occurrences[0].toObject();
    item[QStringLiteral("name")] = QStringLiteral("   ");
    occurrences[0] = item;
    branch[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = branch;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
    item[QStringLiteral("name")] = QStringLiteral("leaf");
    auto placement = item.value("placement").toObject();
    placement[QStringLiteral("quaternionWxyz")] = QJsonArray{2, 0, 0, 0};
    item[QStringLiteral("placement")] = placement;
    occurrences[0] = item;
    branch[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = branch;
    invalid = nested;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    const auto exact = root(
        1, QJsonArray{definition(1, {}, v4WithPatternWork(minimalV4(), 99)),
                      definition(2, {}, v4WithPatternWork(minimalV4(), 99))});
    const auto exactScan = scan(exact);
    CHECK(exactScan.hasCandidate());
    CHECK(exactScan.candidate->rebuildWork ==
          kProductEnvelopeV10MaximumRebuildWork);
    auto excessive = exact;
    auto definitions = excessive.value("definitions").toArray();
    auto second = definitions[1].toObject();
    auto payload = second.value("payload").toObject();
    payload[QStringLiteral("data")] =
        v4WithPatternWork(minimalV4(), 100);
    second[QStringLiteral("payload")] = payload;
    definitions[1] = second;
    excessive[QStringLiteral("definitions")] = definitions;
    expectInvalid(excessive);
  }
  {
    QJsonArray occurrences;
    for (int id = 1;
         id <= static_cast<int>(kAssemblyGraphMaximumAuthoredOccurrences + 1);
         ++id)
      occurrences.push_back(occurrence(id, 2));
    expectInvalid(root(1, QJsonArray{definition(1, occurrences),
                                     definition(2)}));
  }
  {
    QJsonArray definitions;
    for (int id = 1; id <= 34; ++id) {
      const QJsonArray occurrences =
          id == 34 ? QJsonArray{} : QJsonArray{occurrence(1, id + 1)};
      definitions.push_back(definition(id, occurrences));
    }
    expectInvalid(root(1, definitions));
  }
  {
    QJsonArray definitions;
    for (int id = 1; id <= 18; ++id) {
      const QJsonArray occurrences =
          id == 18
              ? QJsonArray{}
              : QJsonArray{occurrence(1, id + 1), occurrence(2, id + 1)};
      definitions.push_back(definition(id, occurrences));
    }
    expectInvalid(root(1, definitions));
  }

  auto source = componentProduct();
  auto sourceWithDrawing = componentProductWithUnreachableDrawing();
  const auto lossyEncode = encodeProductEnvelopeV10Root(
      *sourceWithDrawing, kProject, metadata());
  CHECK(!lossyEncode.succeeded());
  CHECK(lossyEncode.status ==
        ProductEnvelopeV10EncodeStatus::UnsupportedComponentDrawings);
  CHECK(!lossyEncode.payload);
  CHECK(!lossyEncode.error.isEmpty());
  {
    Transaction transaction;
    const auto rejected = writeProductEnvelopeV10Atomically(
        *sourceWithDrawing, kProject, metadata(), transaction);
    CHECK(!rejected.succeeded());
    CHECK(rejected.failure == ProjectAtomicWriteFailure::Encode);
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.staged.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
  }
  const auto encoded = encodeProductEnvelopeV10Root(
      *source, kProject, metadata());
  const auto encodedAgain = encodeProductEnvelopeV10Root(
      *source, kProject, metadata());
  CHECK(encoded.succeeded());
  CHECK(encodedAgain.succeeded());
  CHECK(encoded.payload->bytes == encodedAgain.payload->bytes);
  const auto scanned = scanProductEnvelopeV10(
      QByteArrayView(encoded.payload->bytes));
  CHECK(scanned.hasCandidate());
  const auto encodedDefinitions =
      encoded.payload->root.value("definitions").toArray();
  CHECK(encodedDefinitions[0].toObject().value("definitionId").toInt() == 1);
  CHECK(encodedDefinitions[1].toObject().value("definitionId").toInt() == 2);
  CHECK(encodedDefinitions[2].toObject().value("definitionId").toInt() == 3);
  CHECK(scanned.candidate->topologyReferenceCount > 0);
  auto materialized = materializeProductEnvelopeV10(*scanned.candidate);
  CHECK(materialized.succeeded());
  CHECK(materialized.product->rootDefinitionKind() == DefinitionKind::Component);
  const auto* rootComponent =
      materialized.product->definitions().findComponent(DefinitionId{3});
  CHECK(rootComponent);
  for (const DefinitionId id : materialized.product->definitions().definitionIds()) {
    const auto* component =
        materialized.product->definitions().findComponent(id);
    CHECK(component);
    CHECK(component->drawings().empty());
  }
  CHECK(rootComponent->document().findBody(BodyId{3002}));
  CHECK(rootComponent->document().referenceImages().size() == 1);
  const auto* mirror = dynamic_cast<const MirrorFeature*>(
      rootComponent->document().findFeature(BodyId{3002}, FeatureId{3004}));
  CHECK(mirror && mirror->planeFace() && mirror->planeFace()->isV2());
  const auto roundtrip = encodeProductEnvelopeV10Root(
      *materialized.product, kProject, metadata());
  CHECK(roundtrip.succeeded());
  CHECK(roundtrip.payload->bytes == encoded.payload->bytes);

  {
    const auto before = consumeCursors();
    auto validated = materializeProductEnvelopeV10(
        *scanned.candidate,
        ProductEnvelopeV10MaterializationFailurePoint::None,
        ProductEnvelopeV10MaterializationMode::ValidationOnly);
    const auto after = consumeCursors();
    CHECK(validated.succeeded());
    checkCursorDelta(before, after, 1);
  }
  {
    const auto before = consumeCursors();
    auto published = materializeProductEnvelopeV10(*scanned.candidate);
    const auto after = consumeCursors();
    CHECK(published.succeeded());
    checkCursorDelta(before, after, 2);
  }
  for (const auto point :
       {ProductEnvelopeV10MaterializationFailurePoint::BeforeLastDocument,
        ProductEnvelopeV10MaterializationFailurePoint::BeforeProductConstruction,
        ProductEnvelopeV10MaterializationFailurePoint::Finalization}) {
    const auto before = consumeCursors();
    auto rejected = materializeProductEnvelopeV10(
        *scanned.candidate, point,
        ProductEnvelopeV10MaterializationMode::Publication);
    const auto after = consumeCursors();
    CHECK(!rejected.succeeded());
    checkCursorDelta(before, after, 1);
  }
  {
    auto tampered = *scanned.candidate;
    tampered.definitions[0].occurrences.push_back(
        {OccurrenceId{999}, DefinitionId{1}, RigidTransform3d::identity(),
         {}, {}, {}});
    const auto before = consumeCursors();
    auto rejected = materializeProductEnvelopeV10(tampered);
    const auto after = consumeCursors();
    CHECK(!rejected.succeeded());
    checkCursorDelta(before, after, 1);
  }

  {
    const auto before = consumeCursors();
    Transaction transaction;
    const auto written = writeProductEnvelopeV10Atomically(
        *source, kProject, metadata(), transaction);
    const auto after = consumeCursors();
    CHECK(written.succeeded());
    CHECK(transaction.destination == encoded.payload->bytes);
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    checkCursorDelta(before, after, 1);
  }
  constexpr std::array preparationPoints{
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::EncodeInitial,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::ScanInitial,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::MaterializeInitial,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::EncodeFixedPoint,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::FixedPointMismatch,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::ScanFixedPoint,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::MaterializeFixedPoint,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::EncodeStable,
      ProductEnvelopeV10AtomicWritePreparationFailurePoint::StableMismatch};
  for (const auto point : preparationPoints) {
    const auto before = consumeCursors();
    Transaction transaction;
    const auto rejected = writeProductEnvelopeV10Atomically(
        *source, kProject, metadata(), transaction, point);
    const auto after = consumeCursors();
    CHECK(!rejected.succeeded());
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
    checkCursorDelta(before, after, 1);
  }
  for (const auto failure :
       {ProjectAtomicWriteFailure::OpenTemporary,
        ProjectAtomicWriteFailure::WriteTemporary,
        ProjectAtomicWriteFailure::FlushTemporary,
        ProjectAtomicWriteFailure::SyncTemporary,
        ProjectAtomicWriteFailure::ReplaceDestination}) {
    Transaction transaction;
    transaction.fail = failure;
    const auto rejected = writeProductEnvelopeV10Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(rejected.failure == failure);
    CHECK(transaction.destination == QByteArray("sentinel"));
    CHECK(transaction.calls.contains("cancel"));
  }

  CHECK(scanProductEnvelopeV9(QByteArrayView(encoded.payload->bytes)).status ==
        ProductEnvelopeV9ScanStatus::Unsupported);
  return EXIT_SUCCESS;
}

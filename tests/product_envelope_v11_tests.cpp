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
#include "project/ProductEnvelopeV11Decoder.h"
#include "project/ProductEnvelopeV11Encoder.h"
#include "project/ProductEnvelopeV11Materialization.h"
#include "project/ProductEnvelopeV9Decoder.h"
#include "project/internal/ProductEnvelopeV11AtomicWriting.h"
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

QJsonObject drawing(int id, QString name = QStringLiteral("drawing"),
                    double scale = 1.0, QJsonArray sourceBodyIds = {},
                    QString paper = QStringLiteral("A4"),
                    QString orientation = QStringLiteral("portrait")) {
  return {{QStringLiteral("drawingId"), id},
          {QStringLiteral("name"), std::move(name)},
          {QStringLiteral("paper"), std::move(paper)},
          {QStringLiteral("orientation"), std::move(orientation)},
          {QStringLiteral("scale"), scale},
          {QStringLiteral("sourceBodyIds"), std::move(sourceBodyIds)}};
}

QJsonObject definition(int id, QJsonArray occurrences = {},
                       QJsonObject data = minimalV4(),
                       QJsonArray drawings = {}) {
  return {{QStringLiteral("definitionId"), id},
          {QStringLiteral("definitionKind"), QStringLiteral("component")},
          {QStringLiteral("payload"),
           QJsonObject{{QStringLiteral("payloadKind"),
                        QStringLiteral("embedded-component-v4")},
                       {QStringLiteral("data"), std::move(data)}}},
          {QStringLiteral("drawings"), std::move(drawings)},
          {QStringLiteral("occurrences"), std::move(occurrences)}};
}

QJsonObject root(int rootId, QJsonArray definitions) {
  return {{QStringLiteral("format"), QStringLiteral("solidar-project")},
          {QStringLiteral("version"), 11},
          {QStringLiteral("capabilities"),
           QJsonArray{QStringLiteral("component-definitions-v1"),
                      QStringLiteral("component-drawings-v1"),
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

ProductEnvelopeV11ScanResult scan(const QJsonObject& value) {
  const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
  return scanProductEnvelopeV11(QByteArrayView(bytes));
}

void expectInvalid(const QJsonObject& value) {
  const auto result = scan(value);
  CHECK(result.status == ProductEnvelopeV11ScanStatus::Invalid);
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
  components.emplace_back(
      DefinitionId{1}, authoredDocument(1000, false),
      std::vector<ComponentDrawing>{ComponentDrawing(
          DrawingId{11}, "leaf drawing", ComponentDrawingPaper::A4,
          ComponentDrawingOrientation::Portrait, 1.0,
          {BodyId{1002}})});
  components.emplace_back(DefinitionId{2}, authoredDocument(2000, false));
  components.emplace_back(
      DefinitionId{3}, authoredDocument(3000, true),
      std::vector<ComponentDrawing>{
          ComponentDrawing(DrawingId{31}, "all root bodies",
                           ComponentDrawingPaper::A4,
                           ComponentDrawingOrientation::Landscape, 2.5, {}),
          ComponentDrawing(DrawingId{32}, "root body",
                           ComponentDrawingPaper::A4,
                           ComponentDrawingOrientation::Portrait, 1.0,
                           {BodyId{3002}})});
  components.emplace_back(
      DefinitionId{4}, authoredDocument(4000, false),
      std::vector<ComponentDrawing>{ComponentDrawing(
          DrawingId{41}, "unreachable drawing", ComponentDrawingPaper::A4,
          ComponentDrawingOrientation::Portrait, 4.0,
          {BodyId{4002}})});
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
  auto minimalProduct = materializeProductEnvelopeV11(*minimalScan.candidate);
  CHECK(minimalProduct.succeeded());
  const auto minimalFixed = encodeProductEnvelopeV11Root(
      *minimalProduct.product, kProject, metadata());
  CHECK(minimalFixed.succeeded());
  CHECK(minimalFixed.payload->bytes ==
        QJsonDocument(minimal).toJson(QJsonDocument::Indented));

  const auto drawingData = v4WithPatternWork(minimalV4(), 99);
  const auto authoredDrawing = root(
      1, QJsonArray{definition(
             1, {}, drawingData,
             QJsonArray{drawing(7, QStringLiteral("Лист"), 2.5,
                                QJsonArray{10}, QStringLiteral("A4"),
                                QStringLiteral("landscape"))})});
  const auto authoredDrawingScan = scan(authoredDrawing);
  CHECK(authoredDrawingScan.hasCandidate());
  CHECK(authoredDrawingScan.candidate->drawingCount == 1);
  CHECK(authoredDrawingScan.candidate->drawingSourceReferenceCount == 1);
  CHECK(authoredDrawingScan.candidate->definitions[0].drawings.size() == 1);
  CHECK(authoredDrawingScan.candidate->definitions[0].drawings[0].id() ==
        DrawingId{7});
  CHECK(authoredDrawingScan.candidate->definitions[0]
            .drawings[0]
            .sourceBodyIds() == std::vector<BodyId>{BodyId{10}});
  CHECK(authoredDrawingScan.candidate->graph.definitions.at(DefinitionId{1})
            .drawings.size() == 1);

  {
    auto invalid = authoredDrawing;
    auto definitions = invalid.value("definitions").toArray();
    auto definitionValue = definitions[0].toObject();
    definitionValue.remove(QStringLiteral("drawings"));
    definitions[0] = definitionValue;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  for (const QString& field :
       {QStringLiteral("drawingId"), QStringLiteral("name"),
        QStringLiteral("paper"), QStringLiteral("orientation"),
        QStringLiteral("scale"), QStringLiteral("sourceBodyIds")}) {
    auto invalid = authoredDrawing;
    auto definitions = invalid.value("definitions").toArray();
    auto definitionValue = definitions[0].toObject();
    auto drawings = definitionValue.value("drawings").toArray();
    auto value = drawings[0].toObject();
    value.remove(field);
    drawings[0] = value;
    definitionValue[QStringLiteral("drawings")] = drawings;
    definitions[0] = definitionValue;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  for (const QJsonObject& invalidDrawing :
       {drawing(7, QString(), 1.0, QJsonArray{10}),
        drawing(7, QStringLiteral("bad paper"), 1.0, QJsonArray{10},
                QStringLiteral("A3")),
        drawing(7, QStringLiteral("bad orientation"), 1.0,
                QJsonArray{10}, QStringLiteral("A4"),
                QStringLiteral("upside-down")),
        drawing(7, QStringLiteral("zero scale"), 0.0, QJsonArray{10}),
        drawing(7, QStringLiteral("missing body"), 1.0, QJsonArray{11}),
        drawing(7, QStringLiteral("unordered bodies"), 1.0,
                QJsonArray{10, 9}),
        drawing(7, QStringLiteral("duplicate bodies"), 1.0,
                QJsonArray{10, 10})}) {
    expectInvalid(root(
        1, QJsonArray{definition(1, {}, drawingData,
                                 QJsonArray{invalidDrawing})}));
  }
  expectInvalid(root(
      1, QJsonArray{definition(
             1, {}, drawingData,
             QJsonArray{drawing(7, QStringLiteral("first"), 1.0,
                                QJsonArray{10}),
                        drawing(7, QStringLiteral("duplicate"), 1.0,
                                QJsonArray{10})})}));
  expectInvalid(root(
      1, QJsonArray{definition(
             1, {}, drawingData,
             QJsonArray{drawing(
                 7,
                 QString(static_cast<qsizetype>(
                             kComponentDrawingMaximumNameUtf8Bytes + 1),
                         QLatin1Char('x')),
                 1.0, QJsonArray{10})})}));
  {
    QJsonArray excessiveDrawings;
    for (std::uint64_t id = 1;
         id <= kComponentGraphMaximumDrawings + 1; ++id)
      excessiveDrawings.push_back(
          drawing(static_cast<int>(id), QStringLiteral("d")));
    expectInvalid(root(
        1, QJsonArray{definition(1, {}, minimalV4(), excessiveDrawings)}));
  }
  {
    QByteArray negativeZero =
        QJsonDocument(authoredDrawing).toJson(QJsonDocument::Compact);
    CHECK(negativeZero.contains(QByteArrayLiteral("\"scale\":2.5")));
    negativeZero.replace(QByteArrayLiteral("\"scale\":2.5"),
                         QByteArrayLiteral("\"scale\":-0"));
    CHECK(scanProductEnvelopeV11(QByteArrayView(negativeZero)).status ==
          ProductEnvelopeV11ScanStatus::Invalid);
  }
  {
    QByteArray invalidUtf8 =
        QJsonDocument(authoredDrawing).toJson(QJsonDocument::Compact);
    const QByteArray validName = QString::fromUtf8("Лист").toUtf8();
    const qsizetype offset = invalidUtf8.indexOf(validName);
    CHECK(offset >= 0);
    invalidUtf8[offset] = static_cast<char>(0xFF);
    CHECK(scanProductEnvelopeV11(QByteArrayView(invalidUtf8)).status ==
          ProductEnvelopeV11ScanStatus::Invalid);
  }

  const auto nested = root(
      3,
      QJsonArray{
          definition(1, {}, minimalV4(),
                     QJsonArray{drawing(101, QStringLiteral("leaf sheet"))}),
          definition(2, QJsonArray{occurrence(7, 1, "leaf")}, minimalV4(),
                     QJsonArray{drawing(102, QStringLiteral("shared sheet"))}),
          definition(3, QJsonArray{occurrence(5, 2, "left"),
                                   occurrence(6, 2, "right")},
                     minimalV4(),
                     QJsonArray{drawing(103, QStringLiteral("root sheet"))}),
          definition(4, {}, minimalV4(),
                     QJsonArray{drawing(104,
                                        QStringLiteral("unreachable sheet"))})});
  const auto nestedScan = scan(nested);
  CHECK(nestedScan.hasCandidate());
  CHECK(nestedScan.candidate->definitions.size() == 4);
  CHECK(nestedScan.candidate->drawingCount == 4);
  CHECK(nestedScan.candidate->definitions[2].occurrences[0].displayNameUtf8 ==
        "left");
  CHECK(nestedScan.candidate->definitions[3].drawings[0].nameUtf8() ==
        "unreachable sheet");
  const auto nestedMaterialized =
      materializeProductEnvelopeV11(*nestedScan.candidate);
  CHECK(nestedMaterialized.succeeded());
  CHECK(nestedMaterialized.product->definitions()
            .findComponent(DefinitionId{4})
            ->drawings()[0]
            .id() == DrawingId{104});

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
    const auto rejected = scanProductEnvelopeV11(QByteArrayView(bytes));
    CHECK(rejected.status == ProductEnvelopeV11ScanStatus::Invalid);
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
          kProductEnvelopeV11MaximumRebuildWork);
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
  const auto encoded = encodeProductEnvelopeV11Root(
      *source, kProject, metadata());
  const auto encodedAgain = encodeProductEnvelopeV11Root(
      *source, kProject, metadata());
  CHECK(encoded.succeeded());
  CHECK(encodedAgain.succeeded());
  CHECK(encoded.payload->bytes == encodedAgain.payload->bytes);
  const auto scanned = scanProductEnvelopeV11(
      QByteArrayView(encoded.payload->bytes));
  CHECK(scanned.hasCandidate());
  const auto encodedDefinitions =
      encoded.payload->root.value("definitions").toArray();
  CHECK(encodedDefinitions[0].toObject().value("definitionId").toInt() == 1);
  CHECK(encodedDefinitions[1].toObject().value("definitionId").toInt() == 2);
  CHECK(encodedDefinitions[2].toObject().value("definitionId").toInt() == 3);
  CHECK(encodedDefinitions[3].toObject().value("definitionId").toInt() == 4);
  CHECK(scanned.candidate->topologyReferenceCount > 0);
  auto materialized = materializeProductEnvelopeV11(*scanned.candidate);
  CHECK(materialized.succeeded());
  CHECK(materialized.product->rootDefinitionKind() == DefinitionKind::Component);
  const auto* rootComponent =
      materialized.product->definitions().findComponent(DefinitionId{3});
  CHECK(rootComponent);
  CHECK(rootComponent->drawings().size() == 2);
  CHECK(rootComponent->drawings()[0].id() == DrawingId{31});
  CHECK(rootComponent->drawings()[0].orientation() ==
        ComponentDrawingOrientation::Landscape);
  CHECK(rootComponent->drawings()[0].sourceBodyIds().empty());
  CHECK(rootComponent->drawings()[1].sourceBodyIds() ==
        std::vector<BodyId>{BodyId{3002}});
  const auto* unreachable =
      materialized.product->definitions().findComponent(DefinitionId{4});
  CHECK(unreachable);
  CHECK(unreachable->drawings().size() == 1);
  CHECK(unreachable->drawings()[0].nameUtf8() == "unreachable drawing");
  CHECK(rootComponent->document().findBody(BodyId{3002}));
  CHECK(rootComponent->document().referenceImages().size() == 1);
  const auto* mirror = dynamic_cast<const MirrorFeature*>(
      rootComponent->document().findFeature(BodyId{3002}, FeatureId{3004}));
  CHECK(mirror && mirror->planeFace() && mirror->planeFace()->isV2());
  const auto roundtrip = encodeProductEnvelopeV11Root(
      *materialized.product, kProject, metadata());
  CHECK(roundtrip.succeeded());
  CHECK(roundtrip.payload->bytes == encoded.payload->bytes);

  {
    const auto before = consumeCursors();
    auto validated = materializeProductEnvelopeV11(
        *scanned.candidate,
        ProductEnvelopeV11MaterializationFailurePoint::None,
        ProductEnvelopeV11MaterializationMode::ValidationOnly);
    const auto after = consumeCursors();
    CHECK(validated.succeeded());
    checkCursorDelta(before, after, 1);
  }
  {
    const auto before = consumeCursors();
    auto published = materializeProductEnvelopeV11(*scanned.candidate);
    const auto after = consumeCursors();
    CHECK(published.succeeded());
    checkCursorDelta(before, after, 2);
  }
  for (const auto point :
       {ProductEnvelopeV11MaterializationFailurePoint::BeforeLastDocument,
        ProductEnvelopeV11MaterializationFailurePoint::BeforeProductConstruction,
        ProductEnvelopeV11MaterializationFailurePoint::Finalization}) {
    const auto before = consumeCursors();
    auto rejected = materializeProductEnvelopeV11(
        *scanned.candidate, point,
        ProductEnvelopeV11MaterializationMode::Publication);
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
    auto rejected = materializeProductEnvelopeV11(tampered);
    const auto after = consumeCursors();
    CHECK(!rejected.succeeded());
    checkCursorDelta(before, after, 1);
  }

  {
    const auto before = consumeCursors();
    Transaction transaction;
    const auto written = writeProductEnvelopeV11Atomically(
        *source, kProject, metadata(), transaction);
    const auto after = consumeCursors();
    CHECK(written.succeeded());
    CHECK(transaction.destination == encoded.payload->bytes);
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    checkCursorDelta(before, after, 1);
  }
  constexpr std::array preparationPoints{
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::EncodeInitial,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::ScanInitial,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::MaterializeInitial,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::EncodeFixedPoint,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::FixedPointMismatch,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::ScanFixedPoint,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::MaterializeFixedPoint,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::EncodeStable,
      ProductEnvelopeV11AtomicWritePreparationFailurePoint::StableMismatch};
  for (const auto point : preparationPoints) {
    const auto before = consumeCursors();
    Transaction transaction;
    const auto rejected = writeProductEnvelopeV11Atomically(
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
    const auto rejected = writeProductEnvelopeV11Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(rejected.failure == failure);
    CHECK(transaction.destination == QByteArray("sentinel"));
    CHECK(transaction.calls.contains("cancel"));
  }

  CHECK(scanProductEnvelopeV9(QByteArrayView(encoded.payload->bytes)).status ==
        ProductEnvelopeV9ScanStatus::Unsupported);
  return EXIT_SUCCESS;
}

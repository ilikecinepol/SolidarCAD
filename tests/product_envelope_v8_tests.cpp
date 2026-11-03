#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <BRepPrimAPI_MakeBox.hxx>

#include <memory>
#include <limits>
#include <tuple>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProductEnvelopeV7Encoder.h"
#include "project/ProductEnvelopeV8Decoder.h"
#include "project/ProductEnvelopeV8Encoder.h"
#include "project/ProductEnvelopeV8Materialization.h"
#include "project/ProjectFile.h"
#include "project/internal/ProductEnvelopeV7Budgets.h"
#include "project/internal/ProductEnvelopeV8AtomicWriting.h"

namespace {
using namespace solidar;
using namespace solidar::project;

constexpr ProjectId kProject{{0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                              0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x08}};
constexpr ProjectId kExternal{{0x22, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                               0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x09}};

ProductEnvelopeV5Metadata metadata() {
  return {QStringLiteral("вложенная сборка"),
          QStringLiteral("2026-10-27T00:00:00Z")};
}

QJsonObject handAuthoredFixture() {
  static constexpr char kJson[] = R"JSON({
    "format":"solidar-project","version":8,
    "capabilities":["linked-parts-v1","nested-assemblies-v1","product-envelope-v1"],
    "projectId":"123e4567-e89b-42d3-a456-426614174008",
    "rootDefinitionId":3,
    "metadata":{"name":"fixture","createdAt":"2026-10-27T00:00:00Z"},
    "definitions":[
      {"definitionId":1,"definitionKind":"part","payload":{"payloadKind":"embedded-part-v4","data":{"format":"solidar-project","version":4,"capabilities":["persistent-topology-v2"],"constraintTypeEncoding":"stable-name-v1","name":"fixture","createdAt":"2026-10-27T00:00:00Z","document":{"widthMm":60,"heightMm":40,"extrusionMm":25},"sketches":[],"extrusion":{"enabled":false},"model":{"sketches":[],"bodies":[],"referenceImages":[]}}}},
      {"definitionId":2,"definitionKind":"assembly","occurrences":[{"occurrenceId":7,"targetDefinitionId":1,"placement":{"translationMm":[0,2,0],"quaternionWxyz":[1,0,0,0]},"name":"leaf","visible":true,"suppressed":false,"grounded":true}]},
      {"definitionId":3,"definitionKind":"assembly","occurrences":[{"occurrenceId":5,"targetDefinitionId":2,"placement":{"translationMm":[1,0,0],"quaternionWxyz":[1,0,0,0]},"name":"branch","visible":true,"suppressed":false,"grounded":false}]}
    ]
  })JSON";
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(QByteArray(kJson), &error);
  CHECK(error.error == QJsonParseError::NoError);
  CHECK(document.isObject());
  return document.object();
}

struct CursorSample {
  BodyId body{};
  SketchId sketch{};
  FeatureId feature{};
  ProductIncarnationId product;
};

CursorSample consumeCursors() {
  Document document;
  const SketchId sketch = document.addSketch("cursor sketch").id;
  auto& body = document.addBody("cursor body");
  auto feature = std::make_unique<ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(), "cursor feature");
  const FeatureId featureId = feature->id();
  body.addFeature(std::move(feature));
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{1}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto product = ProductDocument::create(std::move(definitions),
                                         DefinitionId{1});
  CHECK(product);
  return {body.id(), sketch, featureId, product.product->incarnation()};
}

std::unique_ptr<ProductDocument> nestedProduct() {
  auto makePart = [](std::uint64_t base, bool withFeature) {
    auto document = std::make_unique<Document>();
    document->addSketch(static_cast<SketchId>(base), "persisted sketch");
    auto& body = document->addBody(static_cast<BodyId>(base), "persisted body");
    if (withFeature)
      body.addFeature(std::make_unique<ImportedShapeFeature>(
          static_cast<FeatureId>(base),
          std::make_shared<const TopoDS_Shape>(
              BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape()),
          "persisted feature"));
    CHECK(document->recompute());
    return document;
  };
  std::vector<PartDefinition> parts;
  parts.emplace_back(DefinitionId{4}, makePart(900, true));
  SourceContentHash hash;
  hash.bytes[0] = 42;
  LinkedPartBinding binding{{kExternal, DefinitionId{44}},
                            "library/terminal.solidar", hash};
  parts.emplace_back(DefinitionId{8}, makePart(901, false), binding);

  std::vector<AssemblyDefinition> assemblies;
  AssemblyDefinition shared(DefinitionId{3});
  CHECK(shared.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{4}, RigidTransform3d::identity(),
            {true, false, true}, "leaf")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition linkedBranch(DefinitionId{9});
  CHECK(linkedBranch.insert(ComponentOccurrence(
            OccurrenceId{2}, DefinitionId{8}, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{1});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{5}, DefinitionId{3}, RigidTransform3d::identity(),
            {false, false, false}, "left")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{6}, DefinitionId{3}, RigidTransform3d::identity(),
            {true, true, false}, "right")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{9}, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(root));
  assemblies.push_back(std::move(shared));
  assemblies.push_back(std::move(linkedBranch));
  auto created = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

ProductEnvelopeV8ScanResult scan(const QJsonObject& root) {
  const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  return scanProductEnvelopeV8(QByteArrayView(bytes));
}

class RecordingTransaction final
    : public project::detail::ProjectAtomicWriteTransaction {
 public:
  enum class Failure { None, Open, ShortWrite, Flush, Sync, Commit };

  bool open(QString* error) override {
    calls << QStringLiteral("open");
    if (failure == Failure::Open) {
      if (error) *error = QStringLiteral("V8 open failure");
      return false;
    }
    return true;
  }
  qint64 write(QByteArrayView bytes, QString* error) override {
    calls << QStringLiteral("write");
    if (failure == Failure::ShortWrite) {
      if (error) *error = QStringLiteral("short V8 write");
      return bytes.size() - 1;
    }
    staged = QByteArray(bytes.data(), bytes.size());
    return bytes.size();
  }
  bool flush(QString* error) override {
    calls << QStringLiteral("flush");
    if (failure == Failure::Flush) {
      if (error) *error = QStringLiteral("V8 flush failure");
      return false;
    }
    return true;
  }
  bool sync(QString* error) override {
    calls << QStringLiteral("sync");
    if (failure == Failure::Sync) {
      if (error) *error = QStringLiteral("V8 sync failure");
      return false;
    }
    return true;
  }
  bool commit(QString* error) override {
    calls << QStringLiteral("commit");
    if (failure == Failure::Commit) {
      if (error) *error = QStringLiteral("V8 replace failure");
      return false;
    }
    destination = staged;
    return true;
  }
  void cancel() noexcept override { calls << QStringLiteral("cancel"); }

  Failure failure{Failure::None};
  QStringList calls;
  QByteArray staged;
  QByteArray destination{"sentinel"};
};

void expectInvalid(const QJsonObject& root) {
  const auto result = scan(root);
  CHECK(result.status == ProductEnvelopeV8ScanStatus::Invalid);
  CHECK(!result.candidate);
}

QJsonArray definitionsOf(const QJsonObject& root) {
  return root.value(QStringLiteral("definitions")).toArray();
}

QJsonObject occurrence(int id, int target, QString name = {}) {
  return {{QStringLiteral("occurrenceId"), id},
          {QStringLiteral("targetDefinitionId"), target},
          {QStringLiteral("placement"),
           QJsonObject{{QStringLiteral("translationMm"), QJsonArray{0, 0, 0}},
                       {QStringLiteral("quaternionWxyz"), QJsonArray{1, 0, 0, 0}}}},
          {QStringLiteral("name"), std::move(name)},
          {QStringLiteral("visible"), true},
          {QStringLiteral("suppressed"), false},
          {QStringLiteral("grounded"), false}};
}

QJsonObject assemblyDefinition(int id, QJsonArray occurrences = {}) {
  return {{QStringLiteral("definitionId"), id},
          {QStringLiteral("definitionKind"), QStringLiteral("assembly")},
          {QStringLiteral("occurrences"), std::move(occurrences)}};
}

QJsonObject withDefinitions(QJsonObject root, int rootId,
                            QJsonArray definitions) {
  root[QStringLiteral("rootDefinitionId")] = rootId;
  root[QStringLiteral("definitions")] = std::move(definitions);
  return root;
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
  auto model = data[QStringLiteral("model")].toObject();
  model[QStringLiteral("bodies")] = QJsonArray{QJsonObject{
      {QStringLiteral("id"), 10}, {QStringLiteral("name"), QStringLiteral("body")},
      {QStringLiteral("visible"), true}, {QStringLiteral("features"), features}}};
  data[QStringLiteral("model")] = model;
  return data;
}

bool sameV4(const ProjectFormatV4ModelCandidate& lhs,
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
         lp.rebuildWork == rp.rebuildWork &&
         lt.topologyReferences == rt.topologyReferences &&
         lt.topologyInspections == rt.topologyInspections &&
         lt.deferredUnsupported == rt.deferredUnsupported &&
         lt.provenanceRecordCount == rt.provenanceRecordCount &&
         lt.localNameUtf8Bytes == rt.localNameUtf8Bytes;
}

void checkCandidateSemantics(const ProductEnvelopeV8Candidate& actual,
                             const ProductEnvelopeV8Candidate& expected) {
  CHECK(actual.authoredRoot == expected.authoredRoot);
  CHECK(actual.projectId == expected.projectId);
  CHECK(actual.rootDefinitionId == expected.rootDefinitionId);
  CHECK(actual.metadata == expected.metadata);
  CHECK(actual.capabilities == expected.capabilities);
  CHECK(actual.assemblies == expected.assemblies);
  CHECK(actual.parts.size() == expected.parts.size());
  for (std::size_t index = 0; index < actual.parts.size(); ++index) {
    const auto& left = actual.parts[index];
    const auto& right = expected.parts[index];
    CHECK(left.definitionId == right.definitionId);
    CHECK(left.kind == right.kind);
    CHECK(left.authoredDefinition == right.authoredDefinition);
    CHECK(left.authoredPayload == right.authoredPayload);
    CHECK(left.binding == right.binding);
    CHECK(left.lastValid.has_value() == right.lastValid.has_value());
    if (left.lastValid) CHECK(sameV4(*left.lastValid, *right.lastValid));
  }
  CHECK(actual.graph.status == expected.graph.status);
  CHECK(actual.graph.childFirstAssemblyOrder ==
        expected.graph.childFirstAssemblyOrder);
  CHECK(actual.graph.authoredOccurrences ==
        expected.graph.authoredOccurrences);
  CHECK(actual.graph.expandedVisits == expected.graph.expandedVisits);
  CHECK(actual.graph.uniqueSources == expected.graph.uniqueSources);
  CHECK(actual.graph.reachableSources == expected.graph.reachableSources);
  CHECK(actual.graph.leaves.size() == expected.graph.leaves.size());
  for (std::size_t index = 0; index < actual.graph.leaves.size(); ++index) {
    const auto& left = actual.graph.leaves[index];
    const auto& right = expected.graph.leaves[index];
    CHECK(left.path == right.path);
    CHECK(left.terminalPartDefinitionId == right.terminalPartDefinitionId);
    CHECK(left.worldTransform == right.worldTransform);
    CHECK(left.authoredLeafFlags == right.authoredLeafFlags);
    CHECK(left.effectiveVisible == right.effectiveVisible);
    CHECK(left.effectiveSuppressed == right.effectiveSuppressed);
  }
  CHECK(actual.jsonStringUtf8Bytes == expected.jsonStringUtf8Bytes);
  CHECK(actual.partPayloadBytes == expected.partPayloadBytes);
  CHECK(actual.relativePathUtf8Bytes == expected.relativePathUtf8Bytes);
  CHECK(actual.rebuildWork == expected.rebuildWork);
}

}  // namespace

int main() {
  for (int version : {6, 7}) {
    const QByteArray legacy = QStringLiteral(
        R"({"format":"solidar-project","version":%1,"quaternionWxyz":[-0,0,0,0]})")
                                  .arg(version)
                                  .toUtf8();
    const auto dispatched = scanProductEnvelopeV8(QByteArrayView(legacy));
    CHECK(dispatched.status == ProductEnvelopeV8ScanStatus::NotApplicable);
    CHECK(!dispatched.candidate);
  }
  {
    const QByteArray future = QByteArrayLiteral(
        R"({"format":"solidar-project","version":9,"quaternionWxyz":[-0,0,0,0]})");
    const auto dispatched = scanProductEnvelopeV8(QByteArrayView(future));
    CHECK(dispatched.status == ProductEnvelopeV8ScanStatus::Unsupported);
    CHECK(!dispatched.candidate);
  }
  {
    const QByteArray unrelated = QByteArrayLiteral(
        R"({"format":"another-format","version":8,"quaternionWxyz":[-0,0,0,0]})");
    const auto dispatched = scanProductEnvelopeV8(QByteArrayView(unrelated));
    CHECK(dispatched.status == ProductEnvelopeV8ScanStatus::NotApplicable);
    CHECK(!dispatched.candidate);
  }
  const QJsonObject authored = handAuthoredFixture();
  const QByteArray authoredBytes =
      QJsonDocument(authored).toJson(QJsonDocument::Compact);
  CHECK(scanProductEnvelopeV8(QByteArrayView(authoredBytes)).hasCandidate());
  {
    QByteArray escapedPositive = authoredBytes;
    const qsizetype key = escapedPositive.indexOf(
        QByteArrayLiteral("quaternionWxyz"));
    CHECK(key >= 0);
    escapedPositive.replace(key,
                            QByteArrayLiteral("quaternionWxyz").size(),
                            QByteArrayLiteral("quaternion\\u0057xyz"));
    CHECK(scanProductEnvelopeV8(QByteArrayView(escapedPositive)).hasCandidate());
    QByteArray escapedNegative = escapedPositive;
    const QByteArray positive = QByteArrayLiteral(
        "\"quaternion\\u0057xyz\":[1,0,0,0]");
    const QByteArray negative = QByteArrayLiteral(
        "\"quaternion\\u0057xyz\":[1,-0,0,0]");
    CHECK(escapedNegative.contains(positive));
    escapedNegative.replace(positive, negative);
    const auto rejected =
        scanProductEnvelopeV8(QByteArrayView(escapedNegative));
    CHECK(rejected.status == ProductEnvelopeV8ScanStatus::Invalid);
    CHECK(rejected.error.contains(QStringLiteral("negative zero")));
  }
  const QByteArray canonicalQuaternion =
      QByteArrayLiteral("\"quaternionWxyz\":[1,0,0,0]");
  const std::array<QByteArray, 4> negativeZeroSpellings{
      QByteArrayLiteral("-0"), QByteArrayLiteral("-0.0"),
      QByteArrayLiteral("-0e0"), QByteArrayLiteral("-0E0")};
  for (int context = 0; context < 2; ++context) {
    for (int component = 0; component < 4; ++component) {
      for (const QByteArray& spelling : negativeZeroSpellings) {
        QByteArray replacement = QByteArrayLiteral("\"quaternionWxyz\":[");
        for (int index = 0; index < 4; ++index) {
          if (index) replacement.append(',');
          replacement.append(index == component
                                 ? spelling
                                 : (index == 0 ? QByteArrayLiteral("1")
                                               : QByteArrayLiteral("0")));
        }
        replacement.append(']');
        QByteArray raw = authoredBytes;
        qsizetype position = -1;
        qsizetype searchFrom = 0;
        for (int found = 0; found <= context; ++found) {
          position = raw.indexOf(canonicalQuaternion, searchFrom);
          CHECK(position >= 0);
          searchFrom = position + canonicalQuaternion.size();
        }
        raw.replace(position, canonicalQuaternion.size(), replacement);
        const auto rejected = scanProductEnvelopeV8(QByteArrayView(raw));
        CHECK(rejected.status == ProductEnvelopeV8ScanStatus::Invalid);
        CHECK(rejected.error.contains(QStringLiteral("negative zero")));
      }
    }
  }
  {
    QByteArray raw = authoredBytes;
    CHECK(raw.contains(QByteArrayLiteral("\"translationMm\":[0,2,0]")));
    raw.replace(QByteArrayLiteral("\"translationMm\":[0,2,0]"),
                QByteArrayLiteral("\"translationMm\":[-0,2,0]"));
    CHECK(scanProductEnvelopeV8(QByteArrayView(raw)).hasCandidate());
  }
  {
    QByteArray raw = authoredBytes;
    CHECK(raw.contains(QByteArrayLiteral("\"name\":\"leaf\"")));
    raw.replace(QByteArrayLiteral("\"name\":\"leaf\""),
                QByteArrayLiteral(
                    "\"name\":\"quaternionWxyz [-0,-0.0,-0e0]\""));
    CHECK(scanProductEnvelopeV8(QByteArrayView(raw)).hasCandidate());
  }
  const auto authoredScan = scan(authored);
  CHECK(authoredScan.hasCandidate());
  CHECK(authoredScan.status == ProductEnvelopeV8ScanStatus::Candidate);
  CHECK(authoredScan.error.isEmpty());
  CHECK(authoredScan.candidate->authoredRoot == authored);
  CHECK(authoredScan.candidate->graph.leaves.size() == 1);
  CHECK(authoredScan.candidate->graph.leaves.front().path.entries() ==
        std::vector<OccurrenceId>({OccurrenceId{5}, OccurrenceId{7}}));
  CHECK(authoredScan.candidate->graph.visits.size() == 2);
  CHECK(authoredScan.candidate->graph.visits[0].displayNameUtf8.empty());
  CHECK(authoredScan.candidate->graph.visits[1].displayNameUtf8.empty());
  const auto authoredMaterialized =
      materializeProductEnvelopeV8(*authoredScan.candidate);
  CHECK(authoredMaterialized.succeeded());

  // Characterize name, UTF-8 and flag rejection at the strict occurrence
  // boundary so extracting the codec cannot silently broaden V8.
  {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    auto item = occurrences.at(0).toObject();
    item[QStringLiteral("name")] = QStringLiteral("   ");
    occurrences[0] = item;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    const auto rejected = scan(invalid);
    CHECK(rejected.status == ProductEnvelopeV8ScanStatus::Invalid);
    CHECK(rejected.error.contains(QStringLiteral("name invalid")));
  }
  {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    auto item = occurrences.at(0).toObject();
    item[QStringLiteral("name")] =
        QString(ProjectFile::kMaximumStringCharacters + 1, QLatin1Char('x'));
    occurrences[0] = item;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    const auto rejected = scan(invalid);
    CHECK(rejected.status == ProductEnvelopeV8ScanStatus::Invalid);
    CHECK(rejected.error.contains(QStringLiteral(".name is invalid")));
  }
  {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    auto item = occurrences.at(0).toObject();
    item[QStringLiteral("visible")] = 1;
    occurrences[0] = item;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    const auto rejected = scan(invalid);
    CHECK(rejected.status == ProductEnvelopeV8ScanStatus::Invalid);
    CHECK(rejected.error.contains(QStringLiteral("flags invalid")));
  }
  {
    QByteArray invalidUtf8 = authoredBytes;
    const QByteArray validName = QByteArrayLiteral("\"name\":\"leaf\"");
    CHECK(invalidUtf8.contains(validName));
    QByteArray invalidName = QByteArrayLiteral("\"name\":\"");
    invalidName.append(char(0xc3));
    invalidName.append(char(0x28));
    invalidName.append('"');
    invalidUtf8.replace(validName, invalidName);
    const auto rejected =
        scanProductEnvelopeV8(QByteArrayView(invalidUtf8));
    CHECK(rejected.status == ProductEnvelopeV8ScanStatus::Invalid);
    CHECK(!rejected.candidate);
  }

  for (const QString& field :
       {QStringLiteral("format"), QStringLiteral("version"),
        QStringLiteral("capabilities"), QStringLiteral("projectId"),
        QStringLiteral("rootDefinitionId"), QStringLiteral("metadata"),
        QStringLiteral("definitions")}) {
    auto invalid = authored;
    invalid.remove(field);
    expectInvalid(invalid);
  }
  for (const QString& field :
       {QStringLiteral("name"), QStringLiteral("createdAt")}) {
    auto invalid = authored;
    auto value = invalid.value(QStringLiteral("metadata")).toObject();
    value.remove(field);
    invalid[QStringLiteral("metadata")] = value;
    expectInvalid(invalid);
  }
  for (const QString& field :
       {QStringLiteral("definitionId"), QStringLiteral("definitionKind"),
        QStringLiteral("payload")}) {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto part = definitions.at(0).toObject();
    part.remove(field);
    definitions[0] = part;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  for (const QString& field :
       {QStringLiteral("payloadKind"), QStringLiteral("data")}) {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto part = definitions.at(0).toObject();
    auto payload = part.value(QStringLiteral("payload")).toObject();
    payload.remove(field);
    part[QStringLiteral("payload")] = payload;
    definitions[0] = part;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  for (const QString& field :
       {QStringLiteral("definitionId"), QStringLiteral("definitionKind"),
        QStringLiteral("occurrences")}) {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    assembly.remove(field);
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  for (const QString& field :
       {QStringLiteral("occurrenceId"),
        QStringLiteral("targetDefinitionId"), QStringLiteral("placement"),
        QStringLiteral("name"), QStringLiteral("visible"),
        QStringLiteral("suppressed"), QStringLiteral("grounded")}) {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    auto item = occurrences.at(0).toObject();
    item.remove(field);
    occurrences[0] = item;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  for (const QString& field :
       {QStringLiteral("translationMm"),
        QStringLiteral("quaternionWxyz")}) {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    auto item = occurrences.at(0).toObject();
    auto placement = item.value(QStringLiteral("placement")).toObject();
    placement.remove(field);
    item[QStringLiteral("placement")] = placement;
    occurrences[0] = item;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    const std::array<QString, 7> categories{
        QStringLiteral("root"), QStringLiteral("metadata"),
        QStringLiteral("part"), QStringLiteral("payload"),
        QStringLiteral("assembly"), QStringLiteral("occurrence"),
        QStringLiteral("placement")};
    for (const QString& category : categories) {
      auto invalid = authored;
      auto definitions = definitionsOf(invalid);
      if (category == QStringLiteral("root")) {
        invalid[QStringLiteral("unexpected")] = true;
      } else if (category == QStringLiteral("metadata")) {
        auto object = invalid.value(QStringLiteral("metadata")).toObject();
        object[QStringLiteral("unexpected")] = true;
        invalid[QStringLiteral("metadata")] = object;
      } else if (category == QStringLiteral("part")) {
        auto object = definitions.at(0).toObject();
        object[QStringLiteral("unexpected")] = true;
        definitions[0] = object;
        invalid[QStringLiteral("definitions")] = definitions;
      } else if (category == QStringLiteral("payload")) {
        auto part = definitions.at(0).toObject();
        auto object = part.value(QStringLiteral("payload")).toObject();
        object[QStringLiteral("unexpected")] = true;
        part[QStringLiteral("payload")] = object;
        definitions[0] = part;
        invalid[QStringLiteral("definitions")] = definitions;
      } else if (category == QStringLiteral("assembly")) {
        auto object = definitions.at(1).toObject();
        object[QStringLiteral("unexpected")] = true;
        definitions[1] = object;
        invalid[QStringLiteral("definitions")] = definitions;
      } else {
        auto assembly = definitions.at(1).toObject();
        auto occurrences =
            assembly.value(QStringLiteral("occurrences")).toArray();
        auto object = occurrences.at(0).toObject();
        if (category == QStringLiteral("placement")) {
          auto placement = object.value(QStringLiteral("placement")).toObject();
          placement[QStringLiteral("unexpected")] = true;
          object[QStringLiteral("placement")] = placement;
        } else {
          object[QStringLiteral("unexpected")] = true;
        }
        occurrences[0] = object;
        assembly[QStringLiteral("occurrences")] = occurrences;
        definitions[1] = assembly;
        invalid[QStringLiteral("definitions")] = definitions;
      }
      expectInvalid(invalid);
    }
  }
  {
    for (const QJsonArray& capabilities :
         {QJsonArray{QStringLiteral("nested-assemblies-v1"),
                     QStringLiteral("linked-parts-v1"),
                     QStringLiteral("product-envelope-v1")},
          QJsonArray{QStringLiteral("linked-parts-v1"),
                     QStringLiteral("linked-parts-v1"),
                     QStringLiteral("product-envelope-v1")},
          QJsonArray{QStringLiteral("linked-parts-v1"),
                     QStringLiteral("nested-assemblies-v1"),
                     QStringLiteral("unknown-v1")}}) {
      auto invalid = authored;
      invalid[QStringLiteral("capabilities")] = capabilities;
      expectInvalid(invalid);
    }
  }
  {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    const auto first = definitions.at(0);
    definitions[0] = definitions.at(1);
    definitions[1] = first;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
    definitions = definitionsOf(authored);
    auto duplicate = definitions.at(1).toObject();
    duplicate[QStringLiteral("definitionId")] = 1;
    definitions[1] = duplicate;
    invalid = authored;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    occurrences.push_back(occurrence(8, 1));
    const auto first = occurrences.at(0);
    occurrences[0] = occurrences.at(1);
    occurrences[1] = first;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
    occurrences[0] = first;
    auto duplicate = occurrences.at(1).toObject();
    duplicate[QStringLiteral("occurrenceId")] = 7;
    occurrences[1] = duplicate;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid = authored;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }

  auto product = nestedProduct();
  const auto encoded = encodeProductEnvelopeV8Root(*product, kProject, metadata());
  const auto encodedAgain =
      encodeProductEnvelopeV8Root(*product, kProject, metadata());
  CHECK(encoded.succeeded());
  CHECK(encodedAgain.succeeded());
  CHECK(encoded.payload->bytes == encodedAgain.payload->bytes);
  CHECK(encoded.payload->root.value(QStringLiteral("version")).toInt() == 8);
  CHECK(encoded.payload->root.value(QStringLiteral("capabilities"))
            .toArray().contains(QStringLiteral("nested-assemblies-v1")));

  {
    const auto baseDefinitions = definitionsOf(encoded.payload->root);
    qsizetype linkedIndex = -1;
    for (qsizetype index = 0; index < baseDefinitions.size(); ++index) {
      if (baseDefinitions.at(index).toObject()
              .value(QStringLiteral("definitionId")).toInt() == 8) {
        linkedIndex = index;
        break;
      }
    }
    CHECK(linkedIndex >= 0);
    for (const QString& field :
         {QStringLiteral("payloadKind"), QStringLiteral("binding"),
          QStringLiteral("lastValid")}) {
      auto invalid = encoded.payload->root;
      auto definitions = definitionsOf(invalid);
      auto part = definitions.at(linkedIndex).toObject();
      auto payload = part.value(QStringLiteral("payload")).toObject();
      payload.remove(field);
      part[QStringLiteral("payload")] = payload;
      definitions[linkedIndex] = part;
      invalid[QStringLiteral("definitions")] = definitions;
      expectInvalid(invalid);
    }
    for (const QString& field :
         {QStringLiteral("projectId"), QStringLiteral("definitionId"),
          QStringLiteral("relativePathHint"), QStringLiteral("sha256")}) {
      auto invalid = encoded.payload->root;
      auto definitions = definitionsOf(invalid);
      auto part = definitions.at(linkedIndex).toObject();
      auto payload = part.value(QStringLiteral("payload")).toObject();
      auto binding = payload.value(QStringLiteral("binding")).toObject();
      binding.remove(field);
      payload[QStringLiteral("binding")] = binding;
      part[QStringLiteral("payload")] = payload;
      definitions[linkedIndex] = part;
      invalid[QStringLiteral("definitions")] = definitions;
      expectInvalid(invalid);
    }
    for (bool bindingExtra : {false, true}) {
      auto invalid = encoded.payload->root;
      auto definitions = definitionsOf(invalid);
      auto part = definitions.at(linkedIndex).toObject();
      auto payload = part.value(QStringLiteral("payload")).toObject();
      if (bindingExtra) {
        auto binding = payload.value(QStringLiteral("binding")).toObject();
        binding[QStringLiteral("unexpected")] = true;
        payload[QStringLiteral("binding")] = binding;
      } else {
        payload[QStringLiteral("unexpected")] = true;
      }
      part[QStringLiteral("payload")] = payload;
      definitions[linkedIndex] = part;
      invalid[QStringLiteral("definitions")] = definitions;
      expectInvalid(invalid);
    }
  }

  const auto scanned = scanProductEnvelopeV8(
      QByteArrayView(encoded.payload->bytes));
  CHECK(scanned.hasCandidate());
  CHECK(scanned.candidate->assemblies.size() == 3);
  CHECK(scanned.candidate->graph.leaves.size() == 3);
  CHECK(scanned.candidate->graph.uniqueSources == 2);
  CHECK(scanned.candidate->graph.reachableSources == 2);
  CHECK(scanned.candidate->graph.leaves[0].path.entries() ==
        std::vector<OccurrenceId>({OccurrenceId{5}, OccurrenceId{7}}));
  CHECK(scanned.candidate->graph.leaves[1].path.entries() ==
        std::vector<OccurrenceId>({OccurrenceId{6}, OccurrenceId{7}}));
  CHECK(scanned.candidate->graph.leaves[2].terminalPartDefinitionId ==
        DefinitionId{8});
  CHECK(!scanned.candidate->graph.leaves[0].effectiveVisible);
  CHECK(scanned.candidate->graph.leaves[1].effectiveSuppressed);

  const auto materialized = materializeProductEnvelopeV8(*scanned.candidate);
  CHECK(materialized.succeeded());
  CHECK(materialized.product->definitions().size() == 5);
  CHECK(materialized.product->definitions().findAssembly(DefinitionId{3}));
  CHECK(materialized.product->partLinkedBinding(DefinitionId{8}));
  const auto roundtrip = encodeProductEnvelopeV8Root(
      *materialized.product, kProject, metadata());
  CHECK(roundtrip.succeeded());
  CHECK(roundtrip.payload->bytes == encoded.payload->bytes);

  const ProductEnvelopeV8Candidate candidateBefore = *scanned.candidate;
  const auto sentinelBefore = encodeProductEnvelopeV8Root(
      *product, kProject, metadata());
  CHECK(sentinelBefore.succeeded());
  const auto cursorBefore = consumeCursors();
  const auto late = materializeProductEnvelopeV8(
      *scanned.candidate,
      ProductEnvelopeV8MaterializationFailurePoint::DeepestPartRecompute);
  const auto cursorAfter = consumeCursors();
  CHECK(!late.succeeded());
  CHECK(!late.product);
  CHECK(cursorAfter.body == cursorBefore.body + 1);
  CHECK(cursorAfter.sketch == cursorBefore.sketch + 1);
  CHECK(cursorAfter.feature == cursorBefore.feature + 1);
  CHECK(cursorAfter.product.value() == cursorBefore.product.value() + 1);
  checkCandidateSemantics(*scanned.candidate, candidateBefore);
  const auto sentinelAfter = encodeProductEnvelopeV8Root(
      *product, kProject, metadata());
  CHECK(sentinelAfter.succeeded());
  CHECK(sentinelAfter.payload->bytes == sentinelBefore.payload->bytes);

  // Revalidation must reject a detached semantic carrier that no longer
  // matches the authored root, before consuming any model/product cursor or
  // touching either the source candidate or an existing product.
  auto tampered = candidateBefore;
  CHECK(!tampered.graph.leaves.empty());
  const auto moved = RigidTransform3d::create({99.0, 0.0, 0.0}, {1, 0, 0, 0});
  CHECK(moved);
  tampered.graph.leaves.front().worldTransform = *moved.transform;
  const ProductEnvelopeV8Candidate tamperedBefore = tampered;
  const auto tamperCursorBefore = consumeCursors();
  const auto tamperedResult = materializeProductEnvelopeV8(tampered);
  const auto tamperCursorAfter = consumeCursors();
  CHECK(!tamperedResult.succeeded());
  CHECK(!tamperedResult.product);
  CHECK(tamperCursorAfter.body == tamperCursorBefore.body + 1);
  CHECK(tamperCursorAfter.sketch == tamperCursorBefore.sketch + 1);
  CHECK(tamperCursorAfter.feature == tamperCursorBefore.feature + 1);
  CHECK(tamperCursorAfter.product.value() ==
        tamperCursorBefore.product.value() + 1);
  checkCandidateSemantics(tampered, tamperedBefore);
  checkCandidateSemantics(*scanned.candidate, candidateBefore);
  const auto afterTamper = encodeProductEnvelopeV8Root(
      *product, kProject, metadata());
  CHECK(afterTamper.succeeded());
  CHECK(afterTamper.payload->bytes == sentinelBefore.payload->bytes);

  // V7 remains byte-stable/fail-closed for the nested domain.
  const auto v7 = encodeProductEnvelopeV7Root(*product, kProject, metadata());
  CHECK(!v7.succeeded());
  const auto v7Scan = scanProductEnvelopeV7(
      QByteArrayView(encoded.payload->bytes));
  CHECK(v7Scan.status == ProductEnvelopeV7ScanStatus::Unsupported);

  {
    auto invalid = encoded.payload->root;
    auto capabilities = invalid.value(QStringLiteral("capabilities")).toArray();
    capabilities[1] = QStringLiteral("flat-assembly-v1");
    invalid[QStringLiteral("capabilities")] = capabilities;
    expectInvalid(invalid);
  }
  {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    auto occurrence = occurrences.at(0).toObject();
    auto placement = occurrence.value(QStringLiteral("placement")).toObject();
    placement[QStringLiteral("quaternionWxyz")] = QJsonArray{2, 0, 0, 0};
    occurrence[QStringLiteral("placement")] = placement;
    occurrences[0] = occurrence;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = authored;
    auto definitions = definitionsOf(invalid);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    auto occurrence = occurrences.at(0).toObject();
    auto placement = occurrence.value(QStringLiteral("placement")).toObject();
    placement[QStringLiteral("quaternionWxyz")] = QJsonArray{-1, 0, 0, 0};
    occurrence[QStringLiteral("placement")] = placement;
    occurrences[0] = occurrence;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }
  {
    auto invalid = encoded.payload->root;
    invalid[QStringLiteral("extra")] = true;
    expectInvalid(invalid);
  }
  {
    auto invalid = encoded.payload->root;
    auto definitions = definitionsOf(invalid);
    auto root = definitions.at(0).toObject();
    auto occurrences = root.value(QStringLiteral("occurrences")).toArray();
    auto first = occurrences.at(0).toObject();
    first[QStringLiteral("targetDefinitionId")] = 1;
    occurrences[0] = first;
    root[QStringLiteral("occurrences")] = occurrences;
    definitions[0] = root;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);  // self-cycle
  }
  {
    auto invalid = encoded.payload->root;
    auto definitions = definitionsOf(invalid);
    auto root = definitions.at(0).toObject();
    auto occurrences = root.value(QStringLiteral("occurrences")).toArray();
    auto first = occurrences.at(0).toObject();
    first[QStringLiteral("targetDefinitionId")] = 777;
    occurrences[0] = first;
    root[QStringLiteral("occurrences")] = occurrences;
    definitions[0] = root;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);  // dangling
  }
  {
    auto invalid = encoded.payload->root;
    auto definitions = definitionsOf(invalid);
    // Duplicate a linked source binding onto the embedded part.
    auto embedded = definitions.at(2).toObject();
    const auto linkedPayload =
        definitions.at(3).toObject().value(QStringLiteral("payload")).toObject();
    embedded[QStringLiteral("payload")] = linkedPayload;
    definitions[2] = embedded;
    invalid[QStringLiteral("definitions")] = definitions;
    expectInvalid(invalid);
  }

  const QJsonObject authoredPart = definitionsOf(authored).at(0).toObject();
  {
    QJsonArray definitions{authoredPart};
    for (int id = 2; id <= static_cast<int>(kAssemblyGraphMaximumDefinitions);
         ++id)
      definitions.push_back(assemblyDefinition(id));
    const auto exact = scan(withDefinitions(authored, 2, definitions));
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->graph.childFirstAssemblyOrder.size() ==
          kAssemblyGraphMaximumDefinitions - 1);
    CHECK(materializeProductEnvelopeV8(*exact.candidate).succeeded());
    definitions.push_back(
        assemblyDefinition(kAssemblyGraphMaximumDefinitions + 1));
    expectInvalid(withDefinitions(authored, 2, definitions));
  }
  QJsonObject exactOccurrencesRoot;
  {
    QJsonArray occurrences;
    for (int id = 1;
         id <= static_cast<int>(kAssemblyGraphMaximumAuthoredOccurrences);
         ++id)
      occurrences.push_back(occurrence(id, 1));
    QJsonArray definitions{authoredPart,
                           assemblyDefinition(2, occurrences)};
    exactOccurrencesRoot = withDefinitions(authored, 2, definitions);
    const auto exact = scan(exactOccurrencesRoot);
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->graph.authoredOccurrences ==
          kAssemblyGraphMaximumAuthoredOccurrences);
    CHECK(materializeProductEnvelopeV8(*exact.candidate).succeeded());
    occurrences.push_back(occurrence(
        static_cast<int>(kAssemblyGraphMaximumAuthoredOccurrences + 1), 1));
    definitions[1] = assemblyDefinition(2, occurrences);
    expectInvalid(withDefinitions(authored, 2, definitions));
  }
  {
    QJsonArray definitions{authoredPart};
    for (int id = 2; id <= 33; ++id) {
      const int target = id == 33 ? 1 : id + 1;
      definitions.push_back(
          assemblyDefinition(id, QJsonArray{occurrence(1, target)}));
    }
    const auto exact = scan(withDefinitions(authored, 2, definitions));
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->graph.leaves.front().path.depth() ==
          kAssemblyGraphMaximumPathDepth);
    CHECK(materializeProductEnvelopeV8(*exact.candidate).succeeded());
    definitions[definitions.size() - 1] =
        assemblyDefinition(33, QJsonArray{occurrence(1, 34)});
    definitions.push_back(
        assemblyDefinition(34, QJsonArray{occurrence(1, 1)}));
    expectInvalid(withDefinitions(authored, 2, definitions));
  }
  {
    QJsonArray definitions{authoredPart};
    for (int id = 2; id <= 16; ++id) {
      const int target = id == 16 ? 1 : id + 1;
      definitions.push_back(assemblyDefinition(
          id, QJsonArray{occurrence(1, target), occurrence(2, target)}));
    }
    const auto exact = scan(withDefinitions(authored, 2, definitions));
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->graph.leaves.size() ==
          kAssemblyGraphMaximumLeafInstances);
    CHECK(materializeProductEnvelopeV8(*exact.candidate).succeeded());
    auto root = definitions.at(1).toObject();
    auto occurrences = root.value(QStringLiteral("occurrences")).toArray();
    occurrences.push_back(occurrence(3, 1));
    root[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = root;
    expectInvalid(withDefinitions(authored, 2, definitions));
  }
  {
    QJsonArray definitions{authoredPart};
    for (int id = 2; id <= 16; ++id) {
      const int target = id == 16 ? 17 : id + 1;
      QJsonArray occurrences{occurrence(1, target), occurrence(2, target)};
      if (id == 2) {
        occurrences.push_back(occurrence(3, 1));
        occurrences.push_back(occurrence(4, 1));
      }
      definitions.push_back(assemblyDefinition(id, occurrences));
    }
    definitions.push_back(assemblyDefinition(17));
    const auto exact = scan(withDefinitions(authored, 2, definitions));
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->graph.expandedVisits ==
          kAssemblyGraphMaximumExpandedVisits);
    CHECK(materializeProductEnvelopeV8(*exact.candidate).succeeded());
    auto root = definitions.at(1).toObject();
    auto occurrences = root.value(QStringLiteral("occurrences")).toArray();
    occurrences.push_back(occurrence(5, 1));
    root[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = root;
    expectInvalid(withDefinitions(authored, 2, definitions));
  }
  {
    QJsonArray definitions;
    for (int id = 1; id <= static_cast<int>(kAssemblyGraphMaximumUniqueSources);
         ++id) {
      auto part = authoredPart;
      part[QStringLiteral("definitionId")] = id;
      definitions.push_back(part);
    }
    definitions.push_back(assemblyDefinition(200));
    const auto exact = scan(withDefinitions(authored, 200, definitions));
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->graph.uniqueSources ==
          kAssemblyGraphMaximumUniqueSources);
    CHECK(exact.candidate->graph.reachableSources == 0);
    CHECK(materializeProductEnvelopeV8(*exact.candidate).succeeded());
    auto extra = authoredPart;
    extra[QStringLiteral("definitionId")] = 129;
    definitions.insert(definitions.size() - 1, extra);
    expectInvalid(withDefinitions(authored, 200, definitions));
  }
  {
    auto root = exactOccurrencesRoot;
    auto baseline = scan(root);
    CHECK(baseline.hasCandidate());
    auto definitions = definitionsOf(root);
    auto assembly = definitions.at(1).toObject();
    auto occurrences = assembly.value(QStringLiteral("occurrences")).toArray();
    qsizetype remaining = kProductEnvelopeV7MaximumJsonStringUtf8Bytes -
                          baseline.candidate->jsonStringUtf8Bytes;
    CHECK(remaining > 0);
    const qsizetype common = remaining / occurrences.size();
    const qsizetype remainder = remaining % occurrences.size();
    CHECK(common + remainder <= ProjectFile::kMaximumStringCharacters);
    for (qsizetype index = 0; index < occurrences.size(); ++index) {
      auto item = occurrences.at(index).toObject();
      item[QStringLiteral("name")] = QString(
          common + (index + 1 == occurrences.size() ? remainder : 0),
          QLatin1Char('x'));
      occurrences[index] = item;
    }
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    const auto exact = scan(root);
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->jsonStringUtf8Bytes ==
          kProductEnvelopeV7MaximumJsonStringUtf8Bytes);
    CHECK(materializeProductEnvelopeV8(*exact.candidate).succeeded());
    auto last = occurrences.last().toObject();
    last[QStringLiteral("name")] =
        last[QStringLiteral("name")].toString() + QLatin1Char('x');
    occurrences[occurrences.size() - 1] = last;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = encoded.payload->root;
    auto definitions = definitionsOf(root);
    qsizetype linkedIndex = -1;
    for (qsizetype index = 0; index < definitions.size(); ++index) {
      if (definitions.at(index).toObject()
              .value(QStringLiteral("definitionId")).toInt() == 8) {
        linkedIndex = index;
        break;
      }
    }
    CHECK(linkedIndex >= 0);
    auto linked = definitions.at(linkedIndex).toObject();
    auto payload = linked.value(QStringLiteral("payload")).toObject();
    auto binding = payload.value(QStringLiteral("binding")).toObject();
    QStringList components;
    for (int i = 0; i < 256; ++i) components.push_back(QStringLiteral("a"));
    binding[QStringLiteral("relativePathHint")] = components.join('/');
    payload[QStringLiteral("binding")] = binding;
    linked[QStringLiteral("payload")] = payload;
    definitions[linkedIndex] = linked;
    root[QStringLiteral("definitions")] = definitions;
    CHECK(scan(root).hasCandidate());
    components.push_back(QStringLiteral("a"));
    binding[QStringLiteral("relativePathHint")] = components.join('/');
    payload[QStringLiteral("binding")] = binding;
    linked[QStringLiteral("payload")] = payload;
    definitions[linkedIndex] = linked;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);

    components = {QString(kProductEnvelopeV7MaximumRelativePathUtf8Bytes,
                          QLatin1Char('a'))};
    binding[QStringLiteral("relativePathHint")] = components.front();
    payload[QStringLiteral("binding")] = binding;
    linked[QStringLiteral("payload")] = payload;
    definitions[linkedIndex] = linked;
    root[QStringLiteral("definitions")] = definitions;
    const auto exactBytes = scan(root);
    CHECK(exactBytes.hasCandidate());
    CHECK(exactBytes.candidate->relativePathUtf8Bytes ==
          kProductEnvelopeV7MaximumRelativePathUtf8Bytes);
    binding[QStringLiteral("relativePathHint")] =
        components.front() + QLatin1Char('a');
    payload[QStringLiteral("binding")] = binding;
    linked[QStringLiteral("payload")] = payload;
    definitions[linkedIndex] = linked;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = authored;
    auto definitions = definitionsOf(root);
    auto first = definitions.at(0).toObject();
    auto second = first;
    second[QStringLiteral("definitionId")] = 2;
    for (auto* definition : {&first, &second}) {
      auto payload = definition->value(QStringLiteral("payload")).toObject();
      payload[QStringLiteral("data")] = v4WithPatternWork(
          payload.value(QStringLiteral("data")).toObject(), 99);
      (*definition)[QStringLiteral("payload")] = payload;
    }
    definitions = QJsonArray{first, second,
                             assemblyDefinition(3, QJsonArray{occurrence(1, 1)})};
    root = withDefinitions(root, 3, definitions);
    const auto exact = scan(root);
    CHECK(exact.hasCandidate());
    CHECK(exact.candidate->rebuildWork ==
          ProjectFile::kMaximumDocumentRebuildWork);
    auto payload = second.value(QStringLiteral("payload")).toObject();
    payload[QStringLiteral("data")] = v4WithPatternWork(
        payload.value(QStringLiteral("data")).toObject(), 100);
    second[QStringLiteral("payload")] = payload;
    definitions[1] = second;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    const QByteArray compact =
        QJsonDocument(authored).toJson(QJsonDocument::Compact);
    CHECK(compact.size() < ProjectFile::kMaximumFileBytes);
    QByteArray exact = compact;
    exact.append(ProjectFile::kMaximumFileBytes - exact.size(), ' ');
    const auto exactScan = scanProductEnvelopeV8(QByteArrayView(exact));
    CHECK(exactScan.hasCandidate());
    CHECK(materializeProductEnvelopeV8(*exactScan.candidate).succeeded());
    exact.append(' ');
    CHECK(scanProductEnvelopeV8(QByteArrayView(exact)).status ==
          ProductEnvelopeV8ScanStatus::Invalid);
  }
  CHECK(detail::fitsProductEnvelopeV7Budget(
      0, kProductEnvelopeV7MaximumPartPayloadBytes,
      kProductEnvelopeV7MaximumPartPayloadBytes));
  CHECK(!detail::fitsProductEnvelopeV7Budget(
      kProductEnvelopeV7MaximumPartPayloadBytes, 1,
      kProductEnvelopeV7MaximumPartPayloadBytes));
  CHECK(!detail::fitsProductEnvelopeV7Budget(
      std::numeric_limits<qsizetype>::max(), 1,
      kProductEnvelopeV7MaximumPartPayloadBytes));

  // A6c1b writer completes encode, exact self-scan and disconnected
  // materialization before opening the atomic destination transaction.
  {
    RecordingTransaction transaction;
    const auto written = detail::writeProductEnvelopeV8Atomically(
        *product, kProject, metadata(), transaction);
    CHECK(written.succeeded());
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    CHECK(scanProductEnvelopeV8(QByteArrayView(transaction.destination))
              .hasCandidate());
  }
  for (const auto point :
       {detail::ProductEnvelopeV8AtomicWritePreparationFailurePoint::Encode,
        detail::ProductEnvelopeV8AtomicWritePreparationFailurePoint::Scan,
        detail::ProductEnvelopeV8AtomicWritePreparationFailurePoint::
            Materialize}) {
    RecordingTransaction transaction;
    const auto written = detail::writeProductEnvelopeV8Atomically(
        *product, kProject, metadata(), transaction, point);
    CHECK(!written.succeeded());
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
  }
  for (const auto [failure, expectedFailure, expectedCalls] :
       std::vector<std::tuple<RecordingTransaction::Failure,
                              detail::ProjectAtomicWriteFailure,
                              QStringList>>{
           {RecordingTransaction::Failure::Open,
            detail::ProjectAtomicWriteFailure::OpenTemporary,
            {"open", "cancel"}},
           {RecordingTransaction::Failure::ShortWrite,
            detail::ProjectAtomicWriteFailure::WriteTemporary,
            {"open", "write", "cancel"}},
           {RecordingTransaction::Failure::Flush,
            detail::ProjectAtomicWriteFailure::FlushTemporary,
            {"open", "write", "flush", "cancel"}},
           {RecordingTransaction::Failure::Sync,
            detail::ProjectAtomicWriteFailure::SyncTemporary,
            {"open", "write", "flush", "sync", "cancel"}},
           {RecordingTransaction::Failure::Commit,
            detail::ProjectAtomicWriteFailure::ReplaceDestination,
            {"open", "write", "flush", "sync", "commit", "cancel"}}}) {
    RecordingTransaction transaction;
    transaction.failure = failure;
    const auto written = detail::writeProductEnvelopeV8Atomically(
        *product, kProject, metadata(), transaction);
    CHECK(written.failure == expectedFailure);
    CHECK(transaction.calls == expectedCalls);
    CHECK(transaction.destination == QByteArray("sentinel"));
  }

  return EXIT_SUCCESS;
}

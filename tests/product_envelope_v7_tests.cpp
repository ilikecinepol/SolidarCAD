#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>

#include "model/IdGeneration.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProductEnvelopeV7Encoder.h"
#include "project/ProductEnvelopeV7Materialization.h"
#include "project/ProjectFile.h"
#include "project/internal/ProductEnvelopeV7Budgets.h"
#include "project/internal/ProductEnvelopeV7AtomicWriting.h"

namespace {
using namespace solidar;
using namespace solidar::project;

constexpr ProjectId kProject{{0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                              0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
constexpr ProjectId kExternalProject{{0x22, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                                      0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x01}};

ProductEnvelopeV5Metadata metadata() {
  return {QStringLiteral("сборка"), QStringLiteral("2026-10-27T00:00:00Z")};
}

SourceContentHash contentHash() {
  SourceContentHash result;
  for (std::size_t i = 0; i < result.bytes.size(); ++i)
    result.bytes[i] = static_cast<std::uint8_t>(i);
  return result;
}

std::unique_ptr<ProductDocument> product() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(DefinitionId{1}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  LinkedPartBinding binding{{kExternalProject, DefinitionId{900}},
                            "library/linked-part.solidar", contentHash()};
  CHECK(binding.isValid());
  CHECK(definitions.insert(PartDefinition(DefinitionId{2}, std::make_unique<Document>(), binding)) ==
        DefinitionInsertStatus::Inserted);
  LinkedPartBinding orphanBinding{{kExternalProject, DefinitionId{901}},
                                  "library/orphan.solidar", contentHash()};
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{3}, std::make_unique<Document>(), orphanBinding)) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition assembly(DefinitionId{100});
  CHECK(assembly.insert(ComponentOccurrence(OccurrenceId{11}, DefinitionId{1},
            RigidTransform3d::identity(), {true, false, true}, "embedded")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto placement = RigidTransform3d::create(
      {10.0, 20.0, 30.0}, {0.7071067811865476, 0.0, 0.0, 0.7071067811865476});
  CHECK(placement);
  CHECK(assembly.insert(ComponentOccurrence(OccurrenceId{12}, DefinitionId{2},
            *placement.transform, {false, true, false}, "ссылка")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(assembly)) == DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(definitions), DefinitionId{100});
  CHECK(created);
  return std::move(created.product);
}

ProductEnvelopeV7ScanResult scan(const QJsonObject& root) {
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  return scanProductEnvelopeV7(QByteArrayView(bytes));
}

void expectInvalid(const QJsonObject& root) {
  const auto result = scan(root);
  CHECK(result.status == ProductEnvelopeV7ScanStatus::Invalid);
  CHECK(!result.candidate);
  CHECK(!result.error.isEmpty());
}

QJsonObject linkedPayload(const QJsonObject& root) {
  return root[QStringLiteral("definitions")].toArray().at(1).toObject()
      [QStringLiteral("payload")].toObject();
}

void replaceLinkedPayload(QJsonObject* root, const QJsonObject& payload) {
  auto definitions = (*root)[QStringLiteral("definitions")].toArray();
  auto part = definitions.at(1).toObject();
  part[QStringLiteral("payload")] = payload;
  definitions[1] = part;
  (*root)[QStringLiteral("definitions")] = definitions;
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

std::unique_ptr<ProductDocument> markerProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{500}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{500});
  CHECK(created);
  return std::move(created.product);
}

class RecordingTransaction final
    : public project::detail::ProjectAtomicWriteTransaction {
 public:
  bool open(QString*) override { calls << QStringLiteral("open"); return true; }
  qint64 write(QByteArrayView bytes, QString* error) override {
    calls << QStringLiteral("write");
    if (shortWrite) {
      if (error) *error = QStringLiteral("short V7 write");
      return bytes.size() - 1;
    }
    staged = QByteArray(bytes.data(), bytes.size());
    return bytes.size();
  }
  bool flush(QString*) override { calls << QStringLiteral("flush"); return true; }
  bool sync(QString*) override { calls << QStringLiteral("sync"); return true; }
  bool commit(QString*) override {
    calls << QStringLiteral("commit"); destination = staged; return true;
  }
  void cancel() noexcept override { calls << QStringLiteral("cancel"); }

  bool shortWrite{};
  QStringList calls;
  QByteArray staged;
  QByteArray destination{"sentinel"};
};
}  // namespace

int main() {
  static_assert(sizeof(SourceContentHash::bytes) == 32);
  CHECK(solidar::project::detail::fitsProductEnvelopeV7Budget(
      0, kProductEnvelopeV7MaximumPartPayloadBytes,
      kProductEnvelopeV7MaximumPartPayloadBytes));
  CHECK(!solidar::project::detail::fitsProductEnvelopeV7Budget(
      kProductEnvelopeV7MaximumPartPayloadBytes, 1,
      kProductEnvelopeV7MaximumPartPayloadBytes));

  auto source = product();
  const auto encoded = encodeProductEnvelopeV7Root(*source, kProject, metadata());
  const auto encodedAgain = encodeProductEnvelopeV7Root(*source, kProject, metadata());
  CHECK(encoded.succeeded());
  CHECK(encodedAgain.succeeded());
  CHECK(encoded.payload->bytes == encodedAgain.payload->bytes);
  CHECK(!encoded.payload->bytes.contains("\"brep\""));
  CHECK(!encoded.payload->bytes.contains("canonicalPath"));
  CHECK(!encoded.payload->bytes.contains("status"));

  {
    QByteArray exactFile = encoded.payload->bytes;
    const qsizetype authoredBytes = exactFile.size();
    CHECK(authoredBytes < ProjectFile::kMaximumFileBytes);
    exactFile.resize(ProjectFile::kMaximumFileBytes);
    std::fill(exactFile.begin() + authoredBytes, exactFile.end(), ' ');
    const auto exact = scanProductEnvelopeV7(QByteArrayView(exactFile));
    CHECK(exact.status == ProductEnvelopeV7ScanStatus::Candidate);
    exactFile.append(' ');
    const auto overflow = scanProductEnvelopeV7(QByteArrayView(exactFile));
    CHECK(overflow.status == ProductEnvelopeV7ScanStatus::Invalid);
    CHECK(overflow.error.contains(QStringLiteral("file limit")));
  }

  const auto scanned = scanProductEnvelopeV7(QByteArrayView(encoded.payload->bytes));
  CHECK(scanned.status == ProductEnvelopeV7ScanStatus::Candidate);
  CHECK(scanned.candidate);
  CHECK(scanned.candidate->parts.size() == 3);
  CHECK(scanned.candidate->parts[0].kind == ProductEnvelopeV7PartKind::Embedded);
  CHECK(scanned.candidate->parts[1].kind == ProductEnvelopeV7PartKind::Linked);
  CHECK(scanned.candidate->parts[1].binding);
  CHECK(scanned.candidate->parts[1].binding->key.definitionId == DefinitionId{900});
  CHECK(scanned.candidate->parts[1].binding->relativePathHintUtf8 ==
        "library/linked-part.solidar");
  CHECK(scanned.candidate->parts[1].binding->acceptedHash == contentHash());
  CHECK(scanned.candidate->parts[2].kind == ProductEnvelopeV7PartKind::Linked);
  CHECK(scanned.candidate->parts[2].binding->key.definitionId ==
        DefinitionId{901});
  CHECK(scanned.candidate->assembly->occurrences[1].displayNameUtf8 == "ссылка");

  auto restored = materializeProductEnvelopeV7(*scanned.candidate);
  CHECK(restored.succeeded());
  const auto* restoredLinked = restored.product->definitions().findPart(DefinitionId{2});
  CHECK(restoredLinked && restoredLinked->isLinked());
  const auto* restoredOrphan =
      restored.product->definitions().findPart(DefinitionId{3});
  CHECK(restoredOrphan && restoredOrphan->isLinked());
  CHECK(*restoredLinked->linkedBinding() == *scanned.candidate->parts[1].binding);
  CHECK(restored.product->rootAssemblyDefinition()->find(OccurrenceId{12})
            ->displayNameUtf8() == "ссылка");
  const auto reencoded = encodeProductEnvelopeV7Root(*restored.product, kProject, metadata());
  CHECK(reencoded.succeeded());
  CHECK(reencoded.payload->bytes == encoded.payload->bytes);

  const QJsonObject validRoot = encoded.payload->root;
  {
    auto root = validRoot;
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto third = definitions.at(2).toObject();
    auto thirdPayload = third[QStringLiteral("payload")].toObject();
    const qsizetype thirdOriginalBytes =
        QJsonDocument(thirdPayload[QStringLiteral("lastValid")].toObject())
            .toJson(QJsonDocument::Compact).size();
    const qsizetype priorBytes =
        scanned.candidate->partPayloadBytes - thirdOriginalBytes;
    const qsizetype desiredThirdBytes =
        kProductEnvelopeV7MaximumPartPayloadBytes - priorBytes;
    QJsonObject malformed{{QStringLiteral("pad"), QString{}}};
    const qsizetype emptyBytes =
        QJsonDocument(malformed).toJson(QJsonDocument::Compact).size();
    CHECK(desiredThirdBytes > emptyBytes);
    malformed[QStringLiteral("pad")] =
        QString(desiredThirdBytes - emptyBytes, QLatin1Char('x'));
    thirdPayload = QJsonObject{
        {QStringLiteral("payloadKind"), QStringLiteral("embedded-part-v4")},
        {QStringLiteral("data"), malformed}};
    third[QStringLiteral("payload")] = thirdPayload;
    definitions[2] = third;
    root[QStringLiteral("definitions")] = definitions;
    const auto exact = scan(root);
    CHECK(exact.status == ProductEnvelopeV7ScanStatus::Invalid);
    CHECK(!exact.error.contains(QStringLiteral("byte budget")));
    malformed[QStringLiteral("pad")] =
        malformed[QStringLiteral("pad")].toString() + QLatin1Char('x');
    thirdPayload[QStringLiteral("data")] = malformed;
    third[QStringLiteral("payload")] = thirdPayload;
    definitions[2] = third;
    root[QStringLiteral("definitions")] = definitions;
    const auto overflow = scan(root);
    CHECK(overflow.status == ProductEnvelopeV7ScanStatus::Invalid);
    CHECK(overflow.error.contains(QStringLiteral("byte budget")));
  }
  { auto root = validRoot; root[QStringLiteral("extra")] = true; expectInvalid(root); }
  {
    auto root = validRoot; root[QStringLiteral("version")] = 8;
    const auto result = scan(root);
    CHECK(result.status == ProductEnvelopeV7ScanStatus::Unsupported);
    CHECK(!result.candidate);
  }
  {
    auto root = validRoot;
    root[QStringLiteral("capabilities")] = QJsonArray{
        QStringLiteral("flat-assembly-v1"), QStringLiteral("future-capability"),
        QStringLiteral("product-envelope-v1")};
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    root[QStringLiteral("capabilities")] = QJsonArray{
        QStringLiteral("flat-assembly-v1"),
        QStringLiteral("flat-assembly-v1"),
        QStringLiteral("product-envelope-v1")};
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    root[QStringLiteral("capabilities")] = QJsonArray{
        QStringLiteral("product-envelope-v1"),
        QStringLiteral("linked-parts-v1"),
        QStringLiteral("flat-assembly-v1")};
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    auto definitions = root[QStringLiteral("definitions")].toArray();
    for (qsizetype index = 0; index < 2; ++index) {
      auto part = definitions.at(index).toObject();
      auto payload = part[QStringLiteral("payload")].toObject();
      const QString cacheField = index == 0 ? QStringLiteral("data")
                                             : QStringLiteral("lastValid");
      payload[cacheField] =
          v4WithPatternWork(payload[cacheField].toObject(), 99);
      part[QStringLiteral("payload")] = payload;
      definitions[index] = part;
    }
    root[QStringLiteral("definitions")] = definitions;
    // Linked lastValid forbids B-Rep, so use two embedded variants solely for
    // the aggregate rebuild-work boundary.
    auto second = definitions.at(1).toObject();
    auto secondPayload = second[QStringLiteral("payload")].toObject();
    secondPayload = QJsonObject{
        {QStringLiteral("payloadKind"), QStringLiteral("embedded-part-v4")},
        {QStringLiteral("data"), secondPayload[QStringLiteral("lastValid")]}};
    second[QStringLiteral("payload")] = secondPayload;
    definitions[1] = second;
    root[QStringLiteral("definitions")] = definitions;
    const auto boundary = scan(root);
    CHECK(boundary.status == ProductEnvelopeV7ScanStatus::Candidate);
    CHECK(boundary.candidate->rebuildWork ==
          ProjectFile::kMaximumDocumentRebuildWork);
    secondPayload[QStringLiteral("data")] = v4WithPatternWork(
        secondPayload[QStringLiteral("data")].toObject(), 100);
    second[QStringLiteral("payload")] = secondPayload;
    definitions[1] = second;
    root[QStringLiteral("definitions")] = definitions;
    const auto overflow = scan(root);
    CHECK(overflow.status == ProductEnvelopeV7ScanStatus::Invalid);
    CHECK(overflow.error.contains(QStringLiteral("rebuild-work")));
  }
  {
    auto root = validRoot; auto payload = linkedPayload(root);
    auto binding = payload[QStringLiteral("binding")].toObject();
    binding[QStringLiteral("sha256")] = QString(64, QLatin1Char('A'));
    payload[QStringLiteral("binding")] = binding; replaceLinkedPayload(&root, payload);
    expectInvalid(root);
  }
  {
    auto root = validRoot; auto payload = linkedPayload(root);
    auto binding = payload[QStringLiteral("binding")].toObject();
    binding[QStringLiteral("relativePathHint")] = QStringLiteral("../escape.solidar");
    payload[QStringLiteral("binding")] = binding; replaceLinkedPayload(&root, payload);
    expectInvalid(root);
  }
  {
    auto root = validRoot; auto payload = linkedPayload(root);
    auto binding = payload[QStringLiteral("binding")].toObject();
    const qsizetype otherLocatorBytes =
        scanned.candidate->relativePathUtf8Bytes -
        static_cast<qsizetype>(
            scanned.candidate->parts[1].binding->relativePathHintUtf8.size());
    const qsizetype exactLength =
        kProductEnvelopeV7MaximumRelativePathUtf8Bytes - otherLocatorBytes;
    binding[QStringLiteral("relativePathHint")] =
        QString(exactLength,
                QLatin1Char('a'));
    payload[QStringLiteral("binding")] = binding;
    replaceLinkedPayload(&root, payload);
    CHECK(scan(root).status == ProductEnvelopeV7ScanStatus::Candidate);
    binding[QStringLiteral("relativePathHint")] =
        QString(exactLength + 1,
                QLatin1Char('a'));
    payload[QStringLiteral("binding")] = binding;
    replaceLinkedPayload(&root, payload);
    expectInvalid(root);
  }
  {
    auto root = validRoot; auto payload = linkedPayload(root);
    payload.remove(QStringLiteral("lastValid")); replaceLinkedPayload(&root, payload);
    expectInvalid(root);
  }
  {
    auto root = validRoot; auto payload = linkedPayload(root);
    auto binding = payload[QStringLiteral("binding")].toObject();
    binding[QStringLiteral("extra")] = true;
    payload[QStringLiteral("binding")] = binding;
    replaceLinkedPayload(&root, payload); expectInvalid(root);
  }
  {
    auto root = validRoot; auto definitions = root[QStringLiteral("definitions")].toArray();
    const auto first = definitions.at(0); definitions[0] = definitions.at(1);
    definitions[1] = first; root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = validRoot; auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto occurrences = assembly[QStringLiteral("occurrences")].toArray();
    auto first = occurrences.first().toObject();
    first[QStringLiteral("targetDefinitionId")] = 999; occurrences[0] = first;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions; expectInvalid(root);
  }
  {
    auto root = validRoot;
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto occurrences = assembly[QStringLiteral("occurrences")].toArray();
    const auto first = occurrences.first();
    occurrences[0] = occurrences.at(1);
    occurrences[1] = first;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto prototype =
        assembly[QStringLiteral("occurrences")].toArray().first().toObject();
    prototype[QStringLiteral("name")] = QString{};
    QJsonArray occurrences;
    for (int id = 1; id <= kProductEnvelopeV7MaximumOccurrences; ++id) {
      auto occurrence = prototype;
      occurrence[QStringLiteral("occurrenceId")] = id;
      occurrences.push_back(occurrence);
    }
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    const auto baseline = scan(root);
    CHECK(baseline.status == ProductEnvelopeV7ScanStatus::Candidate);
    const qsizetype remaining =
        kProductEnvelopeV7MaximumJsonStringUtf8Bytes -
        baseline.candidate->jsonStringUtf8Bytes;
    CHECK(remaining > 0);
    const qsizetype common =
        remaining / kProductEnvelopeV7MaximumOccurrences;
    const qsizetype remainder =
        remaining % kProductEnvelopeV7MaximumOccurrences;
    CHECK(common + remainder <= ProjectFile::kMaximumStringCharacters);
    for (qsizetype index = 0; index < occurrences.size(); ++index) {
      auto occurrence = occurrences.at(index).toObject();
      occurrence[QStringLiteral("name")] =
          QString(common + (index + 1 == occurrences.size() ? remainder : 0),
                  QLatin1Char('x'));
      occurrences[index] = occurrence;
    }
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    const auto boundary = scan(root);
    CHECK(boundary.status == ProductEnvelopeV7ScanStatus::Candidate);
    CHECK(boundary.candidate->jsonStringUtf8Bytes ==
          kProductEnvelopeV7MaximumJsonStringUtf8Bytes);
    auto last = occurrences.last().toObject();
    last[QStringLiteral("name")] =
        last[QStringLiteral("name")].toString() + QLatin1Char('x');
    occurrences[occurrences.size() - 1] = last;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto occurrences = assembly[QStringLiteral("occurrences")].toArray();
    auto first = occurrences.first().toObject();
    first[QStringLiteral("targetDefinitionId")] = 100;
    occurrences[0] = first;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    root[QStringLiteral("rootDefinitionId")] = 1;
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto occurrences = assembly[QStringLiteral("occurrences")].toArray();
    auto first = occurrences.first().toObject();
    auto placement = first[QStringLiteral("placement")].toObject();
    placement[QStringLiteral("extra")] = 0;
    first[QStringLiteral("placement")] = placement;
    occurrences[0] = first;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = validRoot; auto definitions = root[QStringLiteral("definitions")].toArray();
    auto part = definitions.at(1).toObject();
    part.remove(QStringLiteral("payload"));
    part[QStringLiteral("definitionKind")] = QStringLiteral("assembly");
    part[QStringLiteral("occurrences")] = QJsonArray{};
    definitions[1] = part; root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    const auto original = root[QStringLiteral("definitions")].toArray();
    const auto prototype = original.first().toObject();
    auto assembly = original.last().toObject();
    assembly[QStringLiteral("definitionId")] = 1000;
    root[QStringLiteral("rootDefinitionId")] = 1000;
    QJsonArray definitions;
    for (int id = 1; id < kProductEnvelopeV7MaximumDefinitions; ++id) {
      auto part = prototype; part[QStringLiteral("definitionId")] = id;
      definitions.push_back(part);
    }
    definitions.push_back(assembly);
    root[QStringLiteral("definitions")] = definitions;
    CHECK(scan(root).status == ProductEnvelopeV7ScanStatus::Candidate);
    auto overflow = prototype;
    overflow[QStringLiteral("definitionId")] = 999;
    definitions.insert(definitions.size() - 1, overflow);
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = validRoot;
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    const auto prototype =
        assembly[QStringLiteral("occurrences")].toArray().first().toObject();
    QJsonArray occurrences;
    for (int id = 1; id <= kProductEnvelopeV7MaximumOccurrences; ++id) {
      auto occurrence = prototype;
      occurrence[QStringLiteral("occurrenceId")] = id;
      occurrences.push_back(occurrence);
    }
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    CHECK(scan(root).status == ProductEnvelopeV7ScanStatus::Candidate);
    auto overflow = prototype;
    overflow[QStringLiteral("occurrenceId")] =
        kProductEnvelopeV7MaximumOccurrences + 1;
    occurrences.push_back(overflow);
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    const QByteArray candidateBytes =
        QJsonDocument(scanned.candidate->authoredRoot)
            .toJson(QJsonDocument::Compact);
    const auto candidateBinding = scanned.candidate->parts[1].binding;
    auto markerBefore = markerProduct();
    const auto incarnationBefore = markerBefore->incarnation().value();
    Document bodyCursorBefore;
    const BodyId bodyIdBefore = bodyCursorBefore.addBody("before").id();
    auto failed = materializeProductEnvelopeV7(
        *scanned.candidate,
        ProductEnvelopeV7MaterializationFailurePoint::SecondPartRecompute);
    CHECK(!failed.succeeded());
    CHECK(!failed.product);
    CHECK(failed.error.contains(
        QStringLiteral("after successful second-part recompute")));
    CHECK(QJsonDocument(scanned.candidate->authoredRoot)
              .toJson(QJsonDocument::Compact) == candidateBytes);
    CHECK(scanned.candidate->parts[1].binding == candidateBinding);
    auto markerAfter = markerProduct();
    CHECK(markerAfter->incarnation().value() == incarnationBefore + 1);
    Document bodyCursorAfter;
    CHECK(bodyCursorAfter.addBody("after").id() == bodyIdBefore + 1);
  }
  {
    auto tampered = *scanned.candidate;
    tampered.authoredRoot[QStringLiteral("rootDefinitionId")] = 999;
    auto failed = materializeProductEnvelopeV7(tampered);
    CHECK(!failed.succeeded());
    CHECK(!failed.product);
  }
  {
    RecordingTransaction transaction;
    const auto written = project::detail::writeProductEnvelopeV7Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(written.succeeded());
    CHECK(transaction.destination == encoded.payload->bytes);
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
  }
  {
    RecordingTransaction transaction;
    transaction.shortWrite = true;
    const auto written = project::detail::writeProductEnvelopeV7Atomically(
        *source, kProject, metadata(), transaction);
    CHECK(written.failure ==
          project::detail::ProjectAtomicWriteFailure::WriteTemporary);
    CHECK(transaction.destination == QByteArray("sentinel"));
    CHECK((transaction.calls ==
           QStringList{"open", "write", "cancel"}));
  }
  {
    RecordingTransaction transaction;
    const auto written = project::detail::writeProductEnvelopeV7Atomically(
        *source, ProjectId{}, metadata(), transaction);
    CHECK(written.failure == project::detail::ProjectAtomicWriteFailure::Encode);
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
  }
  return EXIT_SUCCESS;
}

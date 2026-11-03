#include "TestAssertions.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>

#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV6Decoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProductEnvelopeV6Budgets.h"

namespace {

using namespace solidar;
using namespace solidar::project;

QByteArray fixtureBytes() {
  QFile file(QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH));
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

QJsonObject fixtureRoot() {
  const auto document = QJsonDocument::fromJson(fixtureBytes());
  CHECK(document.isObject());
  return document.object();
}

ProductEnvelopeV6ScanResult scan(const QJsonObject& root) {
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  return scanProductEnvelopeV6(QByteArrayView(bytes));
}

void expectInvalid(const QJsonObject& root) {
  const auto result = scan(root);
  CHECK(result.status == ProductEnvelopeV6ScanStatus::Invalid);
  CHECK(!result.candidate);
  CHECK(!result.error.isEmpty());
}

QJsonObject withOccurrences(QJsonObject root, qsizetype count) {
  auto definitions = root[QStringLiteral("definitions")].toArray();
  auto assembly = definitions.last().toObject();
  const auto prototype =
      assembly[QStringLiteral("occurrences")].toArray().first().toObject();
  QJsonArray occurrences;
  for (qsizetype index = 0; index < count; ++index) {
    auto occurrence = prototype;
    occurrence[QStringLiteral("occurrenceId")] = index + 1;
    occurrences.push_back(occurrence);
  }
  assembly[QStringLiteral("occurrences")] = occurrences;
  definitions[definitions.size() - 1] = assembly;
  root[QStringLiteral("definitions")] = definitions;
  return root;
}

QJsonObject withDefinitions(QJsonObject root, qsizetype count) {
  const auto original = root[QStringLiteral("definitions")].toArray();
  const auto part = original.first().toObject();
  auto assembly = original.last().toObject();
  QJsonArray definitions;
  for (qsizetype index = 1; index < count; ++index) {
    auto next = part;
    next[QStringLiteral("definitionId")] = index;
    definitions.push_back(next);
  }
  assembly[QStringLiteral("definitionId")] = 1000;
  root[QStringLiteral("rootDefinitionId")] = 1000;
  definitions.push_back(assembly);
  root[QStringLiteral("definitions")] = definitions;
  return root;
}

QJsonObject v4WithPatternWork(QJsonObject data, int finalPatternCount) {
  QJsonArray features{QJsonObject{{QStringLiteral("id"), 1},
                                  {QStringLiteral("name"),
                                   QStringLiteral("imported")},
                                  {QStringLiteral("type"),
                                   QStringLiteral("ImportedShape")},
                                  {QStringLiteral("brep"),
                                   QStringLiteral("TQ==")}}};
  for (int index = 0; index < 50; ++index) {
    features.push_back(QJsonObject{
        {QStringLiteral("id"), index + 2},
        {QStringLiteral("name"), QStringLiteral("pattern")},
        {QStringLiteral("type"), QStringLiteral("LinearPattern")},
        {QStringLiteral("sourceBodyId"), 0},
        {QStringLiteral("sourceFeatureId"), index + 1},
        {QStringLiteral("direction"), 0},
        {QStringLiteral("count"),
         index == 49 ? finalPatternCount : 100},
        {QStringLiteral("spacingMm"), 5},
        {QStringLiteral("operation"), 1}});
  }
  auto model = data[QStringLiteral("model")].toObject();
  model[QStringLiteral("bodies")] = QJsonArray{QJsonObject{
      {QStringLiteral("id"), 10},
      {QStringLiteral("name"), QStringLiteral("body")},
      {QStringLiteral("visible"), true},
      {QStringLiteral("features"), features}}};
  data[QStringLiteral("model")] = model;
  return data;
}

}  // namespace

int main() {
  CHECK(detail::fitsProductEnvelopeV6Budget(
      0, kProductEnvelopeV6MaximumEmbeddedPartBytes,
      kProductEnvelopeV6MaximumEmbeddedPartBytes));
  CHECK(detail::fitsProductEnvelopeV6Budget(
      kProductEnvelopeV6MaximumEmbeddedPartBytes, 0,
      kProductEnvelopeV6MaximumEmbeddedPartBytes));
  CHECK(!detail::fitsProductEnvelopeV6Budget(
      kProductEnvelopeV6MaximumEmbeddedPartBytes, 1,
      kProductEnvelopeV6MaximumEmbeddedPartBytes));
  CHECK(!detail::fitsProductEnvelopeV6Budget(
      kProductEnvelopeV6MaximumEmbeddedPartBytes - 1, 2,
      kProductEnvelopeV6MaximumEmbeddedPartBytes));

  const auto valid = scanProductEnvelopeV6(QByteArrayView(fixtureBytes()));
  CHECK(valid.status == ProductEnvelopeV6ScanStatus::Candidate);
  CHECK(valid.candidate);
  CHECK(valid.candidate->rootDefinitionId == DefinitionId{100});
  CHECK(valid.candidate->parts.size() == 2);
  CHECK(valid.candidate->parts[0].definitionId == DefinitionId{1});
  CHECK(valid.candidate->parts[1].definitionId == DefinitionId{2});
  CHECK(valid.candidate->assembly);
  CHECK(valid.candidate->assembly->occurrences.size() == 2);
  CHECK(valid.candidate->assembly->occurrences[0].targetDefinitionId ==
        DefinitionId{1});
  CHECK(valid.candidate->assembly->occurrences[1].flags.suppressed);
  CHECK(valid.candidate->rebuildWork == 0);
  CHECK(valid.candidate->embeddedPartBytes > 0);
  CHECK(valid.candidate->jsonStringUtf8Bytes > 0);

  // V5 remains closed and treats V6 as a newer unsupported version.
  const auto v5 = scanProductEnvelopeV5(QByteArrayView(fixtureBytes()));
  CHECK(v5.status == ProductEnvelopeV5ScanStatus::Unsupported);
  CHECK(!v5.candidate);

  {
    auto root = fixtureRoot();
    root[QStringLiteral("mates")] = QJsonArray{};
    expectInvalid(root);
  }
  {
    // Exercise the scanner's real 48 MiB aggregate rejection branch. Exact
    // N/N+1 arithmetic is covered above without permanently storing a huge
    // corpus file.
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    const auto firstPayload =
        definitions.at(0).toObject()[QStringLiteral("payload")].toObject();
    const qsizetype firstBytes =
        QJsonDocument(firstPayload[QStringLiteral("data")].toObject())
            .toJson(QJsonDocument::Compact)
            .size();
    auto second = definitions.at(1).toObject();
    auto secondPayload = second[QStringLiteral("payload")].toObject();
    secondPayload[QStringLiteral("payloadKind")] =
        QStringLiteral("future-part");
    QJsonObject data{{QStringLiteral("pad"), QString{}}};
    const qsizetype emptyDataBytes =
        QJsonDocument(data).toJson(QJsonDocument::Compact).size();
    const qsizetype padding =
        kProductEnvelopeV6MaximumEmbeddedPartBytes + 1 - firstBytes -
        emptyDataBytes;
    CHECK(padding > 0);
    data[QStringLiteral("pad")] = QString(padding, QLatin1Char('x'));
    secondPayload[QStringLiteral("data")] = data;
    second[QStringLiteral("payload")] = secondPayload;
    definitions[1] = second;
    root[QStringLiteral("definitions")] = definitions;
    const auto rejected = scan(root);
    CHECK(rejected.status == ProductEnvelopeV6ScanStatus::Invalid);
    CHECK(rejected.error.contains(QStringLiteral("embedded part payloads")));
  }
  {
    auto root = fixtureRoot();
    root[QStringLiteral("nesting")] = QJsonObject{};
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    assembly[QStringLiteral("mates")] = QJsonArray{};
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto duplicate = definitions.at(1).toObject();
    duplicate[QStringLiteral("definitionId")] = 1;
    definitions[1] = duplicate;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto occurrences = assembly[QStringLiteral("occurrences")].toArray();
    auto second = occurrences.at(1).toObject();
    second[QStringLiteral("occurrenceId")] = 11;
    occurrences[1] = second;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto occurrences = assembly[QStringLiteral("occurrences")].toArray();
    auto first = occurrences.first().toObject();
    first[QStringLiteral("targetDefinitionId")] = 999;
    occurrences[0] = first;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto assembly = definitions.last().toObject();
    auto occurrences = assembly[QStringLiteral("occurrences")].toArray();
    auto first = occurrences.first().toObject();
    auto placement = first[QStringLiteral("placement")].toObject();
    placement[QStringLiteral("quaternionWxyz")] = QJsonArray{0, 0, 0, 0};
    first[QStringLiteral("placement")] = placement;
    occurrences[0] = first;
    assembly[QStringLiteral("occurrences")] = occurrences;
    definitions[definitions.size() - 1] = assembly;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto nested = definitions.at(1).toObject();
    nested.remove(QStringLiteral("payload"));
    nested[QStringLiteral("definitionKind")] = QStringLiteral("assembly");
    nested[QStringLiteral("occurrences")] = QJsonArray{};
    definitions[1] = nested;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    root[QStringLiteral("capabilities")] =
        QJsonArray{QStringLiteral("flat-assembly-v1"),
                   QStringLiteral("product-envelope-v1"),
                   QStringLiteral("z-future")};
    const auto result = scan(root);
    CHECK(result.status == ProductEnvelopeV6ScanStatus::Unsupported);
    CHECK(result.candidate);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto part = definitions.at(1).toObject();
    part[QStringLiteral("definitionKind")] = QStringLiteral("future-part");
    definitions[1] = part;
    root[QStringLiteral("definitions")] = definitions;
    const auto result = scan(root);
    CHECK(result.status == ProductEnvelopeV6ScanStatus::Unsupported);
    CHECK(result.candidate);

    // Deferred unknown semantics never hide a known structural violation.
    part[QStringLiteral("mates")] = QJsonArray{};
    definitions[1] = part;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto part = definitions.first().toObject();
    auto payload = part[QStringLiteral("payload")].toObject();
    payload[QStringLiteral("payloadKind")] = QStringLiteral("future-part");
    part[QStringLiteral("payload")] = payload;
    definitions[0] = part;
    root[QStringLiteral("definitions")] = definitions;
    const auto result = scan(root);
    CHECK(result.status == ProductEnvelopeV6ScanStatus::Unsupported);
    CHECK(result.candidate);
  }
  {
    auto root = fixtureRoot();
    root[QStringLiteral("version")] = 7;
    CHECK(scan(root).status == ProductEnvelopeV6ScanStatus::Unsupported);
    QJsonObject deeplyNested{
        {QStringLiteral("oversized"),
         QString(kProductEnvelopeV6MaximumJsonStringUtf8Bytes + 1,
                 QLatin1Char('x'))}};
    for (int depth = 0; depth < 70; ++depth)
      deeplyNested =
          QJsonObject{{QStringLiteral("next"), deeplyNested}};
    root[QStringLiteral("futureSchemaContent")] = deeplyNested;
    CHECK(scan(root).status == ProductEnvelopeV6ScanStatus::Unsupported);
    root[QStringLiteral("version")] = 5;
    CHECK(scan(root).status == ProductEnvelopeV6ScanStatus::NotApplicable);
  }
  {
    const auto boundary = scan(withDefinitions(
        fixtureRoot(), kProductEnvelopeV6MaximumDefinitions));
    CHECK(boundary.status == ProductEnvelopeV6ScanStatus::Candidate);
    expectInvalid(withDefinitions(
        fixtureRoot(), kProductEnvelopeV6MaximumDefinitions + 1));
  }
  {
    const auto boundary = scan(withOccurrences(
        fixtureRoot(), kProductEnvelopeV6MaximumOccurrences));
    CHECK(boundary.status == ProductEnvelopeV6ScanStatus::Candidate);
    expectInvalid(withOccurrences(
        fixtureRoot(), kProductEnvelopeV6MaximumOccurrences + 1));
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    for (qsizetype index = 0; index < 2; ++index) {
      auto part = definitions.at(index).toObject();
      auto payload = part[QStringLiteral("payload")].toObject();
      payload[QStringLiteral("data")] = v4WithPatternWork(
          payload[QStringLiteral("data")].toObject(), 99);
      part[QStringLiteral("payload")] = payload;
      definitions[index] = part;
    }
    root[QStringLiteral("definitions")] = definitions;
    const auto boundary = scan(root);
    CHECK(boundary.status == ProductEnvelopeV6ScanStatus::Candidate);
    CHECK(boundary.candidate->rebuildWork ==
          ProjectFile::kMaximumDocumentRebuildWork);

    auto second = definitions.at(1).toObject();
    auto payload = second[QStringLiteral("payload")].toObject();
    payload[QStringLiteral("data")] = v4WithPatternWork(
        payload[QStringLiteral("data")].toObject(), 100);
    second[QStringLiteral("payload")] = payload;
    definitions[1] = second;
    root[QStringLiteral("definitions")] = definitions;
    const auto over = scan(root);
    CHECK(over.status == ProductEnvelopeV6ScanStatus::Invalid);
    CHECK(over.error.contains(QStringLiteral("rebuild-work")));
  }
  {
    auto root = fixtureRoot();
    auto definitions = root[QStringLiteral("definitions")].toArray();
    auto part = definitions.at(1).toObject();
    auto payload = part[QStringLiteral("payload")].toObject();
    payload[QStringLiteral("payloadKind")] = QStringLiteral("future-part");
    payload[QStringLiteral("data")] =
        QJsonObject{{QStringLiteral("pad"), QString{}}};
    part[QStringLiteral("payload")] = payload;
    definitions[1] = part;
    root[QStringLiteral("definitions")] = definitions;
    const auto base = scan(root);
    CHECK(base.status == ProductEnvelopeV6ScanStatus::Unsupported);
    CHECK(base.candidate);
    const qsizetype padding =
        kProductEnvelopeV6MaximumJsonStringUtf8Bytes -
        base.candidate->jsonStringUtf8Bytes;
    CHECK(padding > 0);
    auto data = payload[QStringLiteral("data")].toObject();
    data[QStringLiteral("pad")] = QString(padding, QLatin1Char('x'));
    payload[QStringLiteral("data")] = data;
    part[QStringLiteral("payload")] = payload;
    definitions[1] = part;
    root[QStringLiteral("definitions")] = definitions;
    const auto boundary = scan(root);
    CHECK(boundary.status == ProductEnvelopeV6ScanStatus::Unsupported);
    CHECK(boundary.candidate->jsonStringUtf8Bytes ==
          kProductEnvelopeV6MaximumJsonStringUtf8Bytes);
    data[QStringLiteral("pad")] = QString(padding + 1, QLatin1Char('x'));
    payload[QStringLiteral("data")] = data;
    part[QStringLiteral("payload")] = payload;
    definitions[1] = part;
    root[QStringLiteral("definitions")] = definitions;
    expectInvalid(root);
  }
  return EXIT_SUCCESS;
}

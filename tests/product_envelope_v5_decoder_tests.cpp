#include "TestAssertions.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProjectFile.h"

namespace {

using solidar::project::ProductEnvelopeV5DeferredUnknownKind;
using solidar::project::ProductEnvelopeV5ScanResult;
using solidar::project::ProductEnvelopeV5ScanStatus;

QJsonObject embeddedPartV4() {
  // Hand-authored standalone-v4 fixture.  It is intentionally not produced by
  // the v4 writer so this test guards the persisted contract independently.
  return QJsonObject{
      {"format", "solidar-project"},
      {"version", 4},
      {"capabilities", QJsonArray{"persistent-topology-v2"}},
      {"constraintTypeEncoding", "stable-name-v1"},
      {"name", "embedded"},
      {"createdAt", "2026-10-23T00:00:00Z"},
      {"document", QJsonObject{{"widthMm", 60},
                                 {"heightMm", 40},
                                 {"extrusionMm", 25}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model", QJsonObject{{"sketches", QJsonArray{}},
                              {"bodies", QJsonArray{}},
                              {"referenceImages", QJsonArray{}}}}};
}

QJsonObject validRoot() {
  const QJsonObject payload{{"payloadKind", "embedded-part-v4"},
                            {"data", embeddedPartV4()}};
  const QJsonObject definition{{"definitionId", 17},
                               {"definitionKind", "part"},
                               {"payload", payload}};
  return QJsonObject{
      {"format", "solidar-project"},
      {"version", 5},
      {"capabilities", QJsonArray{"product-envelope-v1"}},
      {"projectId", "123e4567-e89b-42d3-a456-426614174000"},
      {"rootDefinitionId", 17},
      {"metadata", QJsonObject{{"name", "product"},
                                {"createdAt", "2026-10-23T00:00:00Z"}}},
      {"definitions", QJsonArray{definition}}};
}

QByteArray bytes(const QJsonObject& root) {
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

ProductEnvelopeV5ScanResult scan(const QJsonObject& root) {
  const QByteArray encoded = bytes(root);
  return solidar::project::scanProductEnvelopeV5(QByteArrayView(encoded));
}

void checkFailure(const ProductEnvelopeV5ScanResult& result,
                  ProductEnvelopeV5ScanStatus status) {
  CHECK(result.status == status);
  CHECK(!result.hasCandidate());
  CHECK(!result.candidate.has_value());
  CHECK(!result.error.isEmpty());
}

void checkInvalid(const QJsonObject& root) {
  checkFailure(scan(root), ProductEnvelopeV5ScanStatus::Invalid);
}

QJsonObject definitionAt(const QJsonObject& root) {
  return root.value("definitions").toArray().at(0).toObject();
}

void setDefinition(QJsonObject* root, QJsonObject definition) {
  (*root)["definitions"] = QJsonArray{std::move(definition)};
}

QJsonObject payloadAt(const QJsonObject& root) {
  return definitionAt(root).value("payload").toObject();
}

void setPayload(QJsonObject* root, QJsonObject payload) {
  QJsonObject definition = definitionAt(*root);
  definition["payload"] = std::move(payload);
  setDefinition(root, std::move(definition));
}

void setEmbeddedPart(QJsonObject* root, QJsonObject embedded) {
  QJsonObject payload = payloadAt(*root);
  payload["data"] = std::move(embedded);
  setPayload(root, std::move(payload));
}

}  // namespace

int main() {
  constexpr solidar::ProjectId kExpectedProjectId{
      {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
       0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
  const auto valid = scan(validRoot());
  CHECK(valid.status == ProductEnvelopeV5ScanStatus::Candidate);
  CHECK(valid.hasCandidate());
  CHECK(valid.candidate.has_value());
  CHECK(valid.error.isEmpty());
  CHECK(valid.candidate->projectId.isValid());
  CHECK(valid.candidate->projectId == kExpectedProjectId);
  CHECK(valid.candidate->rootDefinitionId == solidar::DefinitionId{17});
  CHECK(valid.candidate->definitionId == solidar::DefinitionId{17});
  CHECK(valid.candidate->metadata.name == QStringLiteral("product"));
  CHECK(valid.candidate->metadata.createdAt ==
        QStringLiteral("2026-10-23T00:00:00Z"));
  CHECK(valid.candidate->capabilities ==
        QStringList{QStringLiteral("product-envelope-v1")});
  CHECK(valid.candidate->authoredRoot == validRoot());
  CHECK(valid.candidate->authoredDefinition == definitionAt(validRoot()));
  CHECK(valid.candidate->authoredPayload == payloadAt(validRoot()));
  CHECK(valid.candidate->embeddedPart.has_value());
  CHECK(valid.candidate->deferredUnknowns.empty());

  for (const char* field : {"format", "version", "capabilities", "projectId",
                            "rootDefinitionId", "metadata", "definitions"}) {
    QJsonObject root = validRoot();
    root.remove(QLatin1String(field));
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    root["extra"] = true;
    checkInvalid(root);
  }
  for (const auto& replacement : {QJsonValue{}, QJsonValue{true},
                                  QJsonValue{QJsonArray{}}}) {
    QJsonObject root = validRoot();
    root["metadata"] = replacement;
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    QJsonObject metadata = root["metadata"].toObject();
    metadata.remove("name");
    root["metadata"] = metadata;
    checkInvalid(root);
    metadata["name"] = "product";
    metadata["extra"] = 1;
    root["metadata"] = metadata;
    checkInvalid(root);
    metadata.remove("extra");
    metadata["createdAt"] = false;
    root["metadata"] = metadata;
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    QJsonObject metadata = root["metadata"].toObject();
    metadata["name"] =
        QString(solidar::project::ProjectFile::kMaximumStringCharacters + 1,
                QLatin1Char('x'));
    root["metadata"] = metadata;
    checkInvalid(root);
  }

  for (const auto& token : {QStringLiteral("123E4567-e89b-42d3-a456-426614174000"),
                            QStringLiteral("{123e4567-e89b-42d3-a456-426614174000}"),
                            QStringLiteral("00000000-0000-0000-0000-000000000000"),
                            QStringLiteral("123e4567e89b42d3a456426614174000")}) {
    QJsonObject root = validRoot();
    root["projectId"] = token;
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    root["projectId"] = 1;
    checkInvalid(root);
  }

  for (const auto& value : {QJsonValue{0}, QJsonValue{1.5},
                            QJsonValue{static_cast<double>(
                                solidar::project::ProjectFile::
                                    kMaximumPersistedId + 1)}}) {
    QJsonObject root = validRoot();
    root["rootDefinitionId"] = value;
    checkInvalid(root);
    root = validRoot();
    QJsonObject definition = definitionAt(root);
    definition["definitionId"] = value;
    setDefinition(&root, definition);
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    const double maximumPersistedId = static_cast<double>(
        solidar::project::ProjectFile::kMaximumPersistedId);
    root["rootDefinitionId"] = maximumPersistedId;
    QJsonObject definition = definitionAt(root);
    definition["definitionId"] = maximumPersistedId;
    setDefinition(&root, definition);
    const auto boundary = scan(root);
    CHECK(boundary.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(boundary.hasCandidate());
    CHECK(boundary.candidate->rootDefinitionId.value() ==
          static_cast<std::uint64_t>(
              solidar::project::ProjectFile::kMaximumPersistedId));
    CHECK(boundary.candidate->definitionId ==
          boundary.candidate->rootDefinitionId);
  }
  {
    QJsonObject root = validRoot();
    root["rootDefinitionId"] = 18;
    checkInvalid(root);
    root = validRoot();
    root["definitions"] = QJsonArray{};
    checkInvalid(root);
    root["definitions"] =
        QJsonArray{definitionAt(validRoot()), definitionAt(validRoot())};
    checkInvalid(root);
  }

  {
    QJsonObject root = validRoot();
    root["capabilities"] = QJsonArray{};
    checkInvalid(root);
    root["capabilities"] = QJsonArray{"future-capability"};
    checkInvalid(root);
    root["capabilities"] =
        QJsonArray{"product-envelope-v1", "product-envelope-v1"};
    checkInvalid(root);
    root["capabilities"] =
        QJsonArray{"product-envelope-v1", "future-capability"};
    checkInvalid(root);
    root["capabilities"] = QJsonArray{1, "product-envelope-v1"};
    checkInvalid(root);
    QJsonArray tooMany;
    for (int index = 0; index < 17; ++index)
      tooMany.push_back(QStringLiteral("capability-%1").arg(index, 2, 10,
                                                            QLatin1Char('0')));
    root["capabilities"] = tooMany;
    checkInvalid(root);
    root["capabilities"] =
        QJsonArray{QString(65, QLatin1Char('a')), "product-envelope-v1"};
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    root["capabilities"] =
        QJsonArray{"future-capability", "product-envelope-v1"};
    const auto unsupported = scan(root);
    CHECK(unsupported.status == ProductEnvelopeV5ScanStatus::Unsupported);
    CHECK(unsupported.hasCandidate());
    CHECK(unsupported.candidate->deferredUnknowns.front().kind ==
          ProductEnvelopeV5DeferredUnknownKind::Capability);

    QJsonObject embedded = embeddedPartV4();
    embedded["model"] = QJsonArray{};
    setEmbeddedPart(&root, embedded);
    checkInvalid(root);  // structural invalid wins over deferred unsupported
  }

  for (const char* field : {"definitionId", "definitionKind", "payload"}) {
    QJsonObject root = validRoot();
    QJsonObject definition = definitionAt(root);
    definition.remove(QLatin1String(field));
    setDefinition(&root, definition);
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    QJsonObject definition = definitionAt(root);
    definition["extra"] = 1;
    setDefinition(&root, definition);
    checkInvalid(root);
    definition.remove("extra");
    definition["definitionKind"] = 1;
    setDefinition(&root, definition);
    checkInvalid(root);
    definition["definitionKind"] = "assembly";
    setDefinition(&root, definition);
    const auto unsupported = scan(root);
    CHECK(unsupported.status == ProductEnvelopeV5ScanStatus::Unsupported);
    CHECK(unsupported.hasCandidate());

    QJsonObject malformedEmbedded = embeddedPartV4();
    malformedEmbedded["model"] = QJsonArray{};
    setEmbeddedPart(&root, malformedEmbedded);
    checkInvalid(root);  // malformed known payload wins over unknown kind
  }

  for (const char* field : {"payloadKind", "data"}) {
    QJsonObject root = validRoot();
    QJsonObject payload = payloadAt(root);
    payload.remove(QLatin1String(field));
    setPayload(&root, payload);
    checkInvalid(root);
  }
  {
    QJsonObject root = validRoot();
    QJsonObject payload = payloadAt(root);
    payload["extra"] = 1;
    setPayload(&root, payload);
    checkInvalid(root);
    payload.remove("extra");
    payload["payloadKind"] = 1;
    setPayload(&root, payload);
    checkInvalid(root);
    payload["payloadKind"] = "future-part";
    setPayload(&root, payload);
    const auto unsupported = scan(root);
    CHECK(unsupported.status == ProductEnvelopeV5ScanStatus::Unsupported);
    CHECK(unsupported.hasCandidate());
    CHECK(!unsupported.candidate->embeddedPart.has_value());
    payload["data"] = QJsonArray{};
    setPayload(&root, payload);
    checkInvalid(root);
  }

  {
    QJsonObject root = validRoot();
    QJsonObject embedded = embeddedPartV4();
    embedded["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    setEmbeddedPart(&root, embedded);
    const auto unsupported = scan(root);
    CHECK(unsupported.status == ProductEnvelopeV5ScanStatus::Unsupported);
    CHECK(unsupported.hasCandidate());
    CHECK(unsupported.candidate->embeddedPart.has_value());
  }
  {
    QJsonObject root = validRoot();
    QJsonObject embedded = embeddedPartV4();
    embedded["version"] = 5;
    setEmbeddedPart(&root, embedded);
    checkInvalid(root);
  }

  for (const auto& pair : {
           std::pair<const char*, QJsonValue>{"format", "other"},
           std::pair<const char*, QJsonValue>{"version", 4}}) {
    QJsonObject root = validRoot();
    root[QLatin1String(pair.first)] = pair.second;
    checkFailure(scan(root), ProductEnvelopeV5ScanStatus::NotApplicable);
  }
  {
    QJsonObject root = validRoot();
    root["version"] = 6;
    checkFailure(scan(root), ProductEnvelopeV5ScanStatus::Unsupported);
  }
  {
    QJsonObject root = validRoot();
    root["version"] = "5";
    checkInvalid(root);
    root = validRoot();
    root["format"] = 5;
    checkInvalid(root);
  }

  {
    const QByteArray malformed{"{"};
    const auto first = solidar::project::scanProductEnvelopeV5(
        QByteArrayView(malformed));
    const auto second = solidar::project::scanProductEnvelopeV5(
        QByteArrayView(malformed));
    checkFailure(first, ProductEnvelopeV5ScanStatus::Invalid);
    checkFailure(second, ProductEnvelopeV5ScanStatus::Invalid);
    CHECK(first.error == second.error);
  }
  {
    const QByteArray oversized(
        static_cast<qsizetype>(
            solidar::project::ProjectFile::kMaximumFileBytes + 1),
        ' ');
    checkFailure(solidar::project::scanProductEnvelopeV5(
                     QByteArrayView(oversized)),
                 ProductEnvelopeV5ScanStatus::Invalid);
  }

  return EXIT_SUCCESS;
}

#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"
#include "project/ProjectFile.h"
#include "project/ProjectFormatV4ReferenceDecoder.h"
#include "project/ProjectFormatV4ReferenceEncoder.h"

namespace {

using namespace solidar;
using namespace solidar::project;

TopologyBounds validBounds() {
  return {{-1.0, -2.0, -3.0}, {4.0, 5.0, 6.0}};
}

FaceSignature validFaceSignature() {
  return {SurfaceKind::Plane,
          42.5,
          {1.0, 2.0, 3.0},
          {0.0, 0.0, 1.0},
          validBounds(),
          0.0,
          {0.0, 0.0, 1.0}};
}

EdgeSignature validEdgeSignature() {
  return {CurveKind::Line,
          12.5,
          {1.0, 2.0, 3.0},
          {1.0, 0.0, 0.0},
          validBounds(),
          0.0,
          {0.0, 2.0, 3.0}};
}

QJsonArray point(double x, double y, double z) { return {x, y, z}; }

QJsonObject expectedBounds() {
  return {{"minimum", point(-1.0, -2.0, -3.0)},
          {"maximum", point(4.0, 5.0, 6.0)}};
}

QJsonObject expectedFaceSignature() {
  return {{"surfaceType", 1},
          {"area", 42.5},
          {"centroid", point(1.0, 2.0, 3.0)},
          {"normal", point(0.0, 0.0, 1.0)},
          {"bounds", expectedBounds()},
          {"radius", 0.0},
          {"axis", point(0.0, 0.0, 1.0)}};
}

QJsonObject expectedEdgeSignature() {
  return {{"curveType", 1},
          {"length", 12.5},
          {"midpoint", point(1.0, 2.0, 3.0)},
          {"tangent", point(1.0, 0.0, 0.0)},
          {"bounds", expectedBounds()},
          {"radius", 0.0},
          {"center", point(0.0, 2.0, 3.0)}};
}

TopologyProvenanceOrigin origin(
    FeatureId featureId, TopologyKind kind, std::string localName,
    TopologyProvenanceOperation operation =
        TopologyProvenanceOperation::Seed,
    TopologySemanticRole role = TopologySemanticRole::Custom,
    std::optional<TopologyOccurrenceKey> occurrence = {}) {
  TopologyProvenanceOrigin value;
  value.featureId = featureId;
  value.kind = kind;
  value.operation = operation;
  value.role = role;
  value.localName = std::move(localName);
  value.occurrence = occurrence;
  return value;
}

TopologyReferenceV2 v2Reference(
    TopologyKind kind, std::vector<TopologyProvenanceOrigin> origins = {}) {
  TopologyReferenceV2 value;
  value.bodyId = 11;
  value.provenance.featureId = 23;
  value.provenance.kind = kind;
  value.provenance.operation = TopologyProvenanceOperation::Generated;
  value.provenance.role = TopologySemanticRole::BooleanResult;
  value.provenance.localName = "result";
  value.provenance.origins = std::move(origins);
  return value;
}

QJsonObject expectedOccurrence(qint64 featureId, qint64 slot) {
  return {{"featureId", featureId}, {"slot", slot}};
}

QJsonObject expectedOrigin(qint64 featureId, const char* kind,
                           const char* operation, const char* role,
                           const QString& localName,
                           std::optional<qint64> slot = {}) {
  QJsonObject value{{"featureId", featureId},
                    {"kind", kind},
                    {"operation", operation},
                    {"role", role},
                    {"localName", localName}};
  if (slot) value["instanceKey"] = expectedOccurrence(featureId, *slot);
  return value;
}

QJsonObject expectedV2(const char* kind, QJsonArray origins = {},
                       std::optional<qint64> slot = {}) {
  QJsonObject provenance{{"featureId", 23},
                         {"kind", kind},
                         {"operation", "Generated"},
                         {"role", "BooleanResult"},
                         {"localName", "result"},
                         {"origins", std::move(origins)}};
  if (slot) provenance["instanceKey"] = expectedOccurrence(23, *slot);
  return {{"referenceVersion", "V2"},
          {"bodyId", 11},
          {"provenance", std::move(provenance)}};
}

void checkInvalid(const ProjectFormatV4ReferenceEncodeResult& result) {
  CHECK(result.status == ProjectFormatV4ReferenceEncodeStatus::Invalid);
  CHECK(!result.succeeded());
  CHECK(!result.reference);
  CHECK(result.provenanceRecordCount == 0);
  CHECK(result.localNameUtf8Bytes == 0);
  CHECK(!result.error.isEmpty());
}

template <typename Reference>
void checkDecodeRoundTrip(const ProjectFormatV4ReferenceEncodeResult& encoded,
                          TopologyKind kind, const Reference& expected) {
  CHECK(encoded.succeeded());
  CHECK(encoded.reference);
  const auto decoded =
      decodeProjectFormatV4TopologyReference(*encoded.reference, kind);
  CHECK(decoded.succeeded());
  CHECK(std::holds_alternative<Reference>(*decoded.reference));
  CHECK(std::get<Reference>(*decoded.reference) == expected);
}

}  // namespace

int main() {
  // Minimal V1 objects omit both optional fields and match an independent
  // exact JSON oracle.
  {
    const FaceReference face(11, 23, 7);
    const auto encoded = encodeProjectFormatV4TopologyReference(face);
    CHECK(encoded.succeeded());
    const QJsonObject expectedFace{{"referenceVersion", "V1"},
                                   {"bodyId", 11},
                                   {"featureId", 23},
                                   {"kind", "Face"},
                                   {"legacyIndex", 7}};
    CHECK(*encoded.reference == expectedFace);
    CHECK(!encoded.reference->contains("persistentTag"));
    CHECK(!encoded.reference->contains("signature"));
    checkDecodeRoundTrip(encoded, TopologyKind::Face, face);

    const EdgeReference edge(11, 23, 9);
    const auto edgeEncoded = encodeProjectFormatV4TopologyReference(edge);
    CHECK(edgeEncoded.succeeded());
    const QJsonObject expectedEdge{{"referenceVersion", "V1"},
                                   {"bodyId", 11},
                                   {"featureId", 23},
                                   {"kind", "Edge"},
                                   {"legacyIndex", 9}};
    CHECK(*edgeEncoded.reference == expectedEdge);
    checkDecodeRoundTrip(edgeEncoded, TopologyKind::Edge, edge);
  }

  // Full V1 signatures preserve every semantic field.
  {
    const FaceReference face(11, 23, 7, "planar:max-z",
                             validFaceSignature());
    auto expected = QJsonObject{{"referenceVersion", "V1"},
                                {"bodyId", 11},
                                {"featureId", 23},
                                {"kind", "Face"},
                                {"legacyIndex", 7},
                                {"persistentTag", "planar:max-z"},
                                {"signature", expectedFaceSignature()}};
    const auto encoded = encodeProjectFormatV4TopologyReference(face);
    CHECK(encoded.succeeded());
    CHECK(*encoded.reference == expected);
    checkDecodeRoundTrip(encoded, TopologyKind::Face, face);

    const EdgeReference edge(11, 23, 8, "ring", validEdgeSignature());
    expected = QJsonObject{{"referenceVersion", "V1"},
                           {"bodyId", 11},
                           {"featureId", 23},
                           {"kind", "Edge"},
                           {"legacyIndex", 8},
                           {"persistentTag", "ring"},
                           {"signature", expectedEdgeSignature()}};
    const auto edgeEncoded = encodeProjectFormatV4TopologyReference(edge);
    CHECK(edgeEncoded.succeeded());
    CHECK(*edgeEncoded.reference == expected);
    checkDecodeRoundTrip(edgeEncoded, TopologyKind::Edge, edge);
  }

  // V2 exact objects support both kinds and independently optional
  // top-level/origin occurrences.
  {
    auto faceValue = v2Reference(
        TopologyKind::Face,
        {origin(5, TopologyKind::Edge, "edge:seed",
                TopologyProvenanceOperation::Modified,
                TopologySemanticRole::Original)});
    faceValue.provenance.occurrence = TopologyOccurrenceKey{23, 7};
    const FaceReference face = FaceReference::fromV2(faceValue);
    const auto encoded = encodeProjectFormatV4TopologyReference(face);
    CHECK(encoded.succeeded());
    CHECK(*encoded.reference ==
          expectedV2("Face",
                     {expectedOrigin(5, "Edge", "Modified", "Original",
                                     "edge:seed")},
                     7));
    CHECK(encoded.provenanceRecordCount == 2);
    CHECK(encoded.localNameUtf8Bytes ==
          std::string("result").size() + std::string("edge:seed").size());
    checkDecodeRoundTrip(encoded, TopologyKind::Face, face);

    const EdgeReference edge = EdgeReference::fromV2(v2Reference(
        TopologyKind::Edge,
        {origin(5, TopologyKind::Face, "face:seed",
                TopologyProvenanceOperation::Retained,
                TopologySemanticRole::Original,
                TopologyOccurrenceKey{5, 4})}));
    const auto edgeEncoded = encodeProjectFormatV4TopologyReference(edge);
    CHECK(edgeEncoded.succeeded());
    CHECK(*edgeEncoded.reference ==
          expectedV2("Edge",
                     {expectedOrigin(5, "Face", "Retained", "Original",
                                     "face:seed", 4)}));
    checkDecodeRoundTrip(edgeEncoded, TopologyKind::Edge, edge);
  }

  // One shared mapping drives decoder and encoder. Every current operation
  // and role is asserted against an independent token table.
  {
    const std::vector<std::pair<TopologyProvenanceOperation, QString>>
        operations{{TopologyProvenanceOperation::Seed, "Seed"},
                   {TopologyProvenanceOperation::Generated, "Generated"},
                   {TopologyProvenanceOperation::Modified, "Modified"},
                   {TopologyProvenanceOperation::Retained, "Retained"},
                   {TopologyProvenanceOperation::Merge, "Merge"}};
    const std::vector<std::pair<TopologySemanticRole, QString>> roles{
        {TopologySemanticRole::SketchBoundary, "SketchBoundary"},
        {TopologySemanticRole::Cap, "Cap"},
        {TopologySemanticRole::Side, "Side"},
        {TopologySemanticRole::Original, "Original"},
        {TopologySemanticRole::Reflected, "Reflected"},
        {TopologySemanticRole::PatternInstance, "PatternInstance"},
        {TopologySemanticRole::BooleanResult, "BooleanResult"},
        {TopologySemanticRole::Custom, "Custom"}};
    for (const auto& [operation, token] : operations) {
      auto value = v2Reference(TopologyKind::Face);
      value.provenance.operation = operation;
      const auto encoded = encodeProjectFormatV4TopologyReference(
          FaceReference::fromV2(std::move(value)));
      CHECK(encoded.succeeded());
      CHECK(encoded.reference->value("provenance")
                .toObject()
                .value("operation") == token);
    }
    for (const auto& [role, token] : roles) {
      auto value = v2Reference(TopologyKind::Edge);
      value.provenance.role = role;
      const auto encoded = encodeProjectFormatV4TopologyReference(
          EdgeReference::fromV2(std::move(value)));
      CHECK(encoded.succeeded());
      CHECK(encoded.reference->value("provenance")
                .toObject()
                .value("role") == token);
    }
  }

  // Persistence canonicalization is bytewise UTF-8, exact-deduplicating, and
  // operates on a copy. ASCII 'z' must sort before the non-ASCII UTF-8 name.
  {
    const auto nonAscii = origin(7, TopologyKind::Face, "\xC3\xA9");
    const auto ascii = origin(7, TopologyKind::Face, "z");
    auto value = v2Reference(TopologyKind::Face,
                             {nonAscii, ascii, nonAscii});
    const auto originalOrigins = value.provenance.origins;
    const FaceReference face = FaceReference::fromV2(value);
    const auto first = encodeProjectFormatV4TopologyReference(face);
    const auto second = encodeProjectFormatV4TopologyReference(face);
    CHECK(first.succeeded());
    CHECK(second.succeeded());
    CHECK(first.reference == second.reference);
    CHECK(face.v2()->provenance.origins == originalOrigins);
    const auto origins = first.reference->value("provenance")
                             .toObject()
                             .value("origins")
                             .toArray();
    CHECK(origins.size() == 2);
    CHECK(origins[0].toObject().value("localName") == QStringLiteral("z"));
    CHECK(origins[1].toObject().value("localName") ==
          QString::fromUtf8("\xC3\xA9"));
    CHECK(first.provenanceRecordCount == 3);

    value = v2Reference(
        TopologyKind::Face,
        {origin(7, TopologyKind::Face, "same",
                TopologyProvenanceOperation::Seed),
         origin(7, TopologyKind::Face, "same",
                TopologyProvenanceOperation::Modified),
         origin(7, TopologyKind::Face, "same",
                TopologyProvenanceOperation::Seed,
                TopologySemanticRole::Cap)});
    const auto unequal = encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(std::move(value)));
    CHECK(unequal.succeeded());
    CHECK(unequal.reference->value("provenance")
              .toObject()
              .value("origins")
              .toArray()
              .size() == 3);

    value = v2Reference(
        TopologyKind::Face,
        {origin(7, TopologyKind::Face, "occurrence"),
         origin(7, TopologyKind::Face, "occurrence",
                TopologyProvenanceOperation::Seed,
                TopologySemanticRole::Custom,
                TopologyOccurrenceKey{7, 2}),
         origin(7, TopologyKind::Face, "occurrence",
                TopologyProvenanceOperation::Seed,
                TopologySemanticRole::Custom,
                TopologyOccurrenceKey{7, 1})});
    const auto occurrenceTuple = encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(std::move(value)));
    CHECK(occurrenceTuple.succeeded());
    const QJsonArray expectedOccurrenceTuple{
        expectedOrigin(7, "Face", "Seed", "Custom", "occurrence"),
        expectedOrigin(7, "Face", "Seed", "Custom", "occurrence", 1),
        expectedOrigin(7, "Face", "Seed", "Custom", "occurrence", 2)};
    CHECK(occurrenceTuple.reference->value("provenance")
              .toObject()
              .value("origins")
              .toArray() == expectedOccurrenceTuple);
  }

  // The per-reference origin boundary is enforced after exact deduplication.
  {
    std::vector<TopologyProvenanceOrigin> origins;
    for (std::size_t index = 0; index < kMaximumTopologyImmediateOrigins;
         ++index) {
      origins.push_back(origin(static_cast<FeatureId>(index + 1),
                               TopologyKind::Face,
                               "origin:" + std::to_string(index)));
    }
    auto accepted = v2Reference(TopologyKind::Face, origins);
    accepted.provenance.origins.push_back(origins.back());
    const auto encoded = encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(std::move(accepted)));
    CHECK(encoded.succeeded());
    CHECK(encoded.provenanceRecordCount ==
          1 + kMaximumTopologyImmediateOrigins);

    origins.push_back(origin(300, TopologyKind::Face, "origin:extra"));
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(v2Reference(TopologyKind::Face, origins))));
  }

  // Exact persisted integer boundaries are retained without narrowing.
  {
    TopologyReference v1;
    v1.bodyId = static_cast<BodyId>(ProjectFile::kMaximumPersistedId);
    v1.featureId = static_cast<FeatureId>(ProjectFile::kMaximumPersistedId);
    v1.kind = TopologyKind::Face;
    v1.legacyIndex = static_cast<std::size_t>(9007199254740991ULL);
    CHECK(encodeProjectFormatV4TopologyReference(
              FaceReference::fromV1(v1))
              .succeeded());
    v1.legacyIndex = static_cast<std::size_t>(9007199254740992ULL);
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV1(v1)));

    auto v2 = v2Reference(TopologyKind::Face);
    v2.bodyId = static_cast<BodyId>(ProjectFile::kMaximumPersistedId);
    v2.provenance.featureId =
        static_cast<FeatureId>(ProjectFile::kMaximumPersistedId);
    v2.provenance.occurrence =
        TopologyOccurrenceKey{v2.provenance.featureId, 9007199254740991ULL};
    CHECK(encodeProjectFormatV4TopologyReference(
              FaceReference::fromV2(v2))
              .succeeded());
    v2.provenance.occurrence->slot = 9007199254740992ULL;
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(v2)));
    v2 = v2Reference(TopologyKind::Face);
    v2.bodyId = static_cast<BodyId>(ProjectFile::kMaximumPersistedId) + 1;
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(v2)));
  }

  // UTF-8 limits are inclusive, and the aggregate budget spans the top key
  // plus its canonical immediate origins.
  {
    const std::string maximumName(16384, 'x');
    CHECK(encodeProjectFormatV4TopologyReference(
              FaceReference(11, 23, 0, maximumName))
              .succeeded());

    auto value = v2Reference(TopologyKind::Face);
    value.provenance.localName = maximumName;
    for (FeatureId featureId = 1; featureId <= 15; ++featureId) {
      value.provenance.origins.push_back(
          origin(featureId, TopologyKind::Face, maximumName));
    }
    const auto exactBudget = encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value));
    CHECK(exactBudget.succeeded());
    CHECK(exactBudget.localNameUtf8Bytes == 256 * 1024);

    value.provenance.origins.push_back(
        origin(16, TopologyKind::Face, "x"));
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(std::move(value))));
  }

  // Invalid typed carriers fail closed and never expose a partial object.
  {
    checkInvalid(encodeProjectFormatV4TopologyReference(FaceReference{}));
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(0, 23, 0)));
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(11, 0, 0)));
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(11, 23, 0,
                      std::string(ProjectFile::kMaximumStringCharacters + 1,
                                  'x'))));
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(11, 23, 0, std::string("\xC3\x28", 2))));

    auto value = v2Reference(TopologyKind::Face);
    value.bodyId = kInvalidBodyId;
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value = v2Reference(TopologyKind::Face);
    value.provenance.featureId = kInvalidFeatureId;
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value = v2Reference(TopologyKind::Edge);
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value = v2Reference(TopologyKind::Face);
    value.provenance.operation =
        static_cast<TopologyProvenanceOperation>(255);
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value = v2Reference(TopologyKind::Face);
    value.provenance.role = static_cast<TopologySemanticRole>(255);
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value = v2Reference(TopologyKind::Face);
    value.provenance.localName.clear();
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value.provenance.localName.assign(16385, 'x');
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value.provenance.localName = std::string("\xC3\x28", 2);
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value = v2Reference(TopologyKind::Face);
    value.provenance.occurrence = TopologyOccurrenceKey{24, 0};
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
    value = v2Reference(
        TopologyKind::Face,
        {origin(0, TopologyKind::Face, "bad-origin")});
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));

    std::vector<TopologyProvenanceOrigin> invalidOrigins;
    auto invalidOrigin = origin(7, TopologyKind::Face, "invalid");
    invalidOrigin.kind = static_cast<TopologyKind>(255);
    invalidOrigins.push_back(invalidOrigin);
    invalidOrigin = origin(7, TopologyKind::Face, "invalid");
    invalidOrigin.operation =
        static_cast<TopologyProvenanceOperation>(255);
    invalidOrigins.push_back(invalidOrigin);
    invalidOrigin = origin(7, TopologyKind::Face, "invalid");
    invalidOrigin.role = static_cast<TopologySemanticRole>(255);
    invalidOrigins.push_back(invalidOrigin);
    invalidOrigins.push_back(
        origin(7, TopologyKind::Face, std::string("\xC3\x28", 2)));
    invalidOrigins.push_back(origin(7, TopologyKind::Face, ""));
    invalidOrigins.push_back(
        origin(7, TopologyKind::Face, std::string(16385, 'x')));
    invalidOrigins.push_back(origin(
        7, TopologyKind::Face, "invalid-slot",
        TopologyProvenanceOperation::Seed, TopologySemanticRole::Custom,
        TopologyOccurrenceKey{7, 9007199254740992ULL}));
    for (const auto& candidate : invalidOrigins) {
      checkInvalid(encodeProjectFormatV4TopologyReference(
          FaceReference::fromV2(
              v2Reference(TopologyKind::Face, {candidate}))));
    }

    value = v2Reference(
        TopologyKind::Face,
        {origin(7, TopologyKind::Face, "bad-occurrence",
                TopologyProvenanceOperation::Seed,
                TopologySemanticRole::Custom,
                TopologyOccurrenceKey{8, 0})});
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference::fromV2(value)));
  }

  // Signature validation is exactly the decoder contract.
  {
    auto faceSignature = validFaceSignature();
    faceSignature.area = std::numeric_limits<double>::quiet_NaN();
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(11, 23, 0, {}, faceSignature)));
    faceSignature = validFaceSignature();
    faceSignature.bounds.minimum.x = 10.0;
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(11, 23, 0, {}, faceSignature)));
    faceSignature = validFaceSignature();
    faceSignature.normal = {0.0, 0.0, 2.0};
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(11, 23, 0, {}, faceSignature)));
    faceSignature = validFaceSignature();
    faceSignature.surface = SurfaceKind::Cylinder;
    faceSignature.radius = 0.0;
    checkInvalid(encodeProjectFormatV4TopologyReference(
        FaceReference(11, 23, 0, {}, faceSignature)));

    auto edgeSignature = validEdgeSignature();
    edgeSignature.tangent = {0.0, 2.0, 0.0};
    checkInvalid(encodeProjectFormatV4TopologyReference(
        EdgeReference(11, 23, 0, {}, edgeSignature)));
    edgeSignature = validEdgeSignature();
    edgeSignature.curve = CurveKind::Circle;
    edgeSignature.radius = 0.0;
    checkInvalid(encodeProjectFormatV4TopologyReference(
        EdgeReference(11, 23, 0, {}, edgeSignature)));
  }

  return EXIT_SUCCESS;
}

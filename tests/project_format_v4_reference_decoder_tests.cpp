#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"
#include "project/ProjectFile.h"
#include "project/ProjectFormatV4ReferenceDecoder.h"

namespace {

using solidar::TopologyKind;
using solidar::project::ProjectFormatV4ReferenceDecodeResult;
using solidar::project::ProjectFormatV4ReferenceDecodeStatus;
using solidar::project::ProjectFormatV4ReferenceVariant;

QJsonArray point(double x, double y, double z) { return {x, y, z}; }

QJsonObject bounds() {
  return {{"minimum", point(-1.0, -2.0, -3.0)},
          {"maximum", point(4.0, 5.0, 6.0)}};
}

QJsonObject faceSignature(int surfaceType = 1) {
  return {{"surfaceType", surfaceType},
          {"area", 42.5},
          {"centroid", point(1.0, 2.0, 3.0)},
          {"normal", point(0.0, 0.0, 1.0)},
          {"bounds", bounds()},
          {"radius", surfaceType == 2 ? 5.0 : 0.0},
          {"axis", point(0.0, 0.0, 1.0)}};
}

QJsonObject edgeSignature(int curveType = 1) {
  return {{"curveType", curveType},
          {"length", 12.5},
          {"midpoint", point(1.0, 2.0, 3.0)},
          {"tangent", point(1.0, 0.0, 0.0)},
          {"bounds", bounds()},
          {"radius", curveType == 2 ? 5.0 : 0.0},
          {"center", point(0.0, 2.0, 3.0)}};
}

QJsonObject v1(const QString& kind) {
  return {{"referenceVersion", "V1"},
          {"bodyId", 11},
          {"featureId", 23},
          {"kind", kind},
          {"legacyIndex", 7}};
}

QJsonObject instance(qint64 featureId, double slot) {
  return {{"featureId", featureId}, {"slot", slot}};
}

QJsonObject provenance(qint64 featureId, const QString& kind,
                       const QString& operation, const QString& role,
                       const QString& localName,
                       const QJsonValue& instanceKey =
                           QJsonValue(QJsonValue::Undefined)) {
  QJsonObject result{{"featureId", featureId},
                     {"kind", kind},
                     {"operation", operation},
                     {"role", role},
                     {"localName", localName}};
  if (!instanceKey.isUndefined()) result["instanceKey"] = instanceKey;
  return result;
}

QJsonObject v2(const QString& kind, QJsonArray origins = {}) {
  auto key = provenance(23, kind, QStringLiteral("Generated"),
                        QStringLiteral("Custom"), QStringLiteral("result"));
  key["origins"] = origins;
  return {{"referenceVersion", "V2"},
          {"bodyId", 11},
          {"provenance", key}};
}

ProjectFormatV4ReferenceDecodeResult decode(const QJsonValue& value,
                                             TopologyKind kind) {
  return solidar::project::decodeProjectFormatV4TopologyReference(value, kind);
}

void checkFailure(const ProjectFormatV4ReferenceDecodeResult& result,
                  ProjectFormatV4ReferenceDecodeStatus status) {
  CHECK(result.status == status);
  CHECK(!result.success);
  CHECK(!result.succeeded());
  CHECK(!result.reference);
  CHECK(!result.error.isEmpty());
  if (status == ProjectFormatV4ReferenceDecodeStatus::Invalid) {
    CHECK(!result.inspection);
    CHECK(result.provenanceRecordCount == 0);
    CHECK(result.localNameUtf8Bytes == 0);
  }
}

void checkInvalid(const QJsonValue& value, TopologyKind kind) {
  const auto result = decode(value, kind);
  checkFailure(result, ProjectFormatV4ReferenceDecodeStatus::Invalid);
  CHECK(!result.inspection);
  CHECK(result.provenanceRecordCount == 0);
  CHECK(result.localNameUtf8Bytes == 0);
}

void checkUnsupported(const QJsonValue& value, TopologyKind kind) {
  checkFailure(decode(value, kind),
               ProjectFormatV4ReferenceDecodeStatus::Unsupported);
}

}  // namespace

int main() {
  using namespace solidar;
  using namespace solidar::project;

  // Minimal V1 variants are decoded into the expected closed carrier.
  {
    const auto face = decode(v1(QStringLiteral("Face")), TopologyKind::Face);
    CHECK(face.succeeded());
    CHECK(face.success);
    CHECK(face.provenanceRecordCount == 0);
    CHECK(face.localNameUtf8Bytes == 0);
    CHECK(face.inspection);
    CHECK(face.inspection->variant == ProjectFormatV4ReferenceVariant::V1);
    CHECK(face.inspection->bodyId == 11);
    CHECK(face.inspection->topLevelFeatureId == 23);
    CHECK(face.inspection->immediateOriginFeatureIds.empty());
    const auto* carrier =
        std::get_if<FaceReference>(&*face.reference);
    CHECK(carrier && carrier->isV1());
    CHECK(carrier->bodyId() == 11);
    CHECK(carrier->featureId() == 23);
    CHECK(carrier->faceIndex() == 7);
    CHECK(carrier->persistentTag().empty());
    CHECK(!carrier->signature());

    const auto edge = decode(v1(QStringLiteral("Edge")), TopologyKind::Edge);
    CHECK(edge.succeeded());
    CHECK(edge.inspection == face.inspection);
    const auto* edgeCarrier = std::get_if<EdgeReference>(&*edge.reference);
    CHECK(edgeCarrier && edgeCarrier->isV1());
    CHECK(edgeCarrier->edgeIndex() == 7);
  }

  // Full V1 signatures preserve every authored value and enum alternative.
  {
    for (int surface = 0; surface <= 2; ++surface) {
      auto value = v1(QStringLiteral("Face"));
      value["persistentTag"] = QStringLiteral("planar:max-z");
      value["signature"] = faceSignature(surface);
      const auto result = decode(value, TopologyKind::Face);
      CHECK(result.succeeded());
      const auto* carrier = std::get<FaceReference>(*result.reference).v1();
      CHECK(carrier);
      CHECK(carrier->persistentTag == "planar:max-z");
      CHECK(carrier->faceSignature);
      CHECK(static_cast<int>(carrier->faceSignature->surface) == surface);
      CHECK(carrier->faceSignature->area == 42.5);
      CHECK(carrier->faceSignature->centroid ==
            (TopologyPoint3d{1.0, 2.0, 3.0}));
      CHECK(carrier->faceSignature->normal ==
            (TopologyPoint3d{0.0, 0.0, 1.0}));
      CHECK(carrier->faceSignature->bounds.minimum.x == -1.0);
      CHECK(carrier->faceSignature->bounds.maximum.z == 6.0);
      CHECK(carrier->faceSignature->radius ==
            (surface == 2 ? 5.0 : 0.0));
      CHECK(carrier->faceSignature->axis ==
            (TopologyPoint3d{0.0, 0.0, 1.0}));
    }
    for (int curve = 0; curve <= 4; ++curve) {
      auto value = v1(QStringLiteral("Edge"));
      value["signature"] = edgeSignature(curve);
      const auto result = decode(value, TopologyKind::Edge);
      CHECK(result.succeeded());
      const auto* carrier = std::get<EdgeReference>(*result.reference).v1();
      CHECK(carrier && carrier->edgeSignature);
      CHECK(static_cast<int>(carrier->edgeSignature->curve) == curve);
      CHECK(carrier->edgeSignature->length == 12.5);
      CHECK(carrier->edgeSignature->midpoint ==
            (TopologyPoint3d{1.0, 2.0, 3.0}));
      CHECK(carrier->edgeSignature->tangent ==
            (TopologyPoint3d{1.0, 0.0, 0.0}));
      CHECK(carrier->edgeSignature->radius ==
            (curve == 2 ? 5.0 : 0.0));
      CHECK(carrier->edgeSignature->center ==
            (TopologyPoint3d{0.0, 2.0, 3.0}));
    }
  }

  // Persisted IDs retain generation headroom while legacy indices use the
  // full IEEE-754 exact-integer range.
  {
    auto value = v1(QStringLiteral("Face"));
    value["bodyId"] = static_cast<double>(ProjectFile::kMaximumPersistedId);
    value["featureId"] =
        static_cast<double>(ProjectFile::kMaximumPersistedId);
    value["legacyIndex"] = 9007199254740991.0;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    value["bodyId"] =
        static_cast<double>(ProjectFile::kMaximumPersistedId) + 1.0;
    checkInvalid(value, TopologyKind::Face);
    value = v1(QStringLiteral("Face"));
    value["legacyIndex"] = 9007199254740992.0;
    checkInvalid(value, TopologyKind::Face);
    value["legacyIndex"] = 1.5;
    checkInvalid(value, TopologyKind::Face);
    value["legacyIndex"] = -1;
    checkInvalid(value, TopologyKind::Face);
  }

  // V1 uses a closed schema and distinguishes unsupported string tokens from
  // malformed or embedding-incompatible data.
  {
    checkInvalid(QJsonObject{}, TopologyKind::Face);
    checkInvalid(QJsonObject{{"reference", v1(QStringLiteral("Face"))}},
                 TopologyKind::Face);
    auto extra = v1(QStringLiteral("Face"));
    extra["extra"] = true;
    checkInvalid(extra, TopologyKind::Face);
    auto hybrid = v1(QStringLiteral("Face"));
    hybrid["provenance"] = QJsonObject{};
    checkInvalid(hybrid, TopologyKind::Face);
    checkInvalid(v1(QStringLiteral("Edge")), TopologyKind::Face);
    auto unknown = v1(QStringLiteral("FutureKind"));
    checkUnsupported(unknown, TopologyKind::Face);
    unknown["signature"] = edgeSignature();
    checkUnsupported(unknown, TopologyKind::Face);
    unknown["signature"] = QJsonObject{};
    checkInvalid(unknown, TopologyKind::Face);
    unknown.remove(QStringLiteral("signature"));
    unknown["bodyId"] = 0;
    checkInvalid(unknown, TopologyKind::Face);
    auto wrongType = v1(QStringLiteral("Face"));
    wrongType["kind"] = 1;
    checkInvalid(wrongType, TopologyKind::Face);
    auto emptyTag = v1(QStringLiteral("Face"));
    emptyTag["persistentTag"] = QString{};
    checkInvalid(emptyTag, TopologyKind::Face);
    auto longTag = v1(QStringLiteral("Face"));
    longTag["persistentTag"] =
        QString(ProjectFile::kMaximumStringCharacters + 1, QChar('x'));
    checkInvalid(longTag, TopologyKind::Face);
    auto invalidUtf16 = v1(QStringLiteral("Face"));
    invalidUtf16["persistentTag"] = QString(1, QChar(0xd800));
    checkInvalid(invalidUtf16, TopologyKind::Face);
    auto missing = v1(QStringLiteral("Face"));
    missing.remove(QStringLiteral("legacyIndex"));
    checkInvalid(missing, TopologyKind::Face);
  }

  // Signature schemas are exact and enforce bounds, enums, directions and
  // kind-specific positive radii.
  {
    const double measureBeyondMaximum =
        std::nextafter(1.0e18, std::numeric_limits<double>::infinity());
    const double coordinateBeyondMaximum =
        std::nextafter(1000000.0, std::numeric_limits<double>::infinity());

    auto bad = v1(QStringLiteral("Face"));
    auto signature = faceSignature();
    signature.remove(QStringLiteral("axis"));
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["extra"] = 1;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["centroid"] = QJsonArray{1.0, 2.0};
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["area"] = measureBeyondMaximum;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["area"] = -1.0;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["area"] = std::numeric_limits<double>::quiet_NaN();
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature(3);
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature(1);
    signature["normal"] = point(0.0, 0.0, 2.0);
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature(2);
    signature["radius"] = 0.0;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature(2);
    signature["axis"] = point(0.0, 0.0, 2.0);
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["radius"] = -1.0;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["radius"] = coordinateBeyondMaximum;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    signature["centroid"] = point(coordinateBeyondMaximum, 0.0, 0.0);
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    auto inverted = bounds();
    inverted["minimum"] = point(9.0, -2.0, -3.0);
    signature["bounds"] = inverted;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    signature = faceSignature();
    auto malformedBounds = bounds();
    malformedBounds.remove(QStringLiteral("maximum"));
    signature["bounds"] = malformedBounds;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);
    malformedBounds = bounds();
    malformedBounds["extra"] = 0;
    signature["bounds"] = malformedBounds;
    bad["signature"] = signature;
    checkInvalid(bad, TopologyKind::Face);

    auto boundaryFace = v1(QStringLiteral("Face"));
    signature = faceSignature(0);
    signature["area"] = 1.0e18;
    signature["radius"] = 1000000.0;
    signature["centroid"] = point(1000000.0, -1000000.0, 0.0);
    boundaryFace["signature"] = signature;
    CHECK(decode(boundaryFace, TopologyKind::Face).succeeded());

    auto badEdge = v1(QStringLiteral("Edge"));
    auto edge = edgeSignature(5);
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(1);
    edge["tangent"] = point(2.0, 0.0, 0.0);
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(2);
    edge["radius"] = 0.0;
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(1);
    edge["center"] = point(1000001.0, 0.0, 0.0);
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(1);
    edge["length"] = -1.0;
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(1);
    edge["length"] = measureBeyondMaximum;
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(1);
    edge["length"] = std::numeric_limits<double>::infinity();
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(1);
    edge["radius"] = -1.0;
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);
    edge = edgeSignature(1);
    edge["radius"] = coordinateBeyondMaximum;
    badEdge["signature"] = edge;
    checkInvalid(badEdge, TopologyKind::Edge);

    auto boundaryEdge = v1(QStringLiteral("Edge"));
    edge = edgeSignature(1);
    edge["length"] = 1.0e18;
    edge["radius"] = 1000000.0;
    edge["center"] = point(-1000000.0, 1000000.0, 0.0);
    boundaryEdge["signature"] = edge;
    CHECK(decode(boundaryEdge, TopologyKind::Edge).succeeded());
  }

  // Minimal V2 variants publish exact provenance and transactional counters.
  {
    auto value = v2(QStringLiteral("Face"));
    auto result = decode(value, TopologyKind::Face);
    CHECK(result.succeeded());
    CHECK(result.provenanceRecordCount == 1);
    CHECK(result.localNameUtf8Bytes == 6);
    CHECK(result.inspection);
    CHECK(result.inspection->variant == ProjectFormatV4ReferenceVariant::V2);
    CHECK(result.inspection->bodyId == 11);
    CHECK(result.inspection->topLevelFeatureId == 23);
    CHECK(result.inspection->immediateOriginFeatureIds.empty());
    const auto* carrier = std::get_if<FaceReference>(&*result.reference);
    CHECK(carrier && carrier->isV2());
    CHECK(carrier->v2()->bodyId == 11);
    CHECK(carrier->v2()->provenance.featureId == 23);
    CHECK(carrier->v2()->provenance.origins.empty());

    value = v2(QStringLiteral("Edge"));
    result = decode(value, TopologyKind::Edge);
    CHECK(result.succeeded());
    const auto* edge = std::get<EdgeReference>(*result.reference).v2();
    CHECK(edge);
    CHECK(edge->bodyId == 11);
    CHECK(edge->provenance.featureId == 23);
    CHECK(edge->provenance.kind == TopologyKind::Edge);
    CHECK(edge->provenance.operation ==
          TopologyProvenanceOperation::Generated);
    CHECK(edge->provenance.role == TopologySemanticRole::Custom);
    CHECK(edge->provenance.localName == "result");
    CHECK(edge->provenance.origins.empty());
    CHECK(!edge->provenance.occurrence);

    value = v2(QStringLiteral("Face"));
    auto key = value.value(QStringLiteral("provenance")).toObject();
    key["instanceKey"] = instance(23, 17);
    value["provenance"] = key;
    result = decode(value, TopologyKind::Face);
    CHECK(result.succeeded());
    const auto& reconstructed =
        std::get<FaceReference>(*result.reference).v2()->provenance;
    CHECK(reconstructed.occurrence);
    CHECK(reconstructed.occurrence->featureId == 23);
    CHECK(reconstructed.occurrence->slot == 17);
  }

  // Every operation/role token and both topology kinds are accepted. Mixed
  // immediate kinds and occurrence keys retain their exact values.
  {
    const QStringList operations = {QStringLiteral("Seed"),
                                    QStringLiteral("Generated"),
                                    QStringLiteral("Modified"),
                                    QStringLiteral("Retained"),
                                    QStringLiteral("Merge")};
    const QStringList roles = {QStringLiteral("SketchBoundary"),
                               QStringLiteral("Cap"),
                               QStringLiteral("Side"),
                               QStringLiteral("Original"),
                               QStringLiteral("Reflected"),
                               QStringLiteral("PatternInstance"),
                               QStringLiteral("BooleanResult"),
                               QStringLiteral("Custom")};
    for (qsizetype operationIndex = 0; operationIndex < operations.size();
         ++operationIndex) {
      for (qsizetype roleIndex = 0; roleIndex < roles.size(); ++roleIndex) {
        auto value = v2(QStringLiteral("Face"));
        auto key = value.value(QStringLiteral("provenance")).toObject();
        key["operation"] = operations.at(operationIndex);
        key["role"] = roles.at(roleIndex);
        value["provenance"] = key;
        const auto decoded = decode(value, TopologyKind::Face);
        CHECK(decoded.succeeded());
        const auto* carrier = std::get<FaceReference>(*decoded.reference).v2();
        CHECK(carrier);
        CHECK(carrier->bodyId == 11);
        CHECK(carrier->provenance.featureId == 23);
        CHECK(carrier->provenance.kind == TopologyKind::Face);
        CHECK(static_cast<qsizetype>(carrier->provenance.operation) ==
              operationIndex);
        CHECK(static_cast<qsizetype>(carrier->provenance.role) == roleIndex);
        CHECK(carrier->provenance.localName == "result");
        CHECK(carrier->provenance.origins.empty());
        CHECK(!carrier->provenance.occurrence);
      }
    }

    QJsonArray origins;
    origins.append(provenance(1, QStringLiteral("Face"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              QStringLiteral("a")));
    origins.append(provenance(2, QStringLiteral("Edge"),
                              QStringLiteral("Modified"),
                              QStringLiteral("Custom"), QStringLiteral("b"),
                              instance(2, 9007199254740991.0)));
    const auto result = decode(v2(QStringLiteral("Face"), origins),
                               TopologyKind::Face);
    CHECK(result.succeeded());
    CHECK(result.provenanceRecordCount == 3);
    CHECK(result.localNameUtf8Bytes == 8);
    CHECK(result.inspection);
    CHECK(result.inspection->variant == ProjectFormatV4ReferenceVariant::V2);
    CHECK(result.inspection->bodyId == 11);
    CHECK(result.inspection->topLevelFeatureId == 23);
    CHECK(result.inspection->immediateOriginFeatureIds ==
          (std::vector<FeatureId>{1, 2}));
    const auto& key = std::get<FaceReference>(*result.reference).v2()->provenance;
    CHECK(key.featureId == 23);
    CHECK(key.kind == TopologyKind::Face);
    CHECK(key.operation == TopologyProvenanceOperation::Generated);
    CHECK(key.role == TopologySemanticRole::Custom);
    CHECK(key.localName == "result");
    CHECK(!key.occurrence);
    CHECK(key.origins.size() == 2);
    CHECK(key.origins[0].featureId == 1);
    CHECK(key.origins[0].kind == TopologyKind::Face);
    CHECK(key.origins[0].operation == TopologyProvenanceOperation::Seed);
    CHECK(key.origins[0].role == TopologySemanticRole::Cap);
    CHECK(key.origins[0].localName == "a");
    CHECK(!key.origins[0].occurrence);
    CHECK(key.origins[1].featureId == 2);
    CHECK(key.origins[1].kind == TopologyKind::Edge);
    CHECK(key.origins[1].operation == TopologyProvenanceOperation::Modified);
    CHECK(key.origins[1].role == TopologySemanticRole::Custom);
    CHECK(key.origins[1].localName == "b");
    CHECK(key.origins[1].occurrence);
    CHECK(key.origins[1].occurrence->featureId == 2);
    CHECK(key.origins[1].occurrence->slot == 9007199254740991ULL);
  }

  // Unknown version is opaque and forward-compatible. Known variants first
  // expose independent structural defects, then classify unknown tokens.
  {
    const auto opaque = decode(
        QJsonObject{{"referenceVersion", "V9"},
                    {"opaque", QJsonArray{1, 2, 3}}},
        TopologyKind::Face);
    checkFailure(opaque, ProjectFormatV4ReferenceDecodeStatus::Unsupported);
    CHECK(!opaque.inspection);
    CHECK(opaque.provenanceRecordCount == 0);
    CHECK(opaque.localNameUtf8Bytes == 0);
    checkUnsupported(QJsonObject{{"referenceVersion", "V9"},
                                 {"bodyId", 0},
                                 {"legacyIndex", 1.5},
                                 {"opaque", false}},
                     TopologyKind::Face);
    checkInvalid(QJsonObject{{"referenceVersion", 9}}, TopologyKind::Face);
    checkInvalid(QJsonObject{{"referenceVersion", ""}}, TopologyKind::Face);
    checkUnsupported(
        QJsonObject{{"referenceVersion",
                     QString(ProjectFile::kMaximumStringCharacters,
                             QChar('V'))}},
        TopologyKind::Face);
    checkInvalid(
        QJsonObject{{"referenceVersion",
                     QString(ProjectFile::kMaximumStringCharacters + 1,
                             QChar('V'))}},
        TopologyKind::Face);

    auto value = v2(QStringLiteral("Face"));
    auto key = value.value(QStringLiteral("provenance")).toObject();
    key["role"] = QStringLiteral("FutureRole");
    value["provenance"] = key;
    checkUnsupported(value, TopologyKind::Face);
    key = value.value(QStringLiteral("provenance")).toObject();
    key["localName"] = QString(1, QChar(0xd800));
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    key["localName"] = QStringLiteral("result");
    key["featureId"] = 0;
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);

    value = v2(QStringLiteral("FutureKind"));
    checkUnsupported(value, TopologyKind::Face);
    value = v2(QStringLiteral("Edge"));
    key = value.value("provenance").toObject();
    key["operation"] = QStringLiteral("FutureOperation");
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    value = v2(QStringLiteral("Edge"));
    checkInvalid(value, TopologyKind::Face);
    key = v2(QStringLiteral("Face")).value("provenance").toObject();
    key["operation"] = QStringLiteral("FutureOperation");
    value = v2(QStringLiteral("Face"));
    value["provenance"] = key;
    checkUnsupported(value, TopologyKind::Face);
  }

  // Fully parsed known variants publish structural inspection even when a
  // bounded future token makes carrier materialization unsupported.
  {
    const auto legacy = decode(v1(QStringLiteral("FutureKind")),
                               TopologyKind::Face);
    checkFailure(legacy, ProjectFormatV4ReferenceDecodeStatus::Unsupported);
    CHECK(legacy.inspection);
    CHECK(legacy.inspection->variant == ProjectFormatV4ReferenceVariant::V1);
    CHECK(legacy.inspection->bodyId == 11);
    CHECK(legacy.inspection->topLevelFeatureId == 23);
    CHECK(legacy.inspection->immediateOriginFeatureIds.empty());
    CHECK(legacy.provenanceRecordCount == 0);
    CHECK(legacy.localNameUtf8Bytes == 0);

    const auto futureTop = decode(v2(QStringLiteral("FutureKind")),
                                  TopologyKind::Face);
    checkFailure(futureTop,
                 ProjectFormatV4ReferenceDecodeStatus::Unsupported);
    CHECK(futureTop.inspection);
    CHECK(futureTop.inspection->variant ==
          ProjectFormatV4ReferenceVariant::V2);
    CHECK(futureTop.inspection->bodyId == 11);
    CHECK(futureTop.inspection->topLevelFeatureId == 23);
    CHECK(futureTop.inspection->immediateOriginFeatureIds.empty());
    CHECK(futureTop.provenanceRecordCount == 1);
    CHECK(futureTop.localNameUtf8Bytes == 6);

    auto futureOrigin = provenance(
        7, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("FutureRole"), QStringLiteral("origin"));
    const auto originResult = decode(
        v2(QStringLiteral("Face"), QJsonArray{futureOrigin}),
        TopologyKind::Face);
    checkFailure(originResult,
                 ProjectFormatV4ReferenceDecodeStatus::Unsupported);
    CHECK(originResult.inspection);
    CHECK(originResult.inspection->bodyId == 11);
    CHECK(originResult.inspection->topLevelFeatureId == 23);
    CHECK(originResult.inspection->immediateOriginFeatureIds ==
          (std::vector<FeatureId>{7}));
    CHECK(originResult.provenanceRecordCount == 2);
    CHECK(originResult.localNameUtf8Bytes == 12);
  }

  // Invalid results are transactional even after all owner IDs and an
  // earlier origin have already been parsed.
  {
    auto malformedOrigin = provenance(
        8, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QStringLiteral("later"));
    malformedOrigin.remove(QStringLiteral("localName"));
    checkInvalid(v2(QStringLiteral("Face"),
                    QJsonArray{provenance(7, QStringLiteral("Face"),
                                          QStringLiteral("Seed"),
                                          QStringLiteral("Cap"),
                                          QStringLiteral("earlier")),
                               malformedOrigin}),
                 TopologyKind::Face);
    checkInvalid(v1(QStringLiteral("Edge")), TopologyKind::Face);
  }

  // All enum token families share the bounded, non-empty, valid-UTF-16
  // string contract. A bounded unknown spelling is forward-compatible.
  {
    const QString boundedUnknown(ProjectFile::kMaximumStringCharacters,
                                 QChar('X'));
    const QString oversizedUnknown(ProjectFile::kMaximumStringCharacters + 1,
                                   QChar('X'));
    const QString invalidSurrogate(1, QChar(0xd800));

    auto legacy = v1(boundedUnknown);
    checkUnsupported(legacy, TopologyKind::Face);
    legacy["kind"] = oversizedUnknown;
    checkInvalid(legacy, TopologyKind::Face);
    legacy["kind"] = invalidSurrogate;
    checkInvalid(legacy, TopologyKind::Face);
    legacy["kind"] = QString{};
    checkInvalid(legacy, TopologyKind::Face);

    auto value = v2(QStringLiteral("Face"));
    auto key = value.value(QStringLiteral("provenance")).toObject();
    key["operation"] = boundedUnknown;
    value["provenance"] = key;
    checkUnsupported(value, TopologyKind::Face);
    key["operation"] = oversizedUnknown;
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    key["operation"] = invalidSurrogate;
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    key["operation"] = QString{};
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);

    auto origin = provenance(1, QStringLiteral("Face"),
                             QStringLiteral("Seed"), boundedUnknown,
                             QStringLiteral("origin"));
    checkUnsupported(v2(QStringLiteral("Face"), QJsonArray{origin}),
                     TopologyKind::Face);
    origin["role"] = oversizedUnknown;
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{origin}),
                 TopologyKind::Face);
    origin["role"] = invalidSurrogate;
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{origin}),
                 TopologyKind::Face);
    origin["role"] = QString{};
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{origin}),
                 TopologyKind::Face);
  }

  // V2 schemas are closed at every nesting level and occurrence ownership is
  // exact.
  {
    auto value = v2(QStringLiteral("Face"));
    value["extra"] = 1;
    checkInvalid(value, TopologyKind::Face);
    value = v2(QStringLiteral("Face"));
    value.remove(QStringLiteral("bodyId"));
    checkInvalid(value, TopologyKind::Face);
    value = v2(QStringLiteral("Face"));
    value["provenance"] = QStringLiteral("not-an-object");
    checkInvalid(value, TopologyKind::Face);
    value = v2(QStringLiteral("Face"));
    auto key = value.value("provenance").toObject();
    key["origins"] = QStringLiteral("not-an-array");
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    value = v2(QStringLiteral("Face"));
    key = value.value("provenance").toObject();
    key["origins"] = QJsonArray{1};
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    value = v2(QStringLiteral("Face"));
    key = value.value("provenance").toObject();
    key["legacyIndex"] = 0;
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    key = v2(QStringLiteral("Face")).value("provenance").toObject();
    key["instanceKey"] = instance(99, 0);
    value = v2(QStringLiteral("Face"));
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    key = v2(QStringLiteral("Face")).value("provenance").toObject();
    auto malformedInstance = instance(23, 0);
    malformedInstance.remove(QStringLiteral("slot"));
    key["instanceKey"] = malformedInstance;
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    malformedInstance = instance(23, 0);
    malformedInstance["extra"] = true;
    key["instanceKey"] = malformedInstance;
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);

    QJsonArray origins;
    auto origin = provenance(1, QStringLiteral("Face"), QStringLiteral("Seed"),
                             QStringLiteral("Cap"), QStringLiteral("a"));
    origin["origins"] = QJsonArray{};
    origins.append(origin);
    checkInvalid(v2(QStringLiteral("Face"), origins), TopologyKind::Face);
    origin = provenance(1, QStringLiteral("Face"), QStringLiteral("Seed"),
                        QStringLiteral("Cap"), QStringLiteral("a"));
    origin.remove(QStringLiteral("localName"));
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{origin}),
                 TopologyKind::Face);
    origins = QJsonArray{};
    origins.append(provenance(1, QStringLiteral("Face"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              QStringLiteral("a"), instance(2, 0)));
    checkInvalid(v2(QStringLiteral("Face"), origins), TopologyKind::Face);
    value = v2(QStringLiteral("Face"));
    key = value.value("provenance").toObject();
    key["instanceKey"] = instance(23, 1.5);
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
    key["instanceKey"] = instance(23, 9007199254740992.0);
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
  }

  // Every V2 integer site uses exact JSON-integer and persisted-ID bounds.
  {
    const double maximumId =
        static_cast<double>(ProjectFile::kMaximumPersistedId);
    const double maximumExact = 9007199254740991.0;

    auto value = v2(QStringLiteral("Face"));
    value["bodyId"] = 1;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    value["bodyId"] = maximumId;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    for (const QJsonValue invalid :
         {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5),
          QJsonValue(maximumId + 1.0), QJsonValue(QStringLiteral("1"))}) {
      value = v2(QStringLiteral("Face"));
      value["bodyId"] = invalid;
      checkInvalid(value, TopologyKind::Face);
    }

    value = v2(QStringLiteral("Face"));
    auto key = value.value("provenance").toObject();
    key["featureId"] = 1;
    value["provenance"] = key;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    key["featureId"] = maximumId;
    value["provenance"] = key;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    for (const QJsonValue invalid :
         {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5),
          QJsonValue(maximumId + 1.0), QJsonValue(QStringLiteral("1"))}) {
      value = v2(QStringLiteral("Face"));
      key = value.value("provenance").toObject();
      key["featureId"] = invalid;
      value["provenance"] = key;
      checkInvalid(value, TopologyKind::Face);
    }

    auto origin = provenance(1, QStringLiteral("Face"),
                             QStringLiteral("Seed"), QStringLiteral("Cap"),
                             QStringLiteral("origin"));
    CHECK(decode(v2(QStringLiteral("Face"), QJsonArray{origin}),
                 TopologyKind::Face)
              .succeeded());
    origin["featureId"] = maximumId;
    CHECK(decode(v2(QStringLiteral("Face"), QJsonArray{origin}),
                 TopologyKind::Face)
              .succeeded());
    for (const QJsonValue invalid :
         {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5),
          QJsonValue(maximumId + 1.0), QJsonValue(QStringLiteral("1"))}) {
      origin = provenance(1, QStringLiteral("Face"),
                          QStringLiteral("Seed"), QStringLiteral("Cap"),
                          QStringLiteral("origin"));
      origin["featureId"] = invalid;
      checkInvalid(v2(QStringLiteral("Face"), QJsonArray{origin}),
                   TopologyKind::Face);
    }

    value = v2(QStringLiteral("Face"));
    key = value.value("provenance").toObject();
    key["featureId"] = maximumId;
    key["instanceKey"] = instance(ProjectFile::kMaximumPersistedId, 0);
    value["provenance"] = key;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    for (const QJsonValue invalid :
         {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5),
          QJsonValue(maximumId + 1.0), QJsonValue(QStringLiteral("23"))}) {
      value = v2(QStringLiteral("Face"));
      key = value.value("provenance").toObject();
      QJsonObject occurrence{{"featureId", invalid}, {"slot", 0}};
      key["instanceKey"] = occurrence;
      value["provenance"] = key;
      checkInvalid(value, TopologyKind::Face);
    }

    value = v2(QStringLiteral("Face"));
    key = value.value("provenance").toObject();
    key["instanceKey"] = instance(23, maximumExact);
    value["provenance"] = key;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    for (const QJsonValue invalid :
         {QJsonValue(-1), QJsonValue(1.5),
          QJsonValue(9007199254740992.0), QJsonValue(QStringLiteral("0"))}) {
      value = v2(QStringLiteral("Face"));
      key = value.value("provenance").toObject();
      QJsonObject occurrence{{"featureId", 23}, {"slot", invalid}};
      key["instanceKey"] = occurrence;
      value["provenance"] = key;
      checkInvalid(value, TopologyKind::Face);
    }
  }

  // Raw origin order is authoritative. Duplicates, tuple inversions, UTF-8
  // byte-order inversions and present-before-absent occurrences are rejected.
  {
    const auto checkOrderedPair = [](const QJsonObject& lower,
                                     const QJsonObject& upper) {
      CHECK(decode(v2(QStringLiteral("Face"), QJsonArray{lower, upper}),
                   TopologyKind::Face)
                .succeeded());
      checkInvalid(v2(QStringLiteral("Face"), QJsonArray{upper, lower}),
                   TopologyKind::Face);
    };

    const auto a = provenance(1, QStringLiteral("Face"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              QStringLiteral("a"));
    const auto b = provenance(2, QStringLiteral("Face"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              QStringLiteral("b"));
    checkOrderedPair(a, b);
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{a, a}),
                 TopologyKind::Face);

    const auto face = provenance(3, QStringLiteral("Face"),
                                 QStringLiteral("Seed"), QStringLiteral("Cap"),
                                 QStringLiteral("same"));
    const auto edge = provenance(3, QStringLiteral("Edge"),
                                 QStringLiteral("Seed"), QStringLiteral("Cap"),
                                 QStringLiteral("same"));
    checkOrderedPair(face, edge);

    const auto seed = provenance(4, QStringLiteral("Face"),
                                 QStringLiteral("Seed"), QStringLiteral("Cap"),
                                 QStringLiteral("same"));
    const auto generated = provenance(
        4, QStringLiteral("Face"), QStringLiteral("Generated"),
        QStringLiteral("Cap"), QStringLiteral("same"));
    checkOrderedPair(seed, generated);

    const auto cap = provenance(5, QStringLiteral("Face"),
                                QStringLiteral("Seed"), QStringLiteral("Cap"),
                                QStringLiteral("same"));
    const auto side = provenance(5, QStringLiteral("Face"),
                                 QStringLiteral("Seed"), QStringLiteral("Side"),
                                 QStringLiteral("same"));
    checkOrderedPair(cap, side);

    const auto ascii = provenance(1, QStringLiteral("Face"),
                                  QStringLiteral("Seed"),
                                  QStringLiteral("Cap"), QStringLiteral("a"));
    const auto latin = provenance(1, QStringLiteral("Face"),
                                  QStringLiteral("Seed"),
                                  QStringLiteral("Cap"), QString::fromUtf8("é"));
    const auto cyrillic = provenance(1, QStringLiteral("Face"),
                                     QStringLiteral("Seed"),
                                     QStringLiteral("Cap"),
                                     QString::fromUtf8("я"));
    CHECK(decode(v2(QStringLiteral("Face"), QJsonArray{ascii, latin, cyrillic}),
                 TopologyKind::Face)
              .succeeded());
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{latin, ascii}),
                 TopologyKind::Face);

    QString supplementary;
    supplementary.append(QChar(0xd800));
    supplementary.append(QChar(0xdc00));
    const auto privateUse = provenance(
        6, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QString(1, QChar(0xe000)));
    const auto supplementaryName = provenance(
        6, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), supplementary);
    // UTF-16 orders the surrogate pair first; unsigned UTF-8 bytes order
    // U+E000 (0xEE...) before U+10000 (0xF0...).
    checkOrderedPair(privateUse, supplementaryName);

    const auto absent = provenance(3, QStringLiteral("Face"),
                                   QStringLiteral("Seed"),
                                   QStringLiteral("Cap"), QStringLiteral("x"));
    const auto present = provenance(3, QStringLiteral("Face"),
                                    QStringLiteral("Seed"),
                                    QStringLiteral("Cap"), QStringLiteral("x"),
                                    instance(3, 0));
    CHECK(decode(v2(QStringLiteral("Face"), QJsonArray{absent, present}),
                 TopologyKind::Face)
              .succeeded());
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{present, absent}),
                 TopologyKind::Face);
    const auto slotZero = provenance(4, QStringLiteral("Face"),
                                     QStringLiteral("Seed"),
                                     QStringLiteral("Cap"), QStringLiteral("x"),
                                     instance(4, 0));
    const auto slotOne = provenance(4, QStringLiteral("Face"),
                                    QStringLiteral("Seed"),
                                    QStringLiteral("Cap"), QStringLiteral("x"),
                                    instance(4, 1));
    CHECK(decode(v2(QStringLiteral("Face"), QJsonArray{slotZero, slotOne}),
                 TopologyKind::Face)
              .succeeded());
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{slotOne, slotZero}),
                 TopologyKind::Face);

    const auto occurrenceOwnerLow = provenance(
        7, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QStringLiteral("x"), instance(7, 0));
    const auto occurrenceOwnerHigh = provenance(
        8, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QStringLiteral("x"), instance(8, 0));
    checkOrderedPair(occurrenceOwnerLow, occurrenceOwnerHigh);

    auto future = provenance(2, QStringLiteral("FutureKind"),
                             QStringLiteral("Seed"), QStringLiteral("Cap"),
                             QStringLiteral("future"));
    auto earlier = provenance(1, QStringLiteral("Face"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              QStringLiteral("earlier"));
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{future, earlier}),
                 TopologyKind::Face);
    checkInvalid(v2(QStringLiteral("Face"), QJsonArray{future, future}),
                 TopologyKind::Face);

    const auto knownSide = provenance(
        9, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Side"), QStringLiteral("same"));
    const auto unknownRole = provenance(
        9, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("FutureRole"), QStringLiteral("same"));
    const auto knownCap = provenance(
        9, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QStringLiteral("same"));
    checkInvalid(v2(QStringLiteral("Face"),
                    QJsonArray{knownSide, unknownRole, knownCap}),
                 TopologyKind::Face);
    checkUnsupported(v2(QStringLiteral("Face"),
                        QJsonArray{knownCap, unknownRole, knownSide}),
                     TopologyKind::Face);
  }

  // Per-reference resource boundaries are checked before carrier allocation.
  {
    QJsonArray origins;
    for (int id = 1; id <= 256; ++id)
      origins.append(provenance(id, QStringLiteral("Face"),
                                QStringLiteral("Seed"), QStringLiteral("Cap"),
                                QStringLiteral("o%1").arg(id, 3, 10, QChar('0'))));
    CHECK(decode(v2(QStringLiteral("Face"), origins), TopologyKind::Face)
              .succeeded());
    origins.append(provenance(257, QStringLiteral("Face"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              QStringLiteral("o257")));
    checkInvalid(v2(QStringLiteral("Face"), origins), TopologyKind::Face);

    auto value = v2(QStringLiteral("Face"));
    auto key = value.value("provenance").toObject();
    key["localName"] = QString(16384, QChar('x'));
    value["provenance"] = key;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    key["localName"] = QString(16385, QChar('x'));
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);

    key["localName"] = QString(8192, QChar(0x00e9));
    value["provenance"] = key;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    key["localName"] = QString(8192, QChar(0x00e9)) + QChar('a');
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);

    QJsonArray exactBudget;
    for (int id = 1; id <= 15; ++id)
      exactBudget.append(provenance(id, QStringLiteral("Face"),
                                    QStringLiteral("Seed"),
                                    QStringLiteral("Cap"),
                                    QString(16384, QChar('a' + id))));
    exactBudget.append(provenance(16, QStringLiteral("Face"),
                                  QStringLiteral("Seed"),
                                  QStringLiteral("Cap"),
                                  QString(16383, QChar('z'))));
    value = v2(QStringLiteral("Face"), exactBudget);
    key = value.value("provenance").toObject();
    key["localName"] = QStringLiteral("r");
    value["provenance"] = key;
    CHECK(decode(value, TopologyKind::Face).succeeded());
    auto plusOne = exactBudget;
    plusOne.removeLast();
    plusOne.append(provenance(16, QStringLiteral("Face"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              QString(16384, QChar('z'))));
    value = v2(QStringLiteral("Face"), plusOne);
    key = value.value("provenance").toObject();
    key["localName"] = QStringLiteral("r");
    value["provenance"] = key;
    checkInvalid(value, TopologyKind::Face);
  }

  // Failure classification and diagnostics are deterministic and never leak
  // a partial carrier or usage counters.
  {
    auto malformed = v2(QStringLiteral("Face"));
    auto key = malformed.value("provenance").toObject();
    key["localName"] = QString{};
    malformed["provenance"] = key;
    const auto first = decode(malformed, TopologyKind::Face);
    const auto second = decode(malformed, TopologyKind::Face);
    checkFailure(first, ProjectFormatV4ReferenceDecodeStatus::Invalid);
    checkFailure(second, ProjectFormatV4ReferenceDecodeStatus::Invalid);
    CHECK(first.error == second.error);
  }

  return EXIT_SUCCESS;
}

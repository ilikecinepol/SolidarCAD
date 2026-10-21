#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <concepts>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/EdgeReference.h"
#include "model/FaceReference.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/ProjectFormatV4TopologyAggregator.h"

namespace {

using solidar::BodyId;
using solidar::EdgeReference;
using solidar::FaceReference;
using solidar::FeatureId;
using solidar::TopologyKind;
using solidar::project::ProjectFormatV4DeferredTopologyReference;
using solidar::project::ProjectFormatV4FeatureOwner;
using solidar::project::ProjectFormatV4PayloadCandidate;
using solidar::project::ProjectFormatV4TopologyAggregateCandidate;
using solidar::project::ProjectFormatV4TopologyAggregateResult;
using solidar::project::ProjectFormatV4TopologyAggregateStatus;
using solidar::project::ProjectFormatV4TopologySite;

template <typename T>
concept HasSucceeded = requires(const T& value) { value.succeeded(); };

template <typename T>
concept HasSuccessMember = requires(const T& value) { value.success; };

template <typename T>
concept HasDocumentMember = requires(const T& value) { value.document; };

template <typename T>
concept ExposesMaterializedDocument = requires(T& value) {
  static_cast<solidar::Document&>(value.document);
};

static_assert(!HasSucceeded<ProjectFormatV4TopologyAggregateResult>);
static_assert(!HasSuccessMember<ProjectFormatV4TopologyAggregateResult>);
static_assert(!HasDocumentMember<ProjectFormatV4TopologyAggregateResult>);
static_assert(
    !ExposesMaterializedDocument<ProjectFormatV4TopologyAggregateResult>);
static_assert(!HasSucceeded<ProjectFormatV4TopologyAggregateCandidate>);
static_assert(!HasSuccessMember<ProjectFormatV4TopologyAggregateCandidate>);
static_assert(!HasDocumentMember<ProjectFormatV4TopologyAggregateCandidate>);
static_assert(
    !ExposesMaterializedDocument<ProjectFormatV4TopologyAggregateCandidate>);
static_assert(!HasSucceeded<ProjectFormatV4DeferredTopologyReference>);
static_assert(!HasSuccessMember<ProjectFormatV4DeferredTopologyReference>);
static_assert(!HasDocumentMember<ProjectFormatV4DeferredTopologyReference>);

QJsonObject rootEnvelope() {
  return QJsonObject{
      {"format", "solidar-project"},
      {"version", 4},
      {"capabilities", QJsonArray{"persistent-topology-v2"}},
      {"constraintTypeEncoding", "stable-name-v1"},
      {"name", "aggregator-fixture"},
      {"createdAt", "2026-10-21T00:00:00Z"},
      {"document",
       QJsonObject{{"widthMm", 60}, {"heightMm", 40}, {"extrusionMm", 25}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model",
       QJsonObject{{"sketches", QJsonArray{}},
                   {"bodies", QJsonArray{}},
                   {"referenceImages", QJsonArray{}}}}};
}

QJsonObject wrapper(const QJsonObject& reference) {
  return QJsonObject{{"reference", reference}};
}

QJsonObject v1(const QString& kind, qint64 bodyId, qint64 featureId,
               qint64 legacyIndex) {
  return QJsonObject{{"referenceVersion", "V1"},
                     {"bodyId", bodyId},
                     {"featureId", featureId},
                     {"kind", kind},
                     {"legacyIndex", legacyIndex}};
}

QJsonObject provenance(qint64 featureId, const QString& kind,
                       const QString& operation, const QString& role,
                       const QString& localName) {
  return QJsonObject{{"featureId", featureId},
                     {"kind", kind},
                     {"operation", operation},
                     {"role", role},
                     {"localName", localName}};
}

QJsonObject v2(const QString& kind, qint64 bodyId, qint64 featureId,
               const QString& localName, QJsonArray origins = {}) {
  QJsonObject key = provenance(featureId, kind, QStringLiteral("Generated"),
                               QStringLiteral("Custom"), localName);
  key["origins"] = origins;
  return QJsonObject{{"referenceVersion", "V2"},
                     {"bodyId", bodyId},
                     {"provenance", key}};
}

QJsonObject moveFeature(qint64 id) {
  return QJsonObject{{"id", id},
                     {"name", "producer"},
                     {"type", "Move"},
                     {"sourceFeatureId", 1},
                     {"offsetXmm", 0},
                     {"offsetYmm", 0},
                     {"offsetZmm", 0}};
}

QJsonObject body(qint64 id, const QJsonArray& features) {
  return QJsonObject{{"id", id},
                     {"name", "body"},
                     {"visible", true},
                     {"features", features}};
}

void setModel(QJsonObject* root, const QJsonArray& sketches,
              const QJsonArray& bodies) {
  (*root)["model"] = QJsonObject{{"sketches", sketches},
                                  {"bodies", bodies},
                                  {"referenceImages", QJsonArray{}}};
}

QJsonObject faceExtrude(qint64 id, const QJsonObject& reference) {
  return QJsonObject{{"id", id},
                     {"name", "extrude"},
                     {"type", "Extrude"},
                     {"lengthMm", 10},
                     {"operation", 0},
                     {"reversed", false},
                     {"sourceKind", "face"},
                     {"face", wrapper(reference)}};
}

QJsonObject fillet(qint64 id, const QJsonArray& references) {
  QJsonArray edges;
  for (const auto& reference : references) edges.append(wrapper(reference.toObject()));
  return QJsonObject{{"id", id},
                     {"name", "fillet"},
                     {"type", "Fillet"},
                     {"radiusMm", 1},
                     {"edges", edges}};
}

ProjectFormatV4PayloadCandidate payloadFor(const QJsonObject& root) {
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  auto envelope = solidar::project::scanProjectFormatV4Envelope(bytes);
  CHECK(envelope.hasCandidate());
  auto payload = solidar::project::scanProjectFormatV4Payload(
      std::move(*envelope.candidate));
  CHECK(payload.hasCandidate());
  return std::move(*payload.candidate);
}

ProjectFormatV4TopologyAggregateResult aggregate(const QJsonObject& root) {
  return solidar::project::aggregateProjectFormatV4Topology(payloadFor(root));
}

void checkCandidate(const ProjectFormatV4TopologyAggregateResult& result) {
  CHECK(result.status == ProjectFormatV4TopologyAggregateStatus::Candidate);
  CHECK(result.hasCandidate());
  CHECK(result.candidate.has_value());
  CHECK(result.error.isEmpty());
  CHECK(result.candidate->topologyInspections.size() ==
        result.candidate->payload.topologySites.size());
}

void checkUnsupported(const ProjectFormatV4TopologyAggregateResult& result) {
  CHECK(result.status ==
        ProjectFormatV4TopologyAggregateStatus::UnsupportedCandidate);
  CHECK(result.hasCandidate());
  CHECK(result.candidate.has_value());
  CHECK(result.error.isEmpty());
  CHECK(result.candidate->topologyInspections.size() ==
        result.candidate->payload.topologySites.size());
}

void checkInvalid(const ProjectFormatV4TopologyAggregateResult& result) {
  CHECK(result.status == ProjectFormatV4TopologyAggregateStatus::Invalid);
  CHECK(!result.hasCandidate());
  CHECK(!result.candidate.has_value());
  CHECK(!result.allTopologyReferencesDecoded());
  CHECK(!result.error.isEmpty());
}

QJsonObject ownershipRoot(const QJsonObject& reference) {
  QJsonObject root = rootEnvelope();
  setModel(&root, {},
           {body(10, QJsonArray{faceExtrude(1, reference), moveFeature(2)}),
            body(20, QJsonArray{moveFeature(3)})});
  return root;
}

QJsonObject edgeListRoot(const QJsonArray& references,
                         qsizetype producerCount = 2) {
  QJsonObject root = rootEnvelope();
  QJsonArray producers;
  for (qsizetype id = 1; id <= producerCount; ++id)
    producers.append(moveFeature(id));
  setModel(&root, {},
           {body(10, QJsonArray{fillet(300, references)}),
            body(20, producers)});
  return root;
}

QJsonObject allSitesRoot(std::array<QJsonObject, 9>* referencesOut) {
  const QJsonObject crossBodyOrigin =
      provenance(901, QStringLiteral("Face"), QStringLiteral("Seed"),
                 QStringLiteral("Cap"), QStringLiteral("cross-body"));
  std::array<QJsonObject, 9> references{
      v1(QStringLiteral("Face"), 20, 900, 10),
      v2(QStringLiteral("Face"), 20, 900, QStringLiteral("extrude-face"),
         QJsonArray{crossBodyOrigin}),
      v1(QStringLiteral("Edge"), 20, 900, 12),
      v2(QStringLiteral("Edge"), 20, 900, QStringLiteral("chamfer-edge")),
      v1(QStringLiteral("Face"), 20, 900, 14),
      v2(QStringLiteral("Face"), 20, 900, QStringLiteral("shell-face")),
      v1(QStringLiteral("Face"), 20, 900, 16),
      v2(QStringLiteral("Edge"), 20, 900, QStringLiteral("rotation-edge")),
      v1(QStringLiteral("Face"), 20, 900, 18)};

  QJsonObject sketch{{"id", 50},
                     {"name", "supported"},
                     {"origin", QJsonArray{0, 0, 0}},
                     {"xDirection", QJsonArray{1, 0, 0}},
                     {"yDirection", QJsonArray{0, 1, 0}},
                     {"support", QJsonObject{{"type", 1},
                                              {"reference", references[0]}}}};
  QJsonObject extrude = faceExtrude(1, references[1]);
  QJsonObject filletFeature = fillet(2, QJsonArray{references[2]});
  QJsonObject chamfer{{"id", 3},
                      {"name", "chamfer"},
                      {"type", "Chamfer"},
                      {"distanceMm", 1},
                      {"edges", QJsonArray{wrapper(references[3])}}};
  QJsonObject mirror{{"id", 4},
                     {"name", "mirror"},
                     {"type", "Mirror"},
                     {"sourceFeatureId", 1},
                     {"plane", 0},
                     {"planeFace", wrapper(references[4])}};
  QJsonObject shell{{"id", 5},
                    {"name", "shell"},
                    {"type", "Shell"},
                    {"sourceFeatureId", 1},
                    {"thicknessMm", 1},
                    {"outside", false},
                    {"removedFaces", QJsonArray{wrapper(references[5])}}};
  QJsonObject draft{{"id", 6},
                    {"name", "draft"},
                    {"type", "Draft"},
                    {"sourceFeatureId", 1},
                    {"angleDeg", 5},
                    {"reversed", false},
                    {"neutralPlaneType", 3},
                    {"pullDirectionType", 3},
                    {"pullDirectionSketchId", 0},
                    {"pullDirectionLineId", 0},
                    {"neutralPlaneFace", wrapper(references[6])},
                    {"rotationEdge", wrapper(references[7])},
                    {"draftedFaces", QJsonArray{wrapper(references[8])}}};

  QJsonObject root = rootEnvelope();
  setModel(&root, QJsonArray{sketch},
           {body(10, QJsonArray{extrude, filletFeature, chamfer, mirror, shell,
                                draft}),
            body(20, QJsonArray{moveFeature(900)}),
            body(30, QJsonArray{moveFeature(901)})});
  if (referencesOut) *referencesOut = references;
  return root;
}

QJsonArray maximumOriginSet(const QString& localName) {
  QJsonArray origins;
  for (qint64 id = 1; id <= 256; ++id)
    origins.append(provenance(id, QStringLiteral("Edge"),
                              QStringLiteral("Seed"), QStringLiteral("Cap"),
                              localName));
  return origins;
}

}  // namespace

int main() {
  using namespace solidar::project;

  // All nine site families remain 1:1 and in authored scanner order. Mixed
  // V1/V2 carriers preserve distinct data, including a cross-body V2 origin.
  {
    std::array<QJsonObject, 9> authoredReferences;
    const QJsonObject root = allSitesRoot(&authoredReferences);
    const ProjectFormatV4PayloadCandidate original = payloadFor(root);
    const auto result = aggregateProjectFormatV4Topology(original);
    checkCandidate(result);
    CHECK(result.allTopologyReferencesDecoded());
    const auto& candidate = *result.candidate;
    CHECK(candidate.topologyReferences.size() == 9);
    CHECK(candidate.topologyInspections.size() == 9);
    for (const auto& inspection : candidate.topologyInspections)
      CHECK(inspection.has_value());
    CHECK(candidate.payload.topologySites == original.topologySites);
    CHECK(candidate.payload.featureOwners == original.featureOwners);
    CHECK(candidate.payload.envelope.authoredRoot ==
          original.envelope.authoredRoot);
    CHECK(candidate.deferredUnsupported.empty());
    CHECK(candidate.provenanceRecordCount == 5);
    CHECK(candidate.localNameUtf8Bytes ==
          QStringLiteral("extrude-face").toUtf8().size() +
              QStringLiteral("cross-body").toUtf8().size() +
              QStringLiteral("chamfer-edge").toUtf8().size() +
              QStringLiteral("shell-face").toUtf8().size() +
              QStringLiteral("rotation-edge").toUtf8().size());

    const std::array<QString, 9> paths{
        "$.model.sketches[0].support.reference",
        "$.model.bodies[0].features[0].face.reference",
        "$.model.bodies[0].features[1].edges[0].reference",
        "$.model.bodies[0].features[2].edges[0].reference",
        "$.model.bodies[0].features[3].planeFace.reference",
        "$.model.bodies[0].features[4].removedFaces[0].reference",
        "$.model.bodies[0].features[5].neutralPlaneFace.reference",
        "$.model.bodies[0].features[5].rotationEdge.reference",
        "$.model.bodies[0].features[5].draftedFaces[0].reference"};
    for (std::size_t index = 0; index < paths.size(); ++index) {
      CHECK(candidate.payload.topologySites[index].path == paths[index]);
      CHECK(candidate.payload.topologySites[index].authoredReference ==
            authoredReferences[index]);
      CHECK(candidate.payload.topologySites[index].expectedKind ==
            (index == 2 || index == 3 || index == 7 ? TopologyKind::Edge
                                                     : TopologyKind::Face));
    }
    const auto& references = candidate.topologyReferences;
    for (std::size_t index = 0; index < references.size(); ++index) {
      const TopologyKind expectedKind =
          index == 2 || index == 3 || index == 7 ? TopologyKind::Edge
                                                  : TopologyKind::Face;
      const bool expectedV1 = index % 2 == 0;
      if (expectedKind == TopologyKind::Face) {
        CHECK(std::holds_alternative<FaceReference>(*references[index]));
        const auto& face = std::get<FaceReference>(*references[index]);
        CHECK(face.bodyId() == BodyId{20});
        CHECK(face.featureId() == FeatureId{900});
        CHECK(face.isV1() == expectedV1);
        CHECK(expectedV1 ? face.v1()->kind == expectedKind
                         : face.v2()->provenance.kind == expectedKind);
      } else {
        CHECK(std::holds_alternative<EdgeReference>(*references[index]));
        const auto& edge = std::get<EdgeReference>(*references[index]);
        CHECK(edge.bodyId() == BodyId{20});
        CHECK(edge.featureId() == FeatureId{900});
        CHECK(edge.isV1() == expectedV1);
        CHECK(expectedV1 ? edge.v1()->kind == expectedKind
                         : edge.v2()->provenance.kind == expectedKind);
      }
    }
    CHECK(std::get<FaceReference>(*references[0]).v1()->legacyIndex == 10);
    CHECK(std::get<FaceReference>(*references[1])
              .v2()
              ->provenance.localName == "extrude-face");
    CHECK(std::get<EdgeReference>(*references[2]).v1()->legacyIndex == 12);
    CHECK(std::get<EdgeReference>(*references[3])
              .v2()
              ->provenance.localName == "chamfer-edge");
    CHECK(std::get<FaceReference>(*references[4]).v1()->legacyIndex == 14);
    CHECK(std::get<FaceReference>(*references[5])
              .v2()
              ->provenance.localName == "shell-face");
    CHECK(std::get<FaceReference>(*references[6]).v1()->legacyIndex == 16);
    CHECK(std::get<EdgeReference>(*references[7])
              .v2()
              ->provenance.localName == "rotation-edge");
    CHECK(std::get<FaceReference>(*references[8]).v1()->legacyIndex == 18);
    CHECK(std::get<FaceReference>(*references[1])
              .v2()
              ->provenance.origins[0]
              .featureId == 901);
  }

  // P2a statuses propagate without turning unsupported references into final
  // invalidity. Recognized unsupported references still undergo ownership.
  {
    QJsonObject opaque{{"referenceVersion", "V9"}, {"future", true}};
    auto result = aggregate(ownershipRoot(opaque));
    checkUnsupported(result);
    CHECK(result.candidate->topologyReferences.size() == 1);
    CHECK(!result.candidate->topologyReferences[0]);
    CHECK(!result.candidate->topologyInspections[0]);
    CHECK(result.candidate->deferredUnsupported.size() == 1);
    CHECK(result.candidate->deferredUnsupported[0].path ==
          "$.model.bodies[0].features[0].face.reference");
    CHECK(!result.allTopologyReferencesDecoded());

    QJsonObject future = v2(QStringLiteral("Face"), 10, 2,
                            QStringLiteral("known-unsupported"));
    QJsonObject key = future["provenance"].toObject();
    key["operation"] = "FutureOperation";
    future["provenance"] = key;
    result = aggregate(ownershipRoot(future));
    checkUnsupported(result);
    CHECK(result.candidate->provenanceRecordCount == 1);
    CHECK(result.candidate->localNameUtf8Bytes == 17);
    CHECK(!result.candidate->topologyReferences[0]);
    CHECK(result.candidate->topologyInspections[0].has_value());

    QJsonObject malformed = v1(QStringLiteral("Face"), 10, 2, 1);
    malformed.remove("legacyIndex");
    checkInvalid(aggregate(ownershipRoot(malformed)));
    malformed = v2(QStringLiteral("Face"), 10, 2, QStringLiteral("bad"));
    malformed["provenance"] = QJsonArray{};
    checkInvalid(aggregate(ownershipRoot(malformed)));
  }

  // Recognized V1/V2 variants remain ownership-checked even when an unknown
  // topology token makes the otherwise structural reference unsupported.
  // Each baseline below has valid ownership; the derived cases change only
  // producer/origin ownership and must therefore become Invalid.
  {
    const QJsonObject futureV1 =
        v1(QStringLiteral("FutureKind"), 10, 2, 1);
    checkUnsupported(aggregate(ownershipRoot(futureV1)));
    QJsonObject wrongV1Owner = futureV1;
    wrongV1Owner["bodyId"] = 20;
    checkInvalid(aggregate(ownershipRoot(wrongV1Owner)));
    QJsonObject missingV1Owner = futureV1;
    missingV1Owner["featureId"] = 999;
    checkInvalid(aggregate(ownershipRoot(missingV1Owner)));

    const QJsonObject futureV2Top =
        v2(QStringLiteral("FutureKind"), 10, 2,
           QStringLiteral("future-top"));
    checkUnsupported(aggregate(ownershipRoot(futureV2Top)));
    QJsonObject wrongV2TopOwner = futureV2Top;
    wrongV2TopOwner["bodyId"] = 20;
    checkInvalid(aggregate(ownershipRoot(wrongV2TopOwner)));
    QJsonObject missingV2TopOwner = futureV2Top;
    QJsonObject topKey = missingV2TopOwner["provenance"].toObject();
    topKey["featureId"] = 999;
    missingV2TopOwner["provenance"] = topKey;
    checkInvalid(aggregate(ownershipRoot(missingV2TopOwner)));

    const QJsonObject futureOrigin = provenance(
        3, QStringLiteral("FutureKind"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QStringLiteral("future-origin"));
    const QJsonObject futureV2Origin =
        v2(QStringLiteral("Face"), 10, 2, QStringLiteral("known-top"),
           QJsonArray{futureOrigin});
    checkUnsupported(aggregate(ownershipRoot(futureV2Origin)));
    QJsonObject wrongOriginTopOwner = futureV2Origin;
    wrongOriginTopOwner["bodyId"] = 20;
    checkInvalid(aggregate(ownershipRoot(wrongOriginTopOwner)));
    QJsonObject missingOriginTopOwner = futureV2Origin;
    topKey = missingOriginTopOwner["provenance"].toObject();
    topKey["featureId"] = 999;
    missingOriginTopOwner["provenance"] = topKey;
    checkInvalid(aggregate(ownershipRoot(missingOriginTopOwner)));
    QJsonObject missingOriginOwner = futureV2Origin;
    topKey = missingOriginOwner["provenance"].toObject();
    QJsonArray origins = topKey["origins"].toArray();
    QJsonObject missingOrigin = origins[0].toObject();
    missingOrigin["featureId"] = 999;
    origins[0] = missingOrigin;
    topKey["origins"] = origins;
    missingOriginOwner["provenance"] = topKey;
    checkInvalid(aggregate(ownershipRoot(missingOriginOwner)));
  }

  // Ownership checks use producer metadata, never the containing consumer.
  // V2 immediate origins are document-global and may cross body boundaries.
  {
    checkCandidate(aggregate(ownershipRoot(
        v1(QStringLiteral("Face"), 10, 2, 1))));
    checkInvalid(aggregate(ownershipRoot(
        v1(QStringLiteral("Face"), 99, 2, 1))));
    checkInvalid(aggregate(ownershipRoot(
        v1(QStringLiteral("Face"), 10, 999, 1))));
    checkInvalid(aggregate(ownershipRoot(
        v1(QStringLiteral("Face"), 20, 2, 1))));

    const QJsonObject crossBody = provenance(
        3, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QStringLiteral("origin"));
    checkCandidate(aggregate(ownershipRoot(
        v2(QStringLiteral("Face"), 10, 2, QStringLiteral("top"),
           QJsonArray{crossBody}))));
    checkInvalid(aggregate(ownershipRoot(
        v2(QStringLiteral("Face"), 99, 2, QStringLiteral("top")))));
    checkInvalid(aggregate(ownershipRoot(
        v2(QStringLiteral("Face"), 10, 999, QStringLiteral("top")))));
    checkInvalid(aggregate(ownershipRoot(
        v2(QStringLiteral("Face"), 20, 2, QStringLiteral("top")))));
    const QJsonObject missingOrigin = provenance(
        999, QStringLiteral("Face"), QStringLiteral("Seed"),
        QStringLiteral("Cap"), QStringLiteral("origin"));
    checkInvalid(aggregate(ownershipRoot(
        v2(QStringLiteral("Face"), 10, 2, QStringLiteral("top"),
           QJsonArray{missingOrigin}))));
  }

  // A later malformed or ownership-invalid reference overrides earlier
  // envelope/reference deferrals and returns no partial candidate.
  {
    QJsonObject malformed = v1(QStringLiteral("Edge"), 20, 2, 1);
    malformed.remove("legacyIndex");
    QJsonObject root = edgeListRoot(
        QJsonArray{QJsonObject{{"referenceVersion", "V9"}}, malformed});
    checkInvalid(aggregate(root));

    QJsonObject withEnvelopeUnknown = ownershipRoot(malformed);
    withEnvelopeUnknown["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    checkInvalid(aggregate(withEnvelopeUnknown));

    QJsonObject wrongOwner =
        v1(QStringLiteral("Edge"), 99, 2, 1);
    root = edgeListRoot(
        QJsonArray{QJsonObject{{"referenceVersion", "V9"}}, wrongOwner});
    checkInvalid(aggregate(root));
  }

  // Exact aggregate provenance record boundary. Each full V2 reference has
  // one top record plus 256 immediate origins.
  {
    constexpr std::size_t fullReferences = 255;
    constexpr std::size_t recordsPerFullReference = 257;
    static_assert(fullReferences * recordsPerFullReference + 1 == 65536);
    QJsonArray references;
    const QJsonObject full =
        v2(QStringLiteral("Edge"), 20, 257, QStringLiteral("t"),
           maximumOriginSet(QStringLiteral("o")));
    for (std::size_t index = 0; index < fullReferences; ++index)
      references.append(full);
    references.append(
        v2(QStringLiteral("Edge"), 20, 257, QStringLiteral("z")));
    auto result = aggregate(edgeListRoot(references, 257));
    checkCandidate(result);
    CHECK(result.candidate->provenanceRecordCount ==
          kProjectFormatV4MaximumProvenanceRecords);
    CHECK(result.candidate->topologyReferences.size() == references.size());
    references.append(
        v2(QStringLiteral("Edge"), 20, 257, QStringLiteral("zz")));
    result = aggregate(edgeListRoot(references, 257));
    checkInvalid(result);
    CHECK(result.error.contains("provenance record limit"));
  }

  // Exact aggregate UTF-8 budget uses 32 independently valid 256 KiB
  // references; one additional byte crosses only the document-wide limit.
  {
    constexpr std::size_t nameBytes = 16384;
    constexpr std::size_t recordsPerReference = 16;
    constexpr std::size_t referencesAtLimit = 32;
    static_assert(nameBytes * recordsPerReference == 256 * 1024);
    static_assert(nameBytes * recordsPerReference * referencesAtLimit ==
                  8 * 1024 * 1024);
    const QString name(static_cast<qsizetype>(nameBytes), QLatin1Char('n'));
    QJsonArray origins;
    for (qint64 id = 1; id <= 15; ++id)
      origins.append(provenance(id, QStringLiteral("Edge"),
                                QStringLiteral("Seed"),
                                QStringLiteral("Cap"), name));
    const QJsonObject full =
        v2(QStringLiteral("Edge"), 20, 16, name, origins);
    QJsonArray references;
    for (std::size_t index = 0; index < referencesAtLimit; ++index)
      references.append(full);
    auto result = aggregate(edgeListRoot(references, 16));
    checkCandidate(result);
    CHECK(result.candidate->localNameUtf8Bytes ==
          kProjectFormatV4MaximumLocalNameUtf8Bytes);
    CHECK(result.candidate->provenanceRecordCount ==
          referencesAtLimit * recordsPerReference);
    references.append(
        v2(QStringLiteral("Edge"), 20, 16, QStringLiteral("x")));
    result = aggregate(edgeListRoot(references, 16));
    checkInvalid(result);
    CHECK(result.error.contains("local-name byte limit"));
  }

  // The public DTO is mutable, so every supplied site field and owner list is
  // compared against a canonical P2c rescan before any decoding begins.
  {
    const QJsonObject root = ownershipRoot(
        v1(QStringLiteral("Face"), 10, 2, 1));
    const auto canonical = payloadFor(root);
    const auto expectTamperInvalid = [](ProjectFormatV4PayloadCandidate value) {
      checkInvalid(aggregateProjectFormatV4Topology(std::move(value)));
    };

    auto tampered = canonical;
    tampered.topologySites[0].family =
        ProjectFormatV4TopologySiteFamily::FilletEdge;
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.topologySites[0].path += ".tampered";
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.topologySites[0].expectedKind = TopologyKind::Edge;
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.topologySites[0].authoredReference = QJsonObject{};
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.topologySites[0].containingBodyId = BodyId{20};
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.topologySites[0].containingFeatureId = FeatureId{2};
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.topologySites.clear();
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.topologySites.push_back(tampered.topologySites[0]);
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.featureOwners[0].bodyId = 20;
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.featureOwners[0].featureId = 999;
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    CHECK(tampered.featureOwners.size() >= 2);
    CHECK(tampered.featureOwners[0] != tampered.featureOwners[1]);
    std::swap(tampered.featureOwners[0], tampered.featureOwners[1]);
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.featureOwners.clear();
    expectTamperInvalid(std::move(tampered));
    tampered = canonical;
    tampered.featureOwners.push_back(tampered.featureOwners[0]);
    expectTamperInvalid(std::move(tampered));

    tampered = canonical;
    tampered.topologySites[0].path += ".deterministic";
    const auto first = aggregateProjectFormatV4Topology(tampered);
    const auto second = aggregateProjectFormatV4Topology(tampered);
    checkInvalid(first);
    checkInvalid(second);
    CHECK(first.status == second.status);
    CHECK(first.error == second.error);
  }

  // Envelope deferrals are seeded deterministically while decoded slots may
  // still be complete.
  {
    QJsonObject root = ownershipRoot(
        v1(QStringLiteral("Face"), 10, 2, 1));
    root["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    root["constraintTypeEncoding"] = "future-encoding";
    const auto result = aggregate(root);
    checkUnsupported(result);
    CHECK(result.allTopologyReferencesDecoded());
    CHECK(result.candidate->deferredUnsupported ==
          (std::vector<ProjectFormatV4DeferredTopologyReference>{
              {"$.capabilities[0]",
               "unsupported capability: future-capability"},
              {"$.constraintTypeEncoding",
               "unsupported constraint type encoding: future-encoding"}}));
  }

  return EXIT_SUCCESS;
}

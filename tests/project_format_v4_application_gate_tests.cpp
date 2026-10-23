#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <array>
#include <cstdlib>
#include <memory>
#include <vector>

#include "model/FilletFeature.h"
#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4ApplicationGate.h"
#include "ui/application/ProjectApplicationService.h"

namespace {

using namespace solidar;
using namespace solidar::project;

ShapeFeature::ShapePtr box() {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape());
}

Document importedDocument(BodyId bodyId, FeatureId featureId) {
  Document document;
  auto& body = document.addBody(bodyId, "body");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(featureId, box(), "source"));
  return document;
}

EdgeReference missingEdge(BodyId bodyId, FeatureId featureId,
                          const char* localName) {
  return EdgeReference::fromV2(
      {bodyId, TopologyProvenanceKey{
                   featureId, TopologyKind::Edge,
                   TopologyProvenanceOperation::Generated,
                   TopologySemanticRole::Custom, localName}});
}

Document topologyFailureDocument(bool twoRoots, bool hardRoot) {
  constexpr BodyId firstBodyId = 410001;
  constexpr FeatureId firstSourceId = 410002;
  Document document = importedDocument(firstBodyId, firstSourceId);
  auto& firstBody = *document.findBody(firstBodyId);
  firstBody.addFeature(std::make_unique<FilletFeature>(
      410003, missingEdge(firstBodyId, firstSourceId, "missing-a"), 1.0,
      "allowed missing"));
  if (twoRoots) {
    constexpr BodyId secondBodyId = 410011;
    constexpr FeatureId secondSourceId = 410012;
    auto& secondBody = document.addBody(secondBodyId, "second body");
    secondBody.addFeature(std::make_unique<ImportedShapeFeature>(
        secondSourceId, box(), "second source"));
    secondBody.addFeature(std::make_unique<FilletFeature>(
        410013, missingEdge(secondBodyId, secondSourceId, "missing-b"), 1.0,
        "second allowed missing"));
  }
  if (hardRoot) {
    constexpr BodyId hardBodyId = 410021;
    constexpr FeatureId hardSourceId = 410022;
    auto& hardBody = document.addBody(hardBodyId, "hard body");
    hardBody.addFeature(std::make_unique<ImportedShapeFeature>(
        hardSourceId, box(), "hard source"));
    hardBody.addFeature(std::make_unique<FilletFeature>(
        410023, EdgeReference(hardBodyId, hardSourceId, 0), 1000.0,
        "hard geometry failure"));
  }
  return document;
}

qint64 jsonVersion(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  const auto parsed = QJsonDocument::fromJson(file.readAll());
  CHECK(parsed.isObject());
  return parsed.object().value(QStringLiteral("version")).toInteger();
}

QJsonObject readRoot(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  const auto parsed = QJsonDocument::fromJson(file.readAll());
  CHECK(parsed.isObject());
  return parsed.object();
}

void writeRoot(const QString& path, const QJsonObject& root) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  CHECK(file.write(bytes) == bytes.size());
  CHECK(file.flush());
}

RecomputeFailure failure(RecomputeFailureSite site,
                         OperationFailureCode code) {
  return {site, 1, 2, 3, {code, "diagnostic text is not policy"}};
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-format-v4-application-gate-XXXXXX")));
  CHECK(directory.isValid());

  using solidar::project::detail::acceptsProjectFormatV4RecomputeResult;
  CHECK(acceptsProjectFormatV4RecomputeResult({true, {}}));
  CHECK(!acceptsProjectFormatV4RecomputeResult(
      {true, {failure(RecomputeFailureSite::Feature,
                      OperationFailureCode::TopologyReferenceMissing)}}));
  CHECK(!acceptsProjectFormatV4RecomputeResult({false, {}}));

  constexpr std::array allCodes{
      OperationFailureCode::None,
      OperationFailureCode::Unknown,
      OperationFailureCode::InvalidInput,
      OperationFailureCode::MissingSource,
      OperationFailureCode::TopologyIndexUnavailable,
      OperationFailureCode::TopologyReferenceMismatch,
      OperationFailureCode::TopologyReferenceMissing,
      OperationFailureCode::TopologyReferenceAmbiguous,
      OperationFailureCode::TopologyReferenceInvalid,
      OperationFailureCode::TopologyResolutionUnexpected,
      OperationFailureCode::NonPlanarFace,
      OperationFailureCode::UnsupportedSurface,
      OperationFailureCode::UnsupportedGeometry,
      OperationFailureCode::NoIntersection,
      OperationFailureCode::InvalidProfileOpen,
      OperationFailureCode::InvalidProfileOverlap,
      OperationFailureCode::InvalidProfile,
      OperationFailureCode::BodiesDoNotTouch,
      OperationFailureCode::GeometryOperationFailed};
  constexpr std::array allSites{
      RecomputeFailureSite::Structural, RecomputeFailureSite::SketchSupport,
      RecomputeFailureSite::Feature};
  for (const auto site : allSites) {
    for (const auto code : allCodes) {
      const bool expected =
          site != RecomputeFailureSite::Structural &&
          (code == OperationFailureCode::TopologyReferenceMissing ||
           code == OperationFailureCode::TopologyReferenceAmbiguous);
      CHECK(acceptsProjectFormatV4RecomputeResult(
                {false, {failure(site, code)}}) == expected);
    }
  }
  CHECK(acceptsProjectFormatV4RecomputeResult(
      {false,
       {failure(RecomputeFailureSite::Feature,
                OperationFailureCode::TopologyReferenceMissing),
        failure(RecomputeFailureSite::SketchSupport,
                OperationFailureCode::TopologyReferenceAmbiguous)}}));
  CHECK(!acceptsProjectFormatV4RecomputeResult(
      {false,
       {failure(RecomputeFailureSite::Feature,
                OperationFailureCode::TopologyReferenceMissing),
        failure(RecomputeFailureSite::Feature,
                OperationFailureCode::GeometryOperationFailed)}}));

  QString error;
  const ProjectPersistenceState v4{ProjectPersistenceFormat::V4};
  const ProjectPersistenceState v3{ProjectPersistenceFormat::V3};
  const QString validPath = directory.filePath(QStringLiteral("valid-v4.solidar"));
  Document valid = importedDocument(420001, 420002);
  CHECK(ProjectFile::saveDocument(validPath, valid, v4, &error));
  CHECK(jsonVersion(validPath) == 4);
  auto stagedValid = ProjectFile::stageLoad(validPath);
  CHECK(stagedValid.succeeded());
  CHECK(stagedValid.payloadKind == ProjectPayloadKind::ParametricPartV4);
  CHECK(stagedValid.persistenceState == v4);
  CHECK(stagedValid.document && stagedValid.document->bodies().size() == 1);

  auto unsupportedRoot = readRoot(validPath);
  unsupportedRoot[QStringLiteral("capabilities")] =
      QJsonArray{QStringLiteral("future-v9"),
                 QStringLiteral("persistent-topology-v2")};
  const QString unsupportedPath =
      directory.filePath(QStringLiteral("unsupported-v4.solidar"));
  writeRoot(unsupportedPath, unsupportedRoot);
  const auto unsupported = ProjectFile::stageLoad(unsupportedPath);
  CHECK(!unsupported.succeeded());
  CHECK(unsupported.status == ProjectLoadStatus::Unsupported);
  CHECK(unsupported.sourceVersion == 4);
  CHECK(!unsupported.document);

  auto invalidRoot = readRoot(validPath);
  invalidRoot.remove(QStringLiteral("model"));
  const QString invalidPath =
      directory.filePath(QStringLiteral("invalid-v4.solidar"));
  writeRoot(invalidPath, invalidRoot);
  const auto invalid = ProjectFile::stageLoad(invalidPath);
  CHECK(!invalid.succeeded());
  CHECK(invalid.status == ProjectLoadStatus::Invalid);
  CHECK(invalid.sourceVersion == 4);
  CHECK(!invalid.document);

  const QString missingPath =
      directory.filePath(QStringLiteral("allowed-missing-v4.solidar"));
  Document missing = topologyFailureDocument(false, false);
  CHECK(ProjectFile::saveDocument(missingPath, missing, v4, &error));
  auto stagedMissing = ProjectFile::stageLoad(missingPath);
  CHECK(stagedMissing.succeeded());
  CHECK(stagedMissing.document);
  CHECK(!stagedMissing.document->recomputeDetailed().success);
  CHECK(stagedMissing.document->recomputeDetailed().primaryFailures.size() == 1);
  CHECK(stagedMissing.document->recomputeDetailed().primaryFailures[0]
            .failure.code == OperationFailureCode::TopologyReferenceMissing);

  const QString mixedAllowedPath =
      directory.filePath(QStringLiteral("mixed-allowed-v4.solidar"));
  Document mixedAllowed = topologyFailureDocument(true, false);
  CHECK(ProjectFile::saveDocument(mixedAllowedPath, mixedAllowed, v4, &error));
  auto stagedMixedAllowed = ProjectFile::stageLoad(mixedAllowedPath);
  CHECK(stagedMixedAllowed.succeeded());
  CHECK(stagedMixedAllowed.document->recomputeDetailed().primaryFailures.size() ==
        2);

  const QString hardPath =
      directory.filePath(QStringLiteral("mixed-hard-v4.solidar"));
  Document hard = topologyFailureDocument(false, true);
  CHECK(ProjectFile::saveDocument(hardPath, hard, v4, &error));
  auto stagedHard = ProjectFile::stageLoad(hardPath);
  CHECK(!stagedHard.succeeded());
  CHECK(stagedHard.status == ProjectLoadStatus::Invalid);
  CHECK(!stagedHard.document);

  const QString v3Path = directory.filePath(QStringLiteral("routed-v3.solidar"));
  CHECK(ProjectFile::saveDocument(v3Path, valid, v3, &error));
  CHECK(jsonVersion(v3Path) == 3);
  auto stagedV3 = ProjectFile::stageLoad(v3Path);
  CHECK(stagedV3.succeeded());
  CHECK(stagedV3.persistenceState == v3);

  const ProjectApplicationService service;
  auto openedV4 = service.stageOpen(validPath);
  CHECK(openedV4.succeeded());
  CHECK(openedV4.staged->persistenceState == v4);
  auto openedV3 = service.stageOpen(v3Path);
  CHECK(openedV3.succeeded());
  CHECK(openedV3.staged->persistenceState == v3);
  const QString serviceV4Path =
      directory.filePath(QStringLiteral("service-v4.solidar"));
  CHECK(service.save(serviceV4Path, valid, v4).succeeded());
  CHECK(jsonVersion(serviceV4Path) == 4);

  // Detached staging must not reserve authored high IDs globally. Construct
  // the source under the same reservation pause used by decoders, then prove
  // that save+load does not move the next generated ID.
  Document cursorBefore;
  const BodyId beforeId = cursorBefore.addBody("cursor-before").id();
  Document highIds;
  Document rejectedHighIds;
  {
    solidar::detail::ScopedExplicitIdReservationPause pause;
    highIds = importedDocument(800000000, 800000001);
    rejectedHighIds = importedDocument(900000000, 900000001);
    rejectedHighIds.findBody(900000000)
        ->addFeature(std::make_unique<FilletFeature>(
            900000002, EdgeReference(900000000, 900000001, 0), 1000.0,
            "rejected high-id geometry"));
  }
  const QString highPath = directory.filePath(QStringLiteral("high-ids.solidar"));
  CHECK(ProjectFile::saveDocument(highPath, highIds, v4, &error));
  CHECK(ProjectFile::stageLoad(highPath).succeeded());
  auto stagedHighThroughService = service.stageOpen(highPath);
  CHECK(stagedHighThroughService.succeeded());
  CHECK(stagedHighThroughService.staged->rootDocument().findBody(800000000));
  const QString rejectedHighPath =
      directory.filePath(QStringLiteral("rejected-high-ids.solidar"));
  CHECK(ProjectFile::saveDocument(rejectedHighPath, rejectedHighIds, v4,
                                  &error));
  CHECK(!ProjectFile::stageLoad(rejectedHighPath).succeeded());
  const auto rejectedHighThroughService = service.stageOpen(rejectedHighPath);
  CHECK(!rejectedHighThroughService.succeeded());
  CHECK(!rejectedHighThroughService.staged);
  Document cursorAfter;
  CHECK(cursorAfter.addBody("cursor-after").id() == beforeId + 1);

  return EXIT_SUCCESS;
}

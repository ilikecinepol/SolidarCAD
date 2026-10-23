#include "TestAssertions.h"

#include "ui/application/ProjectApplicationService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <type_traits>

#include "model/ImportedShapeFeature.h"
#include "project/ProjectFile.h"

namespace {

solidar::Document boxDocument(const char* name = "Box") {
  solidar::Document document;
  auto& body = document.addBody(name);
  body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      name));
  static_cast<void>(document.recompute());
  return document;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(bytes) == bytes.size() && file.flush();
}

template <typename T>
concept HasLvalueProductRelease = requires(T& value) {
  value.releaseProductDocument();
};

template <typename T>
concept HasRvalueProductRelease = requires(T&& value) {
  std::move(value).releaseProductDocument();
};

template <typename T>
concept HasStagedRootRelease = requires(T&& value) {
  std::move(value).releaseRootDocument();
};

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-application-service-tests-XXXXXX")));
  CHECK(directory.isValid());

  const solidar::ProjectApplicationService service;
  static_assert(!std::is_default_constructible_v<
                solidar::StagedApplicationDocument>);
  static_assert(!std::is_copy_constructible_v<
                solidar::StagedApplicationDocument>);
  static_assert(!std::is_copy_assignable_v<
                solidar::StagedApplicationDocument>);
  static_assert(std::is_nothrow_move_constructible_v<
                solidar::StagedApplicationDocument>);
  static_assert(std::is_nothrow_move_assignable_v<
                solidar::StagedApplicationDocument>);
  static_assert(!HasLvalueProductRelease<solidar::StagedApplicationDocument>);
  static_assert(HasRvalueProductRelease<solidar::StagedApplicationDocument>);
  static_assert(!HasStagedRootRelease<solidar::StagedApplicationDocument>);
  static_assert(std::is_same_v<
                decltype(std::declval<solidar::StagedApplicationDocument&&>()
                             .releaseProductDocument()),
                solidar::ProductDocument>);
  static_assert(noexcept(std::declval<solidar::StagedApplicationDocument&&>()
                             .releaseProductDocument()));
  static_assert(noexcept(service.stageOpen(QString{})));
  static_assert(noexcept(service.save(QString{},
                                      std::declval<const solidar::Document&>())));
  static_assert(noexcept(service.save(
      QString{}, std::declval<const solidar::Document&>(),
      solidar::project::ProjectPersistenceState{})));
  static_assert(noexcept(service.upgradeProjectFormat(
      QString{}, std::declval<const solidar::Document&>(),
      solidar::project::ProjectPersistenceState{})));
  static_assert(noexcept(service.stageImportStep(
      QString{}, std::declval<const solidar::Document&>(), QString{})));

  // Rejected staging cannot mutate caller-owned committed state.
  auto committed = boxDocument("Committed");
  const auto committedBodyId = committed.bodies().front().id();
  const QString corruptPath = directory.filePath(QStringLiteral("corrupt.solidar"));
  CHECK(writeBytes(corruptPath, QByteArrayLiteral("{broken")));
  const auto corrupt = service.stageOpen(corruptPath);
  CHECK(!corrupt.succeeded());
  CHECK(corrupt.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!corrupt.staged);
  CHECK(committed.bodies().size() == 1);
  CHECK(committed.bodies().front().id() == committedBodyId);

  const QString unsupportedPath =
      directory.filePath(QStringLiteral("unsupported.solidar"));
  CHECK(writeBytes(
      unsupportedPath,
      QByteArrayLiteral(
          R"({"format":"solidar-project","version":99})")));
  const auto unsupported = service.stageOpen(unsupportedPath);
  CHECK(!unsupported.succeeded());
  CHECK(unsupported.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!unsupported.staged);

  const QString malformedV3Path =
      directory.filePath(QStringLiteral("malformed-v3.solidar"));
  CHECK(writeBytes(
      malformedV3Path,
      QByteArrayLiteral(
          R"({"format":"solidar-project","version":3,"constraintTypeEncoding":"stable-name-v1"})")));
  const auto malformedV3 = service.stageOpen(malformedV3Path);
  CHECK(!malformedV3.succeeded());
  CHECK(malformedV3.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!malformedV3.staged);

  const QString unknownSchemaV3Path =
      directory.filePath(QStringLiteral("unknown-schema-v3.solidar"));
  CHECK(writeBytes(
      unknownSchemaV3Path,
      QByteArrayLiteral(
          R"({"format":"solidar-project","version":3,"constraintTypeEncoding":"stable-name-v1","sketches":[{"schema":"future-sketch-v9"}]})")));
  const auto unknownSchemaV3 = service.stageOpen(unknownSchemaV3Path);
  CHECK(!unknownSchemaV3.succeeded());
  CHECK(unknownSchemaV3.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!unknownSchemaV3.staged);
  CHECK(committed.bodies().size() == 1);
  CHECK(committed.bodies().front().id() == committedBodyId);

  // Historical v1 becomes a complete staged Document and carries the legacy
  // extrusion presentation reference without exposing ProjectFile DTOs to UI.
  solidar::project::ProjectData legacy;
  legacy.box = {40.0, 30.0, 20.0};
  solidar::sketch::Sketch legacySketch;
  legacySketch.addRectangle({0.0, 0.0}, {12.0, 8.0});
  legacy.sketches.push_back(
      {std::move(legacySketch), QString::fromUtf8("Верхняя")});
  legacy.hasExtrusion = true;
  legacy.extrusionSourceSketch = 0;
  const QString v1Path = directory.filePath(QStringLiteral("historical-v1.solidar"));
  QString fixtureError;
  CHECK(solidar::project::ProjectFile::save(v1Path, legacy, &fixtureError));
  auto openedV1 = service.stageOpen(v1Path);
  CHECK(openedV1.succeeded());
  CHECK(openedV1.staged->rootDocument().sketches().size() == 1);
  CHECK(openedV1.staged->historicalLegacyExtrusionSourceSketchId ==
        openedV1.staged->rootDocument().sketches().front().id);
  CHECK(openedV1.staged->rootDocument()
            .sketches()
            .front()
            .placement.origin.z == 20.0);
  CHECK(openedV1.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V3});

  // Historical v2 is still accepted through the service boundary and staged
  // into the same transient root carrier as every other supported version.
  const QString v2Path =
      directory.filePath(QStringLiteral("hand-authored-v2.solidar"));
  const QJsonObject v2Root{
      {"format", QStringLiteral("solidar-project")},
      {"version", 2},
      {"name", QStringLiteral("hand-authored-v2")},
      {"createdAt", QStringLiteral("2026-10-18T00:00:00Z")},
      {"document", QJsonObject{{"widthMm", 60.0},
                                {"heightMm", 40.0},
                                {"extrusionMm", 25.0}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model", QJsonObject{{"sketches", QJsonArray{}},
                              {"bodies", QJsonArray{}},
                              {"referenceImages", QJsonArray{}}}}};
  CHECK(writeBytes(v2Path,
                   QJsonDocument(v2Root).toJson(QJsonDocument::Compact)));
  auto openedV2 = service.stageOpen(v2Path);
  CHECK(openedV2.succeeded());
  CHECK(openedV2.staged->rootDocument().bodies().empty());
  CHECK(openedV2.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V3});

  // Canonical v3 save/open round-trip keeps parametric document ownership in
  // the staged result until the caller explicitly commits it.
  const QString v3Path = directory.filePath(QStringLiteral("canonical-v3.solidar"));
  const auto saved = service.save(v3Path, committed);
  CHECK(saved.succeeded());
  auto openedV3 = service.stageOpen(v3Path);
  CHECK(openedV3.succeeded());
  CHECK(openedV3.staged->rootDocument().bodies().size() == 1);
  CHECK(openedV3.staged->rootDocument()
            .bodies()
            .front()
            .features()
            .size() == 1);
  CHECK(!openedV3.staged->historicalLegacyExtrusionSourceSketchId);
  CHECK(openedV3.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V3});
  const auto* const stagedDocument = &openedV3.staged->rootDocument();
  const auto* const stagedBody = &stagedDocument->bodies().front();
  const auto* const stagedFeature = stagedBody->features().front().get();
  const auto* const stagedShapeFeature =
      dynamic_cast<const solidar::ShapeFeature*>(stagedFeature);
  CHECK(stagedShapeFeature);
  const auto* const stagedShape = stagedShapeFeature->lastValidShape().get();
  std::string topologyError;
  const auto stagedTopology =
      stagedShapeFeature->lastValidTopologyIndex(&topologyError);
  CHECK(stagedTopology);
  CHECK(topologyError.empty());
  const auto* const stagedTopologyAddress = stagedTopology.get();
  const auto stagedRevision = stagedShapeFeature->shapeRevision();

  auto releasedV3 =
      std::move(*openedV3.staged).releaseProductDocument();
  CHECK(&releasedV3.rootDocument() == stagedDocument);
  CHECK(&releasedV3.rootDocument().bodies().front() == stagedBody);
  CHECK(releasedV3.rootDocument().bodies().front().features().front().get() ==
        stagedFeature);
  const auto* const releasedShapeFeature =
      dynamic_cast<const solidar::ShapeFeature*>(stagedFeature);
  CHECK(releasedShapeFeature);
  CHECK(releasedShapeFeature->lastValidShape().get() == stagedShape);
  CHECK(releasedShapeFeature->shapeRevision() == stagedRevision);
  CHECK(releasedShapeFeature->lastValidTopologyIndex().get() ==
        stagedTopologyAddress);

  const QString v4Path =
      directory.filePath(QStringLiteral("canonical-v4.solidar"));
  const solidar::project::ProjectPersistenceState v4{
      solidar::project::ProjectPersistenceFormat::V4};
  CHECK(service.save(v4Path, committed, v4).succeeded());
  auto openedV4 = service.stageOpen(v4Path);
  CHECK(openedV4.succeeded());
  CHECK(openedV4.staged->persistenceState == v4);

  // Explicit upgrade uses the existing atomic v4 writer. A V4 call is an
  // exact no-op, while a failed V3 upgrade never manufactures a destination.
  const QString upgradedPath =
      directory.filePath(QStringLiteral("explicit-upgrade.solidar"));
  CHECK(service.save(upgradedPath, committed).succeeded());
  CHECK(service.upgradeProjectFormat(
                    upgradedPath, committed,
                    {solidar::project::ProjectPersistenceFormat::V3})
            .succeeded());
  QFile upgradedFile(upgradedPath);
  CHECK(upgradedFile.open(QIODevice::ReadOnly));
  const auto upgradedRoot =
      QJsonDocument::fromJson(upgradedFile.readAll()).object();
  CHECK(upgradedRoot.value(QStringLiteral("version")).toInt() == 4);

  const QString noOpPath =
      directory.filePath(QStringLiteral("already-v4-no-op.solidar"));
  CHECK(service.upgradeProjectFormat(noOpPath, solidar::Document{}, v4)
            .succeeded());
  CHECK(!QFile::exists(noOpPath));

  const QString missingParent =
      directory.filePath(QStringLiteral("missing/upgrade.solidar"));
  const auto failedUpgrade = service.upgradeProjectFormat(
      missingParent, committed,
      {solidar::project::ProjectPersistenceFormat::V3});
  CHECK(!failedUpgrade.succeeded());
  CHECK(failedUpgrade.code == solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(!QFile::exists(missingParent));

  // STEP import is also staged against a copy. Both successful and failed
  // translations leave the supplied current Document unchanged.
  const QString stepPath = directory.filePath(QStringLiteral("source.step"));
  CHECK(service.exportStep(stepPath, committed).succeeded());
  solidar::Document importBase;
  importBase.addBody("Existing");
  const auto existingId = importBase.bodies().front().id();
  auto imported =
      service.stageImportStep(stepPath, importBase, QStringLiteral("Imported"));
  CHECK(imported.succeeded());
  CHECK(importBase.bodies().size() == 1);
  CHECK(importBase.bodies().front().id() == existingId);
  CHECK(imported.staged->rootDocument().bodies().size() == 2);
  CHECK(!imported.staged->persistenceState);

  const auto failedImport = service.stageImportStep(
      directory.filePath(QStringLiteral("missing.step")), importBase,
      QStringLiteral("Missing"));
  CHECK(!failedImport.succeeded());
  CHECK(failedImport.status.code ==
        solidar::ProjectApplicationErrorCode::StepImportFailed);
  CHECK(!failedImport.staged);
  CHECK(importBase.bodies().size() == 1);
  CHECK(importBase.bodies().front().id() == existingId);

  const auto preflight = service.preflightStlExport(committed);
  CHECK(preflight.succeeded());
  const QString stlPath = directory.filePath(QStringLiteral("source.stl"));
  CHECK(service.exportStl(stlPath, committed).succeeded());
  CHECK(QFile::exists(stlPath));

  const auto emptyPreflight =
      service.preflightStlExport(solidar::Document{});
  CHECK(!emptyPreflight.succeeded());
  CHECK(emptyPreflight.code ==
        solidar::ProjectApplicationErrorCode::NoExportableShapes);

  return EXIT_SUCCESS;
}

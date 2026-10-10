#include "TestAssertions.h"

#include "ui/application/ProjectApplicationService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-application-service-tests-XXXXXX")));
  CHECK(directory.isValid());

  const solidar::ProjectApplicationService service;
  static_assert(noexcept(service.stageOpen(QString{})));
  static_assert(noexcept(service.save(QString{},
                                      std::declval<const solidar::Document&>())));
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
  CHECK(openedV1.staged->document.sketches().size() == 1);
  CHECK(openedV1.staged->historicalLegacyExtrusionSourceSketchId ==
        openedV1.staged->document.sketches().front().id);
  CHECK(openedV1.staged->document.sketches().front().placement.origin.z ==
        20.0);

  // Canonical v2 save/open round-trip keeps parametric document ownership in
  // the staged result until the caller explicitly commits it.
  const QString v2Path = directory.filePath(QStringLiteral("canonical-v2.solidar"));
  const auto saved = service.save(v2Path, committed);
  CHECK(saved.succeeded());
  auto openedV2 = service.stageOpen(v2Path);
  CHECK(openedV2.succeeded());
  CHECK(openedV2.staged->document.bodies().size() == 1);
  CHECK(openedV2.staged->document.bodies().front().features().size() == 1);
  CHECK(!openedV2.staged->historicalLegacyExtrusionSourceSketchId);

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
  CHECK(imported.staged->document.bodies().size() == 2);

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

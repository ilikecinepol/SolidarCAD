#include "TestAssertions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdlib>
#include <memory>

#include "model/ImportedShapeFeature.h"
#include "project/ProjectFile.h"
#include "ui/application/ProjectApplicationService.h"

using namespace solidar;

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary(QDir::current().filePath(
      QStringLiteral("assembly-insert-service-XXXXXX")));
  CHECK(temporary.isValid());
  ProjectApplicationService service;

  auto fresh = service.stageNewAssembly();
  CHECK(fresh.succeeded());
  CHECK(fresh.staged->rootDefinitionKind() == DefinitionKind::Assembly);
  CHECK(fresh.staged->productDocument().definitions().size() == 1);
  CHECK(fresh.staged->productDocument().rootAssemblyDefinition());
  CHECK(fresh.staged->productDocument().rootAssemblyDefinition()->size() == 0);
  // A fresh assembly is an unsaved V7 workspace: persistence is writable,
  // but project identity is assigned only by the first atomic Save.
  CHECK(!fresh.staged->productDocument().identity());
  CHECK(!fresh.staged->historicalLegacyExtrusionSourceSketchId);
  CHECK(fresh.staged->linkedPartRuntimeStates.empty());
  CHECK(fresh.staged->persistenceState);
  CHECK(fresh.staged->persistenceState->format ==
        project::ProjectPersistenceFormat::V7);

  const QString partPath = temporary.filePath(QStringLiteral("part.solidar"));
  QString error;
  Document source;
  auto& sourceBody = source.addBody("Embedded part");
  sourceBody.addFeature(std::make_unique<ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      "Embedded box"));
  CHECK(source.recompute());
  CHECK(project::ProjectFile::saveDocument(
      partPath, source, {project::ProjectPersistenceFormat::V4}, &error));
  const auto before = QFileInfo(partPath).size();
  auto stagedPart = service.stageEmbeddedPart(partPath);
  CHECK(stagedPart.succeeded());
  CHECK(stagedPart.staged->document);
  CHECK(stagedPart.staged->document->bodies().size() == 1);
  CHECK(!stagedPart.staged->authoredDocument.isEmpty());
  CHECK(QFileInfo(partPath).size() == before);

  auto nested = service.stageEmbeddedPart(
      QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH));
  CHECK(!nested.succeeded());
  CHECK(nested.status.code == ProjectApplicationErrorCode::UnsupportedProject);

  const QString corruptPath = temporary.filePath(QStringLiteral("bad.solidar"));
  QFile corrupt(corruptPath);
  CHECK(corrupt.open(QIODevice::WriteOnly));
  CHECK(corrupt.write("not-json") == 8);
  corrupt.close();
  auto invalid = service.stageEmbeddedPart(corruptPath);
  CHECK(!invalid.succeeded());
  CHECK(!invalid.staged);
  return EXIT_SUCCESS;
}

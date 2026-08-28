#include "project/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>
#include <cstdio>
#include <memory>

#include "TestGeometryUtils.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/PocketFeature.h"

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  // Keep test artifacts under CTest's writable build directory. This also
  // avoids platform policies that deny atomic QSaveFile replacement in the
  // user's global temporary directory.
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("project-file-tests-XXXXXX")));
  if (!directory.isValid()) {
    std::fprintf(stderr, "QTemporaryDir failed: %s\n",
                 directory.errorString().toUtf8().constData());
    return 1;
  }
  const QString path = directory.filePath("sample.solidar");

  QString error;
  if (!solidar::project::ProjectFile::create(path, &error)) {
    std::fprintf(stderr, "ProjectFile::create failed for %s: %s\n",
                 path.toUtf8().constData(), error.toUtf8().constData());
    return 1;
  }
  assert(QFile::exists(path));
  assert(solidar::project::ProjectFile::validate(path, &error));

  // Project v2 round-trip preserves IDs, parameters, face support and the
  // complete editable feature chain, then rebuilds B-Rep from history.
  solidar::Document source;
  auto& baseSketch = source.addSketch("Base");
  const auto baseSketchId = baseSketch.id;
  baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& body = source.addBody("Body");
  const auto bodyId = body.id();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(
      baseSketchId, 50.0, "Extrude");
  auto* extrudePtr = extrude.get();
  body.addFeature(std::move(extrude));
  assert(source.recompute());
  const auto topFace =
      solidar::test::topPlanarFace(*body.resultShape(), 50.0);
  assert(topFace);
  auto& pocketSketch = source.addSketch("Pocket sketch");
  const auto pocketSketchId = pocketSketch.id;
  assert(source.attachSketchToFace(
      pocketSketchId, {bodyId, extrudePtr->id(), *topFace}));
  pocketSketch.geometry.addRectangle({-10.0, -5.0}, {10.0, 5.0});
  auto pocket = std::make_unique<solidar::PocketFeature>(
      pocketSketchId, 10.0, "Pocket");
  auto* pocketPtr = pocket.get();
  body.addFeature(std::move(pocket));
  assert(source.recompute());
  std::size_t edgeIndex = 0;
  for (;; ++edgeIndex) {
    std::string buildError;
    if (solidar::buildFilletShape(*body.resultShape(), {edgeIndex}, 2.0,
                                  &buildError))
      break;
    assert(edgeIndex < 64);
  }
  auto fillet = std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{bodyId, pocketPtr->id(), edgeIndex}, 2.0,
      "Fillet");
  const auto filletId = fillet->id();
  body.addFeature(std::move(fillet));
  assert(source.recompute());
  const double sourceVolume =
      solidar::test::volumeOf(*body.resultShape());

  const QString modelPath = directory.filePath("parametric-v2.solidar");
  assert(solidar::project::ProjectFile::saveDocument(modelPath, source,
                                                      &error));
  solidar::Document restored;
  assert(solidar::project::ProjectFile::loadDocument(modelPath, &restored,
                                                      &error));
  const auto* restoredBody = restored.findBody(bodyId);
  assert(restoredBody && restoredBody->features().size() == 3);
  assert(restored.findSketch(baseSketchId));
  const auto* restoredPocketSketch = restored.findSketch(pocketSketchId);
  assert(restoredPocketSketch);
  assert(restoredPocketSketch->support.type ==
         solidar::SketchSupportType::Face);
  assert(restoredPocketSketch->support.face.featureId == extrudePtr->id());
  assert(restoredBody->features().back()->id() == filletId);
  assert(restoredBody->features().back()->typeName() == "Fillet");
  assert(solidar::test::near(
      solidar::test::volumeOf(*restoredBody->resultShape()), sourceVolume,
      1e-4));

  QFile broken(directory.filePath("broken.solidar"));
  assert(broken.open(QIODevice::WriteOnly));
  broken.write("not json");
  broken.close();
  assert(!solidar::project::ProjectFile::validate(broken.fileName(), &error));
  return 0;
}

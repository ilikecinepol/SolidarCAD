#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <memory>
#include "model/CircularPatternFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "project/ProjectFile.h"
#include "TestGeometryUtils.h"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv); solidar::Document document;
  auto& sketch = document.addSketch(); sketch.geometry.addRectangle({10,0},{20,5});
  auto& body = document.addBody();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  const auto extrudeId = extrude->id(); body.addFeature(std::move(extrude));
  auto move = std::make_unique<solidar::MoveFeature>(
      extrudeId, solidar::Vector3d{5.0, -6.0, 7.0});
  const auto moveId = move->id(); body.addFeature(std::move(move));
  auto mirror = std::make_unique<solidar::MirrorFeature>(moveId, solidar::MirrorPlane::YZ);
  const auto mirrorId = mirror->id(); body.addFeature(std::move(mirror));
  auto linear = std::make_unique<solidar::LinearPatternFeature>(mirrorId, solidar::PrincipalAxis::Y, 3, 40.0);
  const auto linearId = linear->id(); body.addFeature(std::move(linear));
  auto circular = std::make_unique<solidar::CircularPatternFeature>(linearId, solidar::PrincipalAxis::Z, 4, 360.0);
  const auto circularId = circular->id(); body.addFeature(std::move(circular));
  const auto sourceBodyId = body.id();
  auto& copies = document.addBody("Separate pattern");
  copies.addFeature(std::make_unique<solidar::LinearPatternFeature>(
      sourceBodyId, circularId, solidar::PrincipalAxis::X, 2, 100.0,
      solidar::PatternOperation::NewBody));
  CHECK(document.recompute());
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("pattern-persistence-tests-XXXXXX")));
  CHECK(directory.isValid());
  QString error; const QString path = directory.filePath("patterns.solidar");
  CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document restored; CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
  CHECK(restored.recompute());
  CHECK(restored.bodies().size() == 2);
  const auto* loaded = restored.findBody(sourceBodyId); CHECK(loaded);
  CHECK(loaded->features().size() == 5);
  const auto* loadedMove = dynamic_cast<const solidar::MoveFeature*>(
      loaded->features()[1].get());
  CHECK(loadedMove && loadedMove->id() == moveId);
  CHECK(solidar::test::near(loadedMove->offsetMm().x, 5.0));
  CHECK(solidar::test::near(loadedMove->offsetMm().y, -6.0));
  CHECK(solidar::test::near(loadedMove->offsetMm().z, 7.0));
  CHECK(loaded->features()[2]->id() == mirrorId && loaded->features()[3]->id() == linearId && loaded->features()[4]->id() == circularId);
  CHECK(loaded->features()[4]->isValid() && loaded->resultShape());
  const auto* loadedCopies = restored.activeBody();
  CHECK(loadedCopies && loadedCopies->features().size() == 1);
  const auto* loadedPattern = dynamic_cast<const solidar::LinearPatternFeature*>(
      loadedCopies->features().front().get());
  CHECK(loadedPattern);
  CHECK(loadedPattern->operation() == solidar::PatternOperation::NewBody);
  CHECK(loadedPattern->sourceBodyId() == sourceBodyId);
  CHECK(solidar::test::solidCount(*loadedCopies->resultShape()) ==
        solidar::test::solidCount(*loaded->resultShape()));
  return EXIT_SUCCESS;
}

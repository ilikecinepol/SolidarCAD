#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <memory>
#include "model/CircularPatternFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "project/ProjectFile.h"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv); solidar::Document document;
  auto& sketch = document.addSketch(); sketch.geometry.addRectangle({10,0},{20,5});
  auto& body = document.addBody();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  const auto extrudeId = extrude->id(); body.addFeature(std::move(extrude));
  auto mirror = std::make_unique<solidar::MirrorFeature>(extrudeId, solidar::MirrorPlane::YZ);
  const auto mirrorId = mirror->id(); body.addFeature(std::move(mirror));
  auto linear = std::make_unique<solidar::LinearPatternFeature>(mirrorId, solidar::PrincipalAxis::Y, 3, 40.0);
  const auto linearId = linear->id(); body.addFeature(std::move(linear));
  auto circular = std::make_unique<solidar::CircularPatternFeature>(linearId, solidar::PrincipalAxis::Z, 4, 360.0);
  const auto circularId = circular->id(); body.addFeature(std::move(circular));
  CHECK(document.recompute()); QTemporaryDir directory; CHECK(directory.isValid());
  QString error; const QString path = directory.filePath("patterns.solidar");
  CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document restored; CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
  CHECK(restored.recompute()); const auto* loaded = restored.activeBody(); CHECK(loaded);
  CHECK(loaded->features().size() == 4);
  CHECK(loaded->features()[1]->id() == mirrorId && loaded->features()[2]->id() == linearId && loaded->features()[3]->id() == circularId);
  CHECK(loaded->features()[3]->isValid() && loaded->resultShape()); return EXIT_SUCCESS;
}

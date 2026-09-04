#include <cstdlib>
#include <iostream>
#include <memory>
#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/MirrorFeature.h"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)
int main() {
  solidar::Document document;
  auto& sketch = document.addSketch("profile");
  sketch.geometry.addRectangle({10, 0}, {20, 10});
  auto& body = document.addBody();
  auto extrusion = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  const auto extrusionId = extrusion->id();
  body.addFeature(std::move(extrusion));
  auto mirror = std::make_unique<solidar::MirrorFeature>(extrusionId, solidar::MirrorPlane::YZ);
  auto* mirrorPtr = mirror.get(); const auto mirrorId = mirror->id();
  body.addFeature(std::move(mirror));
  CHECK(document.recompute());
  CHECK(solidar::test::solidCount(*body.resultShape()) == 2);
  CHECK(solidar::test::near(solidar::test::volumeOf(*body.resultShape()), 1000.0));
  const auto bounds = solidar::test::boundsOf(*body.resultShape());
  CHECK(solidar::test::near(bounds.minX, -20.0) && solidar::test::near(bounds.maxX, 20.0));
  mirrorPtr->setPlane(solidar::MirrorPlane::XZ); body.markDirtyFrom(1);
  CHECK(document.recompute() && mirrorPtr->id() == mirrorId);
  solidar::sketch::Sketch wider; wider.addRectangle({10, 0}, {25, 10});
  CHECK(document.replaceSketchGeometry(sketch.id, wider)); CHECK(document.recompute());
  CHECK(mirrorPtr->id() == mirrorId);
  return EXIT_SUCCESS;
}

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include "TestGeometryUtils.h"
#include "model/ChamferBuilder.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/LinearPatternFeature.h"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)
int main() {
  solidar::Document document; auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({0,0},{10,10}); auto& body = document.addBody();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 10.0);
  const auto extrudeId = extrude->id(); body.addFeature(std::move(extrude));
  body.addFeature(std::make_unique<solidar::LinearPatternFeature>(
      extrudeId, solidar::PrincipalAxis::X, 3, 25.0));
  CHECK(document.recompute()); const auto source = body.resultShape(); CHECK(source);
  std::shared_ptr<TopoDS_Shape> fillet; std::string error;
  for (std::size_t edge = 12; edge < 36 && !fillet; ++edge)
    fillet = solidar::buildFilletShape(*source, {edge}, 1.0, &error);
  CHECK(fillet); CHECK(solidar::test::solidCount(*fillet) == 3);
  CHECK(error != "Fillet result contains multiple solids");
  std::shared_ptr<TopoDS_Shape> chamfer; error.clear();
  for (std::size_t edge = 12; edge < 36 && !chamfer; ++edge)
    chamfer = solidar::buildChamferShape(*source, {edge}, 1.0, &error);
  CHECK(chamfer); CHECK(solidar::test::solidCount(*chamfer) == 3);
  CHECK(error != "Chamfer result contains multiple solids"); return EXIT_SUCCESS;
}

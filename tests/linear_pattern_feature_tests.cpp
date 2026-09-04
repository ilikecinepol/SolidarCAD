#include <cstdlib>
#include <iostream>
#include <memory>
#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/LinearPatternFeature.h"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)
int main() {
  solidar::Document document; auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({0,0},{10,10}); auto& body = document.addBody();
  auto base = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  const auto baseId = base->id(); body.addFeature(std::move(base));
  auto pattern = std::make_unique<solidar::LinearPatternFeature>(baseId, solidar::PrincipalAxis::X, 3, 30.0);
  auto* ptr = pattern.get(); const auto id = ptr->id(); body.addFeature(std::move(pattern));
  CHECK(document.recompute()); CHECK(solidar::test::solidCount(*body.resultShape()) == 3);
  CHECK(solidar::test::near(solidar::test::boundsOf(*body.resultShape()).x(), 70.0));
  ptr->setCount(5); body.markDirtyFrom(1); CHECK(document.recompute());
  CHECK(ptr->id() == id && solidar::test::solidCount(*body.resultShape()) == 5);
  ptr->setSpacingMm(0); body.markDirtyFrom(1); CHECK(!document.recompute());
  CHECK(ptr->isFailed() && !ptr->hasShape()); ptr->setSpacingMm(20); body.markDirtyFrom(1);
  CHECK(document.recompute() && ptr->id() == id); return EXIT_SUCCESS;
}

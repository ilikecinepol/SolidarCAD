#include <cstdlib>
#include <iostream>
#include <memory>
#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/LinearPatternToolSession.h"
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

  solidar::LinearPatternToolSession session;
  session.begin(20.0, 4);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Body);
  session.setBody(body.id(), id, body.resultShape());
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingReference);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Axis);
  session.setDirection(solidar::PrincipalAxis::Y);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  CHECK(solidar::test::solidCount(*session.previewShape()) == 12);
  CHECK(session.parameters().size() == 2);
  const auto manipulator = session.manipulator();
  CHECK(manipulator);
  CHECK(solidar::test::near(manipulator->direction.x, 0.0));
  CHECK(solidar::test::near(manipulator->direction.y, 1.0));
  CHECK(solidar::test::near(manipulator->valueMm, 20.0));
  CHECK(body.features().size() == 2);
  session.setSpacingMm(15.0);
  session.setCount(2);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(solidar::test::solidCount(*session.previewShape()) == 6);
  session.cancel();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);

  ptr->setCount(5); body.markDirtyFrom(1); CHECK(document.recompute());
  CHECK(ptr->id() == id && solidar::test::solidCount(*body.resultShape()) == 5);
  ptr->setSpacingMm(0); body.markDirtyFrom(1); CHECK(!document.recompute());
  CHECK(ptr->isFailed() && !ptr->hasShape()); ptr->setSpacingMm(20); body.markDirtyFrom(1);
  CHECK(document.recompute() && ptr->id() == id); return EXIT_SUCCESS;
}

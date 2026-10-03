#include <cstdlib>
#include <iostream>
#include <memory>
#include "TestGeometryUtils.h"
#include "model/CircularPatternFeature.h"
#include "model/CircularPatternToolSession.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)
int main() {
  solidar::Document document; auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({10,0},{20,5}); auto& body = document.addBody();
  auto base = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  const auto baseId = base->id(); body.addFeature(std::move(base));
  auto pattern = std::make_unique<solidar::CircularPatternFeature>(baseId, solidar::PrincipalAxis::Z, 4, 360.0);
  auto* ptr = pattern.get(); const auto id = ptr->id(); body.addFeature(std::move(pattern));
  CHECK(document.recompute()); CHECK(solidar::test::solidCount(*body.resultShape()) == 4);
  CHECK(solidar::test::near(solidar::test::volumeOf(*body.resultShape()), 1000.0));
  const auto full = solidar::test::boundsOf(*body.resultShape());
  CHECK(solidar::test::near(full.x(), 40.0) && solidar::test::near(full.y(), 40.0));

  solidar::CircularPatternToolSession session;
  session.begin(180.0, 3);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Body);
  session.setBody(body.id(), id, body.resultShape());
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingReference);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Axis);
  session.setAxis(solidar::PrincipalAxis::Y);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  CHECK(solidar::test::solidCount(*session.previewShape()) == 12);
  CHECK(session.parameters().size() == 2);
  const auto manipulator = session.manipulator();
  CHECK(manipulator);
  CHECK(solidar::test::near(manipulator->axis.x, 0.0));
  CHECK(solidar::test::near(manipulator->axis.y, 1.0));
  CHECK(solidar::test::near(manipulator->axis.z, 0.0));
  CHECK(solidar::test::near(manipulator->angleDeg, 180.0));
  CHECK(body.features().size() == 2);
  session.setAngleDeg(90.0);
  session.setCount(2);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(solidar::test::solidCount(*session.previewShape()) == 8);
  session.cancel();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);

  ptr->setCount(3); ptr->setAngleDeg(90); body.markDirtyFrom(1); CHECK(document.recompute());
  CHECK(ptr->id() == id && solidar::test::solidCount(*body.resultShape()) == 3);
  ptr->setAngleDeg(0); body.markDirtyFrom(1); CHECK(!document.recompute() && !ptr->hasShape());
  ptr->setAngleDeg(180); body.markDirtyFrom(1); CHECK(document.recompute()); return EXIT_SUCCESS;
}

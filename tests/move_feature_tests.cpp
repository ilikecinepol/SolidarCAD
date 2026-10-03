#include <limits>
#include <memory>

#include <cstdlib>
#include <iostream>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/MoveFeature.h"
#include "model/MoveToolSession.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main() {
  solidar::Document document;
  auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& body = document.addBody();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  const auto extrudeId = extrude->id();
  body.addFeature(std::move(extrude));
  auto move = std::make_unique<solidar::MoveFeature>(
      extrudeId, solidar::Vector3d{12.0, -4.0, 7.0});
  auto* movePtr = move.get();
  const auto moveId = move->id();
  body.addFeature(std::move(move));

  CHECK(document.recompute());
  const auto bounds = solidar::test::boundsOf(*body.resultShape());
  CHECK(solidar::test::near(bounds.minX, 12.0));
  CHECK(solidar::test::near(bounds.minY, -4.0));
  CHECK(solidar::test::near(bounds.minZ, 7.0));
  CHECK(solidar::test::near(bounds.x(), 10.0));
  CHECK(solidar::test::near(bounds.y(), 10.0));
  CHECK(solidar::test::near(bounds.z(), 5.0));

  movePtr->setOffsetMm({-3.0, 8.0, 2.0});
  body.markDirtyFrom(1);
  CHECK(document.recompute());
  CHECK(movePtr->id() == moveId);
  const auto editedBounds = solidar::test::boundsOf(*body.resultShape());
  CHECK(solidar::test::near(editedBounds.minX, -3.0));
  CHECK(solidar::test::near(editedBounds.minY, 8.0));
  CHECK(solidar::test::near(editedBounds.minZ, 2.0));

  std::string error;
  CHECK(!solidar::buildMovedShape(
      *body.features().front()->shape(),
      {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, &error));
  CHECK(!error.empty());

  solidar::MoveToolSession session;
  session.begin();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  CHECK(session.selectionRequirement());
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Body);
  const auto source = body.features().front()->shape();
  session.setBody(body.id(), extrudeId, source);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  CHECK(session.parameters().size() == 3);
  CHECK(session.manipulator());

  session.setOffsetComponent(0, 20.0);
  session.setOffsetComponent(1, -15.0);
  session.setOffsetComponent(2, 30.0);
  const auto sessionBounds =
      solidar::test::boundsOf(*session.previewShape());
  CHECK(solidar::test::near(sessionBounds.minX, 20.0));
  CHECK(solidar::test::near(sessionBounds.minY, -15.0));
  CHECK(solidar::test::near(sessionBounds.minZ, 30.0));
  const auto sourceBounds = solidar::test::boundsOf(*source);
  CHECK(solidar::test::near(sourceBounds.minX, 0.0));
  CHECK(solidar::test::near(sourceBounds.minY, 0.0));
  CHECK(solidar::test::near(sourceBounds.minZ, 0.0));

  const auto manipulator = session.manipulator();
  CHECK(manipulator);
  CHECK(solidar::test::near(manipulator->offsetMm.x, 20.0));
  CHECK(solidar::test::near(manipulator->offsetMm.y, -15.0));
  CHECK(solidar::test::near(manipulator->offsetMm.z, 30.0));
  session.cancel();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);
  CHECK(!session.previewShape());
  return EXIT_SUCCESS;
}

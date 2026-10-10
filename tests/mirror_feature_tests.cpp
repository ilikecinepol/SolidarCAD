#include "TestAssertions.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/MirrorFeature.h"
#include "model/MirrorToolSession.h"
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

  // The interactive tool is a two-stage transaction: Body, then Plane. The
  // preview is generated without mutating Document history.
  solidar::MirrorToolSession session;
  session.begin();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  CHECK(session.selectionStage() ==
        solidar::ToolSelectionStage::SelectingInput);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Body);
  session.setBody(body.id(), extrusionId, body.features().front()->shape());
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingReference);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Plane);
  session.setPlane(solidar::MirrorPlane::YZ);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  CHECK(solidar::test::solidCount(*session.previewShape()) == 2);
  CHECK(solidar::test::near(solidar::test::volumeOf(*session.previewShape()),
                            1000.0));
  CHECK(body.features().size() == 2);
  session.clearPlane();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingReference);
  CHECK(!session.previewShape());
  session.cancel();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);

  mirrorPtr->setPlane(solidar::MirrorPlane::XZ); body.markDirtyFrom(1);
  CHECK(document.recompute() && mirrorPtr->id() == mirrorId);
  std::string invalidPlaneError;
  CHECK(!solidar::buildMirrorShape(
      *body.features().front()->shape(),
      static_cast<solidar::MirrorPlane>(99), &invalidPlaneError));
  session.begin();
  const auto sourceBeforeInvalidPreview = body.features().front()->shape();
  session.setBody(body.id(), extrusionId, sourceBeforeInvalidPreview);
  session.setPlane(static_cast<solidar::MirrorPlane>(99));
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!session.previewShape());
  CHECK(body.features().front()->shape() == sourceBeforeInvalidPreview);
  solidar::sketch::Sketch wider; wider.addRectangle({10, 0}, {25, 10});
  CHECK(document.replaceSketchGeometry(sketch.id, wider)); CHECK(document.recompute());
  CHECK(mirrorPtr->id() == mirrorId);
  return EXIT_SUCCESS;
}

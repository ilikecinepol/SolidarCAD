#include <TopoDS_Shape.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>

#include "model/Document.h"
#include "model/RevolveFeature.h"
#include "model/RevolveToolSession.h"

namespace {
std::unique_ptr<solidar::RevolveFeature> feature(
    solidar::SketchId id, solidar::AxisReferenceType type,
    double angle = 360.0, bool reversed = false,
    solidar::sketch::GeometryId line = solidar::sketch::kInvalidGeometryId) {
  return std::make_unique<solidar::RevolveFeature>(
      id, solidar::AxisReference{type, id, line}, angle, "Revolve 1",
      solidar::ExtrudeOperation::NewBody, reversed);
}
}

int main() {
  // A tool starts incomplete, previews without changing history, synchronizes
  // panel/manipulator values, and cancel leaves the Document untouched.
  solidar::Document sessionDocument;
  auto& sessionSketch = sessionDocument.addSketch("Session profile");
  sessionSketch.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  const auto featureCountBefore = sessionDocument.bodies().size();
  solidar::RevolveToolSession session;
  session.begin(sessionDocument, solidar::kInvalidBodyId,
                solidar::kInvalidFeatureId);
  assert(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  assert(session.selectionRequirement()->type == solidar::SelectionType::Sketch);
  assert(!session.previewShape());
  session.setProfile(sessionSketch.id);
  assert(session.lifecycle() == solidar::ToolLifecycle::SelectingReference);
  assert(session.selectionRequirement()->type == solidar::SelectionType::Axis);
  session.setAxis({solidar::AxisReferenceType::SketchHorizontalAxis,
                   sessionSketch.id, solidar::sketch::kInvalidGeometryId});
  assert(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  assert(session.previewShape());
  session.setAngleFromPanel(180.0);
  assert(session.angleDeg() == 180.0 && session.previewShape());
  session.setAngleFromManipulator(90.0);
  assert(session.angleDeg() == 90.0 && session.manipulator());
  assert(std::get<double>(session.parameters().front().value) == 90.0);
  assert(sessionDocument.bodies().size() == featureCountBefore);
  session.cancel();
  assert(session.lifecycle() == solidar::ToolLifecycle::Inactive);
  assert(sessionDocument.bodies().size() == featureCountBefore);

  for (const double angle : {360.0, 180.0}) {
    solidar::Document document;
    auto& sketch = document.addSketch();
    sketch.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    auto& body = document.addBody();
    body.addFeature(feature(sketch.id,
                            solidar::AxisReferenceType::SketchHorizontalAxis,
                            angle));
    assert(document.recompute());
    assert(body.activeFeature()->isValid());
    assert(body.resultShape() && !body.resultShape()->IsNull());
  }

  solidar::Document editable;
  auto& profile = editable.addSketch();
  profile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  auto& body = editable.addBody();
  auto revolve = feature(profile.id,
      solidar::AxisReferenceType::SketchHorizontalAxis, 270.0, true);
  auto* ptr = revolve.get();
  body.addFeature(std::move(revolve));
  assert(editable.recompute());
  ptr->setAngleDeg(180.0);
  assert(ptr->isDirty() && editable.recompute() && ptr->isValid());
  auto changed = profile.geometry;
  changed.clear();
  changed.addRectangle({12.0, 5.0}, {35.0, 18.0});
  assert(editable.replaceSketchGeometry(profile.id, std::move(changed)));
  assert(ptr->isDirty() && editable.recompute());

  for (const double bad : {0.0, -1.0, 361.0,
                           std::numeric_limits<double>::infinity()}) {
    ptr->setAngleDeg(bad);
    assert(!editable.recompute());
    assert(ptr->isFailed() && !ptr->hasShape());
  }
  ptr->setAngleDeg(360.0);
  assert(editable.recompute());

  solidar::Document vertical;
  auto& verticalProfile = vertical.addSketch();
  verticalProfile.geometry.addRectangle({5.0, 10.0}, {15.0, 30.0});
  auto& verticalBody = vertical.addBody();
  verticalBody.addFeature(feature(verticalProfile.id,
      solidar::AxisReferenceType::SketchVerticalAxis));
  assert(vertical.recompute());

  solidar::Document lineAxis;
  auto& lineProfile = lineAxis.addSketch();
  lineProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  lineProfile.geometry.addLine({0.0, 0.0}, {40.0, 0.0});
  const auto axisLineId = lineProfile.geometry.lineId(4);
  lineProfile.geometry.setElementDashed(
      lineProfile.geometry.lines()[4].elementId, true);
  auto& lineBody = lineAxis.addBody();
  lineBody.addFeature(feature(lineProfile.id,
      solidar::AxisReferenceType::SketchLine, 360.0, false, axisLineId));
  assert(lineAxis.recompute());

  // Any persistent straight Sketch line can replace the axis without losing
  // the other authoritative session parameters.
  lineProfile.geometry.addLine({0.0, -20.0}, {40.0, -20.0});
  const auto secondAxisLineId = lineProfile.geometry.lineId(
      lineProfile.geometry.lines().size() - 1);
  lineProfile.geometry.setElementDashed(
      lineProfile.geometry.lines().back().elementId, true);
  solidar::RevolveToolSession arbitraryAxisSession;
  arbitraryAxisSession.begin(lineAxis, lineBody.id(),
                             lineBody.activeFeature()->id());
  arbitraryAxisSession.setProfile(lineProfile.id);
  arbitraryAxisSession.setAxis({solidar::AxisReferenceType::SketchLine,
                                lineProfile.id, axisLineId});
  arbitraryAxisSession.setAngleFromPanel(135.0);
  arbitraryAxisSession.setOperation(solidar::ExtrudeOperation::NewBody);
  arbitraryAxisSession.setAxis({solidar::AxisReferenceType::SketchLine,
                                lineProfile.id, secondAxisLineId});
  assert(arbitraryAxisSession.axis()->lineId == secondAxisLineId);
  assert(arbitraryAxisSession.profileSketchId() == lineProfile.id);
  assert(arbitraryAxisSession.angleDeg() == 135.0);
  assert(arbitraryAxisSession.operation() == solidar::ExtrudeOperation::NewBody);

  solidar::Document globalAxis;
  auto& globalProfile = globalAxis.addSketch();
  globalProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  auto& globalBody = globalAxis.addBody();
  globalBody.addFeature(feature(globalProfile.id,
                                solidar::AxisReferenceType::GlobalX));
  assert(globalAxis.recompute());

  solidar::Document invalid;
  auto& invalidBody = invalid.addBody();
  invalidBody.addFeature(feature(9999,
      solidar::AxisReferenceType::SketchHorizontalAxis));
  assert(!invalid.recompute());

  solidar::Document open;
  auto& openSketch = open.addSketch();
  openSketch.geometry.addLine({1.0, 1.0}, {2.0, 1.0});
  openSketch.geometry.addLine({2.0, 1.0}, {2.0, 2.0});
  auto& openBody = open.addBody();
  openBody.addFeature(feature(openSketch.id,
      solidar::AxisReferenceType::SketchHorizontalAxis));
  assert(!open.recompute());
}

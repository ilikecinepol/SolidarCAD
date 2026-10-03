#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <iostream>

#include "model/Document.h"
#include "model/RevolveFeature.h"
#include "model/RevolveToolSession.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

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

  // Ctrl-selected profile regions are kept as exact feature input rather than
  // expanding back to every contour in the owning sketch.
  {
    solidar::Document multiDocument;
    auto& multiSketch = multiDocument.addSketch("Multi profile");
    multiSketch.geometry.addRectangle({5.0, 5.0}, {15.0, 10.0});
    multiSketch.geometry.addRectangle({25.0, 15.0}, {35.0, 20.0});
    multiSketch.geometry.addRectangle({45.0, 25.0}, {55.0, 30.0});

    solidar::sketch::Sketch pickedRegions;
    pickedRegions.addRectangle({5.0, 5.0}, {15.0, 10.0});
    pickedRegions.addRectangle({25.0, 15.0}, {35.0, 20.0});

    solidar::RevolveToolSession multiSession;
    multiSession.begin(multiDocument, solidar::kInvalidBodyId,
                       solidar::kInvalidFeatureId);
    const auto requirement = multiSession.selectionRequirement();
    CHECK(requirement.has_value());
    CHECK(requirement->multiSelect);
    multiSession.setProfile(multiSketch.id, pickedRegions);
    CHECK(multiSession.profileOverride().has_value());
    CHECK(multiSession.profileOverride()->lines().size() == 8);
    multiSession.setAxis({solidar::AxisReferenceType::SketchHorizontalAxis,
                          multiSketch.id,
                          solidar::sketch::kInvalidGeometryId});
    CHECK(multiSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(multiSession.previewShape());

    auto feature = std::make_unique<solidar::RevolveFeature>(
        multiSketch.id,
        solidar::AxisReference{
            solidar::AxisReferenceType::SketchHorizontalAxis,
            multiSketch.id, solidar::sketch::kInvalidGeometryId},
        360.0, "Multi revolve", solidar::ExtrudeOperation::NewBody);
    feature->setProfileOverride(pickedRegions);
    auto& multiBody = multiDocument.addBody();
    multiBody.addFeature(std::move(feature));
    CHECK(multiDocument.recompute());
    CHECK(multiBody.resultShape());
    std::size_t solids = 0;
    for (TopExp_Explorer explorer(*multiBody.resultShape(), TopAbs_SOLID);
         explorer.More(); explorer.Next())
      ++solids;
    CHECK(solids == 2);
  }
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

  // Every global datum axis is selectable by the same session workflow.
  // Global Z is intentionally checked at the interaction level: revolving a
  // profile lying in XY about its normal is geometrically degenerate, but the
  // reference and manipulator must still remain available to the user.
  for (const auto axisType : {solidar::AxisReferenceType::GlobalX,
                              solidar::AxisReferenceType::GlobalY,
                              solidar::AxisReferenceType::GlobalZ}) {
    solidar::RevolveToolSession globalAxisSession;
    globalAxisSession.begin(globalAxis, globalBody.id(),
                            solidar::kInvalidFeatureId);
    globalAxisSession.setProfile(globalProfile.id);
    globalAxisSession.setAxis({axisType, solidar::kInvalidSketchId,
                               solidar::sketch::kInvalidGeometryId});
    assert(globalAxisSession.axis());
    assert(globalAxisSession.axis()->type == axisType);
    assert(globalAxisSession.manipulator());
  }

  // A directly selected Line-tool region may use one of its own boundary
  // segments as the revolution axis. Keep the selected-region geometry exact:
  // moving that boundary even slightly off the referenced source line makes a
  // partial revolution self-intersect instead of producing a sector solid.
  {
    solidar::Document triangleDocument;
    auto& triangle = triangleDocument.addSketch("Triangle profile");
    triangle.geometry.addLine({0.0, 0.0}, {25.0, 30.0});
    triangle.geometry.addLine({25.0, 30.0}, {25.0, 0.0});
    triangle.geometry.addLine({25.0, 0.0}, {0.0, 0.0});
    const auto boundaryAxis = triangle.geometry.lineId(2);

    solidar::sketch::Sketch selectedTriangle;
    selectedTriangle.addLine({0.0, 0.0}, {25.0, 30.0});
    selectedTriangle.addLine({25.0, 30.0}, {25.0, 0.0});
    selectedTriangle.addLine({25.0, 0.0}, {0.0, 0.0});

    solidar::RevolveToolSession triangleSession;
    triangleSession.begin(triangleDocument, solidar::kInvalidBodyId,
                          solidar::kInvalidFeatureId);
    triangleSession.setProfile(triangle.id, selectedTriangle);
    triangleSession.setAngleFromPanel(232.08);
    triangleSession.setAxis({solidar::AxisReferenceType::SketchLine,
                             triangle.id, boundaryAxis});
    if (triangleSession.lifecycle() != solidar::ToolLifecycle::PreviewValid)
      std::cerr << triangleSession.error() << '\n';
    CHECK(triangleSession.lifecycle() ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(triangleSession.previewShape());
  }

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

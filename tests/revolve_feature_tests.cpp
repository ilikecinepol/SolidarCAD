#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "TestAssertions.h"
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <iostream>

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
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Sketch);
  CHECK(!session.previewShape());
  session.setProfile(sessionDocument, sessionSketch.id);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingReference);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Axis);
  session.setAxis(sessionDocument, {solidar::AxisReferenceType::SketchHorizontalAxis,
                   sessionSketch.id, solidar::sketch::kInvalidGeometryId});
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  session.setAngleFromPanel(sessionDocument, 180.0);
  CHECK(session.angleDeg() == 180.0 && session.previewShape());
  session.setAngleFromManipulator(sessionDocument, 90.0);
  CHECK(session.angleDeg() == 90.0 && session.manipulator(sessionDocument));
  CHECK(std::get<double>(session.parameters().front().value) == 90.0);
  CHECK(sessionDocument.bodies().size() == featureCountBefore);

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
    multiSession.setProfile(multiDocument, multiSketch.id, pickedRegions);
    CHECK(multiSession.profileOverride().has_value());
    CHECK(multiSession.profileOverride()->lines().size() == 8);
    multiSession.setAxis(multiDocument, {solidar::AxisReferenceType::SketchHorizontalAxis,
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
  CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);
  CHECK(sessionDocument.bodies().size() == featureCountBefore);

  for (const double angle : {360.0, 180.0}) {
    solidar::Document document;
    auto& sketch = document.addSketch();
    sketch.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    auto& body = document.addBody();
    body.addFeature(feature(sketch.id,
                            solidar::AxisReferenceType::SketchHorizontalAxis,
                            angle));
    CHECK(document.recompute());
    CHECK(body.activeFeature()->isValid());
    CHECK(body.resultShape() && !body.resultShape()->IsNull());
  }

  solidar::Document editable;
  auto& profile = editable.addSketch();
  profile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  auto& body = editable.addBody();
  auto revolve = feature(profile.id,
      solidar::AxisReferenceType::SketchHorizontalAxis, 270.0, true);
  auto* ptr = revolve.get();
  body.addFeature(std::move(revolve));
  CHECK(editable.recompute());
  ptr->setAngleDeg(180.0);
  CHECK(ptr->isDirty() && editable.recompute() && ptr->isValid());
  auto changed = profile.geometry;
  changed.clear();
  changed.addRectangle({12.0, 5.0}, {35.0, 18.0});
  CHECK(editable.replaceSketchGeometry(profile.id, std::move(changed)));
  CHECK(ptr->isDirty() && editable.recompute());

  const auto revolveLastValid = ptr->shape();
  for (const double bad : {0.0, -1.0, 361.0,
                           std::numeric_limits<double>::infinity()}) {
    ptr->setAngleDeg(bad);
    CHECK(!editable.recompute());
    CHECK(ptr->isFailed() && !ptr->shape());
    CHECK(ptr->lastValidShape() == revolveLastValid);
    CHECK(!body.resultShape());
    CHECK(body.lastValidResultShape() == revolveLastValid);
  }
  ptr->setAngleDeg(360.0);
  CHECK(editable.recompute());

  solidar::Document vertical;
  auto& verticalProfile = vertical.addSketch();
  verticalProfile.geometry.addRectangle({5.0, 10.0}, {15.0, 30.0});
  auto& verticalBody = vertical.addBody();
  verticalBody.addFeature(feature(verticalProfile.id,
      solidar::AxisReferenceType::SketchVerticalAxis));
  CHECK(vertical.recompute());

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
  CHECK(lineAxis.recompute());

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
  arbitraryAxisSession.setProfile(lineAxis, lineProfile.id);
  arbitraryAxisSession.setAxis(lineAxis, {solidar::AxisReferenceType::SketchLine,
                                lineProfile.id, axisLineId});
  arbitraryAxisSession.setAngleFromPanel(lineAxis, 135.0);
  arbitraryAxisSession.setOperation(lineAxis, solidar::ExtrudeOperation::NewBody);
  arbitraryAxisSession.setAxis(lineAxis, {solidar::AxisReferenceType::SketchLine,
                                lineProfile.id, secondAxisLineId});
  CHECK(arbitraryAxisSession.axis()->lineId == secondAxisLineId);
  CHECK(arbitraryAxisSession.profileSketchId() == lineProfile.id);
  CHECK(arbitraryAxisSession.angleDeg() == 135.0);
  CHECK(arbitraryAxisSession.operation() == solidar::ExtrudeOperation::NewBody);

  solidar::Document globalAxis;
  auto& globalProfile = globalAxis.addSketch();
  globalProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  auto& globalBody = globalAxis.addBody();
  globalBody.addFeature(feature(globalProfile.id,
                                solidar::AxisReferenceType::GlobalX));
  CHECK(globalAxis.recompute());

  // A stale sketchId carried by a global datum axis is not a dependency.
  // Editing or removing that unrelated Sketch must neither dirty nor cascade
  // delete the Revolve; the profile Sketch remains the only declaration.
  {
    solidar::Document dependencyDocument;
    auto& dependencyProfile = dependencyDocument.addSketch("Profile");
    dependencyProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    const auto profileId = dependencyProfile.id;
    auto& staleAxisSketch = dependencyDocument.addSketch("Stale axis owner");
    staleAxisSketch.geometry.addLine({0.0, 0.0}, {5.0, 0.0});
    const auto staleAxisSketchId = staleAxisSketch.id;
    auto& dependencyBody = dependencyDocument.addBody();
    auto globalDatum = std::make_unique<solidar::RevolveFeature>(
        profileId,
        solidar::AxisReference{solidar::AxisReferenceType::GlobalX,
                               staleAxisSketchId,
                               solidar::sketch::kInvalidGeometryId},
        180.0, "Global datum", solidar::ExtrudeOperation::NewBody);
    auto* globalDatumPtr = globalDatum.get();
    const auto globalDatumId = globalDatumPtr->id();
    dependencyBody.addFeature(std::move(globalDatum));
    CHECK(dependencyDocument.recompute());
    const auto revision = globalDatumPtr->shapeRevision();
    solidar::sketch::Sketch replacementAxisSketch;
    replacementAxisSketch.addLine({0.0, 0.0}, {15.0, 0.0});
    CHECK(dependencyDocument.replaceSketchGeometry(staleAxisSketchId,
                                                    replacementAxisSketch));
    CHECK(!globalDatumPtr->isDirty());
    CHECK(dependencyDocument.recompute());
    CHECK(globalDatumPtr->shapeRevision() == revision);
    const auto plan =
        dependencyDocument.planSketchRemoval(staleAxisSketchId);
    CHECK(plan.applicable);
    CHECK(plan.featureIds.empty());
    std::string removalError;
    CHECK(dependencyDocument.applyRemovalPlan(plan, &removalError));
    CHECK(removalError.empty());
    const auto* retainedGlobalDatum =
        dependencyDocument.findFeature(globalDatumId);
    CHECK(retainedGlobalDatum);
    CHECK(retainedGlobalDatum->shapeRevision() == revision);
  }

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
    globalAxisSession.setProfile(globalAxis, globalProfile.id);
    globalAxisSession.setAxis(globalAxis, {axisType, solidar::kInvalidSketchId,
                               solidar::sketch::kInvalidGeometryId});
    CHECK(globalAxisSession.axis());
    CHECK(globalAxisSession.axis()->type == axisType);
    CHECK(globalAxisSession.manipulator(globalAxis));
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
    triangleSession.setProfile(triangleDocument, triangle.id, selectedTriangle);
    triangleSession.setAngleFromPanel(triangleDocument, 232.08);
    triangleSession.setAxis(triangleDocument, {solidar::AxisReferenceType::SketchLine,
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
  CHECK(!invalid.recompute());

  {
    solidar::Document invalidTypeDocument;
    auto& invalidTypeProfile = invalidTypeDocument.addSketch("Invalid axis");
    invalidTypeProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    auto& invalidTypeBody = invalidTypeDocument.addBody();
    auto invalidTypeFeature = std::make_unique<solidar::RevolveFeature>(
        invalidTypeProfile.id,
        solidar::AxisReference{
            static_cast<solidar::AxisReferenceType>(999),
            invalidTypeProfile.id, solidar::sketch::kInvalidGeometryId},
        180.0, "Invalid axis type",
        solidar::ExtrudeOperation::NewBody);
    auto* invalidTypePtr = invalidTypeFeature.get();
    invalidTypeBody.addFeature(std::move(invalidTypeFeature));
    CHECK(!invalidTypeDocument.recompute());
    CHECK(invalidTypePtr->error() == "Revolve axis type is unsupported");
  }

  solidar::Document open;
  auto& openSketch = open.addSketch();
  openSketch.geometry.addLine({1.0, 1.0}, {2.0, 1.0});
  openSketch.geometry.addLine({2.0, 1.0}, {2.0, 2.0});
  auto& openBody = open.addBody();
  openBody.addFeature(feature(openSketch.id,
      solidar::AxisReferenceType::SketchHorizontalAxis));
  CHECK(!open.recompute());
}

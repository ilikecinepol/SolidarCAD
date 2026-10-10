#include "TestAssertions.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include "TestGeometryUtils.h"
#include "model/CircularPatternFeature.h"
#include "model/CircularPatternToolSession.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/PatternTypes.h"
#include <limits>
int main() {
  CHECK(!solidar::validPatternCount(0));
  CHECK(!solidar::validPatternCount(1));
  CHECK(solidar::validPatternCount(2));
  CHECK(solidar::validPatternCount(100));
  CHECK(!solidar::validPatternCount(101));
  CHECK(!solidar::validPatternCount(std::numeric_limits<int>::max()));
  CHECK(solidar::clampPatternCountForUi(std::numeric_limits<int>::max()) ==
        solidar::kMaximumPatternCount);
  CHECK(!solidar::validPrincipalAxis(
      static_cast<solidar::PrincipalAxis>(99)));
  CHECK(!solidar::validPatternOperation(
      static_cast<solidar::PatternOperation>(99)));
  CHECK(!solidar::validPatternAngle(0.009));
  CHECK(solidar::validPatternAngle(0.01));
  solidar::CircularPatternToolSession lowerBoundSession;
  lowerBoundSession.begin(0.009, 2);
  CHECK(solidar::test::near(lowerBoundSession.angleDeg(), 0.01));
  lowerBoundSession.begin(0.01, 2);
  CHECK(solidar::test::near(lowerBoundSession.angleDeg(), 0.01));
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
  const auto lastValidPattern = ptr->shape();
  ptr->setAngleDeg(0.009); body.markDirtyFrom(1); CHECK(!document.recompute());
  CHECK(ptr->isFailed() && !ptr->shape());
  CHECK(ptr->lastValidShape() == lastValidPattern);
  CHECK(!body.resultShape());
  CHECK(body.lastValidResultShape() == lastValidPattern);
  ptr->setAngleDeg(180); body.markDirtyFrom(1); CHECK(document.recompute());

  std::string patternError;
  const auto sourceShape = body.features().front()->shape();
  CHECK(!solidar::buildCircularPatternShape(
      *sourceShape, solidar::PrincipalAxis::Z, 0, 360.0, &patternError));
  CHECK(!solidar::buildCircularPatternShape(
      *sourceShape, solidar::PrincipalAxis::Z, 1, 360.0, &patternError));
  CHECK(!solidar::buildCircularPatternShape(
      *sourceShape, solidar::PrincipalAxis::Z, 101, 360.0, &patternError));
  CHECK(!solidar::buildCircularPatternShape(
      *sourceShape, static_cast<solidar::PrincipalAxis>(99), 2, 360.0,
      &patternError));
  CHECK(!solidar::buildCircularPatternShape(
      *sourceShape, solidar::PrincipalAxis::Z, 2, 360.01, &patternError));
  CHECK(!solidar::buildCircularPatternShape(
      *sourceShape, solidar::PrincipalAxis::Z, 2,
      std::numeric_limits<double>::quiet_NaN(), &patternError));
  CHECK(!solidar::buildCircularPatternShape(
      *sourceShape, solidar::PrincipalAxis::Z, 2, 0.009, &patternError));
  CHECK(solidar::buildCircularPatternShape(
      *sourceShape, solidar::PrincipalAxis::Z, 2, 0.01, &patternError));

  const auto sourceBeforeInvalidPreview = sourceShape;
  session.begin(180.0, 3);
  session.setBody(body.id(), baseId, sourceShape);
  session.setAxis(static_cast<solidar::PrincipalAxis>(99));
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!session.previewShape());
  CHECK(sourceShape == sourceBeforeInvalidPreview);
  session.setAxis(solidar::PrincipalAxis::Z);
  session.setOperation(static_cast<solidar::PatternOperation>(99));
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!session.previewShape());
  CHECK(sourceShape == sourceBeforeInvalidPreview);

  solidar::Document separate;
  auto& sourceSketch = separate.addSketch();
  sourceSketch.geometry.addRectangle({10, 0}, {20, 5});
  auto& sourceBody = separate.addBody("Source");
  auto sourceFeature =
      std::make_unique<solidar::ExtrudeFeature>(sourceSketch.id, 5.0);
  auto* sourcePtr = sourceFeature.get();
  const auto sourceBodyId = sourceBody.id();
  const auto sourceFeatureId = sourceFeature->id();
  sourceBody.addFeature(std::move(sourceFeature));
  auto& patternBody = separate.addBody("Circular copies");
  patternBody.addFeature(std::make_unique<solidar::CircularPatternFeature>(
      sourceBodyId, sourceFeatureId, solidar::PrincipalAxis::Z, 4, 360.0,
      solidar::PatternOperation::NewBody));
  CHECK(separate.recompute());
  CHECK(separate.bodies().size() == 2);
  CHECK(solidar::test::solidCount(*separate.bodies()[0].resultShape()) == 1);
  CHECK(solidar::test::solidCount(*separate.bodies()[1].resultShape()) == 3);
  const double before = solidar::test::volumeOf(
      *separate.bodies()[1].resultShape());
  sourcePtr->setLengthMm(8.0);
  separate.findBody(sourceBodyId)->markDirtyFrom(0);
  CHECK(separate.recompute());
  CHECK(solidar::test::volumeOf(*separate.bodies()[1].resultShape()) > before);
  return EXIT_SUCCESS;
}

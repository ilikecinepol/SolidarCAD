#include "TestAssertions.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/LinearPatternToolSession.h"
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
  CHECK(!solidar::validPatternSpacing(0.009));
  CHECK(solidar::validPatternSpacing(0.01));
  solidar::LinearPatternToolSession lowerBoundSession;
  lowerBoundSession.begin(0.009, 2);
  CHECK(solidar::test::near(lowerBoundSession.spacingMm(), 0.01));
  lowerBoundSession.begin(0.01, 2);
  CHECK(solidar::test::near(lowerBoundSession.spacingMm(), 0.01));
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
  const auto lastValidPattern = ptr->shape();
  ptr->setSpacingMm(0.009); body.markDirtyFrom(1); CHECK(!document.recompute());
  CHECK(ptr->isFailed() && !ptr->shape());
  CHECK(ptr->lastValidShape() == lastValidPattern);
  CHECK(!body.resultShape());
  CHECK(body.lastValidResultShape() == lastValidPattern);
  ptr->setSpacingMm(20); body.markDirtyFrom(1);
  CHECK(document.recompute() && ptr->id() == id);

  std::string patternError;
  const auto sourceShape = body.features().front()->shape();
  CHECK(!solidar::buildLinearPatternShape(
      *sourceShape, solidar::PrincipalAxis::X, 0, 10.0, &patternError));
  CHECK(!solidar::buildLinearPatternShape(
      *sourceShape, solidar::PrincipalAxis::X, 1, 10.0, &patternError));
  CHECK(!solidar::buildLinearPatternShape(
      *sourceShape, solidar::PrincipalAxis::X, 101, 10.0, &patternError));
  CHECK(!solidar::buildLinearPatternShape(
      *sourceShape, static_cast<solidar::PrincipalAxis>(99), 2, 10.0,
      &patternError));
  CHECK(!solidar::buildLinearPatternShape(
      *sourceShape, solidar::PrincipalAxis::X, 2, 100000.01,
      &patternError));
  CHECK(!solidar::buildLinearPatternShape(
      *sourceShape, solidar::PrincipalAxis::X, 2, 0.009, &patternError));
  CHECK(solidar::buildLinearPatternShape(
      *sourceShape, solidar::PrincipalAxis::X, 2, 0.01, &patternError));

  const auto sourceBeforeInvalidPreview = sourceShape;
  session.begin(20.0, 3);
  session.setBody(body.id(), baseId, sourceShape);
  session.setDirection(static_cast<solidar::PrincipalAxis>(99));
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!session.previewShape());
  CHECK(sourceShape == sourceBeforeInvalidPreview);
  session.setDirection(solidar::PrincipalAxis::X);
  session.setOperation(static_cast<solidar::PatternOperation>(99));
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!session.previewShape());
  CHECK(sourceShape == sourceBeforeInvalidPreview);

  // NewBody keeps the source Body intact and stores only generated copies in
  // a separate parametric Body. Upstream edits dirty/rebuild the dependency.
  solidar::Document separate;
  auto& sourceSketch = separate.addSketch();
  sourceSketch.geometry.addRectangle({0, 0}, {10, 10});
  auto& sourceBody = separate.addBody("Source");
  auto sourceFeature =
      std::make_unique<solidar::ExtrudeFeature>(sourceSketch.id, 5.0);
  auto* sourcePtr = sourceFeature.get();
  const auto sourceBodyId = sourceBody.id();
  const auto sourceFeatureId = sourceFeature->id();
  sourceBody.addFeature(std::move(sourceFeature));
  auto& patternBody = separate.addBody("Linear copies");
  patternBody.addFeature(std::make_unique<solidar::LinearPatternFeature>(
      sourceBodyId, sourceFeatureId, solidar::PrincipalAxis::X, 3, 20.0,
      solidar::PatternOperation::NewBody));
  CHECK(separate.recompute());
  CHECK(separate.bodies().size() == 2);
  CHECK(solidar::test::solidCount(*separate.bodies()[0].resultShape()) == 1);
  CHECK(solidar::test::solidCount(*separate.bodies()[1].resultShape()) == 2);
  const double before = solidar::test::volumeOf(
      *separate.bodies()[1].resultShape());
  sourcePtr->setLengthMm(8.0);
  separate.findBody(sourceBodyId)->markDirtyFrom(0);
  CHECK(separate.recompute());
  CHECK(solidar::test::volumeOf(*separate.bodies()[1].resultShape()) > before);
  CHECK(separate.removeBodyCascade(sourceBodyId));
  CHECK(separate.bodies().size() == 1);
  CHECK(separate.bodies().front().features().empty());
  return EXIT_SUCCESS;
}

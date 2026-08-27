#include <TopAbs_ShapeEnum.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <memory>
#include <numbers>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ExtrudeOperationDetector.h"

namespace {

void verifyRectangle(const solidar::SketchPlacement& placement,
                     double expectedX, double expectedY, double expectedZ) {
  solidar::Document document;
  auto& sketch = document.addSketch("Rectangle");
  sketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  sketch.placement = placement;
  auto& body = document.addBody();
  auto feature = std::make_unique<solidar::ExtrudeFeature>(sketch.id, 50.0);
  auto* extrude = feature.get();
  body.addFeature(std::move(feature));
  assert(document.rebuild());
  assert(extrude->isValid() && extrude->hasShape());
  assert(body.resultShape()->ShapeType() == TopAbs_SOLID);
  assert(solidar::test::solidCount(*body.resultShape()) == 1);
  const auto bounds = solidar::test::boundsOf(*body.resultShape());
  assert(solidar::test::near(bounds.x(), expectedX));
  assert(solidar::test::near(bounds.y(), expectedY));
  assert(solidar::test::near(bounds.z(), expectedZ));
  assert(solidar::test::near(solidar::test::volumeOf(*body.resultShape()),
                             140000.0, 1e-3));
}

void verifyCircle(const solidar::SketchPlacement& placement,
                  double expectedX, double expectedY, double expectedZ) {
  solidar::Document document;
  auto& sketch = document.addSketch("Circle");
  sketch.geometry.addCircle({0.0, 0.0}, 10.0);
  sketch.placement = placement;
  auto& body = document.addBody();
  body.addFeature(std::make_unique<solidar::ExtrudeFeature>(sketch.id, 50.0));
  assert(document.rebuild());
  const auto bounds = solidar::test::boundsOf(*body.resultShape());
  assert(solidar::test::near(bounds.x(), expectedX, 1e-5));
  assert(solidar::test::near(bounds.y(), expectedY, 1e-5));
  assert(solidar::test::near(bounds.z(), expectedZ, 1e-5));
  assert(solidar::test::near(solidar::test::volumeOf(*body.resultShape()),
                             std::numbers::pi * 100.0 * 50.0, 1e-3));
}

}  // namespace

int main() {
  verifyRectangle(solidar::SketchPlacement::xy(), 80.0, 35.0, 50.0);
  verifyRectangle(solidar::SketchPlacement::xz(), 80.0, 50.0, 35.0);
  verifyRectangle(solidar::SketchPlacement::yz(), 50.0, 80.0, 35.0);
  verifyCircle(solidar::SketchPlacement::xy(), 20.0, 20.0, 50.0);
  verifyCircle(solidar::SketchPlacement::xz(), 20.0, 50.0, 20.0);
  verifyCircle(solidar::SketchPlacement::yz(), 50.0, 20.0, 20.0);

  // Arbitrary orthonormal placement: local X is world diagonal, local Y is Z,
  // therefore extrusion follows the horizontal placement normal.
  solidar::Document arbitrary;
  auto& arbitrarySketch = arbitrary.addSketch("Arbitrary rectangle");
  arbitrarySketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  const double invSqrt2 = 1.0 / std::sqrt(2.0);
  arbitrarySketch.placement = {{3.0, 4.0, 5.0},
                               {invSqrt2, invSqrt2, 0.0},
                               {0.0, 0.0, 1.0}};
  auto& arbitraryBody = arbitrary.addBody();
  arbitraryBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      arbitrarySketch.id, 50.0));
  assert(arbitrary.rebuild());
  assert(solidar::test::near(
      solidar::test::volumeOf(*arbitraryBody.resultShape()), 140000.0, 1e-3));
  const auto arbitraryBounds = solidar::test::boundsOf(*arbitraryBody.resultShape());
  assert(arbitraryBounds.x() > 90.0 && arbitraryBounds.y() > 90.0);
  assert(solidar::test::near(arbitraryBounds.z(), 35.0));

  solidar::Document reverse;
  auto& reverseSketch = reverse.addSketch("Reverse rectangle");
  reverseSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& reverseBody = reverse.addBody();
  auto feature = std::make_unique<solidar::ExtrudeFeature>(
      reverseSketch.id, 50.0, "Extrude", solidar::ExtrudeOperation::NewBody);
  auto* reverseFeature = feature.get();
  reverseBody.addFeature(std::move(feature));
  assert(reverse.rebuild());
  auto bounds = solidar::test::boundsOf(*reverseBody.resultShape());
  assert(solidar::test::near(bounds.minZ, 0.0));
  assert(solidar::test::near(bounds.maxZ, 50.0));
  reverseFeature->setReversed(true);
  assert(reverseFeature->isDirty() && reverse.rebuild());
  bounds = solidar::test::boundsOf(*reverseBody.resultShape());
  assert(solidar::test::near(bounds.minZ, -50.0));
  assert(solidar::test::near(bounds.maxZ, 0.0));
  reverseFeature->setReversed(false);
  assert(reverse.rebuild());
  assert(solidar::test::near(solidar::test::boundsOf(*reverseBody.resultShape()).maxZ,
                             50.0));

  const auto normalized = solidar::normalizeExtrusionInput(-20.0, false);
  assert(solidar::test::near(normalized.distanceMm, 20.0));
  assert(normalized.reversed);

  for (const double invalid : {0.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
    reverseFeature->setLengthMm(invalid);
    assert(!reverse.rebuild());
    assert(reverseFeature->state() == solidar::FeatureState::Error);
    assert(!reverseFeature->hasShape() && !reverseBody.resultShape());
  }
  reverseFeature->setLengthMm(60.0);
  assert(reverse.rebuild());
  assert(reverseFeature->isValid() && reverseFeature->hasShape());
  assert(solidar::test::near(solidar::test::boundsOf(*reverseBody.resultShape()).z(),
                             60.0));

  for (const double invalidRadius : {
           0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
           std::numeric_limits<double>::infinity()}) {
    solidar::Document invalidCircle;
    auto& sketch = invalidCircle.addSketch("Invalid circle");
    sketch.geometry.addCircle({0.0, 0.0}, invalidRadius);
    auto& body = invalidCircle.addBody();
    body.addFeature(std::make_unique<solidar::ExtrudeFeature>(sketch.id, 10.0));
    assert(!invalidCircle.rebuild());
    assert(body.activeFeature()->state() == solidar::FeatureState::Error);
    assert(!body.resultShape());
  }
}

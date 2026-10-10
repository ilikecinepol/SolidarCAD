#include <TopAbs_ShapeEnum.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "TestAssertions.h"
#include <limits>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <vector>

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
  CHECK(document.rebuild());
  CHECK(extrude->isValid() && extrude->hasShape());
  CHECK(body.resultShape()->ShapeType() == TopAbs_SOLID);
  CHECK(solidar::test::solidCount(*body.resultShape()) == 1);
  const auto bounds = solidar::test::boundsOf(*body.resultShape());
  CHECK(solidar::test::near(bounds.x(), expectedX));
  CHECK(solidar::test::near(bounds.y(), expectedY));
  CHECK(solidar::test::near(bounds.z(), expectedZ));
  CHECK(solidar::test::near(solidar::test::volumeOf(*body.resultShape()),
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
  CHECK(document.rebuild());
  const auto bounds = solidar::test::boundsOf(*body.resultShape());
  CHECK(solidar::test::near(bounds.x(), expectedX, 1e-5));
  CHECK(solidar::test::near(bounds.y(), expectedY, 1e-5));
  CHECK(solidar::test::near(bounds.z(), expectedZ, 1e-5));
  CHECK(solidar::test::near(solidar::test::volumeOf(*body.resultShape()),
                             std::numbers::pi * 100.0 * 50.0, 1e-3));
}

void verifyPolygon(std::initializer_list<solidar::sketch::Point> points) {
  solidar::Document document;
  auto& sketch = document.addSketch("Polygon");
  std::vector<solidar::sketch::Point> vertices(points);
  for (std::size_t index = 0; index < vertices.size(); ++index)
    sketch.geometry.addLine(vertices[index],
                            vertices[(index + 1) % vertices.size()]);
  auto& body = document.addBody();
  body.addFeature(std::make_unique<solidar::ExtrudeFeature>(sketch.id, 25.0));
  CHECK(document.rebuild());
  CHECK(body.activeFeature()->isValid());
  CHECK(solidar::test::solidCount(*body.resultShape()) == 1);
  CHECK(solidar::test::volumeOf(*body.resultShape()) > 1.0);
}

}  // namespace

int main() {
  verifyRectangle(solidar::SketchPlacement::xy(), 80.0, 35.0, 50.0);
  verifyRectangle(solidar::SketchPlacement::xz(), 80.0, 50.0, 35.0);
  verifyRectangle(solidar::SketchPlacement::yz(), 50.0, 80.0, 35.0);
  verifyCircle(solidar::SketchPlacement::xy(), 20.0, 20.0, 50.0);
  verifyCircle(solidar::SketchPlacement::xz(), 20.0, 50.0, 20.0);
  verifyCircle(solidar::SketchPlacement::yz(), 50.0, 20.0, 20.0);
  verifyPolygon({{0.0, 0.0}, {30.0, 0.0}, {12.0, 20.0}});
  verifyPolygon({{0.0, 0.0}, {28.0, 2.0}, {35.0, 17.0},
                 {17.0, 31.0}, {-4.0, 15.0}});

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
  CHECK(arbitrary.rebuild());
  CHECK(solidar::test::near(
      solidar::test::volumeOf(*arbitraryBody.resultShape()), 140000.0, 1e-3));
  const auto arbitraryBounds = solidar::test::boundsOf(*arbitraryBody.resultShape());
  CHECK(arbitraryBounds.x() > 90.0 && arbitraryBounds.y() > 90.0);
  CHECK(solidar::test::near(arbitraryBounds.z(), 35.0));

  solidar::Document reverse;
  auto& reverseSketch = reverse.addSketch("Reverse rectangle");
  reverseSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& reverseBody = reverse.addBody();
  auto feature = std::make_unique<solidar::ExtrudeFeature>(
      reverseSketch.id, 50.0, "Extrude", solidar::ExtrudeOperation::NewBody);
  auto* reverseFeature = feature.get();
  reverseBody.addFeature(std::move(feature));
  CHECK(reverse.rebuild());
  auto bounds = solidar::test::boundsOf(*reverseBody.resultShape());
  CHECK(solidar::test::near(bounds.minZ, 0.0));
  CHECK(solidar::test::near(bounds.maxZ, 50.0));
  reverseFeature->setReversed(true);
  CHECK(reverseFeature->isDirty() && reverse.rebuild());
  bounds = solidar::test::boundsOf(*reverseBody.resultShape());
  CHECK(solidar::test::near(bounds.minZ, -50.0));
  CHECK(solidar::test::near(bounds.maxZ, 0.0));
  reverseFeature->setReversed(false);
  CHECK(reverse.rebuild());
  CHECK(solidar::test::near(solidar::test::boundsOf(*reverseBody.resultShape()).maxZ,
                             50.0));

  const auto normalized = solidar::normalizeExtrusionInput(-20.0, false);
  CHECK(solidar::test::near(normalized.distanceMm, 20.0));
  CHECK(normalized.reversed);

  const auto reverseLastValid = reverseFeature->shape();
  for (const double invalid : {0.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
    reverseFeature->setLengthMm(invalid);
    CHECK(!reverse.rebuild());
    CHECK(reverseFeature->state() == solidar::FeatureState::Error);
    CHECK(!reverseFeature->shape() &&
           reverseFeature->lastValidShape() == reverseLastValid &&
           !reverseBody.resultShape() &&
           reverseBody.lastValidResultShape() == reverseLastValid);
  }
  reverseFeature->setLengthMm(60.0);
  CHECK(reverse.rebuild());
  CHECK(reverseFeature->isValid() && reverseFeature->hasShape());
  CHECK(solidar::test::near(solidar::test::boundsOf(*reverseBody.resultShape()).z(),
                             60.0));

  for (const double invalidRadius : {
           0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
           std::numeric_limits<double>::infinity()}) {
    solidar::Document invalidCircle;
    auto& sketch = invalidCircle.addSketch("Invalid circle");
    sketch.geometry.addCircle({0.0, 0.0}, invalidRadius);
    auto& body = invalidCircle.addBody();
    body.addFeature(std::make_unique<solidar::ExtrudeFeature>(sketch.id, 10.0));
    CHECK(!invalidCircle.rebuild());
    CHECK(body.activeFeature()->state() == solidar::FeatureState::Error);
    CHECK(!body.resultShape());
  }
}

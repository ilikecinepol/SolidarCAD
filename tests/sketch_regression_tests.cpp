#include "sketch/Sketch.h"
#include "sketch/SketchSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace solidar::sketch;
constexpr double kTolerance = 1e-6;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "sketch regression failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(bool condition, std::string_view message) {
  if (!condition) fail(message);
}

bool near(double actual, double expected, double tolerance = kTolerance) {
  return std::abs(actual - expected) <= tolerance;
}

double length(const Line& line) {
  return std::hypot(line.end.xMm - line.start.xMm,
                    line.end.yMm - line.start.yMm);
}

double angleDegrees(const Line& first, const Line& second) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double cosine = std::clamp((ax * bx + ay * by) /
                                       (std::hypot(ax, ay) * std::hypot(bx, by)),
                                   -1.0, 1.0);
  return std::acos(cosine) * 180.0 / std::acos(-1.0);
}

Constraint geometryConstraint(ConstraintType type, GeometryId first,
                              GeometryId second = kInvalidGeometryId,
                              double value = 0.0) {
  Constraint constraint;
  constraint.type = type;
  constraint.firstGeometry = first;
  constraint.secondGeometry = second;
  constraint.value = value;
  return constraint;
}

Constraint pointConstraint(ConstraintType type, PointReference first,
                           PointReference second, double value) {
  Constraint constraint;
  constraint.type = type;
  constraint.firstPoint = first;
  constraint.secondPoint = second;
  constraint.value = value;
  return constraint;
}

void drivingDimensionsRemainStableAfterDragging() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {13.0, 17.0});
  const GeometryId lineId = sketch.lineId(0);
  sketch.addConstraint(geometryConstraint(ConstraintType::Length, lineId,
                                          kInvalidGeometryId, 50.0));
  expect(near(length(sketch.lines()[0]), 50.0), "line length must become 50");
  sketch.translatePoint({lineId, false}, 19.0, -11.0);
  (void)BasicSketchSolver::solve(sketch);
  expect(near(length(sketch.lines()[0]), 50.0),
         "driving line length must survive endpoint drag");

  sketch.addLine({90.0, 10.0}, {100.0, 20.0});
  const GeometryId otherId = sketch.lineId(1);
  const PointReference first{lineId, false};
  const PointReference second{otherId, true};
  sketch.addConstraint(
      pointConstraint(ConstraintType::DistanceX, first, second, 30.0));
  sketch.addConstraint(
      pointConstraint(ConstraintType::DistanceY, first, second, 40.0));
  sketch.addConstraint(
      pointConstraint(ConstraintType::Distance, first, second, 50.0));
  expect(sketch.constraints().size() == 4,
         "X, Y and aligned constraints must coexist");
  sketch.translatePoint(second, 17.0, 23.0);
  (void)BasicSketchSolver::solve(sketch);
  const Point a = *sketch.referencedPoint(first);
  const Point b = *sketch.referencedPoint(second);
  expect(near(std::abs(b.xMm - a.xMm), 30.0), "X must return to 30");
  expect(near(std::abs(b.yMm - a.yMm), 40.0), "Y must return to 40");
  expect(near(std::hypot(b.xMm - a.xMm, b.yMm - a.yMm), 50.0),
         "aligned distance must return to 50");
}

void centerReferencesMoveWholeObjects() {
  Sketch sketch;
  sketch.addCircle({10.0, 10.0}, 8.0);
  sketch.addLine({0.0, 0.0}, {0.0, 5.0});
  const GeometryId circleId = sketch.circleId(0);
  const PointReference circleCenter{kInvalidGeometryId, true, circleId};
  const PointReference linePoint{sketch.lineId(0), true};
  sketch.addConstraint(pointConstraint(ConstraintType::DistanceX, linePoint,
                                       circleCenter, 25.0));
  expect(near(sketch.circles()[0].center.xMm, 25.0),
         "circle center must be a driving point");
  expect(near(sketch.circles()[0].radiusMm, 8.0),
         "moving a circle center must preserve radius");

  sketch.addRectangle({40.0, 20.0}, {60.0, 40.0});
  const std::size_t rectangleId = sketch.lines()[1].elementId;
  sketch.markElementCenterNode(rectangleId);
  const Point before = *sketch.elementCenterPoint(rectangleId);
  const PointReference rectangleCenter{kInvalidGeometryId, true,
                                       kInvalidGeometryId, rectangleId};
  expect(sketch.translatePoint(rectangleCenter, 12.0, -7.0),
         "rectangle center must be translatable");
  const Point after = *sketch.elementCenterPoint(rectangleId);
  expect(near(after.xMm - before.xMm, 12.0) &&
             near(after.yMm - before.yMm, -7.0),
         "center drag must move the complete rectangle");
  const auto rectangleLines = std::count_if(
      sketch.lines().begin(), sketch.lines().end(),
      [rectangleId](const Line& line) { return line.elementId == rectangleId; });
  expect(rectangleLines == 4, "rectangle must remain a four-line composite");
}

void dimensionEditingPreservesKind() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 10.0});
  sketch.addLine({30.0, 40.0}, {45.0, 45.0});
  const PointReference a{sketch.lineId(0), true};
  const PointReference b{sketch.lineId(1), true};
  for (DimensionKind kind : {DimensionKind::PointDistanceX,
                             DimensionKind::PointDistanceY,
                             DimensionKind::PointDistance}) {
    Dimension dimension;
    dimension.kind = kind;
    dimension.firstPoint = a;
    dimension.secondPoint = b;
    dimension.valueMm = 10.0;
    sketch.storeDimension(dimension);
    const std::size_t index = sketch.dimensions().size() - 1;
    expect(sketch.setDimensionPlacement(index, 22.0, 0.75),
           "dimension placement must be editable");
    expect(sketch.dimensions()[index].kind == kind,
           "moving a dimension must not change its kind");
    expect(sketch.setDimensionValue(index, 35.0),
           "dimension value must be editable");
    expect(sketch.dimensions()[index].kind == kind &&
               near(sketch.dimensions()[index].valueMm, 35.0),
           "numeric edit must preserve dimension kind");
  }
}

void angleBranchesAndCompositeGeometryRemainStable() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {30.0, 10.0});
  sketch.addLine({0.0, 0.0}, {5.0, 35.0});
  const GeometryId first = sketch.lineId(0);
  const GeometryId second = sketch.lineId(1);
  Constraint angle =
      geometryConstraint(ConstraintType::Angle, first, second, 30.0);
  ConstraintId angleId = sketch.addConstraint(angle);
  expect(near(angleDegrees(sketch.lines()[0], sketch.lines()[1]), 30.0),
         "30 degree angle must not resolve to 150 degrees");

  for (double value : {60.0, 120.0, 30.0}) {
    expect(sketch.removeConstraint(angleId),
           "old angle must be removable");

    angle.value = value;
    angle.id = 0;
    angleId = sketch.addConstraint(angle);

    expect(near(angleDegrees(sketch.lines()[0], sketch.lines()[1]), value),
           "edited angle must use the requested branch");
  }

  Sketch composite;
  composite.addRectangle({0.0, 0.0}, {40.0, 20.0});
  composite.addLine({60.0, 0.0}, {75.0, 17.0});
  const GeometryId side = composite.lineId(0);
  const GeometryId loose = composite.lineId(4);
  const auto originalRectangle =
      std::vector<Line>(composite.lines().begin(), composite.lines().begin() + 4);
  composite.addConstraint(
      geometryConstraint(ConstraintType::Angle, side, loose, 30.0));
  for (std::size_t i = 0; i < 4; ++i) {
    expect(near(composite.lines()[i].start.xMm, originalRectangle[i].start.xMm) &&
               near(composite.lines()[i].start.yMm, originalRectangle[i].start.yMm) &&
               near(composite.lines()[i].end.xMm, originalRectangle[i].end.xMm) &&
               near(composite.lines()[i].end.yMm, originalRectangle[i].end.yMm),
           "angle must rotate the loose line, not deform the rectangle");
  }
}

void relationalConstraintsRespectDrivingSizesAndComposites() {
  for (ConstraintType type : {ConstraintType::Parallel,
                              ConstraintType::Perpendicular}) {
    for (bool reverse : {false, true}) {
      Sketch sketch;
      sketch.addRectangle({0.0, 0.0}, {40.0, 20.0});
      sketch.addLine({60.0, 0.0}, {75.0, 17.0});
      const GeometryId side = sketch.lineId(0);
      const GeometryId loose = sketch.lineId(4);
      const auto before =
          std::vector<Line>(sketch.lines().begin(), sketch.lines().begin() + 4);
      sketch.addConstraint(geometryConstraint(type, reverse ? side : loose,
                                              reverse ? loose : side));
      for (std::size_t i = 0; i < 4; ++i)
        expect(near(length(sketch.lines()[i]), length(before[i])),
               "parallel/perpendicular must preserve rectangle sides");
      const double expected = type == ConstraintType::Parallel ? 0.0 : 90.0;
      const double actual = angleDegrees(sketch.lines()[0], sketch.lines()[4]);
      expect(near(actual, expected) || (expected == 0.0 && near(actual, 180.0)),
             "loose line must satisfy orientation relationship");
    }
  }

  Sketch equal;
  equal.addLine({0.0, 0.0}, {13.0, 0.0});
  equal.addLine({0.0, 20.0}, {31.0, 20.0});
  const GeometryId driven = equal.lineId(0);
  const GeometryId follower = equal.lineId(1);
  equal.addConstraint(geometryConstraint(ConstraintType::Length, driven,
                                         kInvalidGeometryId, 50.0));
  equal.addConstraint(
      geometryConstraint(ConstraintType::Equal, driven, follower));
  expect(near(length(equal.lines()[0]), 50.0) &&
             near(length(equal.lines()[1]), 50.0),
         "Equal must follow a 50 mm driving dimension");
}

void tangencyAndPointRelationsStayValidAfterMovement() {
  Sketch sketch;
  sketch.addLine({-100.0, 0.0}, {100.0, 0.0});
  sketch.addLine({0.0, -100.0}, {0.0, 100.0});
  sketch.addCircle({20.0, 20.0}, 10.0);
  const GeometryId circle = sketch.circleId(0);
  sketch.addConstraint(
      geometryConstraint(ConstraintType::Tangent, sketch.lineId(0), circle));
  sketch.addConstraint(
      geometryConstraint(ConstraintType::Tangent, sketch.lineId(1), circle));
  sketch.addConstraint(geometryConstraint(ConstraintType::Diameter, circle,
                                          kInvalidGeometryId, 20.0));
  sketch.translateElement(sketch.lines()[0].elementId, 0.0, 5.0);
  (void)BasicSketchSolver::solve(sketch);
  const Point center = sketch.circles()[0].center;
  expect(near(std::abs(center.yMm - 5.0), 10.0) &&
             near(std::abs(center.xMm), 10.0),
         "both tangencies must survive carrier movement");
  expect(near(sketch.circles()[0].radiusMm, 10.0),
         "diameter constraint must survive tangency solve");

  Sketch points;
  points.addLine({0.0, 0.0}, {10.0, 0.0});
  points.addLine({4.0, 8.0}, {12.0, 8.0});
  points.addCircle({20.0, 0.0}, 5.0);
  const PointReference firstEnd{points.lineId(0), false};
  const PointReference secondStart{points.lineId(1), true};
  points.addConstraint(pointConstraint(ConstraintType::Coincident, firstEnd,
                                       secondStart, 0.0));
  expect(near(points.referencedPoint(firstEnd)->xMm,
              points.referencedPoint(secondStart)->xMm) &&
             near(points.referencedPoint(firstEnd)->yMm,
                  points.referencedPoint(secondStart)->yMm),
         "Coincident endpoints must coincide");
  Constraint onLine = pointConstraint(ConstraintType::PointOnLine, {},
                                      secondStart, 0.0);
  onLine.firstGeometry = points.lineId(0);
  points.addConstraint(onLine);
  Constraint onCircle = pointConstraint(ConstraintType::PointOnCircle, {},
                                        firstEnd, 0.0);
  onCircle.firstGeometry = points.circleId(0);
  points.addConstraint(onCircle);
  (void)BasicSketchSolver::solve(points);
  expect(std::isfinite(points.referencedPoint(firstEnd)->xMm),
         "mixed point relations must solve without dangling references");
}

void deletionStressHasNoDanglingReferenceCrash() {
  Sketch sketch;
  for (int i = 0; i < 10; ++i)
    sketch.addLine({double(i * 10), 0.0}, {double(i * 10 + 7), 11.0});
  for (int i = 0; i < 4; ++i)
    sketch.addCircle({double(i * 20), 30.0}, 4.0 + i);
  for (std::size_t i = 1; i < 10; ++i)
    sketch.addConstraint(geometryConstraint(ConstraintType::Equal,
                                            sketch.lineId(0), sketch.lineId(i)));
  const ConstraintId stale = sketch.addConstraint(
      geometryConstraint(ConstraintType::Tangent, sketch.lineId(0),
                         sketch.circleId(0)));
  sketch.removeLine(0);
  const SolveResult result = BasicSketchSolver::solve(sketch);
  expect(result.invalidReferences == 0,
         "geometry deletion must proactively remove dangling references");
  expect(!sketch.removeConstraint(stale),
         "constraint referencing deleted geometry must already be gone");
  sketch.translateSelection({}, {sketch.circleId(0)}, 3.0, -2.0);
}

}  // namespace

int main() {
  drivingDimensionsRemainStableAfterDragging();
  centerReferencesMoveWholeObjects();
  dimensionEditingPreservesKind();
  angleBranchesAndCompositeGeometryRemainStable();
  relationalConstraintsRespectDrivingSizesAndComposites();
  tangencyAndPointRelationsStayValidAfterMovement();
  deletionStressHasNoDanglingReferenceCrash();
  return EXIT_SUCCESS;
}
#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"

#include <algorithm>
#include <array>
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

void lineArcTangencySurvivesMovement() {
  constexpr double kPi = 3.14159265358979323846;
  Sketch sketch;
  sketch.addLine({-50.0, 0.0}, {50.0, 0.0});
  sketch.addArc({20.0, 30.0}, 10.0, kPi, kPi);
  const GeometryId lineId = sketch.lineId(0);
  const GeometryId arcId = sketch.arcId(0);

  const ConstraintId tangent = sketch.addConstraint(
      geometryConstraint(ConstraintType::Tangent, lineId, arcId));
  expect(tangent != kInvalidConstraintId,
         "Line-Arc Tangent must be accepted");
  expect(near(sketch.arcs()[0].center.yMm, 10.0),
         "Line-Arc Tangent must move the Arc onto the carrier");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "Line-Arc Tangent must be diagnostically satisfied");

  sketch.translateElement(sketch.lines()[0].elementId, 0.0, 5.0);
  expect(near(sketch.arcs()[0].center.yMm, 15.0),
         "Line-Arc Tangent must follow carrier movement");

  sketch.translateArcById(arcId, 0.0, 12.0);
  expect(near(sketch.arcs()[0].center.yMm, 15.0),
         "dragging a constrained Arc must reapply Tangent");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "Line-Arc Tangent must remain satisfied after dragging");

  sketch.removeArc(0);
  expect(sketch.constraints().empty(),
         "deleting an Arc must remove its Tangent constraint");
  expect(BasicSketchSolver::solve(sketch).invalidReferences == 0,
         "Arc deletion must not leave dangling solver references");
}

void rectangleSideArcDragIsStable() {
  constexpr double kPi = 3.14159265358979323846;
  Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {100.0, 20.0});
  sketch.addArc({50.0, 20.0}, 50.0, 0.0, kPi);

  const GeometryId topLineId = sketch.lineId(2);
  const GeometryId arcId = sketch.arcId(0);
  const PointReference topStart{topLineId, true};
  const PointReference topEnd{topLineId, false};
  PointReference arcStart;
  arcStart.arcId = arcId;
  arcStart.start = true;
  PointReference arcEnd = arcStart;
  arcEnd.start = false;

  expect(sketch.addConstraint(
             pointConstraint(ConstraintType::Coincident, topStart,
                             arcStart, 0.0)) != kInvalidConstraintId,
         "rectangle/Arc first endpoint Coincident must be accepted");
  expect(sketch.addConstraint(
             pointConstraint(ConstraintType::Coincident, topEnd,
                             arcEnd, 0.0)) != kInvalidConstraintId,
         "rectangle/Arc second endpoint Coincident must be accepted");

  sketch.translateArcById(arcId, 5.0, 5.0);
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "dragging the attached Arc must keep both endpoints valid");

  sketch.translateElement(sketch.lines()[0].elementId, 7.0, -3.0);
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "dragging the attached rectangle must keep both endpoints valid");
  expect(BasicSketchSolver::solve(sketch).invalidReferences == 0,
         "rectangle/Arc drag must not leave invalid solver references");

  const auto firstLinePoint = sketch.referencedPoint(topStart);
  const auto firstArcPoint = sketch.referencedPoint(arcStart);
  const auto secondLinePoint = sketch.referencedPoint(topEnd);
  const auto secondArcPoint = sketch.referencedPoint(arcEnd);
  expect(firstLinePoint && firstArcPoint && secondLinePoint && secondArcPoint,
         "rectangle/Arc endpoint references must survive dragging");
  expect(near(firstLinePoint->xMm, firstArcPoint->xMm) &&
             near(firstLinePoint->yMm, firstArcPoint->yMm) &&
             near(secondLinePoint->xMm, secondArcPoint->xMm) &&
             near(secondLinePoint->yMm, secondArcPoint->yMm),
         "both Arc endpoints must follow the moved rectangle");
}

void secondArcEndpointCoincidenceReshapesWithoutConflict() {
  constexpr double kPi = 3.14159265358979323846;
  Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {100.0, 50.0});

  // The lower endpoint already touches the rectangle, while the upper one is
  // deliberately five millimetres above its corner (the reported UI case).
  sketch.addArc({0.0, 27.5}, 27.5, -kPi * 0.5, kPi);

  const PointReference bottomLeft{sketch.lineId(0), true};
  const PointReference topLeft{sketch.lineId(2), false};
  PointReference arcStart;
  arcStart.arcId = sketch.arcId(0);
  arcStart.start = true;
  PointReference arcEnd = arcStart;
  arcEnd.start = false;

  expect(sketch.addConstraint(
             pointConstraint(ConstraintType::Coincident, bottomLeft,
                             arcStart, 0.0)) != kInvalidConstraintId,
         "first rectangle/Arc endpoint Coincident must be accepted");
  expect(sketch.addConstraint(
             pointConstraint(ConstraintType::Coincident, topLeft,
                             arcEnd, 0.0)) != kInvalidConstraintId,
         "second rectangle/Arc endpoint Coincident must reshape the Arc");

  const auto bottomArcPoint = sketch.referencedPoint(arcStart);
  const auto topArcPoint = sketch.referencedPoint(arcEnd);
  const auto bottomRectanglePoint = sketch.referencedPoint(bottomLeft);
  const auto topRectanglePoint = sketch.referencedPoint(topLeft);
  expect(bottomArcPoint && topArcPoint && bottomRectanglePoint &&
             topRectanglePoint,
         "all constrained Arc endpoint references must remain valid");
  expect(near(bottomArcPoint->xMm, bottomRectanglePoint->xMm) &&
             near(bottomArcPoint->yMm, bottomRectanglePoint->yMm) &&
             near(topArcPoint->xMm, topRectanglePoint->xMm) &&
             near(topArcPoint->yMm, topRectanglePoint->yMm),
         "both Arc endpoints must coincide with rectangle corners");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "two Arc endpoint coincidences must not conflict");
}

void pointOnCircleSurvivesFurtherSketchEdits() {
  Sketch sketch;
  sketch.addCircle({0.0, 0.0}, 10.0);
  sketch.addLine({10.0, 0.0}, {20.0, 0.0});
  const GeometryId circleId = sketch.circleId(0);
  const PointReference constrainedPoint{sketch.lineId(0), true};
  Constraint pointOnCircle;
  pointOnCircle.type = ConstraintType::PointOnCircle;
  pointOnCircle.firstGeometry = circleId;
  pointOnCircle.secondPoint = constrainedPoint;
  const ConstraintId pointOnCircleId = sketch.addConstraint(pointOnCircle);
  expect(pointOnCircleId != kInvalidConstraintId,
         "Point-on-Circle constraint must be stored in the sketch");

  // Regression: editing geometry after Point-on-Circle used to leave solver
  // references in a crash-prone state. Exercise unrelated creation, movement,
  // solving and constraint removal after the relation exists.
  sketch.addRectangle({25.0, 5.0}, {45.0, 20.0});
  sketch.addLine({0.0, 30.0}, {15.0, 37.0});
  expect(sketch.setLineHorizontalById(sketch.lineId(5)),
         "new line must accept Horizontal after Point-on-Circle");
  sketch.translateCircleById(circleId, 3.0, -2.0);
  const SolveResult result = BasicSketchSolver::solve(sketch);
  expect(result.invalidReferences == 0,
         "Point-on-Circle must remain a valid solver reference after edits");
  const auto point = sketch.referencedPoint(constrainedPoint);
  expect(point && std::isfinite(point->xMm) && std::isfinite(point->yMm),
         "constrained point must remain finite after later sketch edits");
  expect(sketch.removeConstraint(pointOnCircleId),
         "Point-on-Circle must remain removable after later edits");
  (void)BasicSketchSolver::solve(sketch);
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
  sketch.translateSelection({}, {sketch.circleId(0)}, {}, 3.0, -2.0);
}


void parallelLineDistanceKeepsRectangleRigid() {
  Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {40.0, 20.0});
  sketch.addLine({5.0, 35.0}, {30.0, 35.0});
  const GeometryId rectangleTop = sketch.lineId(2);
  const GeometryId loose = sketch.lineId(4);
  const auto rectangleBefore =
      std::vector<Line>(sketch.lines().begin(), sketch.lines().begin() + 4);

  sketch.addConstraint(
      geometryConstraint(ConstraintType::Parallel, rectangleTop, loose));
  sketch.addConstraint(
      geometryConstraint(ConstraintType::LineDistance,
                         rectangleTop, loose, 12.0));

  const double topY =
      (sketch.lines()[2].start.yMm + sketch.lines()[2].end.yMm) * 0.5;
  const double looseY =
      (sketch.lines()[4].start.yMm + sketch.lines()[4].end.yMm) * 0.5;
  expect(near(std::abs(looseY - topY), 12.0),
         "parallel-line distance must be 12 mm");
  expect(near(length(sketch.lines()[4]), 25.0),
         "line distance must preserve loose-line length");
  for (std::size_t i = 0; i < 4; ++i)
    expect(near(sketch.lines()[i].start.xMm, rectangleBefore[i].start.xMm) &&
               near(sketch.lines()[i].start.yMm, rectangleBefore[i].start.yMm) &&
               near(sketch.lines()[i].end.xMm, rectangleBefore[i].end.xMm) &&
               near(sketch.lines()[i].end.yMm, rectangleBefore[i].end.yMm),
           "line-to-rectangle distance must not deform rectangle");
}

void dimensionAndPerpendicularWorkAgainstProjectedCarriers() {
  Sketch sketch;
  sketch.addLine({-40.0, -20.0}, {40.0, -20.0});
  sketch.addCircle({0.0, 0.0}, 10.0);
  sketch.addLine({-10.0, 0.0}, {-8.0, -20.0});
  const GeometryId projectionLine = sketch.lineId(0);
  const GeometryId projectionCircle = sketch.circleId(0);
  const GeometryId activeLine = sketch.lineId(1);

  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, projectionLine)) !=
             kInvalidConstraintId,
         "projected line must be lockable");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, projectionCircle)) !=
             kInvalidConstraintId,
         "projected circle must be lockable");

  Constraint startOnCircle;
  startOnCircle.type = ConstraintType::PointOnCircle;
  startOnCircle.firstGeometry = projectionCircle;
  startOnCircle.secondPoint = {activeLine, true};
  expect(sketch.addConstraint(startOnCircle) != kInvalidConstraintId,
         "new-line start must attach to projected circle");

  Constraint endOnProjection;
  endOnProjection.type = ConstraintType::PointOnLine;
  endOnProjection.firstGeometry = projectionLine;
  endOnProjection.secondPoint = {activeLine, false};
  expect(sketch.addConstraint(endOnProjection) != kInvalidConstraintId,
         "new-line end must attach to projected line");

  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Length, activeLine,
                                kInvalidGeometryId, 25.0)) !=
             kInvalidConstraintId,
         "line length must be applicable between projected carriers");
  const auto perpendicularId = sketch.addConstraint(
      geometryConstraint(ConstraintType::Perpendicular,
                         projectionLine, activeLine));
  expect(perpendicularId != kInvalidConstraintId,
         "line must become perpendicular to projected carrier");

  const auto& result = sketch.lines()[*sketch.lineIndex(activeLine)];
  expect(near(length(result), 25.0, 1e-4),
         "projected-carrier line must retain requested 25 mm length");
  expect(near(angleDegrees(sketch.lines()[*sketch.lineIndex(projectionLine)],
                           result),
              90.0, 1e-3),
         "projected-carrier line must remain perpendicular");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "projected-carrier constraint system must be satisfied");
}

void dimensionAndTangencyWorkAfterPerpendicularProjection() {
  Sketch sketch;
  sketch.addLine({-40.0, -35.0}, {40.0, -35.0});
  sketch.addCircle({0.0, 0.0}, 10.0);
  sketch.addLine({-8.0, -35.0}, {-8.0, -6.0});
  const GeometryId projectionLine = sketch.lineId(0);
  const GeometryId projectionCircle = sketch.circleId(0);
  const GeometryId activeLine = sketch.lineId(1);

  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, projectionLine)) !=
             kInvalidConstraintId,
         "lower projection must be lockable");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, projectionCircle)) !=
             kInvalidConstraintId,
         "projected circle must be lockable");

  Constraint startOnProjection;
  startOnProjection.type = ConstraintType::PointOnLine;
  startOnProjection.firstGeometry = projectionLine;
  startOnProjection.secondPoint = {activeLine, true};
  expect(sketch.addConstraint(startOnProjection) != kInvalidConstraintId,
         "line start must attach to the lower projection");

  Constraint endOnCircle;
  endOnCircle.type = ConstraintType::PointOnCircle;
  endOnCircle.firstGeometry = projectionCircle;
  endOnCircle.secondPoint = {activeLine, false};
  expect(sketch.addConstraint(endOnCircle) != kInvalidConstraintId,
         "line end must attach to the projected circle");

  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Perpendicular,
                                projectionLine, activeLine)) !=
             kInvalidConstraintId,
         "perpendicularity must be accepted before the size");
  Constraint directSize;
  directSize.type = ConstraintType::Distance;
  directSize.firstPoint = {activeLine, true};
  directSize.secondPoint = {activeLine, false};
  directSize.value = 25.0;
  expect(sketch.addConstraint(directSize) != kInvalidConstraintId,
         "25 mm size must slide the perpendicular line to a valid solution");

  const auto& result = sketch.lines()[*sketch.lineIndex(activeLine)];
  const auto& circle = sketch.circles()[*sketch.circleIndex(projectionCircle)];
  expect(near(length(result), 25.0, 1e-4),
         "projected line must retain the requested 25 mm length");
  expect(near(result.end.xMm, circle.center.xMm, 1e-4) &&
             near(result.end.yMm,
                  circle.center.yMm - circle.radiusMm, 1e-4),
         "sized perpendicular line must slide to the circle intersection");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "dimension/perpendicular projection chain must converge");

  Sketch tangentSketch;
  tangentSketch.addLine({-40.0, -35.0}, {40.0, -35.0});
  tangentSketch.addCircle({0.0, 0.0}, 10.0);
  tangentSketch.addLine({-8.0, -35.0}, {-8.0, -6.0});
  const GeometryId tangentProjection = tangentSketch.lineId(0);
  const GeometryId tangentCircle = tangentSketch.circleId(0);
  const GeometryId tangentLine = tangentSketch.lineId(1);
  expect(tangentSketch.addConstraint(
             geometryConstraint(ConstraintType::Lock,
                                tangentProjection)) != kInvalidConstraintId,
         "tangent lower projection must be lockable");
  expect(tangentSketch.addConstraint(
             geometryConstraint(ConstraintType::Lock,
                                tangentCircle)) != kInvalidConstraintId,
         "tangent projected circle must be lockable");
  startOnProjection.firstGeometry = tangentProjection;
  startOnProjection.secondPoint = {tangentLine, true};
  expect(tangentSketch.addConstraint(startOnProjection) !=
             kInvalidConstraintId,
         "tangent line start must attach to the lower projection");
  endOnCircle.firstGeometry = tangentCircle;
  endOnCircle.secondPoint = {tangentLine, false};
  expect(tangentSketch.addConstraint(endOnCircle) != kInvalidConstraintId,
         "tangent line end must attach to the projected circle");
  expect(tangentSketch.addConstraint(
             geometryConstraint(ConstraintType::Perpendicular,
                                tangentProjection, tangentLine)) !=
             kInvalidConstraintId,
         "tangent line must remain perpendicular to the projection");
  expect(tangentSketch.addConstraint(
             geometryConstraint(ConstraintType::Tangent, tangentLine,
                                tangentCircle)) != kInvalidConstraintId,
         "free line must become tangent to a locked projected circle");
  const auto& tangentResult =
      tangentSketch.lines()[*tangentSketch.lineIndex(tangentLine)];
  expect(near(std::abs(tangentResult.start.xMm), 10.0, 1e-4) &&
             near(tangentResult.end.yMm, 0.0, 1e-4),
         "line must slide to the finite tangent contact");
  expect(!analyzeConstraintSystem(tangentSketch).conflicting,
         "perpendicular/tangent projection chain must converge");
}

void tangentPerpendicularLengthAreOrderIndependent() {
  enum class Requested { Tangent, Perpendicular, Length };
  const std::array<std::array<Requested, 3>, 6> orders{{
      {Requested::Tangent, Requested::Perpendicular, Requested::Length},
      {Requested::Tangent, Requested::Length, Requested::Perpendicular},
      {Requested::Perpendicular, Requested::Tangent, Requested::Length},
      {Requested::Perpendicular, Requested::Length, Requested::Tangent},
      {Requested::Length, Requested::Tangent, Requested::Perpendicular},
      {Requested::Length, Requested::Perpendicular, Requested::Tangent},
  }};

  for (std::size_t orderIndex = 0; orderIndex < orders.size(); ++orderIndex) {
    const auto& order = orders[orderIndex];
    Sketch sketch;
    sketch.addLine({-40.0, -25.0}, {40.0, -25.0});
    sketch.addCircle({0.0, 0.0}, 10.0);
    sketch.addLine({-8.0, -25.0}, {-8.0, -6.0});
    const GeometryId projection = sketch.lineId(0);
    const GeometryId circle = sketch.circleId(0);
    const GeometryId line = sketch.lineId(1);

    expect(sketch.addConstraint(
               geometryConstraint(ConstraintType::Lock, projection)) !=
               kInvalidConstraintId,
           "order-independent lower projection must be lockable");
    expect(sketch.addConstraint(
               geometryConstraint(ConstraintType::Lock, circle)) !=
               kInvalidConstraintId,
           "order-independent circle must be lockable");
    Constraint pointOnLine;
    pointOnLine.type = ConstraintType::PointOnLine;
    pointOnLine.firstGeometry = projection;
    pointOnLine.secondPoint = {line, true};
    expect(sketch.addConstraint(pointOnLine) != kInvalidConstraintId,
           "order-independent line start must attach to projection");
    Constraint pointOnCircle;
    pointOnCircle.type = ConstraintType::PointOnCircle;
    pointOnCircle.firstGeometry = circle;
    pointOnCircle.secondPoint = {line, false};
    expect(sketch.addConstraint(pointOnCircle) != kInvalidConstraintId,
           "order-independent line end must attach to circle");

    for (std::size_t step = 0; step < order.size(); ++step) {
      const auto requested = order[step];
      Constraint constraint;
      if (requested == Requested::Tangent) {
        constraint = geometryConstraint(ConstraintType::Tangent,
                                        line, circle);
      } else if (requested == Requested::Perpendicular) {
        constraint = geometryConstraint(ConstraintType::Perpendicular,
                                        projection, line);
      } else {
        constraint.type = ConstraintType::Distance;
        constraint.firstPoint = {line, true};
        constraint.secondPoint = {line, false};
        constraint.value = 25.0;
      }
      if (sketch.addConstraint(constraint) == kInvalidConstraintId) {
        std::cerr << "failed combined-constraint order " << orderIndex
                  << " at step " << step << '\n';
        fail("tangent/perpendicular/length must not depend on click order");
      }
    }

    const auto& result = sketch.lines()[*sketch.lineIndex(line)];
    expect(near(length(result), 25.0, 1e-4),
           "order-independent tangent line must be 25 mm long");
    expect(near(angleDegrees(sketch.lines()[*sketch.lineIndex(projection)],
                             result),
                90.0, 1e-3),
           "order-independent tangent line must be perpendicular");
    expect(near(std::abs(result.start.xMm), 10.0, 1e-4) &&
               near(result.end.yMm, 0.0, 1e-4),
           "order-independent line must touch the circle tangentially");
    expect(!analyzeConstraintSystem(sketch).conflicting,
           "combined tangent/perpendicular/length system must converge");
  }
}

void tangentAngleDimensionAndLengthConvergeTogether() {
  Sketch sketch;
  sketch.addLine({-40.0, -25.0}, {40.0, -25.0});
  sketch.addCircle({0.0, 0.0}, 10.0);
  sketch.addLine({-8.0, -25.0}, {-8.0, -6.0});
  const GeometryId projection = sketch.lineId(0);
  const GeometryId circle = sketch.circleId(0);
  const GeometryId line = sketch.lineId(1);

  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, projection)) !=
             kInvalidConstraintId,
         "angle-dimension lower projection must be lockable");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, circle)) !=
             kInvalidConstraintId,
         "angle-dimension circle must be lockable");
  Constraint pointOnLine;
  pointOnLine.type = ConstraintType::PointOnLine;
  pointOnLine.firstGeometry = projection;
  pointOnLine.secondPoint = {line, true};
  expect(sketch.addConstraint(pointOnLine) != kInvalidConstraintId,
         "angle-dimension line start must attach to projection");
  Constraint pointOnCircle;
  pointOnCircle.type = ConstraintType::PointOnCircle;
  pointOnCircle.firstGeometry = circle;
  pointOnCircle.secondPoint = {line, false};
  expect(sketch.addConstraint(pointOnCircle) != kInvalidConstraintId,
         "angle-dimension line end must attach to circle");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Tangent, line, circle)) !=
             kInvalidConstraintId,
         "tangency must be accepted before the 90 degree dimension");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Angle, projection, line,
                                90.0)) != kInvalidConstraintId,
         "90 degree angle dimension must preserve tangency");

  Constraint size;
  size.type = ConstraintType::Distance;
  size.firstPoint = {line, true};
  size.secondPoint = {line, false};
  size.value = 25.0;
  expect(sketch.addConstraint(size) != kInvalidConstraintId,
         "25 mm dimension must preserve angle and tangency");

  const auto& result = sketch.lines()[*sketch.lineIndex(line)];
  expect(near(length(result), 25.0, 1e-4),
         "angle-dimension tangent line must be 25 mm");
  expect(near(angleDegrees(sketch.lines()[*sketch.lineIndex(projection)],
                           result),
              90.0, 1e-3),
         "angle dimension must remain 90 degrees");
  expect(near(std::abs(result.start.xMm), 10.0, 1e-4) &&
             near(result.end.yMm, 0.0, 1e-4),
         "angle-dimension line must remain tangent");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "tangent/angle/length system must converge");
}

void tangencyFollowsDraggedLineWhenCircleCenterIsAxisConstrained() {
  Sketch sketch;
  sketch.addLine({-50.0, -25.0}, {20.0, -25.0});
  sketch.addCircle({0.0, 0.0}, 10.0);
  sketch.addLine({-20.0, -25.0}, {-8.0, -6.0});
  const GeometryId projection = sketch.lineId(0);
  const GeometryId circle = sketch.circleId(0);
  const GeometryId line = sketch.lineId(1);

  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, projection)) !=
             kInvalidConstraintId,
         "drag-tangent projection must be lockable");
  Constraint centerOnYAxis;
  centerOnYAxis.type = ConstraintType::PointOnYAxis;
  centerOnYAxis.secondPoint.circleId = circle;
  expect(sketch.addConstraint(centerOnYAxis) != kInvalidConstraintId,
         "circle center must attach to the Y axis");
  Constraint baseOnProjection;
  baseOnProjection.type = ConstraintType::PointOnLine;
  baseOnProjection.firstGeometry = projection;
  baseOnProjection.secondPoint = {line, true};
  expect(sketch.addConstraint(baseOnProjection) != kInvalidConstraintId,
         "tangent base must attach to the lower projection");
  Constraint endOnCircle;
  endOnCircle.type = ConstraintType::PointOnCircle;
  endOnCircle.firstGeometry = circle;
  endOnCircle.secondPoint = {line, false};
  expect(sketch.addConstraint(endOnCircle) != kInvalidConstraintId,
         "tangent endpoint must attach to the circle");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Tangent, line, circle)) !=
             kInvalidConstraintId,
         "axis-constrained circle must accept line tangency");

  expect(sketch.translatePoint({line, true}, -10.0, 0.0),
         "tangent line base must remain draggable");
  const auto solved = BasicSketchSolver::solveStable(sketch);
  expect(solved.converged,
         "tangency must reconverge after dragging the line base");
  const auto& result = sketch.lines()[*sketch.lineIndex(line)];
  const auto& resultCircle = sketch.circles()[*sketch.circleIndex(circle)];
  const double dx = result.end.xMm - result.start.xMm;
  const double dy = result.end.yMm - result.start.yMm;
  const double lineLengthSquared = dx * dx + dy * dy;
  const double t = std::clamp(
      ((resultCircle.center.xMm - result.start.xMm) * dx +
       (resultCircle.center.yMm - result.start.yMm) * dy) /
          lineLengthSquared,
      0.0, 1.0);
  const Point contact{result.start.xMm + dx * t,
                      result.start.yMm + dy * t};
  expect(near(std::hypot(contact.xMm - resultCircle.center.xMm,
                         contact.yMm - resultCircle.center.yMm),
              resultCircle.radiusMm, 1e-4),
         "dragged line must have exactly one tangent contact");
  expect(near(result.start.yMm, -25.0, 1e-4),
         "dragged base must remain on the projected line");
  expect(near(resultCircle.center.xMm, 0.0, 1e-6),
         "tangency must not move a circle center constrained to the Y axis");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "dragged tangent system must remain fully satisfied");
}

void lengthUsesRemainingAxisDegreeOfFreedomAfterTangency() {
  Sketch sketch;
  sketch.addLine({-50.0, -25.0}, {20.0, -25.0});
  sketch.addLine({0.0, -50.0}, {0.0, 50.0});
  sketch.addCircle({0.0, 8.0}, 10.0);
  sketch.addLine({-10.0, -25.0}, {-10.0, 8.0});
  const GeometryId projection = sketch.lineId(0);
  const GeometryId verticalProjection = sketch.lineId(1);
  const GeometryId circle = sketch.circleId(0);
  const GeometryId line = sketch.lineId(2);

  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock, projection)) !=
             kInvalidConstraintId,
         "dimension carrier must be lockable");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Lock,
                                verticalProjection)) != kInvalidConstraintId,
         "vertical center projection must be lockable");
  Constraint centerOnProjection;
  centerOnProjection.type = ConstraintType::PointOnLine;
  centerOnProjection.firstGeometry = verticalProjection;
  centerOnProjection.secondPoint.circleId = circle;
  expect(sketch.addConstraint(centerOnProjection) != kInvalidConstraintId,
         "circle center must retain projected-line membership");
  Constraint centerOnYAxis;
  centerOnYAxis.type = ConstraintType::PointOnYAxis;
  centerOnYAxis.secondPoint.circleId = circle;
  expect(sketch.addConstraint(centerOnYAxis) != kInvalidConstraintId,
         "dimension circle center must retain one free axis direction");
  Constraint baseOnProjection;
  baseOnProjection.type = ConstraintType::PointOnLine;
  baseOnProjection.firstGeometry = projection;
  baseOnProjection.secondPoint = {line, true};
  expect(sketch.addConstraint(baseOnProjection) != kInvalidConstraintId,
         "dimension line base must attach to projection");
  Constraint endOnCircle;
  endOnCircle.type = ConstraintType::PointOnCircle;
  endOnCircle.firstGeometry = circle;
  endOnCircle.secondPoint = {line, false};
  expect(sketch.addConstraint(endOnCircle) != kInvalidConstraintId,
         "dimension line endpoint must attach to circle");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Tangent, line, circle)) !=
             kInvalidConstraintId,
         "tangency must be preserved before adding a size");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Perpendicular,
                                projection, line)) != kInvalidConstraintId,
         "perpendicularity must be preserved before adding a size");
  const auto beforeSize = analyzeConstraintSystem(sketch);
  expect(!beforeSize.conflicting && !beforeSize.fullyConstrained &&
             beforeSize.degreesOfFreedom > 0,
         "tangent and perpendicular must not falsely report a fully "
         "determined element while axis motion remains");
  expect(sketch.addConstraint(
             geometryConstraint(ConstraintType::Length, line,
                                kInvalidGeometryId, 25.0)) !=
             kInvalidConstraintId,
         "25 mm size must use the circle center's remaining axis freedom");

  const auto& result = sketch.lines()[*sketch.lineIndex(line)];
  const auto& resultCircle = sketch.circles()[*sketch.circleIndex(circle)];
  expect(near(length(result), 25.0, 1e-4),
         "sized tangent line must be 25 mm long");
  expect(near(resultCircle.center.xMm, 0.0, 1e-7),
         "sizing must preserve the circle center on the Y axis");
  expect(near(resultCircle.center.yMm, 0.0, 1e-4),
         "sizing must move the circle only along its remaining axis freedom");
  expect(near(angleDegrees(sketch.lines()[*sketch.lineIndex(projection)],
                           result),
              90.0, 1e-4),
         "sizing must preserve perpendicularity");
  expect(!analyzeConstraintSystem(sketch).conflicting,
         "tangent/perpendicular/length system must remain satisfied");

  // The same chain must be direction-independent. This is the orientation
  // produced when the user starts the line on the upper projected face edge
  // and finishes it at the Circle's right-hand tangent point.
  Sketch reversed;
  reversed.addLine({-50.0, 40.0}, {50.0, 40.0});
  reversed.addLine({0.0, -50.0}, {0.0, 50.0});
  reversed.addCircle({0.0, 0.0}, 10.0);
  reversed.addLine({10.0, 40.0}, {10.0, 0.0});
  const GeometryId reversedProjection = reversed.lineId(0);
  const GeometryId reversedVerticalProjection = reversed.lineId(1);
  const GeometryId reversedCircle = reversed.circleId(0);
  const GeometryId reversedLine = reversed.lineId(2);

  expect(reversed.addConstraint(
             geometryConstraint(ConstraintType::Lock,
                                reversedProjection)) != kInvalidConstraintId,
         "upper dimension carrier must be lockable");
  expect(reversed.addConstraint(
             geometryConstraint(ConstraintType::Lock,
                                reversedVerticalProjection)) !=
             kInvalidConstraintId,
         "reversed center projection must be lockable");
  Constraint reversedCenterOnProjection;
  reversedCenterOnProjection.type = ConstraintType::PointOnLine;
  reversedCenterOnProjection.firstGeometry = reversedVerticalProjection;
  reversedCenterOnProjection.secondPoint.circleId = reversedCircle;
  expect(reversed.addConstraint(reversedCenterOnProjection) !=
             kInvalidConstraintId,
         "reversed circle center must remain on the vertical projection");
  Constraint reversedCenterOnYAxis;
  reversedCenterOnYAxis.type = ConstraintType::PointOnYAxis;
  reversedCenterOnYAxis.secondPoint.circleId = reversedCircle;
  expect(reversed.addConstraint(reversedCenterOnYAxis) !=
             kInvalidConstraintId,
         "reversed circle center must retain its datum-axis relation");
  Constraint reversedBaseOnProjection;
  reversedBaseOnProjection.type = ConstraintType::PointOnLine;
  reversedBaseOnProjection.firstGeometry = reversedProjection;
  reversedBaseOnProjection.secondPoint = {reversedLine, true};
  expect(reversed.addConstraint(reversedBaseOnProjection) !=
             kInvalidConstraintId,
         "reversed line start must attach to the upper projection");
  Constraint reversedEndOnCircle;
  reversedEndOnCircle.type = ConstraintType::PointOnCircle;
  reversedEndOnCircle.firstGeometry = reversedCircle;
  reversedEndOnCircle.secondPoint = {reversedLine, false};
  expect(reversed.addConstraint(reversedEndOnCircle) != kInvalidConstraintId,
         "reversed line end must attach to the circle");
  expect(reversed.addConstraint(
             geometryConstraint(ConstraintType::Tangent, reversedLine,
                                reversedCircle)) != kInvalidConstraintId,
         "reversed line must become tangent");
  expect(reversed.addConstraint(
             geometryConstraint(ConstraintType::Perpendicular,
                                reversedProjection, reversedLine)) !=
             kInvalidConstraintId,
         "reversed tangent line must remain perpendicular");
  expect(reversed.addConstraint(
             geometryConstraint(ConstraintType::Length, reversedLine,
                                kInvalidGeometryId, 25.0)) !=
             kInvalidConstraintId,
         "reversed tangent line must accept a 25 mm size");

  const auto& reversedResult =
      reversed.lines()[*reversed.lineIndex(reversedLine)];
  const auto& reversedResultCircle =
      reversed.circles()[*reversed.circleIndex(reversedCircle)];
  expect(near(length(reversedResult), 25.0, 1e-4),
         "reversed tangent line must retain its requested size");
  expect(near(reversedResultCircle.center.xMm, 0.0, 1e-7) &&
             near(reversedResultCircle.center.yMm, 15.0, 1e-4),
         "reversed sizing must move the circle along its free datum axis");
  expect(!analyzeConstraintSystem(reversed).conflicting,
         "reversed tangent/perpendicular/length system must remain satisfied");
}

void bezierPointsParticipateInSketchMechanics() {
  Sketch sketch;
  sketch.clear();
  sketch.beginDeltaJournal();
  sketch.addBezier({0.0, 0.0}, {2.0, -4.0}, {8.0, -4.0}, {10.0, 0.0});
  const auto bezierId = sketch.bezierId(0);
  expect(bezierId != kInvalidGeometryId, "Bezier must receive a stable ID");
  const auto creation = sketch.finishDeltaJournal();
  expect(sketch.applyDelta(creation, false) &&
             sketch.beziers().empty() && sketch.applyDelta(creation, true) &&
             sketch.bezierId(0) == bezierId,
         "Bezier creation must round-trip through the undo journal");

  PointReference handle;
  handle.bezierId = bezierId;
  handle.bezierPoint = 1;
  expect(sketch.translatePoint(handle, 1.0, 2.0),
         "Bezier control point must be movable");
  expect(near(sketch.beziers()[0].points[1].xMm, 3.0) &&
             near(sketch.beziers()[0].points[1].yMm, -2.0),
         "moving one control point must update that point");

  PointReference endpoint;
  endpoint.bezierId = bezierId;
  endpoint.bezierPoint = 3;
  sketch.addLine({10.0, 0.0}, {10.0, 10.0});
  const auto lineId = sketch.lineId(0);
  Constraint coincident;
  coincident.type = ConstraintType::Coincident;
  coincident.firstPoint = endpoint;
  coincident.secondPoint = {lineId, true};
  expect(sketch.addConstraint(coincident) != kInvalidConstraintId,
         "Bezier endpoint must accept coincidence constraints");

  Dimension dimension;
  dimension.kind = DimensionKind::PointDistance;
  dimension.firstPoint = handle;
  dimension.secondPoint = endpoint;
  dimension.valueMm = 8.0;
  sketch.storeDimension(dimension);
  expect(sketch.dimensions().size() == 1 &&
             sketch.dimensions()[0].id != kInvalidDimensionId,
         "Bezier points must accept point dimensions");

  sketch.addLine({10.0, 10.0}, {0.0, 10.0});
  sketch.addLine({0.0, 10.0}, {0.0, 0.0});
  expect(sketch.isClosed(),
         "Bezier endpoints must close a mixed curve/line contour");

  const auto bezierIndex = sketch.bezierIndex(bezierId);
  expect(bezierIndex.has_value(), "Bezier ID must remain resolvable");
  sketch.removeBezier(*bezierIndex);
  expect(!sketch.bezierIndex(bezierId) && sketch.dimensions().empty() &&
             std::none_of(sketch.constraints().begin(),
                          sketch.constraints().end(),
                          [bezierId](const Constraint& item) {
                            return item.firstPoint.bezierId == bezierId ||
                                   item.secondPoint.bezierId == bezierId;
                          }),
         "deleting a Bezier must remove dependent references");
}

}  // namespace

int main() {
  drivingDimensionsRemainStableAfterDragging();
  centerReferencesMoveWholeObjects();
  dimensionEditingPreservesKind();
  angleBranchesAndCompositeGeometryRemainStable();
  relationalConstraintsRespectDrivingSizesAndComposites();
  tangencyAndPointRelationsStayValidAfterMovement();
  lineArcTangencySurvivesMovement();
  rectangleSideArcDragIsStable();
  secondArcEndpointCoincidenceReshapesWithoutConflict();
  pointOnCircleSurvivesFurtherSketchEdits();
  deletionStressHasNoDanglingReferenceCrash();
  parallelLineDistanceKeepsRectangleRigid();
  dimensionAndPerpendicularWorkAgainstProjectedCarriers();
  dimensionAndTangencyWorkAfterPerpendicularProjection();
  tangentAngleDimensionAndLengthConvergeTogether();
  tangentPerpendicularLengthAreOrderIndependent();
  tangencyFollowsDraggedLineWhenCircleCenterIsAxisConstrained();
  lengthUsesRemainingAxisDegreeOfFreedomAfterTangency();
  bezierPointsParticipateInSketchMechanics();
  return EXIT_SUCCESS;
}

#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace solidar::sketch;

namespace {

void require(bool condition,
             const char* message) {
  if (!condition) {
    std::cerr << "FAILED: "
              << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void transactionalConstraintDoesNotBreakOldOne() {
  Sketch sketch;
  sketch.addLine(
      {0.0, 0.0},
      {20.0, 0.0});

  const GeometryId id =
      sketch.lineId(0);

  Constraint firstLength;
  firstLength.type =
      ConstraintType::Length;
  firstLength.firstGeometry = id;
  firstLength.value = 20.0;

  require(
      sketch.addConstraint(firstLength) !=
          kInvalidConstraintId,
      "first length must be accepted");

  Constraint conflictingLength;
  conflictingLength.type =
      ConstraintType::Length;
  conflictingLength.firstGeometry = id;
  conflictingLength.value = 30.0;

  require(
      sketch.addConstraint(
          conflictingLength) ==
          kInvalidConstraintId,
      "conflicting new length must be rejected");

  require(
      sketch.constraints().size() == 1,
      "rejected constraint must not remain stored");

  require(
      std::abs(
          std::hypot(
              sketch.lines()[0].end.xMm -
                  sketch.lines()[0].start.xMm,
              sketch.lines()[0].end.yMm -
                  sketch.lines()[0].start.yMm) -
          20.0) <= 1e-6,
      "old valid geometry must be restored");

  require(
      !analyzeConstraintSystem(sketch).conflicting,
      "old system must remain valid");
}

void pointOnLineSurvivesLaterDimension() {
  Sketch sketch;

  sketch.addLine(
      {0.0, 20.0},
      {50.0, 20.0});

  sketch.addLine(
      {15.0, 20.0},
      {15.0, 5.0});

  const GeometryId carrier =
      sketch.lineId(0);

  const GeometryId child =
      sketch.lineId(1);

  Constraint onLine;
  onLine.type =
      ConstraintType::PointOnLine;
  onLine.firstGeometry = carrier;
  onLine.secondPoint =
      PointReference{child, true};

  require(
      sketch.addConstraint(onLine) !=
          kInvalidConstraintId,
      "PointOnLine must be accepted");

  Constraint length;
  length.type =
      ConstraintType::Length;
  length.firstGeometry = child;
  length.value = 12.0;

  require(
      sketch.addConstraint(length) !=
          kInvalidConstraintId,
      "compatible later size must be accepted");

  const auto report =
      analyzeConstraintSystem(sketch);

  require(
      !report.conflicting,
      "later size must not sacrifice PointOnLine");

  require(
      std::abs(
          sketch.lines()[1].start.yMm -
          20.0) <= 1e-5,
      "line start must remain on carrier");
}

void dofUsesConstraintRank() {
  Sketch sketch;

  sketch.addLine(
      {0.0, 0.0},
      {20.0, 4.0});

  const auto freeReport =
      analyzeConstraintSystem(sketch);

  require(
      freeReport.degreesOfFreedom == 4,
      "free standalone line must have 4 DOF");

  Constraint horizontal;
  horizontal.type =
      ConstraintType::Horizontal;
  horizontal.firstGeometry =
      sketch.lineId(0);

  require(
      sketch.addConstraint(horizontal) !=
          kInvalidConstraintId,
      "horizontal must be accepted");

  const auto horizontalReport =
      analyzeConstraintSystem(sketch);

  require(
      horizontalReport.degreesOfFreedom == 3,
      "horizontal removes one independent DOF");

  Constraint length;
  length.type =
      ConstraintType::Length;
  length.firstGeometry =
      sketch.lineId(0);
  length.value = 20.0;

  require(
      sketch.addConstraint(length) !=
          kInvalidConstraintId,
      "length must be accepted");

  const auto sizedReport =
      analyzeConstraintSystem(sketch);

  require(
      sizedReport.degreesOfFreedom == 2,
      "length removes one more independent DOF");
}

void incompatibleDatumAxisDoesNotReenterSolver() {
  Sketch sketch;
  sketch.addLine(
      {10.0, 0.0},
      {30.0, 0.0});

  const PointReference start{
      sketch.lineId(0), true};

  // Explicit IDs model project loading, where legacy or hand-edited files
  // remain permissive and conflicting constraints are diagnosed instead of
  // transactionally rejected.
  Constraint onYAxis;
  onYAxis.id = 1;
  onYAxis.type = ConstraintType::PointOnYAxis;
  onYAxis.secondPoint = start;
  require(
      sketch.addConstraint(onYAxis) == 1,
      "explicit Y-axis constraint must load");

  Constraint distanceFromYAxis;
  distanceFromYAxis.id = 2;
  distanceFromYAxis.type = ConstraintType::DistanceX;
  distanceFromYAxis.firstPoint.origin = true;
  distanceFromYAxis.secondPoint = start;
  distanceFromYAxis.value = 10.0;
  require(
      sketch.addConstraint(distanceFromYAxis) == 2,
      "conflicting explicit axis distance must load without recursion");

  const auto solved =
      BasicSketchSolver::solveStable(sketch);
  require(
      !solved.converged,
      "incompatible datum constraints must be reported as conflicting");
  require(
      solved.violatedConstraints > 0,
      "incompatible datum constraints must retain a diagnostic");
}

}  // namespace

int main() {
  transactionalConstraintDoesNotBreakOldOne();
  pointOnLineSurvivesLaterDimension();
  dofUsesConstraintRank();
  incompatibleDatumAxisDoesNotReenterSolver();
  return EXIT_SUCCESS;
}

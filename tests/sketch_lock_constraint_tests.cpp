#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace solidar::sketch;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

ConstraintId lockGeometry(Sketch& sketch, GeometryId id) {
  Constraint lock;
  lock.type = ConstraintType::Lock;
  lock.firstGeometry = id;
  return sketch.addConstraint(lock);
}

void lockedLineIsImmutable() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {20.0, 0.0});

  const GeometryId id = sketch.lineId(0);
  const auto element = sketch.lines()[0].elementId;

  require(lockGeometry(sketch, id) != kInvalidConstraintId,
          "Lock must be accepted");
  require(sketch.isGeometryLocked(id),
          "line must report locked");

  sketch.translateElement(element, 10.0, 5.0);
  require(std::abs(sketch.lines()[0].start.xMm) <= 1e-9 &&
              std::abs(sketch.lines()[0].start.yMm) <= 1e-9,
          "locked element must not translate");

  require(!sketch.setLineLengthById(id, 30.0),
          "locked length edit must be rejected");
  require(std::abs(sketch.lines()[0].end.xMm - 20.0) <= 1e-9,
          "locked line length must remain unchanged");

  sketch.removeElement(element);
  require(sketch.lines().size() == 1,
          "locked element must not be deleted");
}

void oneLockFreezesWholeCompositeElement() {
  Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {40.0, 20.0});
  require(sketch.lines().size() == 4,
          "rectangle must have four lines");

  require(lockGeometry(sketch, sketch.lineId(0)) !=
              kInvalidConstraintId,
          "rectangle Lock must be accepted");

  for (std::size_t i = 0; i < sketch.lines().size(); ++i)
    require(sketch.isGeometryLocked(sketch.lineId(i)),
            "one Lock must freeze whole element");

  const auto before = sketch.lines();
  sketch.translateElement(
      sketch.lines()[0].elementId, 15.0, -8.0);

  for (std::size_t i = 0; i < sketch.lines().size(); ++i) {
    require(std::abs(sketch.lines()[i].start.xMm -
                     before[i].start.xMm) <= 1e-9 &&
                std::abs(sketch.lines()[i].start.yMm -
                         before[i].start.yMm) <= 1e-9 &&
                std::abs(sketch.lines()[i].end.xMm -
                         before[i].end.xMm) <= 1e-9 &&
                std::abs(sketch.lines()[i].end.yMm -
                         before[i].end.yMm) <= 1e-9,
            "locked composite must remain unchanged");
  }
}

void unlockedPointCanReferenceLockedGeometry() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {40.0, 0.0});
  sketch.addLine({10.0, 8.0}, {10.0, 18.0});

  const GeometryId carrier = sketch.lineId(0);
  const GeometryId child = sketch.lineId(1);

  require(lockGeometry(sketch, carrier) != kInvalidConstraintId,
          "carrier Lock must be accepted");

  Constraint onLine;
  onLine.type = ConstraintType::PointOnLine;
  onLine.firstGeometry = carrier;
  onLine.secondPoint = PointReference{child, true};

  require(sketch.addConstraint(onLine) != kInvalidConstraintId,
          "PointOnLine to locked carrier must be accepted");

  require(std::abs(sketch.lines()[0].start.xMm) <= 1e-9 &&
              std::abs(sketch.lines()[0].start.yMm) <= 1e-9 &&
              std::abs(sketch.lines()[0].end.xMm - 40.0) <= 1e-9 &&
              std::abs(sketch.lines()[0].end.yMm) <= 1e-9,
          "locked carrier must not move");

  require(std::abs(sketch.lines()[1].start.yMm) <= 1e-6,
          "unlocked endpoint must move onto locked carrier");
}

void lockConsumesGeometryDof() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {20.0, 7.0});

  const auto freeState = analyzeConstraintSystem(sketch);
  require(freeState.degreesOfFreedom == 4,
          "free line must have four DOF");

  require(lockGeometry(sketch, sketch.lineId(0)) !=
              kInvalidConstraintId,
          "Lock must be accepted");

  const auto lockedState = analyzeConstraintSystem(sketch);
  require(!lockedState.conflicting,
          "Lock must not be conflicting");
  require(lockedState.degreesOfFreedom == 0,
          "locked line must have zero DOF");
}

void removingLockRestoresEditability() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {20.0, 0.0});

  const GeometryId id = sketch.lineId(0);
  const auto element = sketch.lines()[0].elementId;
  const ConstraintId lock = lockGeometry(sketch, id);

  require(lock != kInvalidConstraintId,
          "Lock must exist");
  require(sketch.removeConstraint(lock),
          "Lock must be removable");
  require(!sketch.isGeometryLocked(id),
          "geometry must unlock");

  sketch.translateElement(element, 5.0, 3.0);
  require(std::abs(sketch.lines()[0].start.xMm - 5.0) <= 1e-9 &&
              std::abs(sketch.lines()[0].start.yMm - 3.0) <= 1e-9,
          "unlocked geometry must move again");
}


void coupledExternalRectangleGapsBeforeConstraintInsert() {
  Sketch sketch;

  sketch.addLine({0.0, 0.0}, {0.0, -50.0});
  sketch.addLine({100.0, 0.0}, {100.0, -50.0});

  const GeometryId leftProjection = sketch.lineId(0);
  const GeometryId rightProjection = sketch.lineId(1);

  require(lockGeometry(sketch, leftProjection) !=
              kInvalidConstraintId,
          "left reference must lock");
  require(lockGeometry(sketch, rightProjection) !=
              kInvalidConstraintId,
          "right reference must lock");

  sketch.addRectangle({20.0, 0.0}, {80.0, -30.0});

  const GeometryId top = sketch.lineId(2);
  const PointReference rectLeft{top, true};
  const PointReference rectRight{top, false};
  const PointReference leftRef{leftProjection, true};
  const PointReference rightRef{rightProjection, true};

  // First dimension: right gap = 7 mm.
  require(sketch.setPointDistanceX(
              rightRef, rectRight, 7.0),
          "first right gap setter");

  Constraint rightGap;
  rightGap.type = ConstraintType::DistanceX;
  rightGap.firstPoint = rightRef;
  rightGap.secondPoint = rectRight;
  rightGap.value = 7.0;

  require(sketch.addConstraint(rightGap) !=
              kInvalidConstraintId,
          "first right gap constraint");

  // Second dimension is intentionally entered in the opposite order.
  // This mirrors AutoDimension: geometry is changed BEFORE the new
  // constraint is appended to constraints_.
  require(sketch.setPointDistanceX(
              rectLeft, leftRef, 7.0),
          "second left gap setter before insert");

  const auto leftAfterSetter =
      sketch.referencedPoint(rectLeft);
  const auto rightAfterSetter =
      sketch.referencedPoint(rectRight);

  require(leftAfterSetter && rightAfterSetter,
          "rectangle references remain valid");
  require(std::abs(leftAfterSetter->xMm - 7.0) <= 1e-7,
          "left side must be 7 mm after second setter");
  require(std::abs(rightAfterSetter->xMm - 93.0) <= 1e-7,
          "right side must remain 7 mm after second setter");

  Constraint leftGap;
  leftGap.type = ConstraintType::DistanceX;
  leftGap.firstPoint = rectLeft;
  leftGap.secondPoint = leftRef;
  leftGap.value = 7.0;

  require(sketch.addConstraint(leftGap) !=
              kInvalidConstraintId,
          "second left gap constraint must be accepted");

  const auto leftFinal =
      sketch.referencedPoint(rectLeft);
  const auto rightFinal =
      sketch.referencedPoint(rectRight);

  require(leftFinal && rightFinal,
          "final rectangle references remain valid");
  require(std::abs(leftFinal->xMm - 7.0) <= 1e-7,
          "final left gap stays 7 mm");
  require(std::abs(rightFinal->xMm - 93.0) <= 1e-7,
          "final right gap stays 7 mm");
}


void constraintFirstTwoExternalGaps() {
  Sketch sketch;

  sketch.addLine({0.0, 0.0}, {0.0, -50.0});
  sketch.addLine({100.0, 0.0}, {100.0, -50.0});

  const GeometryId leftProjection =
      sketch.lineId(0);
  const GeometryId rightProjection =
      sketch.lineId(1);

  require(lockGeometry(
              sketch,
              leftProjection) !=
              kInvalidConstraintId,
          "left projection lock");

  require(lockGeometry(
              sketch,
              rightProjection) !=
              kInvalidConstraintId,
          "right projection lock");

  sketch.addRectangle(
      {20.0, 0.0},
      {80.0, -30.0});

  const GeometryId top =
      sketch.lineId(2);

  PointReference rectLeft{top, true};
  PointReference rectRight{top, false};
  PointReference leftRef{
      leftProjection, true};
  PointReference rightRef{
      rightProjection, true};

  Constraint rightGap;
  rightGap.type =
      ConstraintType::DistanceX;
  rightGap.firstPoint = rightRef;
  rightGap.secondPoint = rectRight;
  rightGap.value = 7.0;

  require(
      sketch.addConstraint(rightGap) !=
          kInvalidConstraintId,
      "first external gap must be accepted");

  Constraint leftGap;
  leftGap.type =
      ConstraintType::DistanceX;
  leftGap.firstPoint = rectLeft;
  leftGap.secondPoint = leftRef;
  leftGap.value = 7.0;

  require(
      sketch.addConstraint(leftGap) !=
          kInvalidConstraintId,
      "second external gap must be accepted");

  const auto leftPoint =
      sketch.referencedPoint(rectLeft);
  const auto rightPoint =
      sketch.referencedPoint(rectRight);

  require(leftPoint && rightPoint,
          "rectangle references remain valid");

  require(
      std::abs(leftPoint->xMm - 7.0) <=
          1e-7,
      "left gap must be exactly 7 mm");

  require(
      std::abs(rightPoint->xMm - 93.0) <=
          1e-7,
      "right gap must stay exactly 7 mm");

  require(
      std::abs(
          (rightPoint->xMm -
           leftPoint->xMm) -
          86.0) <= 1e-7,
      "free rectangle width must become 86 mm");
}

}  // namespace

int main() {
  lockedLineIsImmutable();
  oneLockFreezesWholeCompositeElement();
  unlockedPointCanReferenceLockedGeometry();
  lockConsumesGeometryDof();
  removingLockRestoresEditability();
  constraintFirstTwoExternalGaps();
  coupledExternalRectangleGapsBeforeConstraintInsert();
  return EXIT_SUCCESS;
}

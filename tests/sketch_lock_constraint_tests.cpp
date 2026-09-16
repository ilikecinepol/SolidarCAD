#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace solidar::sketch;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

#define CHECK(condition) require((condition), #condition)

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
  require(sketch.isGeometryLocked(id), "line must report locked");

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
  require(sketch.lines().size() == 4, "rectangle must have four lines");

  require(lockGeometry(sketch, sketch.lineId(0)) != kInvalidConstraintId,
          "rectangle Lock must be accepted");

  for (std::size_t i = 0; i < sketch.lines().size(); ++i)
    require(sketch.isGeometryLocked(sketch.lineId(i)),
            "one Lock must freeze whole element");

  const auto before = sketch.lines();
  sketch.translateElement(sketch.lines()[0].elementId, 15.0, -8.0);

  for (std::size_t i = 0; i < sketch.lines().size(); ++i) {
    require(std::abs(sketch.lines()[i].start.xMm - before[i].start.xMm) <=
                    1e-9 &&
                std::abs(sketch.lines()[i].start.yMm - before[i].start.yMm) <=
                    1e-9 &&
                std::abs(sketch.lines()[i].end.xMm - before[i].end.xMm) <=
                    1e-9 &&
                std::abs(sketch.lines()[i].end.yMm - before[i].end.yMm) <=
                    1e-9,
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
  require(freeState.degreesOfFreedom == 4, "free line must have four DOF");

  require(lockGeometry(sketch, sketch.lineId(0)) != kInvalidConstraintId,
          "Lock must be accepted");

  const auto lockedState = analyzeConstraintSystem(sketch);
  require(!lockedState.conflicting, "Lock must not be conflicting");
  require(lockedState.degreesOfFreedom == 0,
          "locked line must have zero DOF");
}

void removingLockRestoresEditability() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {20.0, 0.0});

  const GeometryId id = sketch.lineId(0);
  const auto element = sketch.lines()[0].elementId;
  const ConstraintId lock = lockGeometry(sketch, id);

  require(lock != kInvalidConstraintId, "Lock must exist");
  require(sketch.removeConstraint(lock), "Lock must be removable");
  require(!sketch.isGeometryLocked(id), "geometry must unlock");

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

  require(lockGeometry(sketch, leftProjection) != kInvalidConstraintId,
          "left reference must lock");
  require(lockGeometry(sketch, rightProjection) != kInvalidConstraintId,
          "right reference must lock");

  sketch.addRectangle({20.0, 0.0}, {80.0, -30.0});

  const GeometryId top = sketch.lineId(2);
  const PointReference rectLeft{top, true};
  const PointReference rectRight{top, false};
  const PointReference leftRef{leftProjection, true};
  const PointReference rightRef{rightProjection, true};

  require(sketch.setPointDistanceX(rightRef, rectRight, 7.0),
          "first right gap setter");

  Constraint rightGap;
  rightGap.type = ConstraintType::DistanceX;
  rightGap.firstPoint = rightRef;
  rightGap.secondPoint = rectRight;
  rightGap.value = 7.0;
  require(sketch.addConstraint(rightGap) != kInvalidConstraintId,
          "first right gap constraint");

  require(sketch.setPointDistanceX(rectLeft, leftRef, 7.0),
          "second left gap setter before insert");

  const auto leftAfterSetter = sketch.referencedPoint(rectLeft);
  const auto rightAfterSetter = sketch.referencedPoint(rectRight);
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
  require(sketch.addConstraint(leftGap) != kInvalidConstraintId,
          "second left gap constraint must be accepted");

  const auto leftFinal = sketch.referencedPoint(rectLeft);
  const auto rightFinal = sketch.referencedPoint(rectRight);
  require(leftFinal && rightFinal, "final rectangle references remain valid");
  require(std::abs(leftFinal->xMm - 7.0) <= 1e-7,
          "final left gap stays 7 mm");
  require(std::abs(rightFinal->xMm - 93.0) <= 1e-7,
          "final right gap stays 7 mm");
}

void constraintFirstTwoExternalGaps() {
  Sketch sketch;

  sketch.addLine({0.0, 0.0}, {0.0, -50.0});
  sketch.addLine({100.0, 0.0}, {100.0, -50.0});

  const GeometryId leftProjection = sketch.lineId(0);
  const GeometryId rightProjection = sketch.lineId(1);

  require(lockGeometry(sketch, leftProjection) != kInvalidConstraintId,
          "left projection lock");
  require(lockGeometry(sketch, rightProjection) != kInvalidConstraintId,
          "right projection lock");

  sketch.addRectangle({20.0, 0.0}, {80.0, -30.0});
  const GeometryId top = sketch.lineId(2);

  PointReference rectLeft{top, true};
  PointReference rectRight{top, false};
  PointReference leftRef{leftProjection, true};
  PointReference rightRef{rightProjection, true};

  Constraint rightGap;
  rightGap.type = ConstraintType::DistanceX;
  rightGap.firstPoint = rightRef;
  rightGap.secondPoint = rectRight;
  rightGap.value = 7.0;
  require(sketch.addConstraint(rightGap) != kInvalidConstraintId,
          "first external gap must be accepted");

  Constraint leftGap;
  leftGap.type = ConstraintType::DistanceX;
  leftGap.firstPoint = rectLeft;
  leftGap.secondPoint = leftRef;
  leftGap.value = 7.0;
  require(sketch.addConstraint(leftGap) != kInvalidConstraintId,
          "second external gap must be accepted");

  const auto leftPoint = sketch.referencedPoint(rectLeft);
  const auto rightPoint = sketch.referencedPoint(rectRight);
  require(leftPoint && rightPoint, "rectangle references remain valid");
  require(std::abs(leftPoint->xMm - 7.0) <= 1e-7,
          "left gap must be exactly 7 mm");
  require(std::abs(rightPoint->xMm - 93.0) <= 1e-7,
          "right gap must stay exactly 7 mm");
  require(std::abs((rightPoint->xMm - leftPoint->xMm) - 86.0) <= 1e-7,
          "free rectangle width must become 86 mm");
}

enum class GapAxis { X, Y };

struct GapFixture {
  Sketch sketch;
  GeometryId projection{kInvalidGeometryId};
  PointReference rectangleLow;
  PointReference rectangleHigh;
  PointReference projectionLow;
  PointReference projectionHigh;
  std::size_t rectangleFirstIndex{};
  std::vector<Constraint> structuralConstraints;
  Line projectedLine;
};

GapFixture makeSingleProjectionGapFixture(GapAxis axis) {
  GapFixture fixture;
  if (axis == GapAxis::X)
    fixture.sketch.addLine({0.0, 0.0}, {100.0, 0.0});
  else
    fixture.sketch.addLine({0.0, 0.0}, {0.0, 100.0});

  fixture.projection = fixture.sketch.lineId(0);
  fixture.sketch.setElementDashed(fixture.sketch.lines()[0].elementId, true);
  require(lockGeometry(fixture.sketch, fixture.projection) !=
              kInvalidConstraintId,
          "projected line must lock");
  fixture.projectedLine = fixture.sketch.lines()[0];

  fixture.rectangleFirstIndex = fixture.sketch.lines().size();
  const std::size_t structuralBegin = fixture.sketch.constraints().size();
  if (axis == GapAxis::X)
    fixture.sketch.addRectangle({20.0, 0.0}, {80.0, -30.0});
  else
    fixture.sketch.addRectangle({0.0, 20.0}, {30.0, 80.0});
  fixture.structuralConstraints.assign(
      fixture.sketch.constraints().begin() + structuralBegin,
      fixture.sketch.constraints().end());

  const GeometryId firstRectangleLine =
      fixture.sketch.lineId(fixture.rectangleFirstIndex);
  fixture.rectangleLow = PointReference{firstRectangleLine, true};
  fixture.rectangleHigh =
      axis == GapAxis::X
          ? PointReference{firstRectangleLine, false}
          : PointReference{fixture.sketch.lineId(
                               fixture.rectangleFirstIndex + 2),
                           false};
  fixture.projectionLow = PointReference{fixture.projection, true};
  fixture.projectionHigh = PointReference{fixture.projection, false};
  return fixture;
}

Constraint alignedGap(PointReference projection, PointReference rectangle) {
  Constraint gap;
  gap.type = ConstraintType::Distance;
  gap.firstPoint = projection;
  gap.secondPoint = rectangle;
  gap.value = 7.0;
  return gap;
}

const Constraint* constraintById(const Sketch& sketch, ConstraintId id) {
  const auto found = std::find_if(
      sketch.constraints().begin(), sketch.constraints().end(),
      [id](const Constraint& constraint) { return constraint.id == id; });
  return found == sketch.constraints().end() ? nullptr : &*found;
}

void requireLineUnchanged(const Sketch& sketch, GeometryId id,
                          const Line& expected) {
  const auto index = sketch.lineIndex(id);
  require(index.has_value(), "projected line ID must remain valid");
  const Line& line = sketch.lines()[*index];
  require(std::abs(line.start.xMm - expected.start.xMm) <= 1e-9 &&
              std::abs(line.start.yMm - expected.start.yMm) <= 1e-9 &&
              std::abs(line.end.xMm - expected.end.xMm) <= 1e-9 &&
              std::abs(line.end.yMm - expected.end.yMm) <= 1e-9 &&
              line.elementId == expected.elementId &&
              line.dashed == expected.dashed,
          "locked projected line snapshot must remain unchanged");
  require(sketch.isGeometryLocked(id), "projected line must remain locked");
}

void requireStructuralConstraintsRetained(const GapFixture& fixture) {
  const auto countType = [&fixture](ConstraintType type) {
    return std::count_if(
        fixture.structuralConstraints.begin(),
        fixture.structuralConstraints.end(),
        [type](const Constraint& constraint) { return constraint.type == type; });
  };
  require(countType(ConstraintType::Coincident) == 4 &&
              countType(ConstraintType::Equal) == 2 &&
              countType(ConstraintType::Parallel) == 2 &&
              countType(ConstraintType::Perpendicular) == 1 &&
              countType(ConstraintType::Horizontal) == 1 &&
              countType(ConstraintType::Vertical) == 1,
          "rectangle structural constraints must pre-exist");

  for (const auto& expected : fixture.structuralConstraints) {
    const Constraint* stored = constraintById(fixture.sketch, expected.id);
    require(stored && stored->type == expected.type,
            "rectangle structural constraint ID and type must remain");
  }
}

double axisCoordinate(const Sketch& sketch, PointReference reference,
                      GapAxis axis) {
  const auto point = sketch.referencedPoint(reference);
  require(point.has_value(), "point reference must remain valid");
  return axis == GapAxis::X ? point->xMm : point->yMm;
}

void requireRigidRectangleTranslation(const GapFixture& fixture,
                                      const std::vector<Line>& before) {
  const std::size_t first = fixture.rectangleFirstIndex;
  const double moveX = fixture.sketch.lines()[first].start.xMm -
                       before[first].start.xMm;
  const double moveY = fixture.sketch.lines()[first].start.yMm -
                       before[first].start.yMm;
  for (std::size_t index = first; index < first + 4; ++index) {
    const Line& oldLine = before[index];
    const Line& line = fixture.sketch.lines()[index];
    require(std::abs(line.start.xMm - oldLine.start.xMm - moveX) <= 1e-7 &&
                std::abs(line.start.yMm - oldLine.start.yMm - moveY) <= 1e-7 &&
                std::abs(line.end.xMm - oldLine.end.xMm - moveX) <= 1e-7 &&
                std::abs(line.end.yMm - oldLine.end.yMm - moveY) <= 1e-7,
            "first gap must translate the complete rectangle rigidly");
  }
}

void alignedDistancesToOneLockedProjectionLine(GapAxis axis, bool highFirst) {
  GapFixture fixture = makeSingleProjectionGapFixture(axis);
  const auto rectangleBefore = fixture.sketch.lines();

  const Constraint low =
      alignedGap(fixture.projectionLow, fixture.rectangleLow);
  const Constraint high =
      alignedGap(fixture.projectionHigh, fixture.rectangleHigh);

  ConstraintId firstId = kInvalidConstraintId;
  ConstraintId secondId = kInvalidConstraintId;
  if (highFirst) {
    firstId = fixture.sketch.addConstraint(high);
    require(firstId != kInvalidConstraintId,
            "first high aligned gap must be accepted");
    require(std::abs(axisCoordinate(fixture.sketch, fixture.rectangleHigh,
                                    axis) -
                     93.0) <= 1e-7,
            "first high gap must position the rectangle at 93 mm");
  } else {
    firstId = fixture.sketch.addConstraint(low);
    require(firstId != kInvalidConstraintId,
            "first low aligned gap must be accepted");
    require(std::abs(axisCoordinate(fixture.sketch, fixture.rectangleLow,
                                    axis) -
                     7.0) <= 1e-7,
            "first low gap must position the rectangle at 7 mm");
  }
  requireRigidRectangleTranslation(fixture, rectangleBefore);

  secondId = fixture.sketch.addConstraint(highFirst ? low : high);
  require(secondId != kInvalidConstraintId,
          "opposite aligned gap on the same projection must be accepted");

  const double lowCoordinate =
      axisCoordinate(fixture.sketch, fixture.rectangleLow, axis);
  const double highCoordinate =
      axisCoordinate(fixture.sketch, fixture.rectangleHigh, axis);
  require(std::abs(lowCoordinate - 7.0) <= 1e-7 &&
              std::abs(highCoordinate - 93.0) <= 1e-7,
          "opposite rectangle vertices must retain both 7 mm gaps");
  require(std::abs(highCoordinate - lowCoordinate - 86.0) <= 1e-7,
          "free rectangle axis size must become 86 mm");

  for (const ConstraintId id : {firstId, secondId}) {
    const Constraint* stored = constraintById(fixture.sketch, id);
    require(stored && stored->type == ConstraintType::Distance &&
                std::abs(stored->value - 7.0) <= 1e-9,
            "both aligned constraints must retain Distance type and value 7");
  }
  require(!analyzeConstraintSystem(fixture.sketch).conflicting,
          "compatible aligned gaps must not conflict");
  requireLineUnchanged(fixture.sketch, fixture.projection,
                       fixture.projectedLine);
  requireStructuralConstraintsRetained(fixture);
}

void incompatibleAxisSizeRejectsSecondGap(GapAxis axis) {
  GapFixture fixture = makeSingleProjectionGapFixture(axis);

  Constraint axisSize;
  axisSize.type = axis == GapAxis::X ? ConstraintType::DistanceX
                                     : ConstraintType::DistanceY;
  axisSize.firstPoint = fixture.rectangleLow;
  axisSize.secondPoint = fixture.rectangleHigh;
  axisSize.value = 60.0;
  const ConstraintId axisSizeId = fixture.sketch.addConstraint(axisSize);
  require(axisSizeId != kInvalidConstraintId,
          "explicit rectangle axis size must be accepted");

  const ConstraintId firstGapId = fixture.sketch.addConstraint(
      alignedGap(fixture.projectionHigh, fixture.rectangleHigh));
  require(firstGapId != kInvalidConstraintId,
          "first aligned gap with explicit axis size must be accepted");

  const auto beforeRejectedGap = fixture.sketch.lines();
  const std::size_t constraintCount = fixture.sketch.constraints().size();
  const ConstraintId rejected = fixture.sketch.addConstraint(
      alignedGap(fixture.projectionLow, fixture.rectangleLow));
  require(rejected == kInvalidConstraintId,
          "incompatible second aligned gap must be rejected");
  require(fixture.sketch.constraints().size() == constraintCount,
          "rejected aligned gap must not remain stored");

  for (std::size_t index = 0; index < beforeRejectedGap.size(); ++index) {
    const Line& before = beforeRejectedGap[index];
    const Line& line = fixture.sketch.lines()[index];
    require(std::abs(line.start.xMm - before.start.xMm) <= 1e-9 &&
                std::abs(line.start.yMm - before.start.yMm) <= 1e-9 &&
                std::abs(line.end.xMm - before.end.xMm) <= 1e-9 &&
                std::abs(line.end.yMm - before.end.yMm) <= 1e-9,
            "rejected gap must transactionally restore all geometry");
  }

  const Constraint* storedAxisSize =
      constraintById(fixture.sketch, axisSizeId);
  const Constraint* storedFirstGap =
      constraintById(fixture.sketch, firstGapId);
  require(storedAxisSize && storedAxisSize->type == axisSize.type &&
              std::abs(storedAxisSize->value - 60.0) <= 1e-9,
          "explicit axis size must survive rollback");
  require(storedFirstGap &&
              storedFirstGap->type == ConstraintType::Distance &&
              std::abs(storedFirstGap->value - 7.0) <= 1e-9,
          "first aligned gap must survive rollback");
  require(!analyzeConstraintSystem(fixture.sketch).conflicting,
          "rolled-back system must remain non-conflicting");
  requireLineUnchanged(fixture.sketch, fixture.projection,
                       fixture.projectedLine);
  requireStructuralConstraintsRetained(fixture);
}

void pointDistanceMovesLooseDiagonalEndpoint(bool diagonalFirst) {
  Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {40.0, 30.0});
  const GeometryId profileTop = sketch.lineId(2);
  const GeometryId profileRight = sketch.lineId(1);
  const PointReference bottomRight{sketch.lineId(0), false};

  sketch.addLine({0.0, 30.0}, {40.0, 18.0});
  const GeometryId diagonal = sketch.lineId(4);
  const PointReference diagonalTop{diagonal, true};
  const PointReference diagonalBottom{diagonal, false};

  Constraint upperOnTop;
  upperOnTop.type = ConstraintType::PointOnLine;
  upperOnTop.firstGeometry = profileTop;
  upperOnTop.secondPoint = diagonalTop;
  CHECK(sketch.addConstraint(upperOnTop) != kInvalidConstraintId);

  Constraint lowerOnRight;
  lowerOnRight.type = ConstraintType::PointOnLine;
  lowerOnRight.firstGeometry = profileRight;
  lowerOnRight.secondPoint = diagonalBottom;
  CHECK(sketch.addConstraint(lowerOnRight) != kInvalidConstraintId);

  const auto anchorBefore = sketch.referencedPoint(bottomRight);
  const auto topBefore = sketch.referencedPoint(diagonalTop);
  const auto bottomBefore = sketch.referencedPoint(diagonalBottom);
  CHECK(anchorBefore && topBefore && bottomBefore);
  const double lengthBefore = std::hypot(bottomBefore->xMm - topBefore->xMm,
                                         bottomBefore->yMm - topBefore->yMm);
  const double angleBefore = std::atan2(bottomBefore->yMm - topBefore->yMm,
                                        bottomBefore->xMm - topBefore->xMm);

  Constraint distance;
  distance.type = ConstraintType::Distance;
  distance.firstPoint = diagonalFirst ? diagonalBottom : bottomRight;
  distance.secondPoint = diagonalFirst ? bottomRight : diagonalBottom;
  distance.value = 12.0;
  CHECK(sketch.addConstraint(distance) != kInvalidConstraintId);

  const auto anchorAfter = sketch.referencedPoint(bottomRight);
  const auto topAfter = sketch.referencedPoint(diagonalTop);
  const auto bottomAfter = sketch.referencedPoint(diagonalBottom);
  CHECK(anchorAfter && topAfter && bottomAfter);
  CHECK(std::abs(std::hypot(bottomAfter->xMm - anchorAfter->xMm,
                            bottomAfter->yMm - anchorAfter->yMm) - 12.0) <= 1e-7);
  CHECK(std::abs(anchorAfter->xMm - anchorBefore->xMm) <= 1e-9);
  CHECK(std::abs(anchorAfter->yMm - anchorBefore->yMm) <= 1e-9);
  const double lengthAfter = std::hypot(bottomAfter->xMm - topAfter->xMm,
                                        bottomAfter->yMm - topAfter->yMm);
  const double angleAfter = std::atan2(bottomAfter->yMm - topAfter->yMm,
                                       bottomAfter->xMm - topAfter->xMm);
  CHECK(std::abs(lengthAfter - lengthBefore) > 1e-6);
  CHECK(std::abs(angleAfter - angleBefore) > 1e-6);
  CHECK(!analyzeConstraintSystem(sketch).conflicting);
}

void projectedRectangleBottomGap(bool rectangleSelectedFirst,
                                 ConstraintType type) {
  Sketch sketch;
  sketch.addLine({0.0, 60.0}, {0.0, 0.0});
  sketch.addLine({100.0, 60.0}, {100.0, 0.0});
  sketch.addLine({0.0, 0.0}, {100.0, 0.0});
  sketch.addLine({0.0, 50.0}, {100.0, 50.0});
  const GeometryId leftProjection = sketch.lineId(0);
  const GeometryId rightProjection = sketch.lineId(1);
  const GeometryId bottomProjection = sketch.lineId(2);
  const GeometryId topProjection = sketch.lineId(3);
  for (const GeometryId id : {leftProjection, rightProjection,
                              bottomProjection, topProjection}) {
    sketch.setElementDashed(sketch.lines()[*sketch.lineIndex(id)].elementId,
                            true);
    CHECK(lockGeometry(sketch, id) != kInvalidConstraintId);
  }
  const std::vector<Line> projectionBefore(sketch.lines().begin(),
                                           sketch.lines().end());

  sketch.addRectangle({20.0, 50.0}, {80.0, 20.0});
  const std::size_t rectangleFirstIndex = 4;
  const PointReference rectangleLeft{sketch.lineId(rectangleFirstIndex), true};
  const PointReference rectangleRight{sketch.lineId(rectangleFirstIndex), false};
  const PointReference rectangleBottomRight{
      sketch.lineId(rectangleFirstIndex + 2), true};
  const PointReference projectedLeft{topProjection, true};
  const PointReference projectedRight{topProjection, false};
  const PointReference projectedBottomRight{bottomProjection, false};

  Constraint topLeftOnProjection;
  topLeftOnProjection.type = ConstraintType::PointOnLine;
  topLeftOnProjection.firstGeometry = topProjection;
  topLeftOnProjection.secondPoint = rectangleLeft;
  CHECK(sketch.addConstraint(topLeftOnProjection) != kInvalidConstraintId);
  Constraint topRightOnProjection = topLeftOnProjection;
  topRightOnProjection.id = kInvalidConstraintId;
  topRightOnProjection.secondPoint = rectangleRight;
  CHECK(sketch.addConstraint(topRightOnProjection) != kInvalidConstraintId);

  Constraint leftGap;
  leftGap.type = ConstraintType::Distance;
  leftGap.firstPoint = projectedLeft;
  leftGap.secondPoint = rectangleLeft;
  leftGap.value = 7.0;
  CHECK(sketch.addConstraint(leftGap) != kInvalidConstraintId);
  Constraint rightGap;
  rightGap.type = ConstraintType::Distance;
  rightGap.firstPoint = projectedRight;
  rightGap.secondPoint = rectangleRight;
  rightGap.value = 7.0;
  CHECK(sketch.addConstraint(rightGap) != kInvalidConstraintId);

  const double topBefore = sketch.lines()[rectangleFirstIndex].start.yMm;
  Constraint bottomGap;
  bottomGap.type = type;
  bottomGap.firstPoint = rectangleSelectedFirst ? rectangleBottomRight
                                                : projectedBottomRight;
  bottomGap.secondPoint = rectangleSelectedFirst ? projectedBottomRight
                                                 : rectangleBottomRight;
  bottomGap.value = 12.0;
  CHECK(sketch.addConstraint(bottomGap) != kInvalidConstraintId);

  const auto bottom = sketch.referencedPoint(rectangleBottomRight);
  CHECK(bottom.has_value());
  CHECK(std::abs(bottom->yMm - 12.0) <= 1e-7);
  CHECK(std::abs(sketch.lines()[rectangleFirstIndex].start.yMm - topBefore) <= 1e-7);
  CHECK(std::abs(sketch.referencedPoint(rectangleLeft)->xMm - 7.0) <= 1e-7);
  CHECK(std::abs(sketch.referencedPoint(rectangleRight)->xMm - 93.0) <= 1e-7);
  for (std::size_t index = 0; index < projectionBefore.size(); ++index) {
    CHECK(std::abs(sketch.lines()[index].start.xMm -
                   projectionBefore[index].start.xMm) <= 1e-9);
    CHECK(std::abs(sketch.lines()[index].start.yMm -
                   projectionBefore[index].start.yMm) <= 1e-9);
    CHECK(std::abs(sketch.lines()[index].end.xMm -
                   projectionBefore[index].end.xMm) <= 1e-9);
    CHECK(std::abs(sketch.lines()[index].end.yMm -
                   projectionBefore[index].end.yMm) <= 1e-9);
  }
  CHECK(!analyzeConstraintSystem(sketch).conflicting);
}

void incompatiblePointDistanceRestoresSnapshot() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {0.0, 20.0});
  const PointReference first{sketch.lineId(0), true};
  const PointReference second{sketch.lineId(0), false};

  Constraint original;
  original.type = ConstraintType::Distance;
  original.firstPoint = first;
  original.secondPoint = second;
  original.value = 20.0;
  CHECK(sketch.addConstraint(original) != kInvalidConstraintId);

  const auto geometryBefore = sketch.lines();
  const std::size_t constraintCount = sketch.constraints().size();
  Constraint incompatible = original;
  incompatible.id = kInvalidConstraintId;
  incompatible.value = 12.0;
  CHECK(sketch.addConstraint(incompatible) == kInvalidConstraintId);
  CHECK(sketch.constraints().size() == constraintCount);
  CHECK(sketch.lines().size() == geometryBefore.size());
  for (std::size_t index = 0; index < geometryBefore.size(); ++index) {
    CHECK(std::abs(sketch.lines()[index].start.xMm -
                   geometryBefore[index].start.xMm) <= 1e-9);
    CHECK(std::abs(sketch.lines()[index].start.yMm -
                   geometryBefore[index].start.yMm) <= 1e-9);
    CHECK(std::abs(sketch.lines()[index].end.xMm -
                   geometryBefore[index].end.xMm) <= 1e-9);
    CHECK(std::abs(sketch.lines()[index].end.yMm -
                   geometryBefore[index].end.yMm) <= 1e-9);
  }
  CHECK(!analyzeConstraintSystem(sketch).conflicting);
}

void rotatedViewOrthogonalPointGaps(bool rectangleSelectedFirst) {
  Sketch sketch;
  sketch.addLine({11.0, 0.0}, {11.0, 58.0});
  sketch.addLine({-24.0, 0.0}, {-24.0, 58.0});
  const GeometryId rightProjection = sketch.lineId(0);
  const GeometryId leftProjection = sketch.lineId(1);
  CHECK(lockGeometry(sketch, rightProjection) != kInvalidConstraintId);
  CHECK(lockGeometry(sketch, leftProjection) != kInvalidConstraintId);

  sketch.addRectangle({11.0, 51.0}, {-12.0, 7.0});
  const PointReference rectangleBottomLeft{sketch.lineId(4), true};
  const PointReference projectedBottomLeft{leftProjection, true};

  Constraint existingScreenHorizontal;
  existingScreenHorizontal.type = ConstraintType::DistanceY;
  existingScreenHorizontal.firstPoint = rectangleBottomLeft;
  existingScreenHorizontal.secondPoint = projectedBottomLeft;
  existingScreenHorizontal.value = 7.0;
  CHECK(sketch.addConstraint(existingScreenHorizontal) != kInvalidConstraintId);

  Constraint screenVertical;
  screenVertical.type = ConstraintType::DistanceX;
  screenVertical.firstPoint = rectangleSelectedFirst ? rectangleBottomLeft
                                                     : projectedBottomLeft;
  screenVertical.secondPoint = rectangleSelectedFirst ? projectedBottomLeft
                                                      : rectangleBottomLeft;
  screenVertical.value = 12.0;
  CHECK(sketch.addConstraint(screenVertical) != kInvalidConstraintId);
  const auto rectanglePoint = sketch.referencedPoint(rectangleBottomLeft);
  const auto projectedPoint = sketch.referencedPoint(projectedBottomLeft);
  CHECK(rectanglePoint && projectedPoint);
  CHECK(std::abs(std::abs(rectanglePoint->xMm - projectedPoint->xMm) - 12.0) <=
        1e-7);
  CHECK(std::abs(std::abs(rectanglePoint->yMm - projectedPoint->yMm) - 7.0) <=
        1e-7);
  CHECK(!analyzeConstraintSystem(sketch).conflicting);
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
  alignedDistancesToOneLockedProjectionLine(GapAxis::X, false);
  alignedDistancesToOneLockedProjectionLine(GapAxis::X, true);
  alignedDistancesToOneLockedProjectionLine(GapAxis::Y, false);
  alignedDistancesToOneLockedProjectionLine(GapAxis::Y, true);
  incompatibleAxisSizeRejectsSecondGap(GapAxis::X);
  incompatibleAxisSizeRejectsSecondGap(GapAxis::Y);
  pointDistanceMovesLooseDiagonalEndpoint(false);
  pointDistanceMovesLooseDiagonalEndpoint(true);
  projectedRectangleBottomGap(false, ConstraintType::DistanceY);
  projectedRectangleBottomGap(true, ConstraintType::DistanceY);
  incompatiblePointDistanceRestoresSnapshot();
  rotatedViewOrthogonalPointGaps(false);
  rotatedViewOrthogonalPointGaps(true);
  return EXIT_SUCCESS;
}

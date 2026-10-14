#include "TestAssertions.h"

#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace solidar::sketch {

class SketchTestAccess {
 public:
  static bool installPersistedConstraintsWithoutSolve(
      Sketch& sketch, std::vector<Constraint> constraints) {
    std::vector<ConstraintId> ids;
    ids.reserve(constraints.size());
    ConstraintId nextId = 1;
    for (const auto& constraint : constraints) {
      if (constraint.id == kInvalidConstraintId ||
          std::find(ids.begin(), ids.end(), constraint.id) != ids.end())
        return false;
      ids.push_back(constraint.id);
      nextId = std::max(nextId, constraint.id + 1);
    }
    sketch.constraints_ = std::move(constraints);
    sketch.nextConstraintId_ = nextId;
    sketch.invalidateStructureIndexes();
    sketch.hasLastSolvedFingerprint_ = false;
    return true;
  }

  static void perturbGeometry(Sketch& sketch, GeometryId id) {
    const auto location = sketch.geometryLocation(id);
    CHECK(location.has_value());
    switch (location->kind) {
      case GeometryKind::Line:
        sketch.lines_[location->index].end.xMm += 0.2;
        sketch.lines_[location->index].end.yMm += 0.3;
        break;
      case GeometryKind::Circle:
        sketch.circles_[location->index].center.xMm += 0.2;
        sketch.circles_[location->index].radiusMm += 0.1;
        break;
      case GeometryKind::Arc:
        sketch.arcs_[location->index].center.xMm += 0.2;
        sketch.arcs_[location->index].radiusMm += 0.1;
        break;
    }
    sketch.hasLastSolvedFingerprint_ = false;
  }

  static void perturbPoint(Sketch& sketch, PointReference reference) {
    if (reference.lineId != kInvalidGeometryId) {
      const auto index = sketch.lineIndex(reference.lineId);
      CHECK(index.has_value());
      auto& point = reference.start ? sketch.lines_[*index].start
                                    : sketch.lines_[*index].end;
      point.xMm += 0.2;
      point.yMm += 0.3;
    } else if (reference.circleId != kInvalidGeometryId) {
      const auto index = sketch.circleIndex(reference.circleId);
      CHECK(index.has_value());
      sketch.circles_[*index].center.xMm += 0.2;
      sketch.circles_[*index].center.yMm += 0.3;
    } else if (reference.arcId != kInvalidGeometryId) {
      const auto index = sketch.arcIndex(reference.arcId);
      CHECK(index.has_value());
      sketch.arcs_[*index].center.xMm += 0.2;
      sketch.arcs_[*index].center.yMm += 0.3;
    }
    sketch.hasLastSolvedFingerprint_ = false;
  }
};

}  // namespace solidar::sketch

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

bool storeReferenceDimension(Sketch& sketch, Dimension dimension) {
  Constraint annotation;
  switch (dimension.kind) {
    case DimensionKind::LineLength:
      annotation = Constraint{
          LengthConstraint{dimension.geometryId, std::nullopt},
          ConstraintMode::Reference};
      break;
    case DimensionKind::CircleDiameter:
      annotation = Constraint{
          DiameterConstraint{dimension.geometryId, std::nullopt},
          ConstraintMode::Reference};
      break;
    case DimensionKind::PointDistance:
      annotation = Constraint{
          DistanceConstraint{dimension.firstPoint, dimension.secondPoint,
                             std::nullopt},
          ConstraintMode::Reference};
      break;
    case DimensionKind::PointDistanceX:
      annotation = Constraint{
          DistanceXConstraint{dimension.firstPoint, dimension.secondPoint,
                              std::nullopt},
          ConstraintMode::Reference};
      break;
    case DimensionKind::PointDistanceY:
      annotation = Constraint{
          DistanceYConstraint{dimension.firstPoint, dimension.secondPoint,
                              std::nullopt},
          ConstraintMode::Reference};
      break;
    case DimensionKind::LineAngle:
      annotation = Constraint{
          AngleConstraint{dimension.geometryId, dimension.secondPoint.lineId,
                          std::nullopt},
          ConstraintMode::Reference};
      break;
    case DimensionKind::LineDistance:
      annotation = Constraint{
          LineDistanceConstraint{dimension.geometryId,
                                 dimension.secondPoint.lineId, std::nullopt},
          ConstraintMode::Reference};
      break;
  }
  dimension.constraintId = sketch.addConstraint(std::move(annotation));
  return dimension.constraintId != kInvalidConstraintId &&
         sketch.storeDimension(dimension);
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

  // The persistence-only bulk boundary may restore a structurally valid but
  // conflicting system so that diagnostics remain available.
  Constraint onYAxis;
  onYAxis.id = 1;
  onYAxis.type = ConstraintType::PointOnYAxis;
  onYAxis.secondPoint = start;

  Constraint distanceFromYAxis;
  distanceFromYAxis.id = 2;
  distanceFromYAxis.type = ConstraintType::DistanceX;
  distanceFromYAxis.firstPoint.origin = true;
  distanceFromYAxis.secondPoint = start;
  distanceFromYAxis.value = 10.0;
  require(sketch.restoreConstraints({onYAxis, distanceFromYAxis}),
          "conflicting persisted datum constraints must restore");

  const auto solved =
      SketchSolver::solveStable(sketch);
  require(
      !solved.converged,
      "incompatible datum constraints must be reported as conflicting");
  require(
      solved.violatedConstraints > 0,
      "incompatible datum constraints must retain a diagnostic");
}

void removingArcDropsPointAndConstraintReferences() {
  Sketch sketch;
  constexpr double kHalfPi = 1.5707963267948966;
  sketch.addArc({0.0, 0.0}, 10.0, 0.0, kHalfPi);
  sketch.addLine({10.0, 0.0}, {20.0, 0.0});
  sketch.addLine({0.0, 20.0}, {0.0, 10.0});

  const GeometryId arcId = sketch.arcId(0);
  PointReference arcStart;
  arcStart.arcId = arcId;
  arcStart.start = true;
  PointReference arcEnd;
  arcEnd.arcId = arcId;
  arcEnd.start = false;

  Dimension endpointDistance;
  endpointDistance.kind = DimensionKind::PointDistance;
  endpointDistance.firstPoint = arcStart;
  endpointDistance.secondPoint = arcEnd;
  endpointDistance.valueMm = std::sqrt(200.0);
  Constraint endpointReference{
      DistanceConstraint{arcStart, arcEnd, std::nullopt},
      ConstraintMode::Reference};
  endpointDistance.constraintId = sketch.addConstraint(endpointReference);
  CHECK(endpointDistance.constraintId != kInvalidConstraintId);
  CHECK(sketch.storeDimension(endpointDistance));

  Constraint startCoincident;
  startCoincident.type = ConstraintType::Coincident;
  startCoincident.firstPoint = arcStart;
  startCoincident.secondPoint = PointReference{sketch.lineId(0), true};
  CHECK(sketch.addConstraint(startCoincident) != kInvalidConstraintId);

  Constraint endCoincident;
  endCoincident.type = ConstraintType::Coincident;
  endCoincident.firstPoint = PointReference{sketch.lineId(1), false};
  endCoincident.secondPoint = arcEnd;
  CHECK(sketch.addConstraint(endCoincident) != kInvalidConstraintId);

  CHECK(sketch.dimensions().size() == 1);
  CHECK(sketch.constraints().size() == 3);

  sketch.removeArc(0);

  CHECK(sketch.arcs().empty());
  CHECK(sketch.dimensions().empty());
  CHECK(sketch.constraints().empty());
}

void removingCenteredElementLineDropsCenterReferences() {
  Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {20.0, 10.0});

  const std::size_t elementId = sketch.lines().front().elementId;
  sketch.markElementCenterNode(elementId);
  CHECK(sketch.hasElementCenterNode(elementId));

  PointReference origin;
  origin.origin = true;
  PointReference center;
  center.elementCenterId = elementId;

  Dimension centerDistance;
  centerDistance.kind = DimensionKind::PointDistance;
  centerDistance.firstPoint = origin;
  centerDistance.secondPoint = center;
  centerDistance.valueMm = std::sqrt(125.0);

  Constraint centerConstraint;
  centerConstraint.type = ConstraintType::Distance;
  centerConstraint.firstPoint = origin;
  centerConstraint.secondPoint = center;
  centerConstraint.value = std::sqrt(125.0);
  const ConstraintId centerConstraintId =
      sketch.addConstraint(centerConstraint);
  CHECK(centerConstraintId != kInvalidConstraintId);
  centerDistance.constraintId = centerConstraintId;
  CHECK(sketch.storeDimension(centerDistance));
  CHECK(sketch.dimensions().size() == 1);

  sketch.removeLine(0);

  CHECK(sketch.lines().size() == 3);
  CHECK(!sketch.hasElementCenterNode(elementId));
  CHECK(sketch.dimensions().empty());
  CHECK(std::none_of(
      sketch.constraints().begin(), sketch.constraints().end(),
      [centerConstraintId](const Constraint& constraint) {
        return constraint.id == centerConstraintId;
      }));
  CHECK(std::none_of(
      sketch.constraints().begin(), sketch.constraints().end(),
      [elementId](const Constraint& constraint) {
        return constraint.firstPoint.elementCenterId == elementId ||
               constraint.secondPoint.elementCenterId == elementId;
      }));
}

void indexesAndConnectivityStayDeterministic() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 1.0});
  sketch.addCircle({20.0, 0.0}, 3.0);
  sketch.addArc({30.0, 0.0}, 4.0, 0.0, 1.5);
  sketch.addRectangle({40.0, -5.0}, {50.0, 5.0});
  const GeometryId line = sketch.lineId(0);
  const GeometryId circle = sketch.circleId(0);
  const GeometryId arc = sketch.arcId(0);
  const std::size_t rectangleElement = sketch.lines()[1].elementId;
  sketch.markElementCenterNode(rectangleElement);

  Constraint lineCircle;
  lineCircle.id = 101;
  lineCircle.type = ConstraintType::Coincident;
  lineCircle.firstPoint = PointReference{line, false};
  lineCircle.secondPoint.circleId = circle;

  Constraint circleArc;
  circleArc.id = 102;
  circleArc.type = ConstraintType::Coincident;
  circleArc.firstPoint.circleId = circle;
  circleArc.secondPoint.arcId = arc;

  Constraint centerDistance;
  centerDistance.id = 103;
  centerDistance.type = ConstraintType::DistanceX;
  centerDistance.firstPoint.origin = true;
  centerDistance.secondPoint.elementCenterId = rectangleElement;
  centerDistance.value = 45.0;
  auto restored = sketch.constraints();
  restored.push_back(lineCircle);
  restored.push_back(circleArc);
  restored.push_back(centerDistance);
  CHECK(sketch.restoreConstraints(std::move(restored)));

  CHECK(sketch.geometryLocation(line) ==
        std::optional<GeometryLocation>({GeometryKind::Line, 0}));
  CHECK(sketch.geometryLocation(circle) ==
        std::optional<GeometryLocation>({GeometryKind::Circle, 0}));
  CHECK(sketch.geometryLocation(arc) ==
        std::optional<GeometryLocation>({GeometryKind::Arc, 0}));
  CHECK(sketch.constraintIndex(102).has_value());
  CHECK(sketch.constraints()[*sketch.constraintIndex(102)].id == 102);

  const auto connected = sketch.connectedComponent({line});
  CHECK(connected.geometryIds == std::vector<GeometryId>({line, circle, arc}));
  CHECK(connected.constraintIds ==
        std::vector<ConstraintId>({101, 102}));
  const auto rectangle = sketch.connectedComponent({sketch.lineId(1)});
  CHECK(rectangle.geometryIds.size() == 4);
  CHECK(rectangle.constraintIds.size() >= 1);

  // The shared fixed origin must not connect independent components.
  CHECK(std::find(rectangle.geometryIds.begin(), rectangle.geometryIds.end(),
                  line) == rectangle.geometryIds.end());

  sketch.removeCircle(0);
  CHECK(!sketch.geometryLocation(circle));
  CHECK(sketch.geometryLocation(arc) ==
        std::optional<GeometryLocation>({GeometryKind::Arc, 0}));
  CHECK(!sketch.constraintIndex(101));
  CHECK(!sketch.constraintIndex(102));

  Sketch copied = sketch;
  CHECK(copied.geometryLocation(arc) == sketch.geometryLocation(arc));
  Sketch moved = std::move(copied);
  CHECK(moved.geometryLocation(arc) == sketch.geometryLocation(arc));
  moved.clearConstraints();
  CHECK(!moved.constraintIndex(103));
}

void componentSolveLeavesIndependentGeometryBitwiseStable() {
  Sketch sketch;
  sketch.addLine({0.0, 2.0}, {10.0, 4.0});
  sketch.addLine({100.0, 7.0}, {110.0, 9.0});
  Constraint first;
  first.type = ConstraintType::Horizontal;
  first.firstGeometry = sketch.lineId(0);
  CHECK(sketch.addConstraint(first) != kInvalidConstraintId);
  Constraint second;
  second.type = ConstraintType::Horizontal;
  second.firstGeometry = sketch.lineId(1);
  CHECK(sketch.addConstraint(second) != kInvalidConstraintId);

  const Line independent = sketch.lines()[1];
  sketch.translateLinesByIds({sketch.lineId(0)}, 1.25, 0.75);
  CHECK(sketch.lines()[1].start.xMm == independent.start.xMm);
  CHECK(sketch.lines()[1].start.yMm == independent.start.yMm);
  CHECK(sketch.lines()[1].end.xMm == independent.end.xMm);
  CHECK(sketch.lines()[1].end.yMm == independent.end.yMm);

  const auto result = SketchSolver::solveStableComponent(
      sketch, {sketch.lineId(0)});
  CHECK(result.componentsVisited == 1);
  CHECK(result.geometriesVisited == 1);
  CHECK(result.constraintsVisited == 1);
  CHECK(result.lockedSnapshotSize == 0);

  const auto fullAfterPartial = SketchSolver::solveStable(sketch);
  CHECK(fullAfterPartial.passes > 0);
  CHECK(fullAfterPartial.componentsVisited == 2);
  const auto unchanged = SketchSolver::solveStable(sketch);
  CHECK(unchanged.passes == 0);
  CHECK(unchanged.componentsVisited == 0);
  CHECK(unchanged.converged);

  Constraint lock;
  lock.type = ConstraintType::Lock;
  lock.firstGeometry = sketch.lineId(0);
  CHECK(sketch.addConstraint(lock) != kInvalidConstraintId);
  const auto locked = SketchSolver::solveStableComponent(
      sketch, {sketch.lineId(0)});
  CHECK(locked.lockedSnapshotSize == 1);
  CHECK(locked.geometriesVisited == 1);
}

void rejectedConstraintNeverPoisonsFullSolveCache() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch.addLine({50.0, 2.0}, {60.0, 3.0});
  const auto beforeRejected = sketch.semanticFingerprint();
  Constraint invalid;
  invalid.id = 700;
  invalid.type = ConstraintType::Horizontal;
  invalid.firstGeometry = 999999;
  CHECK(sketch.addConstraint(invalid) == kInvalidConstraintId);
  CHECK(sketch.semanticFingerprint() == beforeRejected);
  Constraint local;
  local.type = ConstraintType::Horizontal;
  local.firstGeometry = sketch.lineId(1);
  CHECK(sketch.addConstraint(local) != kInvalidConstraintId);

  const auto partial = SketchSolver::solveStableComponent(
      sketch, {sketch.lineId(1)});
  CHECK(partial.converged);
  CHECK(partial.invalidReferences == 0);
  const auto full = SketchSolver::solveStable(sketch);
  CHECK(full.converged);
  CHECK(full.invalidReferences == 0);
  CHECK(full.constraintsVisited == 1);
}

void sharedOriginDoesNotJoinComponents() {
  Sketch sketch;
  sketch.addLine({5.0, 0.0}, {10.0, 0.0});
  sketch.addLine({0.0, 8.0}, {0.0, 12.0});
  for (std::size_t index = 0; index < 2; ++index) {
    Constraint distance;
    distance.type = index == 0 ? ConstraintType::DistanceX
                               : ConstraintType::DistanceY;
    distance.firstPoint.origin = true;
    distance.secondPoint = {sketch.lineId(index), true};
    distance.value = index == 0 ? 5.0 : 8.0;
    CHECK(sketch.addConstraint(distance) != kInvalidConstraintId);
  }
  const auto components = sketch.constraintComponents();
  CHECK(components.size() == 2);
  CHECK(components[0].geometryIds.size() == 1);
  CHECK(components[1].geometryIds.size() == 1);
}

void localizedSketchDeltaIsBoundedAndReversible() {
  Sketch before;
  for (std::size_t index = 0; index < 1000; ++index) {
    const double x = static_cast<double>(index) * 3.0;
    before.addLine({x, 0.0}, {x + 1.0, 1.0});
  }
  Sketch after = before;
  after.translateLinesByIds({after.lineId(500)}, 0.25, -0.5);
  const auto delta = Sketch::makeDelta(before, after);
  CHECK(!delta.empty());
  CHECK(delta.lines.size() == 1);
  CHECK(delta.retainedBytes < before.ownedBytes() / 10);
  const auto beforeHash = before.solverFingerprint();
  const auto afterHash = after.solverFingerprint();
  CHECK(beforeHash != afterHash);
  CHECK(after.applyDelta(delta, false));
  CHECK(after.solverFingerprint() == beforeHash);
  CHECK(after.applyDelta(delta, true));
  CHECK(after.solverFingerprint() == afterHash);
}

void mutationJournalUsesNoFullSketchCheckpoints() {
  Sketch sketch;
  for (std::size_t index = 0; index < 240; ++index) {
    const double x = static_cast<double>(index) * 2.0;
    sketch.addLine({x, 0.0}, {x + 1.0, 1.0});
  }
  const auto originalHash = sketch.solverFingerprint();
  std::vector<SketchDelta> history;
  history.reserve(201);

  Sketch::resetFullCopyCountForTesting();
  for (std::size_t index = 0; index < 200; ++index) {
    sketch.beginDeltaJournal();
    sketch.translateLinesByIds({sketch.lineId(index)}, 0.125, -0.25);
    auto delta = sketch.finishDeltaJournal();
    CHECK(!delta.empty());
    CHECK(delta.lines.size() == 1);
    history.push_back(std::move(delta));
  }
  sketch.beginDeltaJournal();
  sketch.removeLine(220);
  auto deleteDelta = sketch.finishDeltaJournal();
  CHECK(!deleteDelta.empty());
  CHECK(deleteDelta.lines.size() == 1);
  history.push_back(std::move(deleteDelta));
  const auto editedHash = sketch.solverFingerprint();
  CHECK(editedHash != originalHash);
  CHECK(Sketch::fullCopyCountForTesting() == 0);

  for (auto it = history.rbegin(); it != history.rend(); ++it)
    CHECK(sketch.applyDelta(*it, false));
  CHECK(sketch.solverFingerprint() == originalHash);
  for (const auto& delta : history) CHECK(sketch.applyDelta(delta, true));
  CHECK(sketch.solverFingerprint() == editedHash);
  CHECK(Sketch::fullCopyCountForTesting() == 0);
}

void deltaApplyIsAtomicAndIndependentOfConstraintIdOrder() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {20.0, 0.0});
  const GeometryId shared = sketch.lineId(0);
  Constraint first;
  first.id = 20;
  first.type = ConstraintType::Horizontal;
  first.firstGeometry = shared;
  Constraint second;
  second.id = 10;
  second.type = ConstraintType::Length;
  second.firstGeometry = shared;
  second.value = 20.0;
  CHECK(sketch.restoreConstraints({first, second}));
  CHECK(sketch.constraints()[0].id == 20);
  CHECK(sketch.constraints()[1].id == 10);
  const auto beforeHash = sketch.semanticFingerprint();

  sketch.beginDeltaJournal();
  sketch.removeLine(0);
  auto deletion = sketch.finishDeltaJournal();
  CHECK(sketch.lines().empty());
  CHECK(sketch.constraints().empty());
  CHECK(deletion.constraints.size() == 2);

  // Journal records preserve persisted constraint order. Apply itself must
  // still be independent of record order, so exercise both forms below.
  CHECK(deletion.constraints[0].before->id == 20);
  CHECK(deletion.constraints[1].before->id == 10);
  CHECK(sketch.applyDelta(deletion, false));
  CHECK(sketch.semanticFingerprint() == beforeHash);
  CHECK(sketch.constraints().size() == 2);
  CHECK(sketch.constraints()[0].id == 20);
  CHECK(sketch.constraints()[1].id == 10);
  const auto restoredHash = sketch.semanticFingerprint();
  CHECK(!sketch.applyDelta(deletion, false));
  CHECK(sketch.semanticFingerprint() == restoredHash);
  CHECK(sketch.lines().size() == 1);
  CHECK(sketch.constraints().size() == 2);
  CHECK(sketch.applyDelta(deletion, true));
  CHECK(sketch.lines().empty());
  CHECK(sketch.constraints().empty());
  auto shuffled = deletion;
  std::reverse(shuffled.constraints.begin(), shuffled.constraints.end());
  CHECK(sketch.applyDelta(shuffled, false));
  CHECK(sketch.semanticFingerprint() == beforeHash);
  CHECK(sketch.applyDelta(shuffled, true));
  CHECK(sketch.lines().empty());

  // A deliberately stale record fails after planning, but before any of the
  // otherwise valid geometry changes are committed.
  Sketch edited;
  edited.addLine({0.0, 0.0}, {10.0, 0.0});
  Sketch moved = edited;
  moved.translateLinesByIds({moved.lineId(0)}, 2.0, 1.0);
  auto invalid = Sketch::makeDelta(edited, moved);
  Constraint missing;
  missing.id = 999;
  missing.type = ConstraintType::Horizontal;
  missing.firstGeometry = edited.lineId(0);
  invalid.constraints.push_back({7, 7, missing, std::nullopt});
  const auto unchangedHash = edited.semanticFingerprint();
  const Line unchangedLine = edited.lines().front();
  CHECK(!edited.applyDelta(invalid, true));
  CHECK(edited.semanticFingerprint() == unchangedHash);
  CHECK(edited.lines().size() == 1);
  CHECK(edited.lines().front().start.xMm == unchangedLine.start.xMm);
  CHECK(edited.lines().front().start.yMm == unchangedLine.start.yMm);
  CHECK(edited.lines().front().end.xMm == unchangedLine.end.xMm);
  CHECK(edited.lines().front().end.yMm == unchangedLine.end.yMm);
}

void dimensionIdsAndAllocatorAreDeltaSafe() {
  const auto makeDimension = [](GeometryId carrier, DimensionId id) {
    Dimension dimension;
    dimension.id = id;
    dimension.kind = DimensionKind::LineLength;
    dimension.geometryId = carrier;
    dimension.valueMm = 10.0;
    return dimension;
  };

  Sketch before;
  before.addLine({0.0, 0.0}, {10.0, 0.0});
  Sketch after = before;
  CHECK(storeReferenceDimension(after, makeDimension(after.lineId(0), 1)));
  CHECK(after.dimensions().size() == 1);
  CHECK(after.dimensions()[0].id == 1);

  const auto addition = Sketch::makeDelta(before, after);
  CHECK(addition.dimensions.size() == 1);
  CHECK(addition.beforeNextDimensionId == 1);
  CHECK(addition.afterNextDimensionId == 2);

  Sketch replay = before;
  CHECK(replay.applyDelta(addition, true));
  CHECK(replay.dimensions().size() == 1);
  Dimension automatic = makeDimension(replay.lineId(0), kInvalidDimensionId);
  CHECK(storeReferenceDimension(replay, automatic));
  CHECK(replay.dimensions().size() == 2);
  CHECK(replay.dimensions()[1].id != kInvalidDimensionId);
  CHECK(replay.dimensions()[1].id != replay.dimensions()[0].id);

  Sketch repeat = before;
  CHECK(repeat.applyDelta(addition, true));
  CHECK(repeat.applyDelta(addition, false));
  CHECK(repeat.semanticallyEqual(before));
  CHECK(repeat.applyDelta(addition, true));
  CHECK(repeat.semanticallyEqual(after));
  CHECK(repeat.applyDelta(addition, false));
  CHECK(repeat.semanticallyEqual(before));

  Sketch journal = before;
  journal.beginDeltaJournal();
  CHECK(storeReferenceDimension(journal,
                                makeDimension(journal.lineId(0), 1)));
  const auto journalDelta = journal.finishDeltaJournal();
  CHECK(journalDelta.beforeNextDimensionId == 1);
  CHECK(journalDelta.afterNextDimensionId == 2);
  CHECK(journal.applyDelta(journalDelta, false));
  CHECK(journal.semanticallyEqual(before));
  CHECK(journal.applyDelta(journalDelta, true));
  CHECK(journal.semanticallyEqual(after));

  // A pure persistent-ID replacement must not disappear from makeDelta.
  Sketch idBefore = before;
  CHECK(storeReferenceDimension(idBefore,
                                makeDimension(idBefore.lineId(0), 7)));
  Sketch idAfter = before;
  CHECK(storeReferenceDimension(idAfter,
                                makeDimension(idAfter.lineId(0), 9)));
  const auto idOnly = Sketch::makeDelta(idBefore, idAfter);
  CHECK(idOnly.dimensions.size() == 1);
  CHECK(idOnly.dimensions[0].before->id == 7);
  CHECK(idOnly.dimensions[0].after->id == 9);
  CHECK(idBefore.applyDelta(idOnly, true));
  CHECK(idBefore.dimensions()[0].id == 9);
  CHECK(idBefore.applyDelta(idOnly, false));
  CHECK(idBefore.dimensions()[0].id == 7);

  // UINT64_MAX is a valid explicit persistent ID. The allocator wraps and
  // still chooses a genuinely free, nonzero automatic ID.
  Sketch high;
  high.addLine({0.0, 0.0}, {10.0, 0.0});
  CHECK(storeReferenceDimension(
      high, makeDimension(high.lineId(0),
                          std::numeric_limits<DimensionId>::max())));
  CHECK(storeReferenceDimension(
      high, makeDimension(high.lineId(0), kInvalidDimensionId)));
  CHECK(high.dimensions().size() == 2);
  CHECK(high.dimensions()[0].id == std::numeric_limits<DimensionId>::max());
  CHECK(high.dimensions()[1].id != kInvalidDimensionId);
  CHECK(high.dimensions()[1].id != high.dimensions()[0].id);

  const auto checkAtomicRejection = [&before](const SketchDelta& malformed) {
    Sketch candidate = before;
    const auto fingerprint = candidate.semanticFingerprint();
    CHECK(!candidate.applyDelta(malformed, true));
    CHECK(candidate.semanticFingerprint() == fingerprint);
    CHECK(candidate.semanticallyEqual(before));
  };

  auto zeroId = addition;
  zeroId.dimensions[0].after->id = kInvalidDimensionId;
  checkAtomicRejection(zeroId);

  Sketch twoAfter = before;
  CHECK(storeReferenceDimension(
      twoAfter, makeDimension(twoAfter.lineId(0), 1)));
  CHECK(storeReferenceDimension(
      twoAfter, makeDimension(twoAfter.lineId(0), 2)));
  auto duplicateId = Sketch::makeDelta(before, twoAfter);
  CHECK(duplicateId.dimensions.size() == 2);
  duplicateId.dimensions[1].after->id = 1;
  checkAtomicRejection(duplicateId);

  auto counterCollision = addition;
  counterCollision.afterNextDimensionId = 1;
  checkAtomicRejection(counterCollision);
}

void persistedSemanticFingerprintIncludesNonSolverFields() {
  Sketch base;
  base.addLine({0.0, 0.0}, {10.0, 0.0}, 42);
  const auto solverHash = base.solverFingerprint();
  const auto semanticHash = base.semanticFingerprint();

  Sketch dashed = base;
  dashed.setLineDashedById(dashed.lineId(0), true);
  CHECK(dashed.solverFingerprint() == solverHash);
  CHECK(dashed.semanticFingerprint() != semanticHash);
  CHECK(!dashed.semanticallyEqual(base));

  Sketch dimensioned = base;
  Dimension dimension;
  dimension.kind = DimensionKind::PointDistanceX;
  dimension.firstPoint.origin = true;
  dimension.secondPoint = {dimensioned.lineId(0), false};
  dimension.valueMm = 10.0;
  dimension.offsetMm = 7.0;
  dimension.angleRad = 0.25;
  Constraint dimensionReference{
      DistanceXConstraint{dimension.firstPoint, dimension.secondPoint,
                          std::nullopt},
      ConstraintMode::Reference};
  dimension.constraintId = dimensioned.addConstraint(dimensionReference);
  CHECK(dimension.constraintId != kInvalidConstraintId);
  const auto linkedSolverHash = dimensioned.solverFingerprint();
  CHECK(dimensioned.storeDimension(dimension));
  CHECK(dimensioned.solverFingerprint() == linkedSolverHash);
  CHECK(dimensioned.semanticFingerprint() != semanticHash);
  CHECK(!dimensioned.semanticallyEqual(base));

  Sketch circlePlain;
  circlePlain.addCircle({2.0, 3.0}, 4.0);
  Sketch circleDashed = circlePlain;
  circleDashed.setCircleDashedById(circleDashed.circleId(0), true);
  CHECK(circleDashed.semanticFingerprint() !=
        circlePlain.semanticFingerprint());

  Sketch arcPlain;
  arcPlain.addArc({3.0, 4.0}, 5.0, 0.25, 1.5, false);
  Sketch arcDashed;
  arcDashed.addArc({3.0, 4.0}, 5.0, 0.25, 1.5, true);
  CHECK(arcDashed.semanticFingerprint() != arcPlain.semanticFingerprint());

  Sketch otherElement;
  otherElement.addLine({0.0, 0.0}, {10.0, 0.0}, 43);
  CHECK(otherElement.semanticFingerprint() != semanticHash);

  Sketch references;
  references.addLine({0.0, 0.0}, {10.0, 0.0});
  references.addCircle({20.0, 10.0}, 3.0);
  references.addArc({30.0, 10.0}, 4.0, 0.0, 1.0);
  references.addRectangle({40.0, 0.0}, {50.0, 10.0});
  const std::size_t rectangleElement = references.lines().back().elementId;
  references.markElementCenterNode(rectangleElement);
  const auto pointFingerprint = [&references](PointReference point) {
    Sketch candidate = references;
    Dimension item;
    item.kind = DimensionKind::PointDistance;
    item.firstPoint = point;
    if (point.origin)
      item.secondPoint = PointReference{references.lineId(0), true};
    else
      item.secondPoint.origin = true;
    item.valueMm = 2.0;
    CHECK(storeReferenceDimension(candidate, item));
    return candidate.semanticFingerprint();
  };
  PointReference linePoint;
  linePoint.lineId = references.lineId(0);
  linePoint.start = false;
  PointReference circlePoint;
  circlePoint.circleId = references.circleId(0);
  PointReference arcPoint;
  arcPoint.arcId = references.arcId(0);
  arcPoint.start = false;
  PointReference centerPoint;
  centerPoint.elementCenterId = rectangleElement;
  PointReference originPoint;
  originPoint.origin = true;
  const std::vector<std::uint64_t> referenceFingerprints{
      pointFingerprint(linePoint), pointFingerprint(circlePoint),
      pointFingerprint(arcPoint), pointFingerprint(centerPoint),
      pointFingerprint(originPoint)};
  auto uniqueReferenceFingerprints = referenceFingerprints;
  std::sort(uniqueReferenceFingerprints.begin(),
            uniqueReferenceFingerprints.end());
  CHECK(std::adjacent_find(uniqueReferenceFingerprints.begin(),
                           uniqueReferenceFingerprints.end()) ==
        uniqueReferenceFingerprints.end());

  Sketch placement;
  placement.addLine({0.0, 0.0}, {10.0, 0.0});
  Dimension placed;
  placed.kind = DimensionKind::LineLength;
  placed.geometryId = placement.lineId(0);
  placed.valueMm = 10.0;
  CHECK(storeReferenceDimension(placement, placed));
  const auto placementBefore = placement.semanticFingerprint();
  placement.beginDeltaJournal();
  CHECK(placement.setDimensionPlacement(0, 9.0, 0.75));
  auto placementDelta = placement.finishDeltaJournal();
  const auto placementAfter = placement.semanticFingerprint();
  CHECK(placementAfter != placementBefore);
  CHECK(placement.applyDelta(placementDelta, false));
  CHECK(placement.semanticFingerprint() == placementBefore);
  CHECK(placement.applyDelta(placementDelta, true));
  CHECK(placement.semanticFingerprint() == placementAfter);

  // Removing the only newly-created entity still changes the persisted next
  // IDs. The counter-only delta must therefore be non-empty and reversible.
  Sketch counters;
  const auto countersBefore = counters.semanticFingerprint();
  counters.beginDeltaJournal();
  counters.addLine({0.0, 0.0}, {1.0, 0.0});
  counters.removeLine(0);
  auto counterDelta = counters.finishDeltaJournal();
  CHECK(counterDelta.lines.empty());
  CHECK(!counterDelta.empty());
  CHECK(counters.semanticFingerprint() != countersBefore);
  CHECK(counters.applyDelta(counterDelta, false));
  CHECK(counters.semanticFingerprint() == countersBefore);
}

void constraintJournalRollbackIsReversible() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  Constraint horizontal;
  horizontal.type = ConstraintType::Horizontal;
  horizontal.firstGeometry = sketch.lineId(0);
  const auto horizontalId = sketch.addConstraint(horizontal);
  CHECK(horizontalId != kInvalidConstraintId);
  const auto original = sketch.semanticFingerprint();

  sketch.beginDeltaJournal();
  CHECK(sketch.removeConstraint(horizontalId));
  const auto deletion = sketch.finishDeltaJournal();
  CHECK(sketch.constraints().empty());
  CHECK(sketch.applyDelta(deletion, false));
  CHECK(sketch.semanticFingerprint() == original);
  CHECK(sketch.constraints().size() == 1);
  CHECK(sketch.constraints()[0].id == horizontalId);

  sketch.beginDeltaJournal();
  CHECK(sketch.removeConstraint(horizontalId));
  const auto cancelled = sketch.cancelDeltaJournal();
  CHECK(!cancelled.empty());
  CHECK(sketch.semanticFingerprint() == original);
  CHECK(sketch.constraints().size() == 1);
}

void multiDeleteJournalUsesTransactionOriginIndices() {
  const auto makeLines = [] {
    Sketch sketch;
    sketch.addLine({0.0, 0.0}, {1.0, 0.0});
    sketch.addLine({3.0, 0.0}, {4.0, 0.0});
    sketch.addLine({6.0, 0.0}, {7.0, 0.0});
    return sketch;
  };
  const auto exercise = [&makeLines](bool removeLastFirst, bool removeAll) {
    Sketch sketch = makeLines();
    const auto original = sketch.semanticFingerprint();
    const auto originalIds = std::vector<GeometryId>{
        sketch.lineId(0), sketch.lineId(1), sketch.lineId(2)};
    sketch.beginDeltaJournal();
    if (removeAll) {
      sketch.removeLine(1);
      sketch.removeLine(0);
      sketch.removeLine(0);
    } else if (removeLastFirst) {
      sketch.removeLine(2);
      sketch.removeLine(0);
    } else {
      sketch.removeLine(0);
      sketch.removeLine(1);
    }
    const auto delta = sketch.finishDeltaJournal();
    const auto removed = sketch.semanticFingerprint();
    CHECK(delta.lines.size() == (removeAll ? 3 : 2));
    for (int cycle = 0; cycle < 2; ++cycle) {
      CHECK(sketch.applyDelta(delta, false));
      CHECK(sketch.semanticFingerprint() == original);
      CHECK(std::vector<GeometryId>({sketch.lineId(0), sketch.lineId(1),
                                     sketch.lineId(2)}) == originalIds);
      CHECK(sketch.applyDelta(delta, true));
      CHECK(sketch.semanticFingerprint() == removed);
    }
  };
  exercise(false, false);  // A then C.
  exercise(true, false);   // C then A.
  exercise(false, true);   // All three, deliberately non-monotonic.

  Sketch mixed;
  mixed.addLine({0.0, 0.0}, {2.0, 0.0});
  mixed.addLine({5.0, 0.0}, {7.0, 0.0});
  mixed.addCircle({20.0, 0.0}, 2.0);
  mixed.addCircle({30.0, 0.0}, 3.0);
  mixed.addCircle({40.0, 0.0}, 4.0);
  mixed.addArc({50.0, 0.0}, 2.0, 0.0, 1.0);
  mixed.addArc({60.0, 0.0}, 3.0, 0.2, 1.2);
  mixed.addArc({70.0, 0.0}, 4.0, 0.4, 1.4);
  Constraint first;
  first.id = 44;
  first.type = ConstraintType::Horizontal;
  first.firstGeometry = mixed.lineId(0);
  Constraint second = first;
  second.id = 12;
  second.firstGeometry = mixed.lineId(1);
  Constraint firstDiameter{
      DiameterConstraint{mixed.circleId(0), std::nullopt},
      ConstraintMode::Reference, 45};
  Constraint secondDiameter{
      DiameterConstraint{mixed.circleId(2), std::nullopt},
      ConstraintMode::Reference, 46};
  CHECK(mixed.restoreConstraints(
      {first, second, firstDiameter, secondDiameter}));
  Dimension dimension;
  dimension.kind = DimensionKind::CircleDiameter;
  dimension.geometryId = mixed.circleId(0);
  dimension.valueMm = 4.0;
  dimension.constraintId = 45;
  CHECK(mixed.storeDimension(dimension));
  dimension.id = kInvalidDimensionId;
  dimension.geometryId = mixed.circleId(2);
  dimension.valueMm = 8.0;
  dimension.constraintId = 46;
  CHECK(mixed.storeDimension(dimension));
  const auto mixedBefore = mixed.semanticFingerprint();
  const std::vector<GeometryId> lineIds{mixed.lineId(0), mixed.lineId(1)};
  const std::vector<GeometryId> circleIds{mixed.circleId(0), mixed.circleId(1),
                                          mixed.circleId(2)};
  const std::vector<GeometryId> arcIds{mixed.arcId(0), mixed.arcId(1),
                                       mixed.arcId(2)};
  mixed.beginDeltaJournal();
  mixed.removeCircle(0);
  mixed.removeCircle(1);
  mixed.removeArc(2);
  mixed.removeArc(0);
  mixed.removeLine(0);
  CHECK(mixed.translatePoint(PointReference{mixed.lineId(0), false},
                             1.0, 0.0)
            .committed());
  const auto mixedDelta = mixed.finishDeltaJournal();
  const auto mixedAfter = mixed.semanticFingerprint();
  for (int cycle = 0; cycle < 2; ++cycle) {
    CHECK(mixed.applyDelta(mixedDelta, false));
    CHECK(mixed.semanticFingerprint() == mixedBefore);
    CHECK(std::vector<GeometryId>({mixed.lineId(0), mixed.lineId(1)}) ==
          lineIds);
    CHECK(std::vector<GeometryId>({mixed.circleId(0), mixed.circleId(1),
                                   mixed.circleId(2)}) == circleIds);
    CHECK(std::vector<GeometryId>({mixed.arcId(0), mixed.arcId(1),
                                   mixed.arcId(2)}) == arcIds);
    CHECK(mixed.constraints()[0].id == 44);
    CHECK(mixed.constraints()[1].id == 12);
    CHECK(mixed.applyDelta(mixedDelta, true));
    CHECK(mixed.semanticFingerprint() == mixedAfter);
  }

  Sketch transient;
  transient.beginDeltaJournal();
  transient.addCircle({1.0, 1.0}, 2.0);
  transient.removeCircle(0);
  const auto transientDelta = transient.finishDeltaJournal();
  CHECK(transientDelta.circles.empty());
  CHECK(transientDelta.circleIds.empty());

  Sketch centered;
  for (const auto [element, x] :
       std::vector<std::pair<std::size_t, double>>{{700, 0.0}, {800, 20.0}}) {
    centered.addLine({x, 0.0}, {x + 10.0, 0.0}, element);
    centered.addLine({x + 10.0, 0.0}, {x + 10.0, 10.0}, element);
    centered.addLine({x + 10.0, 10.0}, {x, 10.0}, element);
    centered.addLine({x, 10.0}, {x, 0.0}, element);
    centered.markElementCenterNode(element);
  }
  Constraint centerConstraint;
  centerConstraint.id = 91;
  centerConstraint.type = ConstraintType::DistanceX;
  centerConstraint.firstPoint.elementCenterId = 700;
  centerConstraint.secondPoint.elementCenterId = 800;
  centerConstraint.value = 20.0;
  CHECK(centered.restoreConstraints({centerConstraint}));
  Dimension centerDimension;
  centerDimension.kind = DimensionKind::PointDistanceX;
  centerDimension.firstPoint.elementCenterId = 700;
  centerDimension.secondPoint.elementCenterId = 800;
  centerDimension.valueMm = 20.0;
  centerDimension.constraintId = 91;
  CHECK(centered.storeDimension(centerDimension));
  const auto centersBefore = centered.semanticFingerprint();
  centered.beginDeltaJournal();
  centered.removeElement(800);
  centered.removeElement(700);
  const auto centersDelta = centered.finishDeltaJournal();
  const auto centersAfter = centered.semanticFingerprint();
  CHECK(!centered.hasElementCenterNode(700));
  CHECK(!centered.hasElementCenterNode(800));
  CHECK(centered.constraints().empty());
  CHECK(centered.dimensions().empty());
  for (int cycle = 0; cycle < 2; ++cycle) {
    CHECK(centered.applyDelta(centersDelta, false));
    CHECK(centered.semanticFingerprint() == centersBefore);
    CHECK(centered.hasElementCenterNode(700));
    CHECK(centered.hasElementCenterNode(800));
    CHECK(centered.constraints().size() == 1);
    CHECK(centered.dimensions().size() == 1);
    CHECK(centered.applyDelta(centersDelta, true));
    CHECK(centered.semanticFingerprint() == centersAfter);
  }
}

void indexedDimensionRemovalIsStableAndReversible() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch.addLine({0.0, 5.0}, {10.0, 5.0});
  for (std::size_t index = 0; index < 4; ++index) {
    Dimension dimension;
    dimension.kind = index == 3 ? DimensionKind::PointDistanceX
                                : DimensionKind::LineLength;
    dimension.geometryId = sketch.lineId(index % 2);
    dimension.firstPoint = {sketch.lineId(0), true};
    dimension.secondPoint = {sketch.lineId(1), false};
    dimension.valueMm = 10.0 + static_cast<double>(index);
    dimension.offsetMm = 2.0 + static_cast<double>(index);
    dimension.angleRad = 0.1 * static_cast<double>(index);
    CHECK(storeReferenceDimension(sketch, dimension));
  }
  const auto original = sketch.semanticFingerprint();
  sketch.beginDeltaJournal();
  CHECK(sketch.removeDimension(0));
  CHECK(sketch.removeDimension(1));  // Original middle item after shift.
  CHECK(sketch.removeDimension(1));  // Original last item after shift.
  const auto delta = sketch.finishDeltaJournal();
  CHECK(sketch.dimensions().size() == 1);
  const auto removed = sketch.semanticFingerprint();
  for (int cycle = 0; cycle < 2; ++cycle) {
    CHECK(sketch.applyDelta(delta, false));
    CHECK(sketch.semanticFingerprint() == original);
    CHECK(sketch.dimensions().size() == 4);
    CHECK(sketch.dimensions()[3].kind == DimensionKind::PointDistanceX);
    CHECK(sketch.dimensions()[3].firstPoint.lineId == sketch.lineId(0));
    CHECK(sketch.dimensions()[3].secondPoint.lineId == sketch.lineId(1));
    CHECK(sketch.applyDelta(delta, true));
    CHECK(sketch.semanticFingerprint() == removed);
  }
  CHECK(!sketch.removeDimension(99));

  Sketch nested;
  nested.addLine({0.0, 0.0}, {10.0, 0.0});
  Dimension base;
  base.kind = DimensionKind::LineLength;
  base.geometryId = nested.lineId(0);
  base.valueMm = 10.0;
  CHECK(storeReferenceDimension(nested, base));
  const auto nestedBefore = nested.semanticFingerprint();
  nested.beginDeltaJournal();
  nested.beginDeltaJournal();
  Dimension temporary = base;
  temporary.valueMm = 12.0;
  CHECK(storeReferenceDimension(nested, temporary));
  CHECK(nested.dimensions().size() == 2);
  static_cast<void>(nested.cancelDeltaJournal());
  CHECK(nested.dimensions().size() == 1);
  CHECK(nested.removeDimension(0));
  const auto outer = nested.finishDeltaJournal();
  CHECK(nested.dimensions().empty());
  CHECK(nested.applyDelta(outer, false));
  CHECK(nested.semanticFingerprint() == nestedBefore);
}

void coordinateProximityDoesNotDefineLocalSolveComponents() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch.addLine({10.0 + 0.5e-7, 0.0}, {10.0, 5.0});
  sketch.addLine({10.0, 5.0}, {15.0, 5.0});
  sketch.addLine({15.0 + 2e-7, 5.0}, {20.0, 5.0});
  const auto first = sketch.lineId(0);
  const auto second = sketch.lineId(1);
  const auto third = sketch.lineId(2);
  const auto separate = sketch.lineId(3);
  CHECK(sketch.connectedComponent({first}).geometryIds ==
        std::vector<GeometryId>({first}));
  CHECK(sketch.connectedComponent({separate}).geometryIds ==
        std::vector<GeometryId>({separate}));

  Sketch dynamic;
  dynamic.addLine({0.0, 0.0}, {10.0, 0.0});
  dynamic.addLine({10.0 + 0.5e-7, 0.0}, {10.0, 5.0});
  const auto dynamicFirst = dynamic.lineId(0);
  const auto dynamicSecond = dynamic.lineId(1);
  CHECK(dynamic.connectedComponent({dynamicFirst}).geometryIds ==
        std::vector<GeometryId>({dynamicFirst}));
  dynamic.translateLinesByIds({dynamicSecond}, 1.0, 0.0);
  CHECK(dynamic.connectedComponent({dynamicFirst}).geometryIds ==
        std::vector<GeometryId>({dynamicFirst}));
  dynamic.translateLinesByIds({dynamicSecond}, -1.0, 0.0);
  CHECK(dynamic.connectedComponent({dynamicFirst}).geometryIds ==
        std::vector<GeometryId>({dynamicFirst}));

  const Line untouched = sketch.lines()[3];
  const auto original = sketch.semanticFingerprint();
  sketch.beginDeltaJournal();
  CHECK(sketch.translatePoint(PointReference{sketch.lineId(0), false},
                              2.0, 0.0)
            .committed());
  const auto delta = sketch.finishDeltaJournal();
  CHECK(!delta.empty());
  CHECK(sketch.lines()[0].end.xMm != sketch.lines()[1].start.xMm);
  CHECK(sketch.lines()[1].start.xMm == 10.0 + 0.5e-7);
  CHECK(sketch.lines()[3].start.xMm == untouched.start.xMm);
  CHECK(sketch.lines()[3].end.xMm == untouched.end.xMm);
  const auto edited = sketch.semanticFingerprint();
  CHECK(sketch.applyDelta(delta, false));
  CHECK(sketch.semanticFingerprint() == original);
  CHECK(sketch.applyDelta(delta, true));
  CHECK(sketch.semanticFingerprint() == edited);

  const auto result = SketchSolver::solveStableComponent(sketch, {first});
  CHECK(result.componentsVisited == 1);
  CHECK(result.geometriesVisited == 1);
}

void circleCenterCoordinateProximityIsNotTopology() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch.addCircle({10.0 + 0.5e-7, 0.0}, 2.0);
  sketch.addLine({10.0 + 1.4e-7, 0.0}, {15.0, 5.0});
  sketch.addCircle({10.0 + 4.0e-7, 0.0}, 3.0);
  const auto line = sketch.lineId(0);
  const auto circle = sketch.circleId(0);
  const auto chainedLine = sketch.lineId(1);
  const auto outsideCircle = sketch.circleId(1);
  CHECK(sketch.connectedComponent({line}).geometryIds ==
        std::vector<GeometryId>({line}));
  CHECK(sketch.connectedComponent({outsideCircle}).geometryIds ==
        std::vector<GeometryId>({outsideCircle}));

  const Circle untouched = sketch.circles()[1];
  const auto before = sketch.semanticFingerprint();
  sketch.beginDeltaJournal();
  CHECK(sketch.translatePoint(PointReference{sketch.lineId(0), false},
                              2.0, 0.0)
            .committed());
  const auto delta = sketch.finishDeltaJournal();
  CHECK(!delta.empty());
  CHECK(std::abs(sketch.lines()[0].end.xMm - 12.0) <= 1e-9);
  CHECK(std::abs(sketch.circles()[0].center.xMm - (10.0 + 0.5e-7)) <= 1e-12);
  CHECK(std::abs(sketch.lines()[1].start.xMm - (10.0 + 1.4e-7)) <= 1e-12);
  CHECK(sketch.circles()[1].center.xMm == untouched.center.xMm);
  CHECK(sketch.circles()[1].center.yMm == untouched.center.yMm);
  const auto after = sketch.semanticFingerprint();
  for (int cycle = 0; cycle < 2; ++cycle) {
    CHECK(sketch.applyDelta(delta, false));
    CHECK(sketch.semanticFingerprint() == before);
    CHECK(sketch.applyDelta(delta, true));
    CHECK(sketch.semanticFingerprint() == after);
  }
  const auto solved = SketchSolver::solveStableComponent(sketch, {line});
  CHECK(solved.componentsVisited == 1);
  CHECK(solved.geometriesVisited == 1);
  CHECK(solved.constraintsVisited == 0);
  CHECK(sketch.circles()[1].center.xMm == untouched.center.xMm);

  // The public index-based circle mutator shares the transactional component
  // path with the ID-based drag primitive; it cannot bypass junction
  // propagation or solver observability.
  Sketch dynamic;
  dynamic.addLine({0.0, 0.0}, {10.0, 0.0});
  dynamic.addCircle({10.0 + 0.5e-7, 0.0}, 2.0);
  const auto dynamicLine = dynamic.lineId(0);
  const auto dynamicCircle = dynamic.circleId(0);
  CHECK(dynamic.connectedComponent({dynamicLine}).geometryIds ==
        std::vector<GeometryId>({dynamicLine}));
  CHECK(dynamic.translateCircle(0, 1.0, 0.0).committed());
  CHECK(dynamic.connectedComponent({dynamicLine}).geometryIds ==
        std::vector<GeometryId>({dynamicLine}));
  CHECK(dynamic.geometryLocation(dynamicCircle) ==
        std::optional<GeometryLocation>({GeometryKind::Circle, 0}));
  CHECK(dynamic.translateCircle(0, -1.0, 0.0).committed());
  CHECK(dynamic.connectedComponent({dynamicLine}).geometryIds ==
        std::vector<GeometryId>({dynamicLine}));

  // A production circle drag only moves the selected geometry. Equal raw
  // coordinates are not a relationship and cannot move the line endpoint.
  const auto dragBefore = dynamic.semanticFingerprint();
  const Point lineEndBefore = dynamic.lines()[0].end;
  dynamic.beginDeltaJournal();
  dynamic.translateCircleById(dynamicCircle, 2.0, 3.0);
  const auto dragDelta = dynamic.finishDeltaJournal();
  CHECK(dynamic.lines()[0].end.xMm == lineEndBefore.xMm);
  CHECK(dynamic.lines()[0].end.yMm == lineEndBefore.yMm);
  const auto dragAfter = dynamic.semanticFingerprint();
  CHECK(dynamic.applyDelta(dragDelta, false));
  CHECK(dynamic.semanticFingerprint() == dragBefore);
  CHECK(dynamic.applyDelta(dragDelta, true));
  CHECK(dynamic.semanticFingerprint() == dragAfter);
}

void equationSolverIgnoresPersistedConstraintVectorOrder() {
  // R3: vector order is persistence detail and cannot select a solution.
  // Install the persisted vector without restoreConstraints(): that public
  // loader boundary performs a full solve and would precondition both copies
  // before this regression got a chance to distinguish vector order from ID
  // order.
  Sketch raw;
  raw.addLine({0.0, 0.0}, {10.0, 0.0});
  raw.addLine({100.0, 0.0}, {120.0, 0.0});
  raw.addLine({200.0, 0.0}, {230.0, 0.0});
  Constraint first;
  first.id = 20;
  first.type = ConstraintType::Equal;
  first.firstGeometry = raw.lineId(1);
  first.secondGeometry = raw.lineId(2);
  Constraint second;
  second.id = 10;
  second.type = ConstraintType::Equal;
  second.firstGeometry = raw.lineId(0);
  second.secondGeometry = raw.lineId(2);
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      raw, {first, second}));
  CHECK(raw.connectedComponent({raw.lineId(0)}).constraintIds ==
        std::vector<ConstraintId>({20, 10}));
  const auto rawFingerprint = raw.semanticFingerprint();
  Sketch full = raw;
  Sketch local = raw;
  const auto fullResult = SketchSolver::solveStable(full);
  const auto localResult = SketchSolver::solveStableComponent(
      local, {local.lineId(0)});
  CHECK(fullResult.converged == localResult.converged);
  CHECK(full.semanticFingerprint() == local.semanticFingerprint());
  CHECK(raw.semanticFingerprint() == rawFingerprint);
  CHECK(localResult.componentsVisited == 1);
  CHECK(localResult.geometriesVisited == 3);
  CHECK(localResult.constraintsVisited == 2);
  for (const auto& line : local.lines())
    CHECK(std::abs(std::hypot(line.end.xMm - line.start.xMm,
                              line.end.yMm - line.start.yMm) - 20.0) <= 1e-7);
  CHECK(local.constraints()[0].id == 20);
  CHECK(local.constraints()[1].id == 10);

  // The same equations in numeric-ID order must select the same nearest pose.
  Sketch idSorted;
  idSorted.addLine({0.0, 0.0}, {10.0, 0.0});
  idSorted.addLine({100.0, 0.0}, {120.0, 0.0});
  idSorted.addLine({200.0, 0.0}, {230.0, 0.0});
  Constraint sortedFirst = second;
  sortedFirst.firstGeometry = idSorted.lineId(0);
  sortedFirst.secondGeometry = idSorted.lineId(2);
  Constraint sortedSecond = first;
  sortedSecond.firstGeometry = idSorted.lineId(1);
  sortedSecond.secondGeometry = idSorted.lineId(2);
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      idSorted, {sortedFirst, sortedSecond}));
  CHECK(SketchSolver::solveStableComponent(
            idSorted, {idSorted.lineId(0)})
            .converged);
  for (const auto& line : idSorted.lines())
    CHECK(std::abs(std::hypot(line.end.xMm - line.start.xMm,
                              line.end.yMm - line.start.yMm) - 20.0) <= 1e-7);
  for (std::size_t index = 0; index < local.lines().size(); ++index) {
    CHECK(std::abs(local.lines()[index].start.xMm -
                   idSorted.lines()[index].start.xMm) <= 1e-7);
    CHECK(std::abs(local.lines()[index].end.xMm -
                   idSorted.lines()[index].end.xMm) <= 1e-7);
  }
}

void arcMidpointAndComponentDiagnosticsAreTruthful() {
  Sketch arcOnly;
  arcOnly.addArc({2.0, 3.0}, 5.0, 0.25, 1.75);
  const auto freeArc = analyzeConstraintSystem(arcOnly);
  CHECK(freeArc.variableCount == 5);
  CHECK(freeArc.degreesOfFreedom == 5);
  CHECK(freeArc.components.size() == 1);
  CHECK(freeArc.components.front().geometryIds ==
        std::vector<GeometryId>{arcOnly.arcId(0)});
  CHECK(freeArc.components.front().degreesOfFreedom == 5);

  Constraint lock;
  lock.type = ConstraintType::Lock;
  lock.firstGeometry = arcOnly.arcId(0);
  const auto locked = arcOnly.tryApplyConstraint(lock);
  CHECK(locked.status == ConstraintApplyStatus::Accepted);
  const auto lockedArc = analyzeConstraintSystem(arcOnly);
  CHECK(lockedArc.equationRank == 5);
  CHECK(lockedArc.degreesOfFreedom == 0);

  Sketch midpoint;
  midpoint.addLine({0.0, 0.0}, {10.0, 0.0});
  midpoint.addLine({2.0, 3.0}, {4.0, 6.0});
  const auto before = analyzeConstraintSystem(midpoint);
  Constraint midpointConstraint;
  midpointConstraint.type = ConstraintType::Midpoint;
  midpointConstraint.firstGeometry = midpoint.lineId(0);
  midpointConstraint.secondPoint = {midpoint.lineId(1), true};
  const auto midpointApplied = midpoint.tryApplyConstraint(midpointConstraint);
  CHECK(midpointApplied.status == ConstraintApplyStatus::Accepted);
  const auto after = analyzeConstraintSystem(midpoint);
  CHECK(after.equationRank == before.equationRank + 2);
  CHECK(after.degreesOfFreedom + 2 == before.degreesOfFreedom);

  Sketch independent;
  independent.addLine({0.0, 0.0}, {10.0, 2.0});
  independent.addLine({100.0, 0.0}, {110.0, 3.0});
  Constraint firstLock;
  firstLock.type = ConstraintType::Lock;
  firstLock.firstGeometry = independent.lineId(0);
  CHECK(independent.tryApplyConstraint(firstLock).accepted());
  const auto independentState = analyzeConstraintSystem(independent);
  CHECK(independentState.components.size() == 2);
  CHECK(independentState.degreesOfFreedom == 4);
  CHECK(independentState.components[0].degreesOfFreedom == 0);
  CHECK(independentState.components[1].degreesOfFreedom == 4);
}

void constraintPreflightClassifiesAndRollsBackAtomically() {
  Sketch angular;
  constexpr double pi = 3.14159265358979323846;
  angular.addLine({0.0, 0.0}, {10.0, 0.0});
  angular.addLine({0.0, 5.0}, {10.0 * std::cos(pi / 6.0),
                               5.0 + 10.0 * std::sin(pi / 6.0)});
  Constraint angle;
  angle.type = ConstraintType::Angle;
  angle.firstGeometry = angular.lineId(0);
  angle.secondGeometry = angular.lineId(1);
  angle.value = 30.0;
  CHECK(angular.tryApplyConstraint(angle).accepted());
  const auto angularFingerprint = angular.semanticFingerprint();
  const auto angleId = angular.constraints().front().id;
  Constraint parallel;
  parallel.type = ConstraintType::Parallel;
  parallel.firstGeometry = angular.lineId(0);
  parallel.secondGeometry = angular.lineId(1);
  const auto conflict = angular.tryApplyConstraint(parallel);
  CHECK(conflict.status == ConstraintApplyStatus::Conflicting);
  CHECK(angular.semanticFingerprint() == angularFingerprint);
  CHECK(angular.constraints().size() == 1);
  CHECK(angular.constraints().front().id == angleId);
  CHECK(std::find(conflict.relatedConstraintIds.begin(),
                  conflict.relatedConstraintIds.end(), angleId) !=
        conflict.relatedConstraintIds.end());

  Constraint stale;
  stale.type = ConstraintType::Horizontal;
  stale.firstGeometry = 999999;
  const auto staleFingerprint = angular.semanticFingerprint();
  const auto staleResult = angular.tryApplyConstraint(stale);
  CHECK(staleResult.status == ConstraintApplyStatus::InvalidReference);
  CHECK(staleResult.diagnosticCode ==
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  CHECK(angular.semanticFingerprint() == staleFingerprint);

  Constraint unsupported;
  angular.addCircle({30.0, 0.0}, 2.0);
  unsupported.type = ConstraintType::Equal;
  unsupported.firstGeometry = angular.lineId(0);
  unsupported.secondGeometry = angular.circleId(0);
  const auto unsupportedFingerprint = angular.semanticFingerprint();
  CHECK(angular.tryApplyConstraint(unsupported).status ==
        ConstraintApplyStatus::Unsupported);
  CHECK(angular.semanticFingerprint() == unsupportedFingerprint);

  Sketch orthogonal;
  orthogonal.addLine({0.0, 0.0}, {10.0, 0.0});
  Constraint horizontal;
  horizontal.type = ConstraintType::Horizontal;
  horizontal.firstGeometry = orthogonal.lineId(0);
  CHECK(orthogonal.tryApplyConstraint(horizontal).accepted());
  const auto horizontalFingerprint = orthogonal.semanticFingerprint();
  const auto horizontalId = orthogonal.constraints().front().id;
  const auto duplicate = orthogonal.tryApplyConstraint(horizontal);
  CHECK(duplicate.status == ConstraintApplyStatus::Redundant);
  CHECK(duplicate.component.geometryIds ==
        std::vector<GeometryId>{orthogonal.lineId(0)});
  CHECK(orthogonal.semanticFingerprint() == horizontalFingerprint);
  Constraint vertical = horizontal;
  vertical.type = ConstraintType::Vertical;
  const auto hvConflict = orthogonal.tryApplyConstraint(vertical);
  CHECK(hvConflict.status == ConstraintApplyStatus::Conflicting);
  CHECK(orthogonal.semanticFingerprint() == horizontalFingerprint);
  CHECK(orthogonal.constraints().size() == 1);
  CHECK(orthogonal.constraints().front().id == horizontalId);

  Sketch history;
  history.addLine({0.0, 0.0}, {10.0, 2.0});
  history.beginDeltaJournal();
  Constraint historyHorizontal;
  historyHorizontal.type = ConstraintType::Horizontal;
  historyHorizontal.firstGeometry = history.lineId(0);
  const auto accepted = history.tryApplyConstraint(historyHorizontal);
  CHECK(accepted.accepted());
  const auto addDelta = history.finishDeltaJournal();
  CHECK(!addDelta.empty());
  const auto constrainedFingerprint = history.semanticFingerprint();
  history.beginDeltaJournal();
  CHECK(history.removeConstraint(accepted.constraintId));
  const auto deleteDelta = history.finishDeltaJournal();
  CHECK(history.constraints().empty());
  CHECK(history.applyDelta(deleteDelta, false));
  CHECK(history.semanticFingerprint() == constrainedFingerprint);
  CHECK(history.constraints().front().id == accepted.constraintId);
  CHECK(history.applyDelta(deleteDelta, true));
  CHECK(history.constraints().empty());
  CHECK(history.applyDelta(deleteDelta, false));
  CHECK(history.constraints().front().id == accepted.constraintId);
}

void interactiveConstraintSemanticsStayInSync() {
  const auto makeCase = [](ConstraintType type) {
    Sketch sketch;
    Constraint constraint;
    constraint.type = type;
    switch (type) {
      case ConstraintType::Horizontal:
      case ConstraintType::Vertical:
      case ConstraintType::Length:
        sketch.addLine({0.0, 0.0}, {6.0, 2.0});
        constraint.firstGeometry = sketch.lineId(0);
        if (type == ConstraintType::Length) constraint.value = 8.0;
        break;
      case ConstraintType::Lock:
        sketch.addArc({0.0, 0.0}, 5.0, 0.2, 1.4);
        constraint.firstGeometry = sketch.arcId(0);
        break;
      case ConstraintType::Radius:
      case ConstraintType::Diameter:
        sketch.addCircle({1.0, 2.0}, 3.0);
        constraint.firstGeometry = sketch.circleId(0);
        constraint.value = type == ConstraintType::Radius ? 4.0 : 8.0;
        break;
      case ConstraintType::Parallel:
      case ConstraintType::Perpendicular:
      case ConstraintType::Equal:
      case ConstraintType::Angle:
        sketch.addLine({0.0, 0.0}, {8.0, 1.0});
        sketch.addLine({0.0, 5.0}, {4.0, 8.0});
        constraint.firstGeometry = sketch.lineId(0);
        constraint.secondGeometry = sketch.lineId(1);
        if (type == ConstraintType::Angle) constraint.value = 35.0;
        break;
      case ConstraintType::LineDistance:
        sketch.addLine({0.0, 0.0}, {8.0, 0.0});
        sketch.addLine({0.0, 5.0}, {8.0, 5.0});
        constraint.firstGeometry = sketch.lineId(0);
        constraint.secondGeometry = sketch.lineId(1);
        constraint.value = 4.0;
        {
          Constraint parallel;
          parallel.type = ConstraintType::Parallel;
          parallel.firstGeometry = constraint.firstGeometry;
          parallel.secondGeometry = constraint.secondGeometry;
          CHECK(sketch.tryApplyConstraint(parallel).accepted());
        }
        break;
      case ConstraintType::Tangent:
        sketch.addLine({-5.0, -8.0}, {-5.0, 8.0});
        sketch.addCircle({0.0, 0.0}, 5.0);
        constraint.firstGeometry = sketch.lineId(0);
        constraint.secondGeometry = sketch.circleId(0);
        break;
      case ConstraintType::Coincident:
      case ConstraintType::Distance:
      case ConstraintType::DistanceX:
      case ConstraintType::DistanceY:
        sketch.addLine({0.0, 0.0}, {3.0, 2.0});
        sketch.addLine({8.0, 7.0}, {10.0, 9.0});
        constraint.firstPoint = {sketch.lineId(0), false};
        constraint.secondPoint = {sketch.lineId(1), true};
        if (type != ConstraintType::Coincident) constraint.value = 6.0;
        break;
      case ConstraintType::Midpoint:
      case ConstraintType::PointOnLine:
        sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addLine({3.0, 4.0}, {5.0, 6.0});
        constraint.firstGeometry = sketch.lineId(0);
        constraint.secondPoint = {sketch.lineId(1), true};
        break;
      case ConstraintType::PointOnCircle:
        sketch.addCircle({0.0, 0.0}, 5.0);
        sketch.addLine({7.0, 1.0}, {9.0, 3.0});
        constraint.firstGeometry = sketch.circleId(0);
        constraint.secondPoint = {sketch.lineId(0), true};
        break;
      case ConstraintType::PointOnArc:
        sketch.addArc({0.0, 0.0}, 5.0, 0.0, 1.5);
        sketch.addLine({4.0, 3.0}, {7.0, 5.0});
        constraint.firstGeometry = sketch.arcId(0);
        constraint.secondPoint = {sketch.lineId(0), true};
        break;
      case ConstraintType::PointOnXAxis:
      case ConstraintType::PointOnYAxis:
        sketch.addLine({3.0, 4.0}, {7.0, 8.0});
        constraint.secondPoint = {sketch.lineId(0), true};
        break;
    }
    return std::pair{std::move(sketch), constraint};
  };

  for (int raw = static_cast<int>(ConstraintType::Horizontal);
       raw <= static_cast<int>(ConstraintType::PointOnYAxis); ++raw) {
    const auto type = static_cast<ConstraintType>(raw);
    auto [sketch, constraint] = makeCase(type);
    const auto applied = sketch.tryApplyConstraint(constraint);
    if (!applied.accepted())
      std::cerr << "constraint semantic matrix rejected type " << raw
                << " status " << static_cast<int>(applied.status) << ": "
                << applied.diagnostic << '\n';
    CHECK(applied.accepted());
    CHECK(applied.constraintId != kInvalidConstraintId);
    auto diagnostics = analyzeConstraintSystem(sketch);
    CHECK(!hasConstraintViolation(diagnostics, applied.constraintId));

    if (type != ConstraintType::Lock) {
      if (constraint.firstGeometry != kInvalidGeometryId)
        SketchTestAccess::perturbGeometry(sketch,
                                          constraint.firstGeometry);
      else
        SketchTestAccess::perturbPoint(sketch, constraint.secondPoint);
      const auto solved = SketchSolver::solveStable(sketch);
      if (solved.unsupported != 0 || solved.invalidReferences != 0)
        std::cerr << "constraint semantic matrix solve failed type " << raw
                  << " unsupported=" << solved.unsupported
                  << " invalid=" << solved.invalidReferences << '\n';
      CHECK(solved.unsupported == 0);
      CHECK(solved.invalidReferences == 0);
      diagnostics = analyzeConstraintSystem(sketch);
      CHECK(!hasConstraintViolation(diagnostics, applied.constraintId));
    }
  }
}

void r0FailedSolveIsAtomicAndObservable() {
  Sketch orphaned;
  orphaned.addLine({0.0, 0.0}, {10.0, 4.0});
  const auto orphanedLine = orphaned.lineId(0);
  Constraint validHorizontal;
  validHorizontal.id = 40;
  validHorizontal.type = ConstraintType::Horizontal;
  validHorizontal.firstGeometry = orphanedLine;
  Constraint orphanHorizontal = validHorizontal;
  orphanHorizontal.id = 41;
  orphanHorizontal.firstGeometry = 999999;
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      orphaned, {validHorizontal, orphanHorizontal}));
  const auto orphanedFingerprint = orphaned.semanticFingerprint();
  const auto orphanedGeometry = orphaned.lines().front();
  const auto orphanedSolve = SketchSolver::solveStable(orphaned);
  CHECK(!orphanedSolve.converged);
  CHECK(orphanedSolve.invalidReferences == 1);
  CHECK(orphaned.lines().front().start == orphanedGeometry.start);
  CHECK(orphaned.lines().front().end == orphanedGeometry.end);
  CHECK(orphaned.semanticFingerprint() == orphanedFingerprint);

  Sketch emptyReference;
  emptyReference.addLine({0.0, 0.0}, {8.0, 3.0});
  Constraint emptyHorizontal;
  emptyHorizontal.id = 43;
  emptyHorizontal.type = ConstraintType::Horizontal;
  Constraint movableHorizontal;
  movableHorizontal.id = 44;
  movableHorizontal.type = ConstraintType::Horizontal;
  movableHorizontal.firstGeometry = emptyReference.lineId(0);
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      emptyReference, {movableHorizontal, emptyHorizontal}));
  const auto emptyFingerprint = emptyReference.semanticFingerprint();
  const auto emptyGeometry = emptyReference.lines().front();
  const auto emptySolve = SketchSolver::solveStable(emptyReference);
  CHECK(!emptySolve.converged);
  CHECK(emptySolve.invalidReferences == 1);
  CHECK(emptyReference.lines().front().start == emptyGeometry.start);
  CHECK(emptyReference.lines().front().end == emptyGeometry.end);
  CHECK(emptyReference.semanticFingerprint() == emptyFingerprint);

  Sketch tinyCircle;
  tinyCircle.addCircle({2.0, 3.0}, 1e-12);
  Constraint tinyLock;
  tinyLock.id = 42;
  tinyLock.type = ConstraintType::Lock;
  tinyLock.firstGeometry = tinyCircle.circleId(0);
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      tinyCircle, {tinyLock}));
  const auto tinySolve = SketchSolver::solveStable(tinyCircle);
  CHECK(tinySolve.converged);
  CHECK(tinySolve.degenerateGeometry == 0);

  Sketch tinyArc;
  tinyArc.addArc({2.0, 3.0}, 1e-12, 0.0, 1.0);
  Constraint tinyArcLock;
  tinyArcLock.id = 46;
  tinyArcLock.type = ConstraintType::Lock;
  tinyArcLock.firstGeometry = tinyArc.arcId(0);
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      tinyArc, {tinyArcLock}));
  const auto tinyArcSolve = SketchSolver::solveStable(tinyArc);
  CHECK(tinyArcSolve.converged);
  CHECK(tinyArcSolve.degenerateGeometry == 0);

  Sketch tinyBezier;
  tinyBezier.addBezier({0.0, 0.0}, {-0.9e-9, 0.0}, {0.9e-9, 0.0},
                       {0.0, 0.0});
  Constraint tinyBezierLock;
  tinyBezierLock.id = 45;
  tinyBezierLock.type = ConstraintType::Lock;
  tinyBezierLock.firstGeometry = tinyBezier.bezierId(0);
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      tinyBezier, {tinyBezierLock}));
  const auto tinyBezierSolve = SketchSolver::solveStable(tinyBezier);
  CHECK(tinyBezierSolve.converged);
  CHECK(tinyBezierSolve.degenerateGeometry == 0);

  Sketch locked;
  locked.addLine({0.0, 0.0}, {10.0, 4.0});
  const auto lockedLine = locked.lineId(0);
  Constraint lock;
  lock.id = 1;
  lock.type = ConstraintType::Lock;
  lock.firstGeometry = lockedLine;
  Constraint horizontal;
  horizontal.id = 2;
  horizontal.type = ConstraintType::Horizontal;
  horizontal.firstGeometry = lockedLine;
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      locked, {lock, horizontal}));
  const auto lockedFingerprint = locked.semanticFingerprint();
  const auto lockedGeometry = locked.lines().front();
  const auto failedSolve =
      SketchSolver::solveStableComponent(locked, {lockedLine});
  CHECK(!failedSolve.converged);
  CHECK(failedSolve.violatedConstraints != 0);
  CHECK(locked.lines().front().start == lockedGeometry.start);
  CHECK(locked.lines().front().end == lockedGeometry.end);
  CHECK(locked.semanticFingerprint() == lockedFingerprint);

  Sketch conflicting;
  conflicting.addLine({0.0, 0.0}, {10.0, 0.0});
  const auto conflictedLine = conflicting.lineId(0);
  Constraint h;
  h.id = 10;
  h.type = ConstraintType::Horizontal;
  h.firstGeometry = conflictedLine;
  Constraint v;
  v.id = 11;
  v.type = ConstraintType::Vertical;
  v.firstGeometry = conflictedLine;
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      conflicting, {h, v}));
  const auto before = conflicting.semanticFingerprint();
  const auto depth = conflicting.deltaJournalDepth();
  const auto rejected = conflicting.translatePoint(
      {conflictedLine, false}, 3.0, 2.0);
  CHECK(!rejected.committed());
  CHECK(rejected.status == SketchMutationStatus::Conflicting ||
        rejected.status == SketchMutationStatus::NonConverged);
  CHECK(rejected.diagnosticCode ==
            SketchMutationDiagnosticCode::ConstraintResidualViolation ||
        rejected.diagnosticCode ==
            SketchMutationDiagnosticCode::DegenerateGeometry ||
        rejected.diagnosticCode ==
            SketchMutationDiagnosticCode::SolverDidNotConverge);
  CHECK(!rejected.relatedConstraintIds.empty());
  CHECK(conflicting.semanticFingerprint() == before);
  CHECK(conflicting.deltaJournalDepth() == depth);

  Sketch independent;
  independent.addLine({0.0, 0.0}, {5.0, 2.0});
  const auto freeLine = independent.lineId(0);
  const auto applied = independent.translateLinesByIds({freeLine}, 2.0, -1.0);
  CHECK(applied.status == SketchMutationStatus::Applied);
  CHECK(applied.committed());
  CHECK(independent.deltaJournalDepth() == 0);
  CHECK(independent.lines().front().start.xMm == 2.0);
  CHECK(independent.lines().front().start.yMm == -1.0);

  const auto validFingerprint = independent.semanticFingerprint();
  const auto invalid = independent.translateLinesByIds({999999}, 1.0, 0.0);
  CHECK(invalid.status == SketchMutationStatus::InvalidReference);
  CHECK(invalid.diagnosticCode ==
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  CHECK(independent.semanticFingerprint() == validFingerprint);
  CHECK(independent.deltaJournalDepth() == 0);

  Constraint length;
  length.type = ConstraintType::Length;
  length.firstGeometry = freeLine;
  length.value = std::hypot(
      independent.lines().front().end.xMm -
          independent.lines().front().start.xMm,
      independent.lines().front().end.yMm -
          independent.lines().front().start.yMm);
  const auto lengthApplied = independent.tryApplyConstraint(length);
  CHECK(lengthApplied.accepted());
  const auto beforeUnsupported = independent.semanticFingerprint();
  const auto unsupported = independent.setConstraintValue(
      lengthApplied.constraintId, std::numeric_limits<double>::infinity());
  CHECK(unsupported.status == SketchMutationStatus::Unsupported);
  CHECK(unsupported.diagnosticCode ==
        SketchMutationDiagnosticCode::UnsupportedConstraint);
  CHECK(independent.semanticFingerprint() == beforeUnsupported);
  CHECK(independent.deltaJournalDepth() == 0);

  const auto beforeNan = independent.semanticFingerprint();
  const auto nanMove = independent.translatePoint(
      {freeLine, true}, std::numeric_limits<double>::quiet_NaN(), 0.0);
  CHECK(nanMove.status == SketchMutationStatus::InternalError);
  CHECK(nanMove.diagnosticCode ==
        SketchMutationDiagnosticCode::NonFiniteGeometry);
  CHECK(independent.semanticFingerprint() == beforeNan);
  CHECK(independent.deltaJournalDepth() == 0);

  Sketch zeroLength;
  zeroLength.addLine({0.0, 0.0}, {2.0, 0.0});
  const auto zeroLine = zeroLength.lineId(0);
  Constraint axis;
  axis.id = 20;
  axis.type = ConstraintType::Horizontal;
  axis.firstGeometry = zeroLine;
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      zeroLength, {axis}));
  const auto zeroBefore = zeroLength.semanticFingerprint();
  const auto collapsed = zeroLength.translatePoint({zeroLine, false}, -2.0, 0.0);
  CHECK(!collapsed.committed());
  CHECK(collapsed.diagnosticCode ==
        SketchMutationDiagnosticCode::DegenerateGeometry);
  CHECK(zeroLength.semanticFingerprint() == zeroBefore);
  CHECK(zeroLength.deltaJournalDepth() == 0);

  Sketch cached;
  cached.addLine({0.0, 0.0}, {4.0, 0.0});
  const auto cachedLine = cached.lineId(0);
  Constraint cachedHorizontal;
  cachedHorizontal.id = 50;
  cachedHorizontal.type = ConstraintType::Horizontal;
  cachedHorizontal.firstGeometry = cachedLine;
  CHECK(SketchTestAccess::installPersistedConstraintsWithoutSolve(
      cached, {cachedHorizontal}));
  CHECK(SketchSolver::solveStable(cached).converged);
  const auto cachedFingerprint = cached.semanticFingerprint();
  const auto cachedRejected =
      cached.translatePoint({cachedLine, false}, -4.0, 0.0);
  CHECK(!cachedRejected.committed());
  CHECK(cached.semanticFingerprint() == cachedFingerprint);
  const auto cachedAfterRollback = SketchSolver::solveStable(cached);
  CHECK(cachedAfterRollback.converged);
  CHECK(cachedAfterRollback.passes == 0);
  CHECK(cachedAfterRollback.componentsVisited == 0);

  Sketch indexedCircle;
  indexedCircle.addCircle({0.0, 0.0}, 2.0);
  const auto indexedMove = indexedCircle.translateCircle(0, 3.0, -1.0);
  CHECK(indexedMove.committed());
  CHECK(indexedCircle.circles().front().center.xMm == 3.0);
  CHECK(indexedCircle.circles().front().center.yMm == -1.0);
  const auto staleIndexedMove = indexedCircle.translateCircle(1, 1.0, 0.0);
  CHECK(staleIndexedMove.status == SketchMutationStatus::InvalidReference);
}

void rectangleSlotPolygonChainsStayExplicit() {
  Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {8.0, 5.0});

  sketch.addLine({15.0, 2.0}, {25.0, 2.0});
  const auto slotTop = sketch.lineId(sketch.lines().size() - 1);
  sketch.addLine({25.0, 2.0}, {25.0, -2.0});
  const auto slotRight = sketch.lineId(sketch.lines().size() - 1);
  sketch.addLine({25.0, -2.0}, {15.0, -2.0});
  const auto slotBottom = sketch.lineId(sketch.lines().size() - 1);
  sketch.addLine({15.0, -2.0}, {15.0, 2.0});
  const auto slotLeft = sketch.lineId(sketch.lines().size() - 1);

  sketch.addLine({35.0, 0.0}, {41.0, 0.0});
  const auto polygonA = sketch.lineId(sketch.lines().size() - 1);
  sketch.addLine({41.0, 0.0}, {38.0, 5.0});
  const auto polygonB = sketch.lineId(sketch.lines().size() - 1);
  sketch.addLine({38.0, 5.0}, {35.0, 0.0});
  const auto polygonC = sketch.lineId(sketch.lines().size() - 1);

  std::vector<Constraint> explicitConnections;
  ConstraintId nextId = 1;
  const auto connect = [&explicitConnections, &nextId](
                           PointReference first, PointReference second) {
    explicitConnections.emplace_back(
        CoincidentConstraint{first, second}, ConstraintMode::Driving,
        nextId++);
  };
  connect({slotTop, false}, {slotRight, true});
  connect({slotRight, false}, {slotBottom, true});
  connect({slotBottom, false}, {slotLeft, true});
  connect({slotLeft, false}, {slotTop, true});
  connect({polygonA, false}, {polygonB, true});
  connect({polygonB, false}, {polygonC, true});
  connect({polygonC, false}, {polygonA, true});
  CHECK(sketch.restoreConstraints(std::move(explicitConnections)));

  const auto components = sketch.constraintComponents();
  CHECK(components.size() == 3);
  CHECK(!analyzeConstraintSystem(sketch).conflicting);
  CHECK(SketchSolver::solveStable(sketch).converged);
}

}  // namespace

int main() {
  transactionalConstraintDoesNotBreakOldOne();
  pointOnLineSurvivesLaterDimension();
  dofUsesConstraintRank();
  incompatibleDatumAxisDoesNotReenterSolver();
  removingArcDropsPointAndConstraintReferences();
  removingCenteredElementLineDropsCenterReferences();
  indexesAndConnectivityStayDeterministic();
  componentSolveLeavesIndependentGeometryBitwiseStable();
  rejectedConstraintNeverPoisonsFullSolveCache();
  sharedOriginDoesNotJoinComponents();
  localizedSketchDeltaIsBoundedAndReversible();
  mutationJournalUsesNoFullSketchCheckpoints();
  deltaApplyIsAtomicAndIndependentOfConstraintIdOrder();
  dimensionIdsAndAllocatorAreDeltaSafe();
  persistedSemanticFingerprintIncludesNonSolverFields();
  constraintJournalRollbackIsReversible();
  multiDeleteJournalUsesTransactionOriginIndices();
  indexedDimensionRemovalIsStableAndReversible();
  coordinateProximityDoesNotDefineLocalSolveComponents();
  circleCenterCoordinateProximityIsNotTopology();
  equationSolverIgnoresPersistedConstraintVectorOrder();
  arcMidpointAndComponentDiagnosticsAreTruthful();
  constraintPreflightClassifiesAndRollsBackAtomically();
  interactiveConstraintSemanticsStayInSync();
  r0FailedSolveIsAtomicAndObservable();
  rectangleSlotPolygonChainsStayExplicit();
  return EXIT_SUCCESS;
}

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
  sketch.storeDimension(endpointDistance);

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
  CHECK(sketch.constraints().size() == 2);

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
  sketch.storeDimension(centerDistance);

  Constraint centerConstraint;
  centerConstraint.type = ConstraintType::Distance;
  centerConstraint.firstPoint = origin;
  centerConstraint.secondPoint = center;
  centerConstraint.value = std::sqrt(125.0);
  const ConstraintId centerConstraintId =
      sketch.addConstraint(centerConstraint);
  CHECK(centerConstraintId != kInvalidConstraintId);
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
  CHECK(sketch.addConstraint(lineCircle) == 101);

  Constraint circleArc;
  circleArc.id = 102;
  circleArc.type = ConstraintType::Coincident;
  circleArc.firstPoint.circleId = circle;
  circleArc.secondPoint.arcId = arc;
  CHECK(sketch.addConstraint(circleArc) == 102);

  Constraint centerDistance;
  centerDistance.id = 103;
  centerDistance.type = ConstraintType::DistanceX;
  centerDistance.firstPoint.origin = true;
  centerDistance.secondPoint.elementCenterId = rectangleElement;
  centerDistance.value = 45.0;
  CHECK(sketch.addConstraint(centerDistance) == 103);

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

  const auto result = BasicSketchSolver::solveStableComponent(
      sketch, {sketch.lineId(0)});
  CHECK(result.componentsVisited == 1);
  CHECK(result.geometriesVisited == 1);
  CHECK(result.constraintsVisited == 1);
  CHECK(result.lockedSnapshotSize == 0);

  const auto fullAfterPartial = BasicSketchSolver::solveStable(sketch);
  CHECK(fullAfterPartial.passes > 0);
  CHECK(fullAfterPartial.componentsVisited == 2);
  const auto unchanged = BasicSketchSolver::solveStable(sketch);
  CHECK(unchanged.passes == 0);
  CHECK(unchanged.componentsVisited == 0);
  CHECK(unchanged.converged);

  Constraint lock;
  lock.type = ConstraintType::Lock;
  lock.firstGeometry = sketch.lineId(0);
  CHECK(sketch.addConstraint(lock) != kInvalidConstraintId);
  const auto locked = BasicSketchSolver::solveStableComponent(
      sketch, {sketch.lineId(0)});
  CHECK(locked.lockedSnapshotSize == 1);
  CHECK(locked.geometriesVisited == 1);
}

void partialSolveNeverPublishesFullSolveCache() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch.addLine({50.0, 2.0}, {60.0, 3.0});
  Constraint invalid;
  invalid.id = 700;
  invalid.type = ConstraintType::Horizontal;
  invalid.firstGeometry = 999999;
  CHECK(sketch.addConstraint(invalid) == 700);
  Constraint local;
  local.id = 701;
  local.type = ConstraintType::Horizontal;
  local.firstGeometry = sketch.lineId(1);
  CHECK(sketch.addConstraint(local) == 701);

  const auto partial = BasicSketchSolver::solveStableComponent(
      sketch, {sketch.lineId(1)});
  CHECK(partial.converged);
  CHECK(partial.invalidReferences == 0);
  const auto full = BasicSketchSolver::solveStable(sketch);
  CHECK(!full.converged);
  CHECK(full.invalidReferences == 1);
  CHECK(full.constraintsVisited == 2);
}

void sharedOriginDoesNotJoinComponents() {
  Sketch sketch;
  sketch.addLine({5.0, 0.0}, {10.0, 0.0});
  sketch.addLine({0.0, 8.0}, {0.0, 12.0});
  for (std::size_t index = 0; index < 2; ++index) {
    Constraint distance;
    distance.id = 800 + index;
    distance.type = index == 0 ? ConstraintType::DistanceX
                               : ConstraintType::DistanceY;
    distance.firstPoint.origin = true;
    distance.secondPoint = {sketch.lineId(index), true};
    distance.value = index == 0 ? 5.0 : 8.0;
    CHECK(sketch.addConstraint(distance) == 800 + index);
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
  CHECK(sketch.addConstraint(first) == 20);
  Constraint second;
  second.id = 10;
  second.type = ConstraintType::Length;
  second.firstGeometry = shared;
  second.value = 20.0;
  CHECK(sketch.addConstraint(second) == 10);
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
  after.storeDimension(makeDimension(after.lineId(0), 1));
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
  replay.storeDimension(automatic);
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
  journal.storeDimension(makeDimension(journal.lineId(0), 1));
  const auto journalDelta = journal.finishDeltaJournal();
  CHECK(journalDelta.beforeNextDimensionId == 1);
  CHECK(journalDelta.afterNextDimensionId == 2);
  CHECK(journal.applyDelta(journalDelta, false));
  CHECK(journal.semanticallyEqual(before));
  CHECK(journal.applyDelta(journalDelta, true));
  CHECK(journal.semanticallyEqual(after));

  // A pure persistent-ID replacement must not disappear from makeDelta.
  Sketch idBefore = before;
  idBefore.storeDimension(makeDimension(idBefore.lineId(0), 7));
  Sketch idAfter = before;
  idAfter.storeDimension(makeDimension(idAfter.lineId(0), 9));
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
  high.storeDimension(makeDimension(
      high.lineId(0), std::numeric_limits<DimensionId>::max()));
  high.storeDimension(makeDimension(high.lineId(0), kInvalidDimensionId));
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
  twoAfter.storeDimension(makeDimension(twoAfter.lineId(0), 1));
  twoAfter.storeDimension(makeDimension(twoAfter.lineId(0), 2));
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
  dimensioned.storeDimension(dimension);
  CHECK(dimensioned.solverFingerprint() == solverHash);
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
    item.secondPoint.origin = true;
    item.valueMm = 2.0;
    candidate.storeDimension(item);
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
  placement.storeDimension(placed);
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

void orphanConstraintJournalRollbackIsReversible() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  Constraint orphan;
  orphan.id = 700;
  orphan.type = ConstraintType::Horizontal;
  orphan.firstGeometry = 999999;
  CHECK(sketch.addConstraint(orphan) == 700);
  const auto original = sketch.semanticFingerprint();

  sketch.beginDeltaJournal();
  CHECK(sketch.removeConstraint(700));
  const auto deletion = sketch.finishDeltaJournal();
  CHECK(sketch.constraints().empty());
  CHECK(sketch.applyDelta(deletion, false));
  CHECK(sketch.semanticFingerprint() == original);
  CHECK(sketch.constraints().size() == 1);
  CHECK(sketch.constraints()[0].id == 700);

  sketch.beginDeltaJournal();
  CHECK(sketch.removeConstraint(700));
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
  CHECK(mixed.addConstraint(first) == 44);
  Constraint second = first;
  second.id = 12;
  second.firstGeometry = mixed.lineId(1);
  CHECK(mixed.addConstraint(second) == 12);
  Dimension dimension;
  dimension.kind = DimensionKind::CircleDiameter;
  dimension.geometryId = mixed.circleId(0);
  dimension.valueMm = 4.0;
  mixed.storeDimension(dimension);
  dimension.geometryId = mixed.circleId(2);
  dimension.valueMm = 8.0;
  mixed.storeDimension(dimension);
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
  CHECK(mixed.setLineLength(0, 3.0));
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
  CHECK(centered.addConstraint(centerConstraint) == 91);
  Dimension centerDimension;
  centerDimension.kind = DimensionKind::PointDistanceX;
  centerDimension.firstPoint.elementCenterId = 700;
  centerDimension.secondPoint.elementCenterId = 800;
  centerDimension.valueMm = 20.0;
  centered.storeDimension(centerDimension);
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
    sketch.storeDimension(dimension);
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
  nested.storeDimension(base);
  const auto nestedBefore = nested.semanticFingerprint();
  nested.beginDeltaJournal();
  nested.beginDeltaJournal();
  Dimension temporary = base;
  temporary.valueMm = 12.0;
  nested.storeDimension(temporary);
  CHECK(nested.dimensions().size() == 2);
  static_cast<void>(nested.cancelDeltaJournal());
  CHECK(nested.dimensions().size() == 1);
  CHECK(nested.removeDimension(0));
  const auto outer = nested.finishDeltaJournal();
  CHECK(nested.dimensions().empty());
  CHECK(nested.applyDelta(outer, false));
  CHECK(nested.semanticFingerprint() == nestedBefore);
}

void coordinateJunctionsDefineLocalSolveComponents() {
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
        std::vector<GeometryId>({first, second, third}));
  CHECK(sketch.connectedComponent({separate}).geometryIds ==
        std::vector<GeometryId>({separate}));

  Sketch dynamic;
  dynamic.addLine({0.0, 0.0}, {10.0, 0.0});
  dynamic.addLine({10.0 + 0.5e-7, 0.0}, {10.0, 5.0});
  const auto dynamicFirst = dynamic.lineId(0);
  const auto dynamicSecond = dynamic.lineId(1);
  CHECK(dynamic.connectedComponent({dynamicFirst}).geometryIds.size() == 2);
  dynamic.translateLinesByIds({dynamicSecond}, 1.0, 0.0);
  CHECK(dynamic.connectedComponent({dynamicFirst}).geometryIds ==
        std::vector<GeometryId>({dynamicFirst}));
  dynamic.translateLinesByIds({dynamicSecond}, -1.0, 0.0);
  CHECK(dynamic.connectedComponent({dynamicFirst}).geometryIds.size() == 2);

  const Line untouched = sketch.lines()[3];
  const auto original = sketch.semanticFingerprint();
  sketch.beginDeltaJournal();
  CHECK(sketch.setLineLength(0, 12.0));
  const auto delta = sketch.finishDeltaJournal();
  CHECK(!delta.empty());
  CHECK(sketch.lines()[0].end.xMm == sketch.lines()[1].start.xMm);
  CHECK(sketch.lines()[0].end.yMm == sketch.lines()[1].start.yMm);
  CHECK(sketch.lines()[3].start.xMm == untouched.start.xMm);
  CHECK(sketch.lines()[3].end.xMm == untouched.end.xMm);
  const auto edited = sketch.semanticFingerprint();
  CHECK(sketch.applyDelta(delta, false));
  CHECK(sketch.semanticFingerprint() == original);
  CHECK(sketch.applyDelta(delta, true));
  CHECK(sketch.semanticFingerprint() == edited);

  const auto result = BasicSketchSolver::solveStableComponent(sketch, {first});
  CHECK(result.componentsVisited == 1);
  CHECK(result.geometriesVisited == 3);
}

void circleCenterCoordinateJunctionsAreTransitiveAndReversible() {
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
        std::vector<GeometryId>({line, circle, chainedLine}));
  CHECK(sketch.connectedComponent({outsideCircle}).geometryIds ==
        std::vector<GeometryId>({outsideCircle}));

  const Circle untouched = sketch.circles()[1];
  const auto before = sketch.semanticFingerprint();
  sketch.beginDeltaJournal();
  CHECK(sketch.setLineLength(0, 12.0));
  const auto delta = sketch.finishDeltaJournal();
  CHECK(!delta.empty());
  CHECK(std::abs(sketch.lines()[0].end.xMm - 12.0) <= 1e-9);
  CHECK(std::abs(sketch.circles()[0].center.xMm - 12.0) <= 1e-9);
  CHECK(std::abs(sketch.lines()[1].start.xMm - 12.0) <= 1e-9);
  CHECK(sketch.circles()[1].center.xMm == untouched.center.xMm);
  CHECK(sketch.circles()[1].center.yMm == untouched.center.yMm);
  const auto after = sketch.semanticFingerprint();
  for (int cycle = 0; cycle < 2; ++cycle) {
    CHECK(sketch.applyDelta(delta, false));
    CHECK(sketch.semanticFingerprint() == before);
    CHECK(sketch.applyDelta(delta, true));
    CHECK(sketch.semanticFingerprint() == after);
  }
  const auto solved = BasicSketchSolver::solveStableComponent(sketch, {line});
  CHECK(solved.componentsVisited == 1);
  CHECK(solved.geometriesVisited == 3);
  CHECK(solved.constraintsVisited == 0);
  CHECK(sketch.circles()[1].center.xMm == untouched.center.xMm);

  // Moving a raw circle primitive away and back invalidates only the
  // coordinate-dependent connectivity cache; stable identity indexes remain
  // usable throughout.
  Sketch dynamic;
  dynamic.addLine({0.0, 0.0}, {10.0, 0.0});
  dynamic.addCircle({10.0 + 0.5e-7, 0.0}, 2.0);
  const auto dynamicLine = dynamic.lineId(0);
  const auto dynamicCircle = dynamic.circleId(0);
  CHECK(dynamic.connectedComponent({dynamicLine}).geometryIds ==
        std::vector<GeometryId>({dynamicLine, dynamicCircle}));
  dynamic.translateCircle(0, 1.0, 0.0);
  CHECK(dynamic.connectedComponent({dynamicLine}).geometryIds ==
        std::vector<GeometryId>({dynamicLine}));
  CHECK(dynamic.geometryLocation(dynamicCircle) ==
        std::optional<GeometryLocation>({GeometryKind::Circle, 0}));
  dynamic.translateCircle(0, -1.0, 0.0);
  CHECK(dynamic.connectedComponent({dynamicLine}).geometryIds ==
        std::vector<GeometryId>({dynamicLine, dynamicCircle}));

  // The production circle drag primitive expands the same implicit raw
  // junction, so line endpoints and circle centres never diverge.
  const auto dragBefore = dynamic.semanticFingerprint();
  const double originalJunctionOffset =
      dynamic.circles()[0].center.xMm - dynamic.lines()[0].end.xMm;
  dynamic.beginDeltaJournal();
  dynamic.translateCircleById(dynamicCircle, 2.0, 3.0);
  const auto dragDelta = dynamic.finishDeltaJournal();
  CHECK(std::abs((dynamic.circles()[0].center.xMm -
                  dynamic.lines()[0].end.xMm) -
                 originalJunctionOffset) <= 1e-12);
  CHECK(std::abs(dynamic.circles()[0].center.xMm -
                 dynamic.lines()[0].end.xMm) <= 1e-7);
  CHECK(std::abs(dynamic.circles()[0].center.yMm -
                 dynamic.lines()[0].end.yMm) <= 1e-9);
  const auto dragAfter = dynamic.semanticFingerprint();
  CHECK(dynamic.applyDelta(dragDelta, false));
  CHECK(dynamic.semanticFingerprint() == dragBefore);
  CHECK(dynamic.applyDelta(dragDelta, true));
  CHECK(dynamic.semanticFingerprint() == dragAfter);
}

void componentSolvePreservesPersistedConstraintOrder() {
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
  const auto fullResult = BasicSketchSolver::solveStable(full);
  const auto localResult = BasicSketchSolver::solveStableComponent(
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

  // The same raw geometry with the vector sorted by numeric ID takes the
  // other deterministic branch (10 mm). This makes the test fail if component
  // extraction ever sorts by ID again.
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
  CHECK(BasicSketchSolver::solveStableComponent(
            idSorted, {idSorted.lineId(0)})
            .converged);
  for (const auto& line : idSorted.lines())
    CHECK(std::abs(std::hypot(line.end.xMm - line.start.xMm,
                              line.end.yMm - line.start.yMm) - 10.0) <= 1e-7);
  CHECK(idSorted.semanticFingerprint() != local.semanticFingerprint());
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
  partialSolveNeverPublishesFullSolveCache();
  sharedOriginDoesNotJoinComponents();
  localizedSketchDeltaIsBoundedAndReversible();
  mutationJournalUsesNoFullSketchCheckpoints();
  deltaApplyIsAtomicAndIndependentOfConstraintIdOrder();
  dimensionIdsAndAllocatorAreDeltaSafe();
  persistedSemanticFingerprintIncludesNonSolverFields();
  orphanConstraintJournalRollbackIsReversible();
  multiDeleteJournalUsesTransactionOriginIndices();
  indexedDimensionRemovalIsStableAndReversible();
  coordinateJunctionsDefineLocalSolveComponents();
  circleCenterCoordinateJunctionsAreTransitiveAndReversible();
  componentSolvePreservesPersistedConstraintOrder();
  return EXIT_SUCCESS;
}

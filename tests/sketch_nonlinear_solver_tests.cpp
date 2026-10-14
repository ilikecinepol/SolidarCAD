#include "TestAssertions.h"

#include "sketch/NonlinearSketchSolver.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

namespace solidar::sketch {

class SketchTestAccess {
 public:
  static void installConstraints(Sketch& sketch,
                                 std::vector<Constraint> constraints) {
    sketch.constraints_ = std::move(constraints);
    sketch.nextConstraintId_ = 1;
    for (const auto& constraint : sketch.constraints_)
      sketch.nextConstraintId_ =
          std::max(sketch.nextConstraintId_, constraint.id + 1);
    sketch.invalidateStructureIndexes();
  }
};

}  // namespace solidar::sketch

using namespace solidar::sketch;

namespace {

Constraint identified(Constraint constraint, ConstraintId id) {
  constraint.id = id;
  return constraint;
}

ConstraintGraphComponent onlyComponent(const Sketch& sketch) {
  const auto graph = ConstraintGraph::build(sketch);
  CHECK(graph.components().size() == 1);
  return graph.components().front();
}

void immutableSolveProducesCandidate() {
  Sketch sketch;
  sketch.addLine({2.0, 3.0}, {8.0, 7.0});
  const auto line = sketch.lineId(0);
  const PointReference start{LineEndpointReference{line, Endpoint::Start}};
  SketchTestAccess::installConstraints(
      sketch,
      {identified(Constraint{PointOnXAxisConstraint{start}}, 1),
       identified(Constraint{PointOnYAxisConstraint{start}}, 2),
       identified(Constraint{HorizontalConstraint{line}}, 3),
       identified(Constraint{LengthConstraint{line, 5.0}}, 4)});
  const auto before = sketch.semanticFingerprint();
  const auto report = productionSketchSolver().solve(
      SolveRequest{sketch, onlyComponent(sketch)});
  CHECK(report.solved());
  CHECK(report.status == SolveStatus::SolvedFullyConstrained);
  CHECK(report.degreesOfFreedom == 0);
  CHECK(report.maxNormalizedResidual <= 1.0);
  CHECK(sketch.semanticFingerprint() == before);
  const auto evaluated = ConstraintEquationRegistry::assemble(
      sketch, report.component, report.candidate);
  CHECK(evaluated.has_value());
  CHECK(EquationDiagnostics::analyze(*evaluated).maxNormalizedResidual <= 1.0);
}

void underConstrainedSolutionIsNearestAndOrderIndependent() {
  auto make = [](bool reverse) {
    Sketch sketch;
    sketch.addLine({1.0, 4.0}, {9.0, 8.0});
    std::vector<Constraint> constraints{
        identified(Constraint{HorizontalConstraint{sketch.lineId(0)}}, 20)};
    if (reverse) std::reverse(constraints.begin(), constraints.end());
    SketchTestAccess::installConstraints(sketch, std::move(constraints));
    return sketch;
  };
  auto first = make(false);
  auto second = make(true);
  const auto a = productionSketchSolver().solve(
      SolveRequest{first, onlyComponent(first)});
  const auto b = productionSketchSolver().solve(
      SolveRequest{second, onlyComponent(second)});
  CHECK(a.status == SolveStatus::SolvedUnderConstrained);
  CHECK(b.status == SolveStatus::SolvedUnderConstrained);
  CHECK(a.candidate.size() == b.candidate.size());
  for (std::size_t index = 0; index < a.candidate.size(); ++index)
    CHECK(std::abs(a.candidate[index] - b.candidate[index]) <= 1e-12);
  CHECK(std::abs(a.candidate[1] - 6.0) <= 1e-7);
  CHECK(std::abs(a.candidate[3] - 6.0) <= 1e-7);
}

void transientHintMovesOnlyWithinNullspace() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {5.0, 0.0});
  const auto line = sketch.lineId(0);
  SketchTestAccess::installConstraints(
      sketch, {identified(Constraint{HorizontalConstraint{line}}, 1)});
  SolveRequest request{sketch, onlyComponent(sketch)};
  request.transientHints.push_back(
      {{line, ParameterRole::LineStartX}, 10.0});
  const auto report = productionSketchSolver().solve(request);
  CHECK(report.solved());
  CHECK(report.usedTransientHints);
  CHECK(std::abs(report.candidate[0] - 10.0) <= 1e-7);
  const auto evaluated = ConstraintEquationRegistry::assemble(
      sketch, report.component, report.candidate);
  CHECK(evaluated.has_value());
  CHECK(EquationDiagnostics::analyze(*evaluated).maxNormalizedResidual <= 1.0);
}

void failedHintCorrectionRestoresAcceptedCandidate() {
  Sketch sketch;
  sketch.addCircle({0.0, 0.0}, 5.0);
  const auto circle = sketch.circleId(0);
  const PointReference center{CircleCenterReference{circle}};
  SketchTestAccess::installConstraints(
      sketch, {identified(Constraint{PointOnXAxisConstraint{center}}, 1)});
  SolveRequest request{sketch, onlyComponent(sketch)};
  request.transientHints.push_back(
      {{circle, ParameterRole::CircleRadius}, -1.0});
  const auto report = productionSketchSolver().solve(request);
  CHECK(report.solved());
  const auto evaluated = ConstraintEquationRegistry::assemble(
      sketch, report.component, report.candidate);
  CHECK(evaluated.has_value());
  CHECK(EquationDiagnostics::analyze(*evaluated).maxNormalizedResidual <= 1.0);
  CHECK(std::all_of(report.candidate.begin(), report.candidate.end(),
                    [](double value) { return std::isfinite(value); }));
}

void redundantAndConflictingConstraintsAreTyped() {
  Sketch redundant;
  redundant.addLine({0.0, 0.0}, {5.0, 0.0});
  const auto redundantLine = redundant.lineId(0);
  SketchTestAccess::installConstraints(
      redundant,
      {identified(Constraint{HorizontalConstraint{redundantLine}}, 1),
       identified(Constraint{HorizontalConstraint{redundantLine}}, 2)});
  const auto redundantReport = productionSketchSolver().solve(
      SolveRequest{redundant, onlyComponent(redundant)});
  CHECK(redundantReport.status == SolveStatus::Redundant);
  CHECK(redundantReport.redundantConstraintIds ==
        std::vector<ConstraintId>({1, 2}));
  const auto redundantDiagnostics = analyzeConstraintSystem(redundant);
  CHECK(redundantDiagnostics.status == SolveStatus::Redundant);
  CHECK(redundantDiagnostics.redundantConstraintIds ==
        std::vector<ConstraintId>({1, 2}));

  Sketch conflicting;
  conflicting.addLine({0.0, 0.0}, {2.5, 0.0});
  const auto conflictingLine = conflicting.lineId(0);
  SketchTestAccess::installConstraints(
      conflicting,
      {identified(Constraint{LengthConstraint{conflictingLine, 2.0}}, 10),
       identified(Constraint{LengthConstraint{conflictingLine, 3.0}}, 20)});
  const auto before = conflicting.semanticFingerprint();
  const auto conflictReport = productionSketchSolver().solve(
      SolveRequest{conflicting, onlyComponent(conflicting)});
  CHECK(conflictReport.status == SolveStatus::Conflicting);
  CHECK(conflictReport.conflictingConstraintIds ==
        std::vector<ConstraintId>({10, 20}));
  CHECK(conflicting.semanticFingerprint() == before);
  const auto conflictDiagnostics = analyzeConstraintSystem(conflicting);
  CHECK(conflictDiagnostics.status == SolveStatus::Conflicting);
  CHECK(conflictDiagnostics.conflictingConstraintIds ==
        std::vector<ConstraintId>({10, 20}));
}

void multipleIndependentConflictsYieldLocalMinimalIds() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {2.5, 1.0});
  const auto line = sketch.lineId(0);
  const std::vector<Constraint> constraints{
      identified(Constraint{LengthConstraint{line, 2.0}}, 10),
      identified(Constraint{LengthConstraint{line, 3.0}}, 20),
      identified(Constraint{HorizontalConstraint{line}}, 30),
      identified(Constraint{VerticalConstraint{line}}, 40)};
  SketchTestAccess::installConstraints(sketch, constraints);
  const auto report = productionSketchSolver().solve(
      SolveRequest{sketch, onlyComponent(sketch)});
  CHECK(report.status == SolveStatus::Conflicting);
  CHECK(!report.conflictingConstraintIds.empty());
  CHECK(report.conflictSetComplete);
  for (const auto removed : report.conflictingConstraintIds) {
    SolveRequest probe{sketch, onlyComponent(sketch)};
    for (const auto& constraint : constraints)
      if (constraint.id == removed ||
          std::find(report.conflictingConstraintIds.begin(),
                    report.conflictingConstraintIds.end(), constraint.id) ==
              report.conflictingConstraintIds.end())
        probe.ignoredConstraintIds.push_back(constraint.id);
    probe.localizeConflicts = false;
    probe.diagnoseRedundancy = false;
    CHECK(productionSketchSolver().solve(probe).solved());
  }
}

void largeConflictLocalizationIsBoundedAndNeverInventsIds() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {2.5, 0.0});
  const auto line = sketch.lineId(0);
  std::vector<Constraint> constraints;
  for (ConstraintId id = 1; id <= 65; ++id)
    constraints.push_back(identified(
        Constraint{LengthConstraint{line, id % 2 == 0 ? 2.0 : 3.0}}, id));
  SketchTestAccess::installConstraints(sketch, std::move(constraints));
  const auto report = productionSketchSolver().solve(
      SolveRequest{sketch, onlyComponent(sketch)});
  CHECK(report.status == SolveStatus::NonConverged);
  CHECK(!report.conflictSetComplete);
  CHECK(report.conflictingConstraintIds.empty());
}

void ignoredLockReallyReleasesItsParameters() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {5.0, 0.0});
  const auto line = sketch.lineId(0);
  SketchTestAccess::installConstraints(
      sketch,
      {identified(Constraint{LockConstraint{line}}, 1),
       identified(Constraint{HorizontalConstraint{line}}, 2)});
  SolveRequest request{sketch, onlyComponent(sketch)};
  request.ignoredConstraintIds = {1};
  const auto report = productionSketchSolver().solve(request);
  CHECK(report.solved());
  CHECK(report.candidate.size() == 4);
  CHECK(report.degreesOfFreedom == 3);
}

void diagnosticsEvaluateTheSolvedCandidateAndDofOwners() {
  Sketch unsolved;
  unsolved.addLine({0.0, 0.0}, {5.0, 3.0});
  SketchTestAccess::installConstraints(
      unsolved,
      {identified(Constraint{HorizontalConstraint{unsolved.lineId(0)}}, 1)});
  const auto diagnostics = analyzeConstraintSystem(unsolved, true);
  CHECK(!diagnostics.conflicting);
  CHECK(diagnostics.violations.empty());
  CHECK(diagnostics.status == SolveStatus::SolvedUnderConstrained);

  Sketch owners;
  owners.addLine({0.0, 0.0}, {5.0, 0.0});
  owners.addLine({0.0, 10.0}, {5.0, 10.0});
  const auto locked = owners.lineId(0);
  const auto free = owners.lineId(1);
  SketchTestAccess::installConstraints(
      owners,
      {identified(Constraint{LockConstraint{locked}}, 1),
       identified(Constraint{EqualConstraint{locked, free}}, 2)});
  const auto report = productionSketchSolver().solve(
      SolveRequest{owners, onlyComponent(owners)});
  CHECK(report.solved());
  CHECK(report.rankComputed);
  CHECK(report.degreesOfFreedom == 3);
  CHECK(report.remainingDofGeometryIds == std::vector<GeometryId>{free});
}

void symmetricEqualDragUsesHintsWithoutOperandLocks() {
  const auto drag = [](bool firstTarget) {
    Sketch sketch;
    sketch.addLine({0.0, 0.0}, {5.0, 0.0});
    sketch.addLine({0.0, 10.0}, {5.0, 10.0});
    const auto first = sketch.lineId(0);
    const auto second = sketch.lineId(1);
    SketchTestAccess::installConstraints(
        sketch, {identified(Constraint{EqualConstraint{first, second}}, 1)});
    const auto target = firstTarget ? first : second;
    const auto mutation = sketch.translatePoint(
        PointReference{LineEndpointReference{target, Endpoint::End}}, 5.0, 0.0);
    CHECK(mutation.committed());
    CHECK(!mutation.solveResult.rankComputed);
    return std::array{
        std::hypot(sketch.lines()[0].end.xMm - sketch.lines()[0].start.xMm,
                   sketch.lines()[0].end.yMm - sketch.lines()[0].start.yMm),
        std::hypot(sketch.lines()[1].end.xMm - sketch.lines()[1].start.xMm,
                   sketch.lines()[1].end.yMm - sketch.lines()[1].start.yMm)};
  };
  const auto first = drag(true);
  const auto second = drag(false);
  CHECK(std::abs(first[0] - first[1]) <= 1e-7);
  CHECK(std::abs(second[0] - second[1]) <= 1e-7);
  CHECK(std::abs(first[0] - second[0]) <= 1e-7);
  CHECK(first[0] > 9.0);
}

void malformedInputIsContained() {
  Sketch sketch;
  SketchTestAccess::installConstraints(
      sketch,
      {identified(Constraint{HorizontalConstraint{GeometryId{999}}}, 1)});
  const auto report = productionSketchSolver().solve(
      SolveRequest{sketch, onlyComponent(sketch)});
  CHECK(report.status == SolveStatus::Malformed);
  CHECK(!report.solved());
}

void allConstraintPermutationsHaveOneCanonicalSolution() {
  std::array<int, 4> order{0, 1, 2, 3};
  std::vector<double> canonical;
  SolveStatus canonicalStatus = SolveStatus::BackendFailure;
  do {
    Sketch sketch;
    sketch.addLine({3.0, 4.0}, {9.0, 8.0});
    const auto line = sketch.lineId(0);
    const PointReference start{LineEndpointReference{line, Endpoint::Start}};
    const std::array<Constraint, 4> source{
        identified(Constraint{PointOnXAxisConstraint{start}}, 10),
        identified(Constraint{PointOnYAxisConstraint{start}}, 20),
        identified(Constraint{HorizontalConstraint{line}}, 30),
        identified(Constraint{LengthConstraint{line, 7.0}}, 40)};
    std::vector<Constraint> constraints;
    for (const int index : order) constraints.push_back(source[index]);
    SketchTestAccess::installConstraints(sketch, std::move(constraints));
    const auto report = productionSketchSolver().solve(
        SolveRequest{sketch, onlyComponent(sketch)});
    CHECK(report.solved());
    CHECK(report.equationProvenance.size() == 4);
    if (canonical.empty()) {
      canonical = report.candidate;
      canonicalStatus = report.status;
    } else {
      CHECK(report.status == canonicalStatus);
      CHECK(report.candidate.size() == canonical.size());
      for (std::size_t index = 0; index < canonical.size(); ++index)
        CHECK(std::abs(report.candidate[index] - canonical[index]) <= 1e-10);
    }
  } while (std::next_permutation(order.begin(), order.end()));
}

void explicitPreviousCandidateAndImpossibleHintAreStable() {
  Sketch sketch;
  sketch.addLine({0.0, 2.0}, {5.0, 7.0});
  const auto line = sketch.lineId(0);
  SketchTestAccess::installConstraints(
      sketch, {identified(Constraint{HorizontalConstraint{line}}, 1)});
  SolveRequest warm{sketch, onlyComponent(sketch)};
  warm.previousCandidate = {20.0, 10.0, 25.0, 10.0};
  const auto warmReport = productionSketchSolver().solve(warm);
  CHECK(warmReport.status == SolveStatus::SolvedUnderConstrained);
  CHECK(std::abs(warmReport.candidate[0] - 20.0) <= 1e-9);
  CHECK(std::abs(warmReport.candidate[2] - 25.0) <= 1e-9);
  CHECK(std::abs(warmReport.candidate[1] - 10.0) <= 1e-9);
  CHECK(std::abs(warmReport.candidate[3] - 10.0) <= 1e-9);

  Sketch fixed;
  fixed.addLine({0.0, 0.0}, {5.0, 0.0});
  const auto fixedLine = fixed.lineId(0);
  const PointReference fixedStart{
      LineEndpointReference{fixedLine, Endpoint::Start}};
  SketchTestAccess::installConstraints(
      fixed,
      {identified(Constraint{PointOnXAxisConstraint{fixedStart}}, 1),
       identified(Constraint{PointOnYAxisConstraint{fixedStart}}, 2),
       identified(Constraint{HorizontalConstraint{fixedLine}}, 3),
       identified(Constraint{LengthConstraint{fixedLine, 5.0}}, 4)});
  SolveRequest impossible{fixed, onlyComponent(fixed)};
  impossible.transientHints.push_back(
      {{fixedLine, ParameterRole::LineEndY}, 100.0});
  const auto impossibleReport = productionSketchSolver().solve(impossible);
  CHECK(impossibleReport.status == SolveStatus::SolvedFullyConstrained);
  const auto evaluated = ConstraintEquationRegistry::assemble(
      fixed, impossibleReport.component, impossibleReport.candidate);
  CHECK(evaluated);
  CHECK(EquationDiagnostics::analyze(*evaluated).maxNormalizedResidual <= 1.0);
}

void nonConvergenceIsNotReportedAsConflict() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {1.0, 0.0});
  SketchTestAccess::installConstraints(
      sketch,
      {identified(Constraint{LengthConstraint{sketch.lineId(0), 50.0}}, 1)});
  SolveRequest request{sketch, onlyComponent(sketch)};
  request.maximumIterations = 0;
  request.localizeConflicts = false;
  request.diagnoseRedundancy = false;
  const auto report = productionSketchSolver().solve(request);
  CHECK(report.status == SolveStatus::NonConverged);
  CHECK(report.conflictingConstraintIds.empty());
}

void seededSmallSystemsPreserveInvariants() {
  std::mt19937_64 random(0x534f4c49444152ULL);
  std::uniform_real_distribution<double> coordinate(-50.0, 50.0);
  std::uniform_real_distribution<double> length(0.5, 40.0);
  for (int sample = 0; sample < 64; ++sample) {
    Sketch sketch;
    const Point start{coordinate(random), coordinate(random)};
    sketch.addLine(start, {start.xMm + coordinate(random),
                           start.yMm + coordinate(random)});
    const auto line = sketch.lineId(0);
    const double target = length(random);
    std::vector<Constraint> constraints{
        identified(Constraint{HorizontalConstraint{line}}, 11),
        identified(Constraint{LengthConstraint{line, target}}, 22)};
    if ((random() & 1U) != 0) std::reverse(constraints.begin(), constraints.end());
    SketchTestAccess::installConstraints(sketch, std::move(constraints));
    const auto before = sketch.semanticFingerprint();
    const auto report = productionSketchSolver().solve(
        SolveRequest{sketch, onlyComponent(sketch)});
    CHECK(report.solved());
    CHECK(sketch.semanticFingerprint() == before);
    const auto evaluated = ConstraintEquationRegistry::assemble(
        sketch, report.component, report.candidate);
    CHECK(evaluated);
    CHECK(EquationDiagnostics::analyze(*evaluated).maxNormalizedResidual <= 1.0);
  }
}

void disconnectedComponentsAndSolveCacheRemainTyped() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {5.0, 2.0});
  sketch.addLine({100.0, 10.0}, {105.0, 14.0});
  const auto firstLine = sketch.lineId(0);
  const auto secondLine = sketch.lineId(1);
  SketchTestAccess::installConstraints(
      sketch,
      {identified(Constraint{HorizontalConstraint{firstLine}}, 10),
       identified(Constraint{HorizontalConstraint{secondLine}}, 20)});

  const auto first = SketchSolver::solveStable(sketch);
  CHECK(first.status == SolveStatus::SolvedUnderConstrained);
  CHECK(first.componentsVisited == 2);
  CHECK(first.rankComputed);
  CHECK(first.degreesOfFreedom == 6);
  const auto fullDiagnostics = analyzeConstraintSystem(sketch, true);
  CHECK(fullDiagnostics.degreesOfFreedom == 6);
  const auto fingerprint = sketch.semanticFingerprint();

  const auto cached = SketchSolver::solveStable(sketch);
  CHECK(cached.status == first.status);
  CHECK(cached.degreesOfFreedom == first.degreesOfFreedom);
  CHECK(cached.equationRank == first.equationRank);
  CHECK(cached.componentsVisited == 0);
  CHECK(cached.passes == 0);
  CHECK(sketch.semanticFingerprint() == fingerprint);

  const auto untouched = sketch.lines()[1];
  const auto localized = SketchSolver::solveStableComponent(
      sketch, {firstLine}, 16, kInvalidConstraintId, false);
  CHECK(localized.componentsVisited == 1);
  CHECK(sketch.lines()[1].start == untouched.start);
  CHECK(sketch.lines()[1].end == untouched.end);
}

}  // namespace

int main() {
  immutableSolveProducesCandidate();
  underConstrainedSolutionIsNearestAndOrderIndependent();
  transientHintMovesOnlyWithinNullspace();
  failedHintCorrectionRestoresAcceptedCandidate();
  redundantAndConflictingConstraintsAreTyped();
  multipleIndependentConflictsYieldLocalMinimalIds();
  largeConflictLocalizationIsBoundedAndNeverInventsIds();
  ignoredLockReallyReleasesItsParameters();
  diagnosticsEvaluateTheSolvedCandidateAndDofOwners();
  symmetricEqualDragUsesHintsWithoutOperandLocks();
  malformedInputIsContained();
  allConstraintPermutationsHaveOneCanonicalSolution();
  explicitPreviousCandidateAndImpossibleHintAreStable();
  nonConvergenceIsNotReportedAsConflict();
  seededSmallSystemsPreserveInvariants();
  disconnectedComponentsAndSolveCacheRemainTyped();
  std::cout << "sketch nonlinear solver tests passed\n";
  return 0;
}

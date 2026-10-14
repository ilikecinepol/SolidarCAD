#include "TestAssertions.h"

#include "sketch/ConstraintEquationRegistry.h"
#include "sketch/ConstraintGraph.h"
#include "sketch/EquationDiagnostics.h"
#include "sketch/ParameterLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
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

  static void reverseConstraints(Sketch& sketch) {
    std::reverse(sketch.constraints_.begin(), sketch.constraints_.end());
    sketch.invalidateStructureIndexes();
  }

  static void setParameter(Sketch& sketch, const ParameterProvenance& parameter,
                           double value) {
    const auto location = sketch.geometryLocation(parameter.geometryId);
    CHECK(location.has_value());
    switch (parameter.role) {
      case ParameterRole::LineStartX:
        sketch.lines_[location->index].start.xMm = value; break;
      case ParameterRole::LineStartY:
        sketch.lines_[location->index].start.yMm = value; break;
      case ParameterRole::LineEndX:
        sketch.lines_[location->index].end.xMm = value; break;
      case ParameterRole::LineEndY:
        sketch.lines_[location->index].end.yMm = value; break;
      case ParameterRole::CircleCenterX:
        sketch.circles_[location->index].center.xMm = value; break;
      case ParameterRole::CircleCenterY:
        sketch.circles_[location->index].center.yMm = value; break;
      case ParameterRole::CircleRadius:
        sketch.circles_[location->index].radiusMm = value; break;
      case ParameterRole::ArcCenterX:
        sketch.arcs_[location->index].center.xMm = value; break;
      case ParameterRole::ArcCenterY:
        sketch.arcs_[location->index].center.yMm = value; break;
      case ParameterRole::ArcRadius:
        sketch.arcs_[location->index].radiusMm = value; break;
      case ParameterRole::ArcStartAngle:
        sketch.arcs_[location->index].startAngleRad = value; break;
      case ParameterRole::ArcSweepAngle:
        sketch.arcs_[location->index].sweepAngleRad = value; break;
      default: {
        const int offset = static_cast<int>(parameter.role) -
                           static_cast<int>(ParameterRole::BezierPoint0X);
        CHECK(offset >= 0 && offset < 8);
        auto& point = sketch.beziers_[location->index].points[offset / 2];
        if (offset % 2 == 0) point.xMm = value;
        else point.yMm = value;
        break;
      }
    }
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

std::vector<EquationSystem> assembleAll(const Sketch& sketch) {
  std::vector<EquationSystem> result;
  const auto graph = ConstraintGraph::build(sketch);
  for (const auto& component : graph.components())
    result.push_back(ConstraintEquationRegistry::assemble(sketch, component));
  return result;
}

Sketch completeFixture() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  const auto horizontal = sketch.lineId(0);
  sketch.addLine({10.0, 0.0}, {10.0, 10.0});
  const auto vertical = sketch.lineId(1);
  sketch.addLine({0.0, 1.0}, {10.0, 1.0});
  const auto parallel = sketch.lineId(2);
  sketch.addLine({5.0, 0.0}, {6.0, 1.0});
  const auto pointOwner = sketch.lineId(3);
  sketch.addLine({5.0, -10.0}, {5.0, 10.0});
  const auto tangentLine = sketch.lineId(4);
  sketch.addCircle({0.0, 0.0}, 5.0);
  const auto circle = sketch.circleId(0);
  // Keep the point at angle zero strictly inside the finite arc span so the
  // numerical Jacobian oracle does not sample a branch boundary.
  sketch.addArc({0.0, 0.0}, 5.0, -0.5, 1.5);
  const auto arc = sketch.arcId(0);
  sketch.addBezier({5.0, 0.0}, {6.0, 0.0}, {7.0, 1.0}, {8.0, 1.0});
  const auto bezier = sketch.bezierId(0);
  sketch.addCircle({30.0, 30.0}, 2.0);
  const auto lockedCircle = sketch.circleId(1);

  const PointReference hStart{LineEndpointReference{horizontal, Endpoint::Start}};
  const PointReference hEnd{LineEndpointReference{horizontal, Endpoint::End}};
  const PointReference vStart{LineEndpointReference{vertical, Endpoint::Start}};
  const PointReference vEnd{LineEndpointReference{vertical, Endpoint::End}};
  const PointReference middle{LineEndpointReference{pointOwner, Endpoint::Start}};

  std::vector<Constraint> constraints;
  constraints.push_back(identified(Constraint{HorizontalConstraint{horizontal}}, 101));
  constraints.push_back(identified(Constraint{VerticalConstraint{vertical}}, 102));
  constraints.push_back(identified(Constraint{CoincidentConstraint{hEnd, vStart}}, 103));
  constraints.push_back(identified(Constraint{PointOnLineConstraint{horizontal, middle}}, 104));
  constraints.push_back(identified(Constraint{DistanceConstraint{hStart, hEnd, 10.0}}, 105));
  constraints.push_back(identified(Constraint{LengthConstraint{horizontal, 10.0}}, 106));
  constraints.push_back(identified(Constraint{RadiusConstraint{circle, 5.0}}, 107));
  constraints.push_back(identified(Constraint{DiameterConstraint{circle, 10.0}}, 108));
  constraints.push_back(identified(Constraint{ParallelConstraint{horizontal, parallel}}, 109));
  constraints.push_back(identified(Constraint{PerpendicularConstraint{horizontal, vertical}}, 110));
  constraints.push_back(identified(Constraint{EqualConstraint{horizontal, vertical}}, 111));
  constraints.push_back(identified(Constraint{AngleConstraint{horizontal, vertical, 90.0}}, 112));
  constraints.push_back(identified(Constraint{DistanceXConstraint{hStart, hEnd, 10.0}}, 113));
  constraints.push_back(identified(Constraint{DistanceYConstraint{vStart, vEnd, 10.0}}, 114));
  constraints.push_back(identified(Constraint{PointOnCircleConstraint{circle, middle}}, 115));
  constraints.push_back(identified(Constraint{TangentConstraint{tangentLine, circle}}, 116));
  constraints.push_back(identified(Constraint{LineDistanceConstraint{horizontal, parallel, 1.0}}, 117));
  constraints.push_back(identified(Constraint{LockConstraint{lockedCircle}}, 118));
  constraints.push_back(identified(Constraint{PointOnArcConstraint{arc, middle}}, 119));
  constraints.push_back(identified(Constraint{MidpointConstraint{horizontal, middle}}, 120));
  constraints.push_back(identified(Constraint{PointOnXAxisConstraint{middle}}, 121));
  constraints.push_back(identified(Constraint{PointOnYAxisConstraint{hStart}}, 122));
  constraints.push_back(identified(Constraint{TangentConstraint{horizontal, bezier}}, 123));
  SketchTestAccess::installConstraints(sketch, std::move(constraints));
  return sketch;
}

void registryCoversEveryProductionType() {
  const auto rules = ConstraintEquationRegistry::rules();
  CHECK(rules.size() == 22);
  std::set<ConstraintType> types;
  for (const auto& rule : rules) types.insert(rule.type);
  CHECK(types.size() == rules.size());
  for (int value = static_cast<int>(ConstraintType::Horizontal);
       value <= static_cast<int>(ConstraintType::PointOnYAxis); ++value)
    CHECK(types.contains(static_cast<ConstraintType>(value)));
}

void exactZeroFixturesHaveFiniteRows() {
  const auto systems = assembleAll(completeFixture());
  std::set<ConstraintType> represented{ConstraintType::Lock};
  for (const auto& system : systems) {
    CHECK(std::isfinite(static_cast<double>(system.semanticHash())));
    for (const auto& row : system.rows()) {
      represented.insert(row.provenance.constraintType);
      CHECK(row.status == EquationStatus::Valid);
      CHECK(std::isfinite(row.residual));
      CHECK(std::abs(row.residual) <= 1e-10);
    }
    for (const double value : system.jacobian()) CHECK(std::isfinite(value));
  }
  CHECK(represented.size() == 22);
}

void everyTypeHasKnownResidual() {
  Sketch sketch;
  std::vector<Constraint> constraints;
  const auto line = [&](Point start, Point end) {
    const auto index = sketch.lines().size();
    sketch.addLine(start, end);
    return sketch.lineId(index);
  };
  const auto circle = [&](Point center, double radius) {
    const auto index = sketch.circles().size();
    sketch.addCircle(center, radius);
    return sketch.circleId(index);
  };
  const auto arc = [&](Point center, double radius, double start, double sweep) {
    const auto index = sketch.arcs().size();
    sketch.addArc(center, radius, start, sweep);
    return sketch.arcId(index);
  };
  const auto start = [](GeometryId id) {
    return PointReference{LineEndpointReference{id, Endpoint::Start}};
  };
  const auto end = [](GeometryId id) {
    return PointReference{LineEndpointReference{id, Endpoint::End}};
  };
  auto add = [&](Constraint value, ConstraintId id) {
    constraints.push_back(identified(std::move(value), id));
  };

  const auto horizontal = line({0.0, 0.0}, {2.0, 3.0});
  add(Constraint{HorizontalConstraint{horizontal}}, 1);
  const auto vertical = line({10.0, 0.0}, {12.0, 3.0});
  add(Constraint{VerticalConstraint{vertical}}, 2);
  const auto coincident = line({20.0, 0.0}, {22.0, 3.0});
  add(Constraint{CoincidentConstraint{start(coincident), end(coincident)}}, 3);
  const auto carrier = line({30.0, 0.0}, {32.0, 0.0});
  const auto pointOwner = line({31.0, 1.0}, {32.0, 2.0});
  add(Constraint{PointOnLineConstraint{carrier, start(pointOwner)}}, 4);
  const auto distanceLine = line({40.0, 0.0}, {43.0, 4.0});
  add(Constraint{DistanceConstraint{start(distanceLine), end(distanceLine), 2.0}}, 5);
  const auto lengthLine = line({50.0, 0.0}, {53.0, 4.0});
  add(Constraint{LengthConstraint{lengthLine, 2.0}}, 6);
  const auto radiusCircle = circle({60.0, 0.0}, 5.0);
  add(Constraint{RadiusConstraint{radiusCircle, 2.0}}, 7);
  const auto diameterCircle = circle({70.0, 0.0}, 5.0);
  add(Constraint{DiameterConstraint{diameterCircle, 8.0}}, 8);
  const auto parallelA = line({80.0, 0.0}, {82.0, 0.0});
  const auto parallelB = line({80.0, 2.0}, {81.0, 3.0});
  add(Constraint{ParallelConstraint{parallelA, parallelB}}, 9);
  const auto perpendicularA = line({90.0, 0.0}, {92.0, 0.0});
  const auto perpendicularB = line({90.0, 2.0}, {91.0, 3.0});
  add(Constraint{PerpendicularConstraint{perpendicularA, perpendicularB}}, 10);
  const auto equalA = line({100.0, 0.0}, {103.0, 4.0});
  const auto equalB = line({100.0, 2.0}, {102.0, 2.0});
  add(Constraint{EqualConstraint{equalA, equalB}}, 11);
  const auto angleA = line({110.0, 0.0}, {112.0, 0.0});
  const auto angleB = line({110.0, 2.0}, {110.0, 4.0});
  add(Constraint{AngleConstraint{angleA, angleB, 60.0}}, 12);
  const auto dxLine = line({120.0, 0.0}, {123.0, 4.0});
  add(Constraint{DistanceXConstraint{start(dxLine), end(dxLine), 2.0}}, 13);
  const auto dyLine = line({130.0, 0.0}, {133.0, 4.0});
  add(Constraint{DistanceYConstraint{start(dyLine), end(dyLine), 2.0}}, 14);
  const auto pointCircle = circle({140.0, 0.0}, 2.0);
  const auto circlePoint = line({145.0, 0.0}, {146.0, 1.0});
  add(Constraint{PointOnCircleConstraint{pointCircle, start(circlePoint)}}, 15);
  const auto tangentLine = line({155.0, -5.0}, {155.0, 5.0});
  const auto tangentCircle = circle({150.0, 0.0}, 2.0);
  add(Constraint{TangentConstraint{tangentLine, tangentCircle}}, 16);
  const auto lineDistanceA = line({160.0, 0.0}, {162.0, 0.0});
  const auto lineDistanceB = line({160.0, 4.0}, {162.0, 4.0});
  add(Constraint{LineDistanceConstraint{lineDistanceA, lineDistanceB, 1.0}}, 17);
  const auto locked = circle({170.0, 0.0}, 2.0);
  add(Constraint{LockConstraint{locked}}, 18);
  const auto pointArc = arc({180.0, 0.0}, 2.0, -0.5, 1.0);
  const auto arcPointOwner = line({185.0, 0.0}, {186.0, 1.0});
  add(Constraint{PointOnArcConstraint{pointArc, start(arcPointOwner)}}, 19);
  const auto midpointLine = line({190.0, 0.0}, {194.0, 0.0});
  const auto midpointPoint = line({193.0, 2.0}, {194.0, 3.0});
  add(Constraint{MidpointConstraint{midpointLine, start(midpointPoint)}}, 20);
  const auto xPoint = line({200.0, 3.0}, {201.0, 4.0});
  add(Constraint{PointOnXAxisConstraint{start(xPoint)}}, 21);
  const auto yPoint = line({2.0, 210.0}, {3.0, 211.0});
  add(Constraint{PointOnYAxisConstraint{start(yPoint)}}, 22);

  SketchTestAccess::installConstraints(sketch, std::move(constraints));
  std::map<ConstraintId, std::vector<double>> residuals;
  bool lockWasFixed = false;
  for (const auto& system : assembleAll(sketch)) {
    if (std::find(system.component().constraintIds.begin(),
                  system.component().constraintIds.end(), ConstraintId{18}) !=
        system.component().constraintIds.end())
      lockWasFixed = system.variableCount() == 0 && system.equationCount() == 0;
    for (const auto& row : system.rows())
      residuals[row.provenance.constraintId].push_back(row.residual);
  }
  const auto check = [&](ConstraintId id, std::size_t scalar, double expected) {
    CHECK(residuals.contains(id));
    CHECK(residuals[id].size() > scalar);
    CHECK(std::abs(residuals[id][scalar] - expected) <= 1e-10);
  };
  check(1, 0, 3.0); check(2, 0, 2.0);
  check(3, 0, 2.0); check(3, 1, 3.0);
  check(4, 0, 1.0); check(4, 1, 0.0);
  check(5, 0, 3.0); check(6, 0, 3.0); check(7, 0, 3.0);
  check(8, 0, 2.0); check(9, 0, std::sqrt(0.5));
  check(10, 0, std::sqrt(0.5)); check(11, 0, 3.0);
  check(12, 0, 3.14159265358979323846 / 6.0);
  check(13, 0, 1.0); check(14, 0, 2.0); check(15, 0, 3.0);
  check(16, 0, 3.0); check(17, 0, 3.0);
  CHECK(lockWasFixed); CHECK(!residuals.contains(18));
  check(19, 0, 3.0); check(20, 0, 1.0); check(20, 1, 2.0);
  check(21, 0, 3.0); check(22, 0, 2.0);
}

void graphUsesOnlyExplicitTopology() {
  Sketch close;
  close.addLine({0.0, 0.0}, {1.0, 0.0});
  close.addLine({0.0, 5e-10}, {1.0, 5e-10});
  CHECK(ConstraintGraph::build(close).components().size() == 2);

  Sketch composite;
  composite.addLine({0.0, 0.0}, {1.0, 0.0}, 77);
  composite.addLine({1.0, 0.0}, {1.0, 1.0}, 77);
  const auto graph = ConstraintGraph::build(composite);
  CHECK(graph.components().size() == 1);
  const auto system = ConstraintEquationRegistry::assemble(
      composite, graph.components().front());
  CHECK(system.equationCount() == 0);
  CHECK(system.parameterCount() == 8);
}

void persistedOrderCannotChangeCanonicalSystem() {
  auto first = completeFixture();
  auto second = first;
  SketchTestAccess::reverseConstraints(second);
  const auto firstGraph = ConstraintGraph::build(first);
  const auto secondGraph = ConstraintGraph::build(second);
  CHECK(firstGraph.components() == secondGraph.components());
  CHECK(firstGraph.components().size() == secondGraph.components().size());
  for (std::size_t index = 0; index < firstGraph.components().size(); ++index) {
    const auto a = ConstraintEquationRegistry::assemble(first,
                                                        firstGraph.components()[index]);
    const auto b = ConstraintEquationRegistry::assemble(second,
                                                         secondGraph.components()[index]);
    CHECK(a.semanticHash() == b.semanticHash());
    CHECK(EquationDiagnostics::analyze(a).equationRank ==
          EquationDiagnostics::analyze(b).equationRank);
    CHECK(a.rows().size() == b.rows().size());
    for (std::size_t row = 0; row < a.rows().size(); ++row)
      CHECK(a.rows()[row].provenance == b.rows()[row].provenance);
  }

  Sketch geometry;
  geometry.addLine({0.0, 0.0}, {3.0, 4.0});
  const auto line = geometry.lineId(0);
  const PointReference start{LineEndpointReference{line, Endpoint::Start}};
  const PointReference end{LineEndpointReference{line, Endpoint::End}};
  const std::vector<Constraint> source{
      identified(Constraint{LengthConstraint{line, 5.0}}, 30),
      identified(Constraint{DistanceXConstraint{start, end, 3.0}}, 10),
      identified(Constraint{DistanceYConstraint{start, end, 4.0}}, 20),
  };
  std::vector<int> order{0, 1, 2};
  std::optional<EquationSystem> canonical;
  do {
    auto candidate = geometry;
    std::vector<Constraint> constraints;
    for (const int index : order) constraints.push_back(source[index]);
    SketchTestAccess::installConstraints(candidate, std::move(constraints));
    const auto system = assembleAll(candidate).front();
    if (!canonical) {
      canonical = system;
      continue;
    }
    CHECK(system.semanticHash() == canonical->semanticHash());
    CHECK(EquationDiagnostics::analyze(system).equationRank ==
          EquationDiagnostics::analyze(*canonical).equationRank);
    CHECK(system.rows().size() == canonical->rows().size());
    CHECK(system.jacobian().size() == canonical->jacobian().size());
    for (std::size_t row = 0; row < system.rows().size(); ++row)
      CHECK(system.rows()[row].provenance == canonical->rows()[row].provenance);
    for (std::size_t entry = 0; entry < system.jacobian().size(); ++entry)
      CHECK((std::abs(system.jacobian()[entry]) > 1e-14) ==
            (std::abs(canonical->jacobian()[entry]) > 1e-14));
  } while (std::next_permutation(order.begin(), order.end()));
}

void checkCentralDifference(const Sketch& sketch) {
  const auto graph = ConstraintGraph::build(sketch);
  for (const auto& component : graph.components()) {
    const auto base = ConstraintEquationRegistry::assemble(sketch, component);
    CHECK(base.status() == EquationStatus::Valid);
    std::vector<double> candidate(base.variableCount());
    for (const auto& descriptor : base.layout().parameters())
      if (descriptor.variableIndex)
        candidate[*descriptor.variableIndex] = descriptor.value;
    for (std::size_t column = 0; column < base.parameterCount(); ++column) {
      const auto& descriptor = base.layout().parameters()[column];
      if (descriptor.fixed) continue;
      const double step = std::max(1e-6, std::abs(descriptor.value) * 1e-6);
      auto plus = candidate;
      auto minus = candidate;
      plus[*descriptor.variableIndex] += step;
      minus[*descriptor.variableIndex] -= step;
      const auto plusSystem = ConstraintEquationRegistry::assemble(
          sketch, component, plus);
      const auto minusSystem = ConstraintEquationRegistry::assemble(
          sketch, component, minus);
      CHECK(plusSystem.has_value());
      CHECK(minusSystem.has_value());
      CHECK(plusSystem->rows().size() == base.rows().size());
      CHECK(minusSystem->rows().size() == base.rows().size());
      for (std::size_t row = 0; row < base.rows().size(); ++row) {
        const double numerical =
            (plusSystem->rows()[row].residual -
             minusSystem->rows()[row].residual) /
            (2.0 * step);
        const double automatic =
            base.jacobian()[row * base.parameterCount() + column] *
            base.rows()[row].scale;
        if (std::abs(numerical - automatic) >
            2e-4 * std::max(1.0, std::abs(numerical))) {
          std::cerr << "Jacobian mismatch constraint="
                    << base.rows()[row].provenance.constraintId
                    << " scalar=" << base.rows()[row].provenance.scalarIndex
                    << " geometry=" << descriptor.provenance.geometryId
                    << " role=" << static_cast<int>(descriptor.provenance.role)
                    << " numerical=" << numerical
                    << " automatic=" << automatic << '\n';
          CHECK(false);
        }
      }
    }
  }
}

void forwardAdMatchesCentralDifference() {
  // This fixture represents every production equation type (Lock contributes
  // fixed columns and intentionally has no residual row).
  checkCentralDifference(completeFixture());

  // Also sample non-zero residuals, not only exact solutions.
  Sketch sketch;
  sketch.addLine({1.0, 2.0}, {4.0, 6.0});
  const auto first = sketch.lineId(0);
  sketch.addLine({-2.0, 1.0}, {3.0, 2.5});
  const auto second = sketch.lineId(1);
  std::vector<Constraint> constraints;
  constraints.push_back(identified(Constraint{LengthConstraint{first, 4.0}}, 10));
  constraints.push_back(identified(Constraint{AngleConstraint{first, second, 50.0}}, 20));
  constraints.push_back(identified(Constraint{LineDistanceConstraint{first, second, 2.0}}, 30));
  SketchTestAccess::installConstraints(sketch, std::move(constraints));
  checkCentralDifference(sketch);
}

void tangentUsesFiniteDomains() {
  Sketch lineCircle;
  lineCircle.addLine({0.0, 0.0}, {1.0, 0.0});
  lineCircle.addCircle({2.0, 1.0}, 1.0);
  SketchTestAccess::installConstraints(
      lineCircle,
      {identified(Constraint{TangentConstraint{lineCircle.lineId(0),
                                               lineCircle.circleId(0)}}, 1)});
  const auto lineCircleSystem = assembleAll(lineCircle).front();
  CHECK(lineCircleSystem.rows().size() == 1);
  // Line/circle tangency is defined on the infinite line carrier.  The
  // finite segment may end before the contact without turning a true carrier
  // tangency into a conflict.
  CHECK(std::abs(lineCircleSystem.rows().front().residual) <= 1e-12);

  Sketch lineBezier;
  lineBezier.addLine({0.0, 0.0}, {1.0, 0.0});
  lineBezier.addBezier({2.0, 0.0}, {3.0, 0.0}, {4.0, 1.0}, {5.0, 1.0});
  SketchTestAccess::installConstraints(
      lineBezier,
      {identified(Constraint{TangentConstraint{lineBezier.lineId(0),
                                               lineBezier.bezierId(0)}}, 1)});
  const auto lineBezierSystem = assembleAll(lineBezier).front();
  CHECK(lineBezierSystem.rows().size() == 3);
  CHECK(std::abs(lineBezierSystem.rows()[0].residual) <= 1e-12);
  CHECK(lineBezierSystem.rows()[1].residual >= 1.0);

  Sketch exactLineBezier;
  exactLineBezier.addLine({0.0, 0.0}, {10.0, 0.0});
  exactLineBezier.addBezier({5.0, 0.0}, {6.0, 0.0},
                            {7.0, 1.0}, {8.0, 1.0});
  SketchTestAccess::installConstraints(
      exactLineBezier,
      {identified(Constraint{TangentConstraint{exactLineBezier.lineId(0),
                                               exactLineBezier.bezierId(0)}},
                  1)});
  const auto exactLineBezierSystem = assembleAll(exactLineBezier).front();
  CHECK(exactLineBezierSystem.rows().size() == 3);
  CHECK(std::abs(exactLineBezierSystem.rows()[0].residual) <= 1e-12);
  CHECK(std::any_of(exactLineBezierSystem.jacobian().begin(),
                    exactLineBezierSystem.jacobian().begin() +
                        exactLineBezierSystem.variableCount(),
                    [](double value) { return std::abs(value) > 1e-10; }));
  CHECK(EquationDiagnostics::analyze(exactLineBezierSystem).equationRank >= 2);

  Sketch arcBezier;
  arcBezier.addArc({0.0, 0.0}, 5.0, 0.0, 0.5 * 3.14159265358979323846);
  constexpr double diagonal = 3.5355339059327378;
  arcBezier.addBezier({diagonal, diagonal}, {diagonal - 1.0, diagonal + 1.0},
                      {1.5, 5.0}, {0.5, 5.0});
  SketchTestAccess::installConstraints(
      arcBezier,
      {identified(Constraint{TangentConstraint{arcBezier.arcId(0),
                                               arcBezier.bezierId(0)}}, 1)});
  const auto validArcSystem = assembleAll(arcBezier).front();
  CHECK(validArcSystem.rows().size() == 3);
  for (const auto& row : validArcSystem.rows())
    CHECK(std::abs(row.residual) <= 1e-10);
  checkCentralDifference(arcBezier);

  Sketch outsideArc;
  outsideArc.addArc({0.0, 0.0}, 5.0, 0.0, 0.5 * 3.14159265358979323846);
  outsideArc.addBezier({-5.0, 0.0}, {-5.0, -1.0}, {-4.0, -2.0}, {-3.0, -2.0});
  SketchTestAccess::installConstraints(
      outsideArc,
      {identified(Constraint{TangentConstraint{outsideArc.arcId(0),
                                               outsideArc.bezierId(0)}}, 1)});
  const auto outsideArcSystem = assembleAll(outsideArc).front();
  CHECK(outsideArcSystem.rows().size() == 3);
  CHECK(outsideArcSystem.rows()[2].residual > 0.0);
}

void everyTangentCombinationHasJacobianOracle() {
  Sketch sketch;
  std::vector<Constraint> constraints;
  ConstraintId id = 1;
  auto addTangent = [&](GeometryId first, GeometryId second) {
    constraints.push_back(identified(Constraint{TangentConstraint{first, second}},
                                     id++));
  };

  sketch.addLine({5.0, -5.0}, {5.0, 5.0});
  sketch.addCircle({0.0, 0.0}, 5.0);
  addTangent(sketch.lineId(0), sketch.circleId(0));

  sketch.addLine({25.0, -5.0}, {25.0, 5.0});
  sketch.addArc({20.0, 0.0}, 5.0, -0.5, 1.0);
  addTangent(sketch.lineId(1), sketch.arcId(0));

  sketch.addCircle({40.0, 0.0}, 2.0);
  sketch.addCircle({44.0, 0.0}, 2.0);
  addTangent(sketch.circleId(1), sketch.circleId(2));

  sketch.addCircle({60.0, 0.0}, 2.0);
  sketch.addArc({64.0, 0.0}, 2.0, 2.5, 1.2);
  addTangent(sketch.circleId(3), sketch.arcId(1));

  sketch.addArc({80.0, 0.0}, 2.0, -0.5, 1.0);
  sketch.addArc({84.0, 0.0}, 2.0, 2.5, 1.2);
  addTangent(sketch.arcId(2), sketch.arcId(3));

  sketch.addLine({100.0, 0.0}, {110.0, 0.0});
  sketch.addBezier({105.0, 0.0}, {106.0, 0.0}, {107.0, 1.0}, {108.0, 1.0});
  addTangent(sketch.lineId(2), sketch.bezierId(0));

  sketch.addCircle({120.0, 0.0}, 5.0);
  sketch.addBezier({125.0, 0.0}, {125.0, 1.0}, {124.0, 2.0}, {123.0, 2.0});
  addTangent(sketch.circleId(4), sketch.bezierId(1));

  constexpr double diagonal = 3.5355339059327378;
  sketch.addArc({140.0, 0.0}, 5.0, 0.0, 0.5 * 3.14159265358979323846);
  sketch.addBezier({140.0 + diagonal, diagonal},
                   {139.0 + diagonal, diagonal + 1.0},
                   {141.5, 5.0}, {140.5, 5.0});
  addTangent(sketch.arcId(4), sketch.bezierId(2));

  sketch.addBezier({160.0, 0.0}, {161.0, 0.0}, {162.0, 1.0}, {163.0, 1.0});
  sketch.addBezier({160.0, 0.0}, {159.0, 0.0}, {158.0, -1.0}, {157.0, -1.0});
  addTangent(sketch.bezierId(3), sketch.bezierId(4));

  SketchTestAccess::installConstraints(sketch, std::move(constraints));
  const auto systems = assembleAll(sketch);
  CHECK(systems.size() == 9);
  for (const auto& system : systems) {
    CHECK(system.status() == EquationStatus::Valid);
    for (const auto& row : system.rows())
      CHECK(std::abs(row.residual) <= 1e-10);
  }
  checkCentralDifference(sketch);
}

void candidateVectorEvaluationDoesNotMutateSketch() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {2.0, 0.0});
  const auto line = sketch.lineId(0);
  SketchTestAccess::installConstraints(
      sketch, {identified(Constraint{LengthConstraint{line, 3.0}}, 1)});
  const auto component = ConstraintGraph::build(sketch).components().front();
  const auto base = ConstraintEquationRegistry::assemble(sketch, component);
  std::vector<double> candidate(base.variableCount());
  for (const auto& descriptor : base.layout().parameters())
    if (descriptor.variableIndex)
      candidate[*descriptor.variableIndex] = descriptor.value;
  const auto endX = base.layout().parameterIndex(line,
                                                  ParameterRole::LineEndX);
  CHECK(endX.has_value());
  const auto variable = base.layout().parameters()[*endX].variableIndex;
  CHECK(variable.has_value());
  candidate[*variable] = 4.0;
  const auto evaluated = ConstraintEquationRegistry::assemble(
      sketch, component, candidate);
  CHECK(evaluated.has_value());
  CHECK(std::abs(base.rows().front().residual + 1.0) <= 1e-12);
  CHECK(std::abs(evaluated->rows().front().residual - 1.0) <= 1e-12);
  CHECK(sketch.lines().front().end.xMm == 2.0);
  CHECK(ConstraintEquationRegistry::assemble(sketch, component)
            .semanticHash() == base.semanticHash());
  candidate.push_back(0.0);
  CHECK(!ConstraintEquationRegistry::assemble(sketch, component, candidate));
}

double normalizedLengthResidual(double scale, double angle, double tx,
                                double ty) {
  Sketch sketch;
  const Point start{tx, ty};
  const Point end{tx + 2.0 * scale * std::cos(angle),
                  ty + 2.0 * scale * std::sin(angle)};
  sketch.addLine(start, end);
  auto constraint = identified(
      Constraint{LengthConstraint{sketch.lineId(0), scale}}, 1);
  SketchTestAccess::installConstraints(sketch, {constraint});
  const auto system = assembleAll(sketch).front();
  return system.rows().front().normalizedResidual;
}

Sketch intrinsicInvariantFixture(double scale, double angle, double tx,
                                 double ty) {
  const auto transform = [&](Point point) {
    return Point{tx + scale * (point.xMm * std::cos(angle) -
                               point.yMm * std::sin(angle)),
                 ty + scale * (point.xMm * std::sin(angle) +
                               point.yMm * std::cos(angle))};
  };
  Sketch sketch;
  sketch.addLine(transform({0.0, 0.0}), transform({3.0, 1.0}));
  sketch.addLine(transform({0.0, 2.0}), transform({4.0, 2.5}));
  const auto first = sketch.lineId(0);
  const auto second = sketch.lineId(1);
  sketch.addCircle(transform({10.0, 0.0}), 2.0 * scale);
  sketch.addCircle(transform({15.0, 1.0}), 1.5 * scale);
  const auto firstCircle = sketch.circleId(0);
  const auto secondCircle = sketch.circleId(1);
  const PointReference start{LineEndpointReference{first, Endpoint::Start}};
  const PointReference end{LineEndpointReference{first, Endpoint::End}};
  SketchTestAccess::installConstraints(
      sketch,
      {identified(Constraint{DistanceConstraint{start, end, 2.5 * scale}}, 1),
       identified(Constraint{LengthConstraint{first, 2.8 * scale}}, 2),
       identified(Constraint{ParallelConstraint{first, second}}, 3),
       identified(Constraint{PerpendicularConstraint{first, second}}, 4),
       identified(Constraint{AngleConstraint{first, second, 45.0}}, 5),
       identified(Constraint{EqualConstraint{first, second}}, 6),
       identified(Constraint{LineDistanceConstraint{first, second,
                                                     1.1 * scale}}, 7),
       identified(Constraint{EqualConstraint{firstCircle, secondCircle}}, 8),
       identified(Constraint{TangentConstraint{firstCircle, secondCircle}}, 9)});
  return sketch;
}

void rigidAndScaleInvarianceIsSeeded() {
  constexpr std::uint32_t seed = 0x5A17E2u;
  std::cerr << "equation-kernel property seed=" << seed << '\n';
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> angle(-3.0, 3.0);
  std::uniform_real_distribution<double> translation(-1e4, 1e4);
  std::uniform_real_distribution<double> logarithm(-3.0, 3.0);
  const double reference = normalizedLengthResidual(1.0, 0.0, 0.0, 0.0);
  for (int sample = 0; sample < 100; ++sample) {
    const double scale = std::pow(10.0, logarithm(random));
    const double observed = normalizedLengthResidual(
        scale, angle(random), translation(random), translation(random));
    if (std::abs(observed - reference) > 2e-9) {
      std::cerr << "property sample=" << sample << " scale=" << scale
                << " reference=" << reference << " observed=" << observed
                << '\n';
      CHECK(false);
    }
  }
  const auto invariantReference = assembleAll(
      intrinsicInvariantFixture(1.0, 0.0, 0.0, 0.0));
  for (int sample = 0; sample < 20; ++sample) {
    const double scale = std::pow(10.0, logarithm(random));
    const auto observed = assembleAll(intrinsicInvariantFixture(
        scale, angle(random), translation(random), translation(random)));
    CHECK(observed.size() == invariantReference.size());
    for (std::size_t component = 0; component < observed.size(); ++component) {
      CHECK(observed[component].rows().size() ==
            invariantReference[component].rows().size());
      for (std::size_t row = 0; row < observed[component].rows().size(); ++row) {
        CHECK(observed[component].rows()[row].provenance ==
              invariantReference[component].rows()[row].provenance);
        CHECK(std::abs(observed[component].rows()[row].normalizedResidual -
                       invariantReference[component].rows()[row]
                           .normalizedResidual) <= 2e-8);
      }
    }
  }
  std::cerr << "equation-kernel property complete\n";
}

void malformedAndLockedInputsAreTyped() {
  Sketch dangling;
  SketchTestAccess::installConstraints(
      dangling,
      {identified(Constraint{HorizontalConstraint{GeometryId{999}}}, 1),
       identified(Constraint{LockConstraint{GeometryId{1000}}}, 2)});
  const auto danglingSystems = assembleAll(dangling);
  CHECK(danglingSystems.size() == 1);
  CHECK(danglingSystems.front().component().geometryIds.empty());
  CHECK(danglingSystems.front().status() == EquationStatus::MalformedReference);
  CHECK(danglingSystems.front().rows().size() == 2);
  for (const auto& row : danglingSystems.front().rows())
    CHECK(std::isfinite(row.residual));

  Sketch degenerate;
  degenerate.addLine({0.0, 0.0}, {1.0, 0.0});
  SketchTestAccess::setParameter(
      degenerate, {degenerate.lineId(0), ParameterRole::LineEndX}, 0.0);
  SketchTestAccess::installConstraints(
      degenerate,
      {identified(Constraint{LengthConstraint{degenerate.lineId(0), 1.0}}, 1)});
  const auto broken = assembleAll(degenerate).front();
  CHECK(broken.status() == EquationStatus::DegenerateGeometry);
  CHECK(std::isfinite(broken.rows().front().residual));
  for (const double value : broken.jacobian()) CHECK(std::isfinite(value));

  Sketch collapsedDirectional;
  collapsedDirectional.addLine({0.0, 0.0}, {1.0, 0.0});
  SketchTestAccess::setParameter(
      collapsedDirectional,
      {collapsedDirectional.lineId(0), ParameterRole::LineEndX}, 0.0);
  SketchTestAccess::installConstraints(
      collapsedDirectional,
      {identified(Constraint{HorizontalConstraint{
           collapsedDirectional.lineId(0)}}, 1)});
  const auto collapsedSystem = assembleAll(collapsedDirectional).front();
  CHECK(collapsedSystem.status() == EquationStatus::DegenerateGeometry);
  CHECK(collapsedSystem.rows().front().status ==
        EquationStatus::DegenerateGeometry);

  Sketch collapsedEqual;
  collapsedEqual.addLine({0.0, 0.0}, {1.0, 0.0});
  collapsedEqual.addLine({2.0, 0.0}, {3.0, 0.0});
  SketchTestAccess::setParameter(
      collapsedEqual,
      {collapsedEqual.lineId(0), ParameterRole::LineEndX}, 0.0);
  SketchTestAccess::setParameter(
      collapsedEqual,
      {collapsedEqual.lineId(1), ParameterRole::LineEndX}, 2.0);
  SketchTestAccess::installConstraints(
      collapsedEqual,
      {identified(Constraint{EqualConstraint{collapsedEqual.lineId(0),
                                             collapsedEqual.lineId(1)}}, 1)});
  CHECK(assembleAll(collapsedEqual).front().status() ==
        EquationStatus::DegenerateGeometry);

  Sketch nonFinite;
  nonFinite.addLine({0.0, 0.0}, {1.0, 0.0});
  SketchTestAccess::setParameter(
      nonFinite, {nonFinite.lineId(0), ParameterRole::LineEndX},
      std::numeric_limits<double>::infinity());
  SketchTestAccess::installConstraints(
      nonFinite,
      {identified(Constraint{LengthConstraint{nonFinite.lineId(0), 1.0}}, 1)});
  const auto nonFiniteSystems = assembleAll(nonFinite);
  CHECK(!nonFiniteSystems.empty());
  CHECK(nonFiniteSystems.front().status() == EquationStatus::NonFiniteInput);

  Sketch nonFiniteLock;
  nonFiniteLock.addLine({0.0, 0.0}, {1.0, 0.0});
  SketchTestAccess::setParameter(
      nonFiniteLock,
      {nonFiniteLock.lineId(0), ParameterRole::LineEndX},
      std::numeric_limits<double>::infinity());
  SketchTestAccess::installConstraints(
      nonFiniteLock,
      {identified(Constraint{LockConstraint{nonFiniteLock.lineId(0)}}, 1)});
  const auto nonFiniteLockSystem = assembleAll(nonFiniteLock).front();
  CHECK(nonFiniteLockSystem.status() == EquationStatus::NonFiniteInput);
  CHECK(nonFiniteLockSystem.rows().size() == 1);
  CHECK(std::isfinite(nonFiniteLockSystem.rows().front().residual));

  Sketch locked;
  locked.addCircle({1.0, 2.0}, 3.0);
  SketchTestAccess::installConstraints(
      locked, {identified(Constraint{LockConstraint{locked.circleId(0)}}, 1)});
  const auto fixed = assembleAll(locked).front();
  CHECK(fixed.variableCount() == 0);
  CHECK(fixed.parameterCount() == 3);
  CHECK(fixed.equationCount() == 0);
  CHECK(std::all_of(fixed.layout().parameters().begin(),
                    fixed.layout().parameters().end(),
                    [](const auto& item) { return item.fixed; }));

  Sketch independentLines;
  independentLines.addLine({0.0, 0.0}, {1.0, 0.0});
  independentLines.addLine({2.0, 0.0}, {3.0, 0.0});
  const auto lockedLine = independentLines.lineId(0);
  const auto freeLine = independentLines.lineId(1);
  SketchTestAccess::installConstraints(
      independentLines,
      {identified(Constraint{LockConstraint{lockedLine}}, 1),
       identified(Constraint{HorizontalConstraint{freeLine}}, 2)});
  const auto independentSystems = assembleAll(independentLines);
  CHECK(independentSystems.size() == 2);
  CHECK(independentSystems[0].variableCount() == 0);
  CHECK(independentSystems[1].variableCount() == 4);
  CHECK(std::none_of(independentSystems[1].layout().parameters().begin(),
                     independentSystems[1].layout().parameters().end(),
                     [](const auto& item) { return item.fixed; }));
}

void singularCandidatesAreTyped() {
  const auto requireDegenerate = [](const Sketch& sketch) {
    const auto systems = assembleAll(sketch);
    CHECK(systems.size() == 1);
    CHECK(systems.front().status() == EquationStatus::DegenerateGeometry);
    CHECK(!systems.front().rows().empty());
    CHECK(std::all_of(systems.front().rows().begin(),
                      systems.front().rows().end(), [](const auto& row) {
                        return std::isfinite(row.residual);
                      }));
    CHECK(std::all_of(systems.front().jacobian().begin(),
                      systems.front().jacobian().end(), [](double value) {
                        return std::isfinite(value);
                      }));
  };

  for (const auto type : {ConstraintType::Distance,
                          ConstraintType::DistanceX,
                          ConstraintType::DistanceY}) {
    Sketch sketch;
    sketch.addLine({0.0, 0.0}, {1.0, 0.0});
    const PointReference point{
        LineEndpointReference{sketch.lineId(0), Endpoint::Start}};
    Constraint constraint;
    if (type == ConstraintType::Distance)
      constraint = Constraint{DistanceConstraint{point, point, 1.0}};
    else if (type == ConstraintType::DistanceX)
      constraint = Constraint{DistanceXConstraint{point, point, 1.0}};
    else
      constraint = Constraint{DistanceYConstraint{point, point, 1.0}};
    SketchTestAccess::installConstraints(
        sketch, {identified(std::move(constraint), 1)});
    requireDegenerate(sketch);
  }

  {
    Sketch sketch;
    sketch.addLine({0.0, 0.0}, {1.0, 0.0});
    sketch.addLine({0.0, 1.0}, {1.0, 1.0});
    SketchTestAccess::installConstraints(
        sketch, {identified(Constraint{AngleConstraint{
                                sketch.lineId(0), sketch.lineId(1), 45.0}},
                            1)});
    requireDegenerate(sketch);
  }

  for (const bool arc : {false, true}) {
    Sketch sketch;
    sketch.addLine({0.0, 0.0}, {1.0, 0.0});
    const PointReference center{
        LineEndpointReference{sketch.lineId(0), Endpoint::Start}};
    Constraint constraint;
    if (arc) {
      sketch.addArc({0.0, 0.0}, 2.0, -0.5, 1.0);
      constraint = Constraint{PointOnArcConstraint{sketch.arcId(0), center}};
    } else {
      sketch.addCircle({0.0, 0.0}, 2.0);
      constraint =
          Constraint{PointOnCircleConstraint{sketch.circleId(0), center}};
    }
    SketchTestAccess::installConstraints(
        sketch, {identified(std::move(constraint), 1)});
    requireDegenerate(sketch);
  }

  {
    Sketch sketch;
    sketch.addLine({-2.0, 0.0}, {2.0, 0.0});
    sketch.addCircle({0.0, 0.0}, 1.0);
    SketchTestAccess::installConstraints(
        sketch, {identified(Constraint{TangentConstraint{
                                sketch.lineId(0), sketch.circleId(0)}},
                            1)});
    requireDegenerate(sketch);
  }

  {
    Sketch sketch;
    sketch.addLine({-2.0, 0.0}, {2.0, 0.0});
    sketch.addLine({0.0, 0.0}, {0.0, 2.0});
    SketchTestAccess::installConstraints(
        sketch, {identified(Constraint{LineDistanceConstraint{
                                sketch.lineId(0), sketch.lineId(1), 1.0}},
                            1)});
    requireDegenerate(sketch);
  }
}

void operationCountsAndHashesAreDeterministic() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {2.0, 0.0});
  sketch.addLine({5.0, 0.0}, {5.0, 2.0});
  const auto first = sketch.lineId(0);
  const auto second = sketch.lineId(1);
  SketchTestAccess::installConstraints(
      sketch,
      {identified(Constraint{HorizontalConstraint{first}}, 1),
       identified(Constraint{LengthConstraint{first, 2.0}}, 2),
       identified(Constraint{VerticalConstraint{second}}, 3),
       identified(Constraint{LengthConstraint{second, 2.0}}, 4)});
  const auto firstPass = assembleAll(sketch);
  const auto secondPass = assembleAll(sketch);
  CHECK(firstPass.size() == 2);
  CHECK(secondPass.size() == 2);
  std::size_t parameters = 0;
  std::size_t equations = 0;
  std::size_t nonZeros = 0;
  for (std::size_t index = 0; index < firstPass.size(); ++index) {
    parameters += firstPass[index].parameterCount();
    equations += firstPass[index].equationCount();
    nonZeros += firstPass[index].nonZeroJacobianCount();
    CHECK(firstPass[index].semanticHash() == secondPass[index].semanticHash());
  }
  CHECK(parameters == 8);
  CHECK(equations == 4);
  CHECK(nonZeros == 8);
  CHECK(firstPass[0].semanticHash() == 15507668604071813813ULL);
  CHECK(firstPass[1].semanticHash() == 12033787661441475554ULL);
}

}  // namespace

int main() {
  registryCoversEveryProductionType();
  exactZeroFixturesHaveFiniteRows();
  everyTypeHasKnownResidual();
  graphUsesOnlyExplicitTopology();
  persistedOrderCannotChangeCanonicalSystem();
  forwardAdMatchesCentralDifference();
  tangentUsesFiniteDomains();
  everyTangentCombinationHasJacobianOracle();
  candidateVectorEvaluationDoesNotMutateSketch();
  rigidAndScaleInvarianceIsSeeded();
  malformedAndLockedInputsAreTyped();
  singularCandidatesAreTyped();
  operationCountsAndHashesAreDeterministic();
  std::cout << "sketch equation kernel tests passed\n";
  return 0;
}

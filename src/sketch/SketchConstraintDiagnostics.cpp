#include "sketch/SketchConstraintDiagnostics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

namespace solidar::sketch {
namespace {

constexpr double kLengthTolerance = 1e-4;
constexpr double kAngularTolerance = 1e-5;
constexpr double kDegeneratePenalty = 1e3;

struct Equation {
  double value{};
  double tolerance{kLengthTolerance};
  ConstraintId constraintId{kInvalidConstraintId};
  ConstraintType type{ConstraintType::Horizontal};
  bool userConstraint{false};
};

struct EndpointLink {
  GeometryId firstId{kInvalidGeometryId};
  bool firstStart{true};
  GeometryId secondId{kInvalidGeometryId};
  bool secondStart{true};
};

struct LineRelation {
  enum class Kind { Parallel, Perpendicular };
  GeometryId firstId{kInvalidGeometryId};
  GeometryId secondId{kInvalidGeometryId};
  Kind kind{Kind::Parallel};
};

struct Layout {
  std::vector<double> variables;
  std::unordered_map<GeometryId, std::size_t> lineBase;
  std::unordered_map<GeometryId, std::size_t> circleBase;
  std::vector<EndpointLink> implicitEndpointLinks;
  std::vector<LineRelation> implicitLineRelations;
};

double pointDistance(Point a, Point b) {
  return std::hypot(b.xMm - a.xMm, b.yMm - a.yMm);
}

double cross2(double ax, double ay, double bx, double by) {
  return ax * by - ay * bx;
}

double dot2(double ax, double ay, double bx, double by) {
  return ax * bx + ay * by;
}

Layout makeLayout(const Sketch& sketch) {
  Layout result;
  result.variables.reserve(sketch.lines().size() * 4 +
                           sketch.circles().size() * 3);

  for (std::size_t i = 0; i < sketch.lines().size(); ++i) {
    const auto id = sketch.lineId(i);
    if (id == kInvalidGeometryId) continue;
    result.lineBase[id] = result.variables.size();
    const auto& line = sketch.lines()[i];
    result.variables.insert(result.variables.end(),
                            {line.start.xMm, line.start.yMm,
                             line.end.xMm, line.end.yMm});
  }
  for (std::size_t i = 0; i < sketch.circles().size(); ++i) {
    const auto id = sketch.circleId(i);
    if (id == kInvalidGeometryId) continue;
    result.circleBase[id] = result.variables.size();
    const auto& circle = sketch.circles()[i];
    result.variables.insert(result.variables.end(),
                            {circle.center.xMm, circle.center.yMm,
                             circle.radiusMm});
  }

  // Preserve implicit topology of composite elements (notably rectangles).
  for (std::size_t i = 0; i < sketch.lines().size(); ++i) {
    for (std::size_t j = i + 1; j < sketch.lines().size(); ++j) {
      if (sketch.lines()[i].elementId != sketch.lines()[j].elementId)
        continue;
      for (const bool firstStart : {true, false}) {
        const Point a = firstStart ? sketch.lines()[i].start
                                   : sketch.lines()[i].end;
        for (const bool secondStart : {true, false}) {
          const Point b = secondStart ? sketch.lines()[j].start
                                      : sketch.lines()[j].end;
          if (pointDistance(a, b) <= 1e-7)
            result.implicitEndpointLinks.push_back(
                {sketch.lineId(i), firstStart,
                 sketch.lineId(j), secondStart});
        }
      }
    }
  }

  std::unordered_map<std::size_t, std::vector<std::size_t>> elements;
  for (std::size_t i = 0; i < sketch.lines().size(); ++i)
    elements[sketch.lines()[i].elementId].push_back(i);

  for (const auto& [elementId, indices] : elements) {
    (void)elementId;
    if (indices.size() != 4) continue;

    std::vector<std::pair<std::size_t, std::size_t>> parallelPairs;
    std::optional<std::pair<std::size_t, std::size_t>> perpendicularPair;
    for (std::size_t a = 0; a < indices.size(); ++a) {
      const auto& first = sketch.lines()[indices[a]];
      const double ax = first.end.xMm - first.start.xMm;
      const double ay = first.end.yMm - first.start.yMm;
      const double al = std::hypot(ax, ay);
      if (al <= 1e-9) continue;

      for (std::size_t b = a + 1; b < indices.size(); ++b) {
        const auto& second = sketch.lines()[indices[b]];
        const double bx = second.end.xMm - second.start.xMm;
        const double by = second.end.yMm - second.start.yMm;
        const double bl = std::hypot(bx, by);
        if (bl <= 1e-9) continue;

        const double nc =
            std::abs(cross2(ax, ay, bx, by) / (al * bl));
        const double nd =
            std::abs(dot2(ax, ay, bx, by) / (al * bl));
        if (nc <= 1e-7)
          parallelPairs.emplace_back(indices[a], indices[b]);
        if (!perpendicularPair && nd <= 1e-7)
          perpendicularPair = {indices[a], indices[b]};
      }
    }

    for (std::size_t i = 0;
         i < std::min<std::size_t>(2, parallelPairs.size()); ++i) {
      result.implicitLineRelations.push_back(
          {sketch.lineId(parallelPairs[i].first),
           sketch.lineId(parallelPairs[i].second),
           LineRelation::Kind::Parallel});
    }
    if (perpendicularPair) {
      result.implicitLineRelations.push_back(
          {sketch.lineId(perpendicularPair->first),
           sketch.lineId(perpendicularPair->second),
           LineRelation::Kind::Perpendicular});
    }
  }

  return result;
}

std::optional<Line> lineOf(const Layout& layout,
                           const std::vector<double>& variables,
                           GeometryId id) {
  const auto found = layout.lineBase.find(id);
  if (found == layout.lineBase.end() ||
      found->second + 3 >= variables.size())
    return std::nullopt;
  const auto b = found->second;
  return Line{{variables[b], variables[b + 1]},
              {variables[b + 2], variables[b + 3]},
              0, false};
}

std::optional<Circle> circleOf(const Layout& layout,
                               const std::vector<double>& variables,
                               GeometryId id) {
  const auto found = layout.circleBase.find(id);
  if (found == layout.circleBase.end() ||
      found->second + 2 >= variables.size())
    return std::nullopt;
  const auto b = found->second;
  return Circle{{variables[b], variables[b + 1]},
                variables[b + 2], false};
}

std::optional<Point> pointOf(const Sketch& sketch,
                             const Layout& layout,
                             const std::vector<double>& variables,
                             PointReference reference) {
  if (reference.elementCenterId != 0) {
    Point center{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < sketch.lines().size(); ++i) {
      if (sketch.lines()[i].elementId != reference.elementCenterId)
        continue;
      const auto line = lineOf(layout, variables, sketch.lineId(i));
      if (!line) continue;
      center.xMm += line->start.xMm + line->end.xMm;
      center.yMm += line->start.yMm + line->end.yMm;
      count += 2;
    }
    if (count == 0) return std::nullopt;
    center.xMm /= static_cast<double>(count);
    center.yMm /= static_cast<double>(count);
    return center;
  }

  if (reference.circleId != kInvalidGeometryId) {
    const auto circle = circleOf(layout, variables, reference.circleId);
    return circle ? std::optional<Point>{circle->center} : std::nullopt;
  }

  // Arc variables are not part of the diagnostic layout yet. Endpoint
  // references are still valid CAD points and are read from the current
  // fixed Arc geometry.
  if (reference.arcId != kInvalidGeometryId) {
    const auto index = sketch.arcIndex(reference.arcId);
    if (!index) return std::nullopt;
    return reference.start
               ? arcStartPoint(sketch.arcs()[*index])
               : arcEndPoint(sketch.arcs()[*index]);
  }

  const auto line = lineOf(layout, variables, reference.lineId);
  if (!line) return std::nullopt;
  return reference.start ? line->start : line->end;
}

double segmentDistance(Point p, const Line& line, double* outside = nullptr) {
  const double dx = line.end.xMm - line.start.xMm;
  const double dy = line.end.yMm - line.start.yMm;
  const double l2 = dx * dx + dy * dy;
  if (l2 <= 1e-18) {
    if (outside) *outside = 0.0;
    return kDegeneratePenalty;
  }
  const double rawT =
      ((p.xMm - line.start.xMm) * dx +
       (p.yMm - line.start.yMm) * dy) / l2;
  const double t = std::clamp(rawT, 0.0, 1.0);

  if (outside) {
    const double length = std::sqrt(l2);
    *outside = rawT < 0.0
                   ? -rawT * length
                   : rawT > 1.0
                         ? (rawT - 1.0) * length
                         : 0.0;
  }

  const Point q{line.start.xMm + t * dx,
                line.start.yMm + t * dy};
  return pointDistance(p, q);
}

double finiteArcDistance(Point point, const Arc& arc) {
  constexpr double kTwoPi = 6.28318530717958647692;
  if (!std::isfinite(arc.center.xMm) ||
      !std::isfinite(arc.center.yMm) ||
      !std::isfinite(arc.radiusMm) ||
      !std::isfinite(arc.startAngleRad) ||
      !std::isfinite(arc.sweepAngleRad) ||
      arc.radiusMm <= 1e-12 ||
      arc.sweepAngleRad <= 1e-12 ||
      arc.sweepAngleRad >= kTwoPi - 1e-12)
    return kDegeneratePenalty;

  const auto normalizeAngle = [](double angle) {
    constexpr double twoPi = 6.28318530717958647692;
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0) angle += twoPi;
    return angle;
  };

  const double dx = point.xMm - arc.center.xMm;
  const double dy = point.yMm - arc.center.yMm;
  const double radialLength = std::hypot(dx, dy);

  if (radialLength > 1e-12) {
    const double candidateAngle =
        normalizeAngle(std::atan2(dy, dx));
    const double delta =
        normalizeAngle(
            candidateAngle -
            normalizeAngle(arc.startAngleRad));

    if (delta <= arc.sweepAngleRad + 1e-12)
      return std::abs(radialLength - arc.radiusMm);
  }

  return std::min(pointDistance(point, arcStartPoint(arc)),
                  pointDistance(point, arcEndPoint(arc)));
}

std::vector<Equation> evaluate(const Sketch& sketch,
                               const Layout& layout,
                               const std::vector<double>& variables) {
  std::vector<Equation> equations;
  equations.reserve(sketch.constraints().size() * 2 +
                    layout.implicitEndpointLinks.size() * 2 +
                    layout.implicitLineRelations.size());

  const auto add =
      [&equations](double value, double tolerance,
                   const Constraint& constraint) {
        equations.push_back({value, tolerance, constraint.id,
                             constraint.type, true});
      };

  const auto invalidEquation =
      [&equations](const Constraint& constraint) {
        equations.push_back({kDegeneratePenalty, kLengthTolerance,
                             constraint.id, constraint.type, true});
      };

  for (const auto& constraint : sketch.constraints()) {
    switch (constraint.type) {
      case ConstraintType::Horizontal: {
        const auto line =
            lineOf(layout, variables, constraint.firstGeometry);
        if (!line) { invalidEquation(constraint); break; }
        add(line->end.yMm - line->start.yMm,
            kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Vertical: {
        const auto line =
            lineOf(layout, variables, constraint.firstGeometry);
        if (!line) { invalidEquation(constraint); break; }
        add(line->end.xMm - line->start.xMm,
            kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Coincident: {
        const auto first =
            pointOf(sketch, layout, variables, constraint.firstPoint);
        const auto second =
            pointOf(sketch, layout, variables, constraint.secondPoint);
        if (!first || !second) { invalidEquation(constraint); break; }
        add(second->xMm - first->xMm, kLengthTolerance, constraint);
        add(second->yMm - first->yMm, kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::PointOnLine: {
        const auto line =
            lineOf(layout, variables, constraint.firstGeometry);
        const auto point =
            pointOf(sketch, layout, variables, constraint.secondPoint);
        if (!line || !point) { invalidEquation(constraint); break; }
        double outside = 0.0;
        add(segmentDistance(*point, *line, &outside),
            kLengthTolerance, constraint);
        add(outside, kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Distance:
      case ConstraintType::DistanceX:
      case ConstraintType::DistanceY: {
        const auto first =
            pointOf(sketch, layout, variables, constraint.firstPoint);
        const auto second =
            pointOf(sketch, layout, variables, constraint.secondPoint);
        if (!first || !second || constraint.value <= 0.0) {
          invalidEquation(constraint);
          break;
        }
        double current = 0.0;
        if (constraint.type == ConstraintType::Distance)
          current = pointDistance(*first, *second);
        else if (constraint.type == ConstraintType::DistanceX)
          current = std::abs(second->xMm - first->xMm);
        else
          current = std::abs(second->yMm - first->yMm);
        add(current - constraint.value, kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Length: {
        const auto line =
            lineOf(layout, variables, constraint.firstGeometry);
        if (!line || constraint.value <= 0.0) {
          invalidEquation(constraint);
          break;
        }
        add(pointDistance(line->start, line->end) - constraint.value,
            kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Radius:
      case ConstraintType::Diameter: {
        const auto circle =
            circleOf(layout, variables, constraint.firstGeometry);
        if (!circle || constraint.value <= 0.0) {
          invalidEquation(constraint);
          break;
        }
        const double current =
            constraint.type == ConstraintType::Radius
                ? circle->radiusMm
                : circle->radiusMm * 2.0;
        add(current - constraint.value, kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Parallel:
      case ConstraintType::Perpendicular:
      case ConstraintType::Angle: {
        const auto first =
            lineOf(layout, variables, constraint.firstGeometry);
        const auto second =
            lineOf(layout, variables, constraint.secondGeometry);
        if (!first || !second) { invalidEquation(constraint); break; }

        const double ax = first->end.xMm - first->start.xMm;
        const double ay = first->end.yMm - first->start.yMm;
        const double bx = second->end.xMm - second->start.xMm;
        const double by = second->end.yMm - second->start.yMm;
        const double al = std::hypot(ax, ay);
        const double bl = std::hypot(bx, by);
        if (al <= 1e-12 || bl <= 1e-12) {
          invalidEquation(constraint);
          break;
        }

        if (constraint.type == ConstraintType::Parallel) {
          add(cross2(ax, ay, bx, by) / (al * bl),
              kAngularTolerance, constraint);
        } else if (constraint.type == ConstraintType::Perpendicular) {
          add(dot2(ax, ay, bx, by) / (al * bl),
              kAngularTolerance, constraint);
        } else {
          if (constraint.value <= 0.0 || constraint.value >= 180.0) {
            invalidEquation(constraint);
            break;
          }
          const double cosine =
              std::clamp(dot2(ax, ay, bx, by) / (al * bl),
                         -1.0, 1.0);
          constexpr double pi = 3.14159265358979323846;
          add(std::acos(cosine) -
                  constraint.value * pi / 180.0,
              kAngularTolerance, constraint);
        }
        break;
      }

      case ConstraintType::Equal: {
        const auto firstLine =
            lineOf(layout, variables, constraint.firstGeometry);
        const auto secondLine =
            lineOf(layout, variables, constraint.secondGeometry);

        if (firstLine && secondLine) {
          add(pointDistance(firstLine->start, firstLine->end) -
                  pointDistance(secondLine->start, secondLine->end),
              kLengthTolerance, constraint);
          break;
        }

        const auto firstCircle =
            circleOf(layout, variables, constraint.firstGeometry);
        const auto secondCircle =
            circleOf(layout, variables, constraint.secondGeometry);
        if (firstCircle && secondCircle) {
          add(firstCircle->radiusMm - secondCircle->radiusMm,
              kLengthTolerance, constraint);
          break;
        }

        invalidEquation(constraint);
        break;
      }

      case ConstraintType::PointOnCircle: {
        const auto circle =
            circleOf(layout, variables, constraint.firstGeometry);
        const auto point =
            pointOf(sketch, layout, variables, constraint.secondPoint);
        if (!circle || !point) { invalidEquation(constraint); break; }
        add(pointDistance(circle->center, *point) - circle->radiusMm,
            kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::PointOnArc: {
        const auto arcIndex =
            sketch.arcIndex(constraint.firstGeometry);
        const auto point =
            pointOf(sketch, layout, variables, constraint.secondPoint);
        if (!arcIndex || !point) {
          invalidEquation(constraint);
          break;
        }
        add(finiteArcDistance(*point, sketch.arcs()[*arcIndex]),
            kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Midpoint: {
        const auto line =
            lineOf(layout, variables, constraint.firstGeometry);
        const auto point =
            pointOf(sketch, layout, variables, constraint.secondPoint);
        if (!line || !point) { invalidEquation(constraint); break; }
        const sketch::Point midpoint{
            (line->start.xMm + line->end.xMm) * 0.5,
            (line->start.yMm + line->end.yMm) * 0.5};
        add(pointDistance(midpoint, *point),
            kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Tangent: {
        const auto line =
            lineOf(layout, variables, constraint.firstGeometry);
        if (!line) { invalidEquation(constraint); break; }
        if (const auto circle =
                circleOf(layout, variables, constraint.secondGeometry)) {
          add(segmentDistance(circle->center, *line) - circle->radiusMm,
              kLengthTolerance, constraint);
          break;
        }

        const auto arcIndex = sketch.arcIndex(constraint.secondGeometry);
        if (!arcIndex) { invalidEquation(constraint); break; }
        const auto& arc = sketch.arcs()[*arcIndex];
        const double dx = line->end.xMm - line->start.xMm;
        const double dy = line->end.yMm - line->start.yMm;
        const double lengthSquared = dx * dx + dy * dy;
        if (lengthSquared <= 1e-12) {
          invalidEquation(constraint);
          break;
        }
        const double t = std::clamp(
            ((arc.center.xMm - line->start.xMm) * dx +
             (arc.center.yMm - line->start.yMm) * dy) /
                lengthSquared,
            0.0, 1.0);
        const Point contact{line->start.xMm + dx * t,
                            line->start.yMm + dy * t};
        add(segmentDistance(arc.center, *line) - arc.radiusMm,
            kLengthTolerance, constraint);
        add(finiteArcDistance(contact, arc),
            kLengthTolerance, constraint);
        break;
      }

      case ConstraintType::Lock: {
        if (const auto baselineIndex =
                sketch.lineIndex(constraint.firstGeometry)) {
          const std::size_t elementId =
              sketch.lines()[*baselineIndex].elementId;

          for (std::size_t index = 0;
               index < sketch.lines().size(); ++index) {
            if (sketch.lines()[index].elementId != elementId)
              continue;

            const auto id = sketch.lineId(index);
            const auto current =
                lineOf(layout, variables, id);
            if (!current) continue;

            const auto& baseline = sketch.lines()[index];
            add(current->start.xMm - baseline.start.xMm,
                kLengthTolerance, constraint);
            add(current->start.yMm - baseline.start.yMm,
                kLengthTolerance, constraint);
            add(current->end.xMm - baseline.end.xMm,
                kLengthTolerance, constraint);
            add(current->end.yMm - baseline.end.yMm,
                kLengthTolerance, constraint);
          }
          break;
        }

        if (const auto baselineIndex =
                sketch.circleIndex(constraint.firstGeometry)) {
          const auto current =
              circleOf(layout, variables,
                       constraint.firstGeometry);
          if (!current) {
            invalidEquation(constraint);
            break;
          }

          const auto& baseline =
              sketch.circles()[*baselineIndex];
          add(current->center.xMm - baseline.center.xMm,
              kLengthTolerance, constraint);
          add(current->center.yMm - baseline.center.yMm,
              kLengthTolerance, constraint);
          add(current->radiusMm - baseline.radiusMm,
              kLengthTolerance, constraint);
          break;
        }

        invalidEquation(constraint);
        break;
      }

      case ConstraintType::LineDistance: {
        const auto first =
            lineOf(layout, variables, constraint.firstGeometry);
        const auto second =
            lineOf(layout, variables, constraint.secondGeometry);
        if (!first || !second || constraint.value <= 0.0) {
          invalidEquation(constraint);
          break;
        }
        const double dx = first->end.xMm - first->start.xMm;
        const double dy = first->end.yMm - first->start.yMm;
        const double length = std::hypot(dx, dy);
        if (length <= 1e-12) {
          invalidEquation(constraint);
          break;
        }
        const double signedDistance =
            ((second->start.xMm - first->start.xMm) * (-dy) +
             (second->start.yMm - first->start.yMm) * dx) / length;
        add(std::abs(signedDistance) - constraint.value,
            kLengthTolerance, constraint);
        break;
      }

      default:
        invalidEquation(constraint);
        break;
    }
  }

  for (const auto& link : layout.implicitEndpointLinks) {
    const auto first =
        pointOf(sketch, layout, variables,
                PointReference{link.firstId, link.firstStart});
    const auto second =
        pointOf(sketch, layout, variables,
                PointReference{link.secondId, link.secondStart});
    if (!first || !second) continue;

    equations.push_back(
        {second->xMm - first->xMm, kLengthTolerance,
         kInvalidConstraintId, ConstraintType::Coincident, false});
    equations.push_back(
        {second->yMm - first->yMm, kLengthTolerance,
         kInvalidConstraintId, ConstraintType::Coincident, false});
  }

  for (const auto& relation : layout.implicitLineRelations) {
    const auto first =
        lineOf(layout, variables, relation.firstId);
    const auto second =
        lineOf(layout, variables, relation.secondId);
    if (!first || !second) continue;

    const double ax = first->end.xMm - first->start.xMm;
    const double ay = first->end.yMm - first->start.yMm;
    const double bx = second->end.xMm - second->start.xMm;
    const double by = second->end.yMm - second->start.yMm;
    const double al = std::hypot(ax, ay);
    const double bl = std::hypot(bx, by);
    if (al <= 1e-12 || bl <= 1e-12) continue;

    const bool parallel =
        relation.kind == LineRelation::Kind::Parallel;
    equations.push_back(
        {parallel
             ? cross2(ax, ay, bx, by) / (al * bl)
             : dot2(ax, ay, bx, by) / (al * bl),
         kAngularTolerance, kInvalidConstraintId,
         parallel ? ConstraintType::Parallel
                  : ConstraintType::Perpendicular,
         false});
  }

  return equations;
}

std::size_t matrixRank(std::vector<std::vector<double>> matrix) {
  if (matrix.empty() || matrix.front().empty()) return 0;

  const std::size_t rows = matrix.size();
  const std::size_t cols = matrix.front().size();
  double largest = 0.0;

  for (const auto& row : matrix)
    for (const double value : row)
      largest = std::max(largest, std::abs(value));

  if (largest <= 1e-14) return 0;
  const double tolerance =
      std::max(1e-9, largest * 1e-7);

  std::size_t rank = 0;
  for (std::size_t col = 0;
       col < cols && rank < rows; ++col) {
    std::size_t pivot = rank;
    for (std::size_t row = rank + 1; row < rows; ++row)
      if (std::abs(matrix[row][col]) >
          std::abs(matrix[pivot][col]))
        pivot = row;

    if (std::abs(matrix[pivot][col]) <= tolerance)
      continue;

    std::swap(matrix[pivot], matrix[rank]);
    const double divisor = matrix[rank][col];

    for (std::size_t c = col; c < cols; ++c)
      matrix[rank][c] /= divisor;

    for (std::size_t row = 0; row < rows; ++row) {
      if (row == rank) continue;
      const double factor = matrix[row][col];
      if (std::abs(factor) <= tolerance) continue;
      for (std::size_t c = col; c < cols; ++c)
        matrix[row][c] -= factor * matrix[rank][c];
    }

    ++rank;
  }

  return rank;
}

}  // namespace

ConstraintDiagnostics analyzeConstraintSystem(
    const Sketch& sketch, bool computeDof) {
  ConstraintDiagnostics result;
  const Layout layout = makeLayout(sketch);
  result.variableCount = layout.variables.size();

  const auto base =
      evaluate(sketch, layout, layout.variables);

  std::unordered_map<
      ConstraintId,
      std::pair<ConstraintType, double>> worst;

  for (const auto& equation : base) {
    if (equation.userConstraint)
      ++result.userEquationCount;

    const double normalized =
        std::abs(equation.value) /
        std::max(equation.tolerance, 1e-12);

    result.maxNormalizedResidual =
        std::max(result.maxNormalizedResidual,
                 normalized);

    if (!equation.userConstraint ||
        equation.constraintId == kInvalidConstraintId ||
        normalized <= 1.0)
      continue;

    auto& entry = worst[equation.constraintId];
    entry.first = equation.type;
    entry.second =
        std::max(entry.second, normalized);
  }

  for (const auto& [id, entry] : worst)
    result.violations.push_back(
        {id, entry.first, entry.second});

  std::sort(
      result.violations.begin(),
      result.violations.end(),
      [](const ConstraintViolation& first,
         const ConstraintViolation& second) {
        return first.id < second.id;
      });

  result.conflicting = !result.violations.empty();

  if (computeDof &&
      !layout.variables.empty() &&
      !base.empty()) {
    std::vector<std::vector<double>> jacobian(
        base.size(),
        std::vector<double>(
            layout.variables.size(), 0.0));

    for (std::size_t column = 0;
         column < layout.variables.size();
         ++column) {
      auto perturbed = layout.variables;
      const double step =
          std::max(
              1e-6,
              std::abs(perturbed[column]) * 1e-7);
      perturbed[column] += step;

      const auto changed =
          evaluate(sketch, layout, perturbed);
      if (changed.size() != base.size())
        continue;

      for (std::size_t row = 0;
           row < base.size(); ++row) {
        const double scale =
            std::max(base[row].tolerance, 1e-6);
        jacobian[row][column] =
            (changed[row].value -
             base[row].value) /
            step / scale;
      }
    }

    result.equationRank =
        matrixRank(std::move(jacobian));

    result.degreesOfFreedom =
        result.variableCount > result.equationRank
            ? result.variableCount -
                  result.equationRank
            : 0;
  } else {
    result.degreesOfFreedom =
        layout.variables.size();
  }

  result.fullyConstrained =
      !result.conflicting &&
      result.variableCount > 0 &&
      result.degreesOfFreedom == 0;

  return result;
}

bool hasConstraintViolation(
    const ConstraintDiagnostics& diagnostics,
    ConstraintId id) noexcept {
  return std::any_of(
      diagnostics.violations.begin(),
      diagnostics.violations.end(),
      [id](const ConstraintViolation& item) {
        return item.id == id;
      });
}

}  // namespace solidar::sketch

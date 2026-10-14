#include "sketch/EquationKernel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>

namespace solidar::sketch {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = 3.1415926535897932384626433832795;

struct UnionFind {
  explicit UnionFind(std::size_t size) : parent(size), rank(size) {
    std::iota(parent.begin(), parent.end(), 0);
  }
  std::size_t find(std::size_t value) {
    while (parent[value] != value) {
      parent[value] = parent[parent[value]];
      value = parent[value];
    }
    return value;
  }
  void unite(std::size_t first, std::size_t second) {
    first = find(first);
    second = find(second);
    if (first == second) return;
    if (rank[first] < rank[second]) std::swap(first, second);
    parent[second] = first;
    if (rank[first] == rank[second]) ++rank[first];
  }
  std::vector<std::size_t> parent;
  std::vector<std::uint8_t> rank;
};

void appendCompositeMembers(const Sketch& sketch, std::uint64_t elementId,
                            std::vector<GeometryId>& result) {
  if (elementId == 0) return;
  for (std::size_t index = 0; index < sketch.lines().size(); ++index)
    if (sketch.lines()[index].elementId == elementId)
      result.push_back(sketch.lineId(index));
}

void appendPointGeometry(const Sketch& sketch, const PointReference& reference,
                         std::vector<GeometryId>& result) {
  std::visit(
      [&](const auto& source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, LineEndpointReference>)
          result.push_back(source.lineId);
        else if constexpr (std::is_same_v<Source, CircleCenterReference>)
          result.push_back(source.circleId);
        else if constexpr (std::is_same_v<Source, ArcEndpointReference>)
          result.push_back(source.arcId);
        else if constexpr (std::is_same_v<Source, BezierPointReference>)
          result.push_back(source.bezierId);
        else if constexpr (std::is_same_v<Source, CompositeCenterReference>)
          appendCompositeMembers(sketch, source.elementId, result);
      },
      reference.payload());
}

std::vector<GeometryId> referencedGeometry(const Sketch& sketch,
                                           const Constraint& constraint) {
  std::vector<GeometryId> result;
  std::visit(
      [&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, HorizontalConstraint> ||
                      std::is_same_v<Payload, VerticalConstraint> ||
                      std::is_same_v<Payload, LengthConstraint>) {
          result.push_back(payload.lineId);
        } else if constexpr (std::is_same_v<Payload, RadiusConstraint> ||
                             std::is_same_v<Payload, DiameterConstraint>) {
          result.push_back(payload.circleId);
        } else if constexpr (std::is_same_v<Payload, ParallelConstraint> ||
                             std::is_same_v<Payload, PerpendicularConstraint> ||
                             std::is_same_v<Payload, AngleConstraint> ||
                             std::is_same_v<Payload, LineDistanceConstraint>) {
          result.push_back(payload.firstLineId);
          result.push_back(payload.secondLineId);
        } else if constexpr (std::is_same_v<Payload, EqualConstraint> ||
                             std::is_same_v<Payload, TangentConstraint>) {
          result.push_back(payload.firstGeometryId);
          result.push_back(payload.secondGeometryId);
        } else if constexpr (std::is_same_v<Payload, PointOnLineConstraint> ||
                             std::is_same_v<Payload, MidpointConstraint>) {
          result.push_back(payload.lineId);
          appendPointGeometry(sketch, payload.point, result);
        } else if constexpr (std::is_same_v<Payload, PointOnCircleConstraint>) {
          result.push_back(payload.circleId);
          appendPointGeometry(sketch, payload.point, result);
        } else if constexpr (std::is_same_v<Payload, PointOnArcConstraint>) {
          result.push_back(payload.arcId);
          appendPointGeometry(sketch, payload.point, result);
        } else if constexpr (std::is_same_v<Payload, CoincidentConstraint> ||
                             std::is_same_v<Payload, DistanceConstraint> ||
                             std::is_same_v<Payload, DistanceXConstraint> ||
                             std::is_same_v<Payload, DistanceYConstraint>) {
          appendPointGeometry(sketch, payload.first, result);
          appendPointGeometry(sketch, payload.second, result);
        } else if constexpr (std::is_same_v<Payload, PointOnXAxisConstraint> ||
                             std::is_same_v<Payload, PointOnYAxisConstraint>) {
          appendPointGeometry(sketch, payload.point, result);
        } else if constexpr (std::is_same_v<Payload, LockConstraint>) {
          result.push_back(payload.geometryId);
        }
      },
      constraint.payload());
  std::erase(result, kInvalidGeometryId);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

EquationStatus worseStatus(EquationStatus first, EquationStatus second) {
  return static_cast<int>(first) >= static_cast<int>(second) ? first : second;
}

struct Dual {
  double value{};
  double derivative{};
  Dual() = default;
  Dual(double valueIn, double derivativeIn = 0.0)
      : value(valueIn), derivative(derivativeIn) {}
};

Dual operator+(Dual a, Dual b) {
  return {a.value + b.value, a.derivative + b.derivative};
}
Dual operator-(Dual a, Dual b) {
  return {a.value - b.value, a.derivative - b.derivative};
}
Dual operator-(Dual value) { return {-value.value, -value.derivative}; }
Dual operator*(Dual a, Dual b) {
  return {a.value * b.value,
          a.derivative * b.value + a.value * b.derivative};
}
Dual operator/(Dual a, Dual b) {
  const double denominator = b.value * b.value;
  return {a.value / b.value,
          (a.derivative * b.value - a.value * b.derivative) / denominator};
}

Dual dualSqrt(Dual value) {
  const double root = std::sqrt(std::max(0.0, value.value));
  return {root, root > 0.0 ? value.derivative / (2.0 * root) : 0.0};
}
Dual dualHypot(Dual x, Dual y) { return dualSqrt(x * x + y * y); }
Dual dualAbs(Dual value) {
  if (value.value > 0.0) return value;
  if (value.value < 0.0) return -value;
  return {0.0, 0.0};
}
Dual dualAcos(Dual value) {
  const double clamped = std::clamp(value.value, -1.0, 1.0);
  const double divisor = std::sqrt(std::max(0.0, 1.0 - clamped * clamped));
  return {std::acos(clamped), divisor > 1e-15 ? -value.derivative / divisor : 0.0};
}

template <typename Scalar>
struct Point2 {
  Scalar x;
  Scalar y;
};
template <typename Scalar>
struct Line2 {
  Point2<Scalar> start;
  Point2<Scalar> end;
};
template <typename Scalar>
struct Circle2 {
  Point2<Scalar> center;
  Scalar radius;
};
template <typename Scalar>
struct Arc2 {
  Point2<Scalar> center;
  Scalar radius;
  Scalar startAngle;
  Scalar sweepAngle;
};
template <typename Scalar>
struct Bezier2 {
  std::array<Point2<Scalar>, 4> points;
};

struct EvaluationRow {
  Dual residual;
  EquationUnit unit{EquationUnit::Length};
  EquationStatus status{EquationStatus::Valid};
  std::uint16_t scalarIndex{};
};

class EvaluationContext {
 public:
  EvaluationContext(const Sketch& sketch, const ParameterLayout& layout,
                    std::optional<std::size_t> seed)
      : sketch_(sketch), layout_(layout), seed_(seed) {}

  Dual parameter(GeometryId id, ParameterRole role) const {
    const auto index = layout_.parameterIndex(id, role);
    if (!index) return {std::numeric_limits<double>::quiet_NaN(), 0.0};
    return {layout_.values()[*index], seed_ && *seed_ == *index ? 1.0 : 0.0};
  }
  std::optional<Line2<Dual>> line(GeometryId id) const {
    if (!sketch_.lineIndex(id)) return std::nullopt;
    return Line2<Dual>{{parameter(id, ParameterRole::LineStartX),
                        parameter(id, ParameterRole::LineStartY)},
                       {parameter(id, ParameterRole::LineEndX),
                        parameter(id, ParameterRole::LineEndY)}};
  }
  std::optional<Circle2<Dual>> circle(GeometryId id) const {
    if (!sketch_.circleIndex(id)) return std::nullopt;
    return Circle2<Dual>{{parameter(id, ParameterRole::CircleCenterX),
                          parameter(id, ParameterRole::CircleCenterY)},
                         parameter(id, ParameterRole::CircleRadius)};
  }
  std::optional<Arc2<Dual>> arc(GeometryId id) const {
    if (!sketch_.arcIndex(id)) return std::nullopt;
    return Arc2<Dual>{{parameter(id, ParameterRole::ArcCenterX),
                       parameter(id, ParameterRole::ArcCenterY)},
                      parameter(id, ParameterRole::ArcRadius),
                      parameter(id, ParameterRole::ArcStartAngle),
                      parameter(id, ParameterRole::ArcSweepAngle)};
  }
  std::optional<Bezier2<Dual>> bezier(GeometryId id) const {
    if (!sketch_.bezierIndex(id)) return std::nullopt;
    constexpr std::array xRoles{
        ParameterRole::BezierPoint0X, ParameterRole::BezierPoint1X,
        ParameterRole::BezierPoint2X, ParameterRole::BezierPoint3X};
    constexpr std::array yRoles{
        ParameterRole::BezierPoint0Y, ParameterRole::BezierPoint1Y,
        ParameterRole::BezierPoint2Y, ParameterRole::BezierPoint3Y};
    Bezier2<Dual> result;
    for (std::size_t index = 0; index < 4; ++index)
      result.points[index] = {parameter(id, xRoles[index]),
                              parameter(id, yRoles[index])};
    return result;
  }
  std::optional<Point2<Dual>> point(const PointReference& reference) const {
    return std::visit(
        [&](const auto& source) -> std::optional<Point2<Dual>> {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<Source, DatumOriginReference>) {
            return Point2<Dual>{{0.0}, {0.0}};
          } else if constexpr (std::is_same_v<Source, LineEndpointReference>) {
            const auto geometry = line(source.lineId);
            if (!geometry) return std::nullopt;
            return source.endpoint == Endpoint::Start ? geometry->start
                                                       : geometry->end;
          } else if constexpr (std::is_same_v<Source, CircleCenterReference>) {
            const auto geometry = circle(source.circleId);
            return geometry ? std::optional{geometry->center} : std::nullopt;
          } else if constexpr (std::is_same_v<Source, ArcEndpointReference>) {
            const auto geometry = arc(source.arcId);
            if (!geometry) return std::nullopt;
            const Dual angle = source.endpoint == Endpoint::Start
                                   ? geometry->startAngle
                                   : geometry->startAngle + geometry->sweepAngle;
            return Point2<Dual>{
                geometry->center.x +
                    geometry->radius * Dual{std::cos(angle.value),
                                             -std::sin(angle.value) * angle.derivative},
                geometry->center.y +
                    geometry->radius * Dual{std::sin(angle.value),
                                             std::cos(angle.value) * angle.derivative}};
          } else if constexpr (std::is_same_v<Source, BezierPointReference>) {
            const auto geometry = bezier(source.bezierId);
            if (!geometry || source.pointIndex >= 4) return std::nullopt;
            return geometry->points[source.pointIndex];
          } else {
            std::vector<GeometryId> members;
            appendCompositeMembers(sketch_, source.elementId, members);
            if (members.empty()) return std::nullopt;
            Point2<Dual> center{{0.0}, {0.0}};
            std::size_t count = 0;
            for (const auto id : members) {
              const auto geometry = line(id);
              if (!geometry) continue;
              center.x = center.x + geometry->start.x + geometry->end.x;
              center.y = center.y + geometry->start.y + geometry->end.y;
              count += 2;
            }
            if (count == 0) return std::nullopt;
            return Point2<Dual>{center.x / Dual{static_cast<double>(count)},
                                center.y / Dual{static_cast<double>(count)}};
          }
        },
        reference.payload());
  }

 private:
  const Sketch& sketch_;
  const ParameterLayout& layout_;
  std::optional<std::size_t> seed_;
};

Dual distance(Point2<Dual> first, Point2<Dual> second) {
  return dualHypot(second.x - first.x, second.y - first.y);
}
Dual cross(Point2<Dual> first, Point2<Dual> second) {
  return first.x * second.y - first.y * second.x;
}
Dual dot(Point2<Dual> first, Point2<Dual> second) {
  return first.x * second.x + first.y * second.y;
}
Point2<Dual> direction(const Line2<Dual>& line) {
  return {line.end.x - line.start.x, line.end.y - line.start.y};
}
Dual norm(Point2<Dual> value) { return dualHypot(value.x, value.y); }

EquationStatus geometryStatus(const Sketch& sketch,
                              const EvaluationContext& context,
                              GeometryId id) {
  const auto finite = [](Dual value) { return std::isfinite(value.value); };
  if (const auto line = context.line(id)) {
    if (!finite(line->start.x) || !finite(line->start.y) ||
        !finite(line->end.x) || !finite(line->end.y))
      return EquationStatus::NonFiniteInput;
    return norm(direction(*line)).value <= 1e-14
               ? EquationStatus::DegenerateGeometry
               : EquationStatus::Valid;
  }
  if (const auto circle = context.circle(id)) {
    if (!finite(circle->center.x) || !finite(circle->center.y) ||
        !finite(circle->radius))
      return EquationStatus::NonFiniteInput;
    return circle->radius.value <= 1e-14
               ? EquationStatus::DegenerateGeometry
               : EquationStatus::Valid;
  }
  if (const auto arc = context.arc(id)) {
    if (!finite(arc->center.x) || !finite(arc->center.y) ||
        !finite(arc->radius) || !finite(arc->startAngle) ||
        !finite(arc->sweepAngle))
      return EquationStatus::NonFiniteInput;
    return arc->radius.value <= 1e-14 ||
                   arc->sweepAngle.value <= 1e-14 ||
                   arc->sweepAngle.value >= kTwoPi - 1e-14
               ? EquationStatus::DegenerateGeometry
               : EquationStatus::Valid;
  }
  if (const auto bezier = context.bezier(id)) {
    double controlPolygonLength = 0.0;
    for (const auto& point : bezier->points)
      if (!finite(point.x) || !finite(point.y))
        return EquationStatus::NonFiniteInput;
    for (std::size_t index = 1; index < bezier->points.size(); ++index)
      controlPolygonLength +=
          distance(bezier->points[index - 1], bezier->points[index]).value;
    return controlPolygonLength <= 1e-14
               ? EquationStatus::DegenerateGeometry
               : EquationStatus::Valid;
  }
  static_cast<void>(sketch);
  return EquationStatus::MalformedReference;
}

void addRow(std::vector<EvaluationRow>& rows, Dual value, EquationUnit unit,
            EquationStatus status = EquationStatus::Valid) {
  rows.push_back({value, unit, status,
                  static_cast<std::uint16_t>(rows.size())});
}
void addInvalid(std::vector<EvaluationRow>& rows, EquationStatus status,
                EquationUnit unit = EquationUnit::Length) {
  addRow(rows, {1.0, 0.0}, unit, status);
}

Dual signedLineDistance(Point2<Dual> point, const Line2<Dual>& line,
                        EquationStatus& status) {
  const auto vector = direction(line);
  const Dual length = norm(vector);
  if (!std::isfinite(length.value) || length.value <= 1e-14) {
    status = EquationStatus::DegenerateGeometry;
    return {1.0, 0.0};
  }
  return cross(vector, {point.x - line.start.x, point.y - line.start.y}) /
         length;
}

Dual segmentOutside(Point2<Dual> point, const Line2<Dual>& line,
                    EquationStatus& status) {
  const auto vector = direction(line);
  const Dual lengthSquared = dot(vector, vector);
  if (!std::isfinite(lengthSquared.value) || lengthSquared.value <= 1e-28) {
    status = EquationStatus::DegenerateGeometry;
    return {1.0, 0.0};
  }
  const Dual raw = dot({point.x - line.start.x, point.y - line.start.y}, vector) /
                   lengthSquared;
  if (raw.value < 0.0) return -raw * dualSqrt(lengthSquared);
  if (raw.value > 1.0) return (raw - Dual{1.0}) * dualSqrt(lengthSquared);
  return {0.0, 0.0};
}

Dual segmentDistance(Point2<Dual> point, const Line2<Dual>& line,
                     EquationStatus& status) {
  const auto vector = direction(line);
  const Dual lengthSquared = dot(vector, vector);
  if (!std::isfinite(lengthSquared.value) || lengthSquared.value <= 1e-28) {
    status = EquationStatus::DegenerateGeometry;
    return {1.0, 0.0};
  }
  const Dual raw = dot({point.x - line.start.x, point.y - line.start.y}, vector) /
                   lengthSquared;
  const Dual t = raw.value < 0.0 ? Dual{0.0}
                 : raw.value > 1.0 ? Dual{1.0} : raw;
  return distance(point, {line.start.x + vector.x * t,
                          line.start.y + vector.y * t});
}

Point2<Dual> arcPoint(const Arc2<Dual>& arc, bool start) {
  const Dual angle = start ? arc.startAngle : arc.startAngle + arc.sweepAngle;
  const Dual cosine{std::cos(angle.value),
                    -std::sin(angle.value) * angle.derivative};
  const Dual sine{std::sin(angle.value),
                  std::cos(angle.value) * angle.derivative};
  return {arc.center.x + arc.radius * cosine,
          arc.center.y + arc.radius * sine};
}

Dual finiteArcDistance(Point2<Dual> point, const Arc2<Dual>& arc,
                       EquationStatus& status) {
  if (!std::isfinite(arc.radius.value) ||
      !std::isfinite(arc.startAngle.value) ||
      !std::isfinite(arc.sweepAngle.value)) {
    status = EquationStatus::NonFiniteInput;
    return {1.0, 0.0};
  }
  if (arc.radius.value <= 1e-14 || arc.sweepAngle.value <= 1e-14 ||
      arc.sweepAngle.value >= kTwoPi - 1e-14) {
    status = EquationStatus::DegenerateGeometry;
    return {1.0, 0.0};
  }
  double angle = std::atan2(point.y.value - arc.center.y.value,
                            point.x.value - arc.center.x.value);
  double delta = std::fmod(angle - arc.startAngle.value, kTwoPi);
  if (delta < 0.0) delta += kTwoPi;
  if (delta <= arc.sweepAngle.value + 1e-12)
    return distance(point, arc.center) - arc.radius;
  const Dual toStart = distance(point, arcPoint(arc, true));
  const Dual toEnd = distance(point, arcPoint(arc, false));
  return toStart.value <= toEnd.value ? toStart : toEnd;
}

std::pair<Point2<Dual>, Point2<Dual>> endpointAndTangent(
    const Bezier2<Dual>& curve, bool start) {
  const auto endpoint = curve.points[start ? 0 : 3];
  const auto handle = curve.points[start ? 1 : 2];
  return {endpoint, {handle.x - endpoint.x, handle.y - endpoint.y}};
}

Dual normalizedCross(Point2<Dual> first, Point2<Dual> second,
                     EquationStatus& status) {
  const Dual denominator = norm(first) * norm(second);
  if (denominator.value <= 1e-14 || !std::isfinite(denominator.value)) {
    status = EquationStatus::DegenerateGeometry;
    return {1.0, 0.0};
  }
  return cross(first, second) / denominator;
}
Dual normalizedDot(Point2<Dual> first, Point2<Dual> second,
                   EquationStatus& status) {
  const Dual denominator = norm(first) * norm(second);
  if (denominator.value <= 1e-14 || !std::isfinite(denominator.value)) {
    status = EquationStatus::DegenerateGeometry;
    return {1.0, 0.0};
  }
  return dot(first, second) / denominator;
}

std::vector<EvaluationRow> evaluateConstraint(const Sketch& sketch,
                                               const ParameterLayout& layout,
                                               const Constraint& constraint,
                                               std::optional<std::size_t> seed) {
  std::vector<EvaluationRow> rows;
  if (constraint.mode != ConstraintMode::Driving) return rows;
  EvaluationContext context(sketch, layout, seed);
  const auto malformed = [&] { addInvalid(rows, EquationStatus::MalformedReference); };
  const auto unsupported = [&] { addInvalid(rows, EquationStatus::UnsupportedCombination); };

  switch (static_cast<ConstraintType>(constraint.type)) {
    case ConstraintType::Horizontal:
    case ConstraintType::Vertical: {
      const auto line = context.line(constraint.firstGeometry);
      if (!line) { malformed(); break; }
      const auto status = geometryStatus(sketch, context,
                                         constraint.firstGeometry);
      if (status != EquationStatus::Valid) {
        addInvalid(rows, status);
        break;
      }
      addRow(rows, static_cast<ConstraintType>(constraint.type) == ConstraintType::Horizontal
                       ? line->end.y - line->start.y
                       : line->end.x - line->start.x,
             EquationUnit::Length);
      break;
    }
    case ConstraintType::Coincident: {
      const auto first = context.point(constraint.firstPoint);
      const auto second = context.point(constraint.secondPoint);
      if (!first || !second) { malformed(); break; }
      addRow(rows, second->x - first->x, EquationUnit::Length);
      addRow(rows, second->y - first->y, EquationUnit::Length);
      break;
    }
    case ConstraintType::PointOnLine: {
      const auto line = context.line(constraint.firstGeometry);
      const auto point = context.point(constraint.secondPoint);
      if (!line || !point) { malformed(); break; }
      EquationStatus status = EquationStatus::Valid;
      const Dual onLine = signedLineDistance(*point, *line, status);
      addRow(rows, onLine, EquationUnit::Length, status);
      addRow(rows, segmentOutside(*point, *line, status), EquationUnit::Length,
             status);
      break;
    }
    case ConstraintType::Distance:
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY: {
      const auto first = context.point(constraint.firstPoint);
      const auto second = context.point(constraint.secondPoint);
      const double target = constraint.value;
      if (!first || !second) { malformed(); break; }
      if (!std::isfinite(target) || target <= 0.0) {
        addInvalid(rows, std::isfinite(target) ? EquationStatus::MalformedReference
                                               : EquationStatus::NonFiniteInput);
        break;
      }
      Dual current;
      const auto type = static_cast<ConstraintType>(constraint.type);
      if (type == ConstraintType::Distance) {
        current = distance(*first, *second);
      } else if (type == ConstraintType::DistanceX) {
        current = dualAbs(second->x - first->x);
      } else {
        current = dualAbs(second->y - first->y);
      }
      // Distance and absolute axis-distance are non-differentiable at zero.
      // Returning a plausible residual with a zero derivative would make the
      // candidate unusable by Newton/LM while incorrectly advertising Valid.
      if (current.value <= 1e-14) {
        addInvalid(rows, EquationStatus::DegenerateGeometry);
        break;
      }
      addRow(rows, current - Dual{target}, EquationUnit::Length);
      break;
    }
    case ConstraintType::Length: {
      const auto line = context.line(constraint.firstGeometry);
      const double target = constraint.value;
      if (!line) { malformed(); break; }
      if (!std::isfinite(target) || target <= 0.0) {
        addInvalid(rows, std::isfinite(target) ? EquationStatus::MalformedReference
                                               : EquationStatus::NonFiniteInput);
        break;
      }
      const Dual current = distance(line->start, line->end);
      if (current.value <= 1e-14) {
        addInvalid(rows, EquationStatus::DegenerateGeometry);
        break;
      }
      addRow(rows, current - Dual{target}, EquationUnit::Length);
      break;
    }
    case ConstraintType::Radius:
    case ConstraintType::Diameter: {
      const auto circle = context.circle(constraint.firstGeometry);
      const double target = constraint.value;
      if (!circle) { malformed(); break; }
      if (!std::isfinite(target)) {
        addInvalid(rows, EquationStatus::NonFiniteInput);
        break;
      }
      if (target <= 0.0) {
        addInvalid(rows, EquationStatus::MalformedReference);
        break;
      }
      if (circle->radius.value <= 1e-14) {
        addInvalid(rows, EquationStatus::DegenerateGeometry);
        break;
      }
      const Dual current = static_cast<ConstraintType>(constraint.type) ==
                                   ConstraintType::Diameter
                               ? circle->radius * Dual{2.0}
                               : circle->radius;
      addRow(rows, current - Dual{target}, EquationUnit::Length);
      break;
    }
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular:
    case ConstraintType::Angle: {
      const auto first = context.line(constraint.firstGeometry);
      const auto second = context.line(constraint.secondGeometry);
      if (!first || !second) { malformed(); break; }
      EquationStatus status = EquationStatus::Valid;
      const auto a = direction(*first);
      const auto b = direction(*second);
      const auto type = static_cast<ConstraintType>(constraint.type);
      if (type == ConstraintType::Parallel)
        addRow(rows, normalizedCross(a, b, status), EquationUnit::Angle, status);
      else if (type == ConstraintType::Perpendicular)
        addRow(rows, normalizedDot(a, b, status), EquationUnit::Angle, status);
      else {
        const double degrees = constraint.value;
        if (!std::isfinite(degrees) || degrees <= 0.0 || degrees >= 180.0) {
          addInvalid(rows, !std::isfinite(degrees) ? EquationStatus::NonFiniteInput
                                                   : EquationStatus::MalformedReference,
                     EquationUnit::Angle);
          break;
        }
        const Dual cosine = normalizedDot(a, b, status);
        if (status != EquationStatus::Valid ||
            std::abs(std::abs(cosine.value) - 1.0) <= 1e-14) {
          addInvalid(rows, status == EquationStatus::Valid
                               ? EquationStatus::DegenerateGeometry
                               : status,
                     EquationUnit::Angle);
          break;
        }
        addRow(rows, dualAcos(cosine) - Dual{degrees * kPi / 180.0},
               EquationUnit::Angle, status);
      }
      break;
    }
    case ConstraintType::Equal: {
      const auto firstLine = context.line(constraint.firstGeometry);
      const auto secondLine = context.line(constraint.secondGeometry);
      if (firstLine && secondLine) {
        const auto firstStatus = geometryStatus(sketch, context,
                                                constraint.firstGeometry);
        const auto secondStatus = geometryStatus(sketch, context,
                                                 constraint.secondGeometry);
        const auto status = worseStatus(firstStatus, secondStatus);
        if (status != EquationStatus::Valid) {
          addInvalid(rows, status);
          break;
        }
        addRow(rows, distance(firstLine->start, firstLine->end) -
                         distance(secondLine->start, secondLine->end),
               EquationUnit::Length);
        break;
      }
      const auto firstCircle = context.circle(constraint.firstGeometry);
      const auto secondCircle = context.circle(constraint.secondGeometry);
      if (firstCircle && secondCircle) {
        const auto firstStatus = geometryStatus(sketch, context,
                                                constraint.firstGeometry);
        const auto secondStatus = geometryStatus(sketch, context,
                                                 constraint.secondGeometry);
        const auto status = worseStatus(firstStatus, secondStatus);
        if (status != EquationStatus::Valid) {
          addInvalid(rows, status);
          break;
        }
        addRow(rows, firstCircle->radius - secondCircle->radius,
               EquationUnit::Length);
        break;
      }
      unsupported();
      break;
    }
    case ConstraintType::PointOnCircle: {
      const auto circle = context.circle(constraint.firstGeometry);
      const auto point = context.point(constraint.secondPoint);
      if (!circle || !point) { malformed(); break; }
      if (circle->radius.value <= 1e-14) {
        addInvalid(rows, EquationStatus::DegenerateGeometry);
        break;
      }
      if (distance(circle->center, *point).value <= 1e-14) {
        addInvalid(rows, EquationStatus::DegenerateGeometry);
        break;
      }
      addRow(rows, distance(circle->center, *point) - circle->radius,
             EquationUnit::Length);
      break;
    }
    case ConstraintType::PointOnArc: {
      const auto arc = context.arc(constraint.firstGeometry);
      const auto point = context.point(constraint.secondPoint);
      if (!arc || !point) { malformed(); break; }
      if (distance(arc->center, *point).value <= 1e-14) {
        addInvalid(rows, EquationStatus::DegenerateGeometry);
        break;
      }
      EquationStatus status = EquationStatus::Valid;
      addRow(rows, finiteArcDistance(*point, *arc, status), EquationUnit::Length,
             status);
      break;
    }
    case ConstraintType::Midpoint: {
      const auto line = context.line(constraint.firstGeometry);
      const auto point = context.point(constraint.secondPoint);
      if (!line || !point) { malformed(); break; }
      addRow(rows, point->x - (line->start.x + line->end.x) / Dual{2.0},
             EquationUnit::Length);
      addRow(rows, point->y - (line->start.y + line->end.y) / Dual{2.0},
             EquationUnit::Length);
      break;
    }
    case ConstraintType::PointOnXAxis:
    case ConstraintType::PointOnYAxis: {
      const auto point = context.point(constraint.secondPoint);
      if (!point) { malformed(); break; }
      addRow(rows, static_cast<ConstraintType>(constraint.type) ==
                           ConstraintType::PointOnXAxis
                       ? point->y
                       : point->x,
             EquationUnit::Length);
      break;
    }
    case ConstraintType::Tangent: {
      const auto firstLine = context.line(constraint.firstGeometry);
      const auto secondLine = context.line(constraint.secondGeometry);
      const auto firstCircle = context.circle(constraint.firstGeometry);
      const auto secondCircle = context.circle(constraint.secondGeometry);
      const auto firstArc = context.arc(constraint.firstGeometry);
      const auto secondArc = context.arc(constraint.secondGeometry);
      const auto firstBezier = context.bezier(constraint.firstGeometry);
      const auto secondBezier = context.bezier(constraint.secondGeometry);
      struct Radial { Point2<Dual> center; Dual radius; bool arc; };
      const auto radial = [](const auto& circle, const auto& arc)
          -> std::optional<Radial> {
        if (circle) return Radial{circle->center, circle->radius, false};
        if (arc) return Radial{arc->center, arc->radius, true};
        return std::nullopt;
      };
      const auto firstRadial = radial(firstCircle, firstArc);
      const auto secondRadial = radial(secondCircle, secondArc);
      EquationStatus status = EquationStatus::Valid;
      if ((firstLine || secondLine) && (firstRadial || secondRadial)) {
        const auto& line = firstLine ? *firstLine : *secondLine;
        const auto data = firstRadial ? *firstRadial : *secondRadial;
        // Tangency is defined against the line carrier.  Using the clamped
        // segment distance makes any endpoint lying on the circle appear
        // tangent even when the line direction cuts through the circle.
        const Dual centerDistance =
            dualAbs(signedLineDistance(data.center, line, status));
        if (status != EquationStatus::Valid || centerDistance.value <= 1e-14) {
          addInvalid(rows, status == EquationStatus::Valid
                               ? EquationStatus::DegenerateGeometry
                               : status);
          break;
        }
        addRow(rows, centerDistance - data.radius,
               EquationUnit::Length,
               status);
        // When an endpoint of this same line is also constrained onto the
        // radial geometry, tangency makes that endpoint the contact point.
        // The ordinary distance equation has a double root in its tangential
        // direction there; this implied orthogonality row removes the local
        // rank loss without changing the accepted geometric solutions.
        const GeometryId lineId = firstLine ? constraint.firstGeometry
                                            : constraint.secondGeometry;
        const GeometryId radialId = firstRadial ? constraint.firstGeometry
                                                : constraint.secondGeometry;
        for (const auto& peer : sketch.constraints()) {
          if (peer.mode != ConstraintMode::Driving) continue;
          const auto* pointOnCircle =
              std::get_if<PointOnCircleConstraint>(&peer.payload());
          if (!pointOnCircle || pointOnCircle->circleId != radialId) continue;
          const auto* endpoint =
              std::get_if<LineEndpointReference>(&pointOnCircle->point.payload());
          if (!endpoint || endpoint->lineId != lineId) continue;
          const auto contact = context.point(pointOnCircle->point);
          if (!contact) continue;
          EquationStatus contactStatus = EquationStatus::Valid;
          addRow(rows,
                 normalizedDot(direction(line),
                               {contact->x - data.center.x,
                                contact->y - data.center.y},
                               contactStatus),
                 EquationUnit::Angle, contactStatus);
        }
        const auto& finite = firstArc ? firstArc : secondArc;
        if (finite) {
          const auto vector = direction(line);
          const Dual lengthSquared = dot(vector, vector);
          if (lengthSquared.value <= 1e-28) {
            addInvalid(rows, EquationStatus::DegenerateGeometry);
            break;
          }
          const Dual raw = dot({finite->center.x - line.start.x,
                                finite->center.y - line.start.y}, vector) /
                           lengthSquared;
          const Dual t = raw.value < 0.0 ? Dual{0.0}
                         : raw.value > 1.0 ? Dual{1.0} : raw;
          const Point2<Dual> contact{line.start.x + vector.x * t,
                                     line.start.y + vector.y * t};
          addRow(rows, finiteArcDistance(contact, *finite, status),
                 EquationUnit::Length, status);
        }
        break;
      }
      if (firstRadial && secondRadial) {
        const Dual centerDistance = distance(firstRadial->center,
                                             secondRadial->center);
        if (centerDistance.value <= 1e-14 ||
            firstRadial->radius.value <= 1e-14 ||
            secondRadial->radius.value <= 1e-14) {
          addInvalid(rows, EquationStatus::DegenerateGeometry);
          break;
        }
        const Dual external = firstRadial->radius + secondRadial->radius;
        const Dual internal = dualAbs(firstRadial->radius - secondRadial->radius);
        const bool internalBranch = internal.value > 1e-12 &&
            std::abs(centerDistance.value - internal.value) <
                std::abs(centerDistance.value - external.value);
        addRow(rows, centerDistance - (internalBranch ? internal : external),
               EquationUnit::Length);
        const Point2<Dual> axis{
            (secondRadial->center.x - firstRadial->center.x) / centerDistance,
            (secondRadial->center.y - firstRadial->center.y) / centerDistance};
        const Point2<Dual> contact =
            internalBranch &&
                    firstRadial->radius.value < secondRadial->radius.value
                ? Point2<Dual>{
                      firstRadial->center.x - axis.x * firstRadial->radius,
                      firstRadial->center.y - axis.y * firstRadial->radius}
                : Point2<Dual>{
                      firstRadial->center.x + axis.x * firstRadial->radius,
                      firstRadial->center.y + axis.y * firstRadial->radius};
        if (firstArc)
          addRow(rows, finiteArcDistance(contact, *firstArc, status),
                 EquationUnit::Length, status);
        if (secondArc)
          addRow(rows, finiteArcDistance(contact, *secondArc, status),
                 EquationUnit::Length, status);
        break;
      }
      if ((firstLine || secondLine) && (firstBezier || secondBezier)) {
        const auto& line = firstLine ? *firstLine : *secondLine;
        const auto& curve = firstBezier ? *firstBezier : *secondBezier;
        const auto start = endpointAndTangent(curve, true);
        const auto end = endpointAndTangent(curve, false);
        EquationStatus firstStatus = EquationStatus::Valid;
        EquationStatus secondStatus = EquationStatus::Valid;
        const auto startDistance = segmentDistance(start.first, line,
                                                   firstStatus);
        const auto endDistance = segmentDistance(end.first, line,
                                                 secondStatus);
        const auto chosen = startDistance.value <= endDistance.value ? start : end;
        EquationStatus onLineStatus = worseStatus(firstStatus, secondStatus);
        EquationStatus outsideStatus = onLineStatus;
        addRow(rows, signedLineDistance(chosen.first, line, onLineStatus),
               EquationUnit::Length, onLineStatus);
        addRow(rows, segmentOutside(chosen.first, line, outsideStatus),
               EquationUnit::Length, outsideStatus);
        addRow(rows, normalizedCross(direction(line), chosen.second, status),
               EquationUnit::Angle, status);
        break;
      }
      if ((firstRadial || secondRadial) && (firstBezier || secondBezier)) {
        const auto data = firstRadial ? *firstRadial : *secondRadial;
        const auto& curve = firstBezier ? *firstBezier : *secondBezier;
        const auto start = endpointAndTangent(curve, true);
        const auto end = endpointAndTangent(curve, false);
        const Dual startError = dualAbs(distance(start.first, data.center) - data.radius);
        const Dual endError = dualAbs(distance(end.first, data.center) - data.radius);
        const auto chosen = startError.value <= endError.value ? start : end;
        const Point2<Dual> radius{chosen.first.x - data.center.x,
                                  chosen.first.y - data.center.y};
        if (norm(radius).value <= 1e-14) {
          addInvalid(rows, EquationStatus::DegenerateGeometry);
          break;
        }
        addRow(rows, distance(chosen.first, data.center) - data.radius,
               EquationUnit::Length);
        addRow(rows, normalizedDot(radius, chosen.second, status),
               EquationUnit::Angle, status);
        const auto& finite = firstArc ? firstArc : secondArc;
        if (finite)
          addRow(rows, finiteArcDistance(chosen.first, *finite, status),
                 EquationUnit::Length, status);
        break;
      }
      if (firstBezier && secondBezier) {
        double best = std::numeric_limits<double>::infinity();
        std::pair<Point2<Dual>, Point2<Dual>> firstData;
        std::pair<Point2<Dual>, Point2<Dual>> secondData;
        for (const bool firstStart : {true, false})
          for (const bool secondStart : {true, false}) {
            const auto a = endpointAndTangent(*firstBezier, firstStart);
            const auto b = endpointAndTangent(*secondBezier, secondStart);
            const double candidate = distance(a.first, b.first).value;
            if (candidate < best) {
              best = candidate;
              firstData = a;
              secondData = b;
            }
          }
        addRow(rows, firstData.first.x - secondData.first.x,
               EquationUnit::Length);
        addRow(rows, firstData.first.y - secondData.first.y,
               EquationUnit::Length);
        addRow(rows, normalizedCross(firstData.second, secondData.second, status),
               EquationUnit::Angle, status);
        break;
      }
      unsupported();
      break;
    }
    case ConstraintType::LineDistance: {
      const auto first = context.line(constraint.firstGeometry);
      const auto second = context.line(constraint.secondGeometry);
      const double target = constraint.value;
      if (!first || !second) { malformed(); break; }
      if (!std::isfinite(target) || target <= 0.0) {
        addInvalid(rows, !std::isfinite(target) ? EquationStatus::NonFiniteInput
                                                : EquationStatus::MalformedReference);
        break;
      }
      EquationStatus status = EquationStatus::Valid;
      const Dual signedDistance = signedLineDistance(second->start, *first,
                                                     status);
      if (status != EquationStatus::Valid ||
          std::abs(signedDistance.value) <= 1e-14) {
        addInvalid(rows, status == EquationStatus::Valid
                             ? EquationStatus::DegenerateGeometry
                             : status);
        break;
      }
      addRow(rows, dualAbs(signedDistance) - Dual{target},
             EquationUnit::Length, status);
      break;
    }
    case ConstraintType::Lock:
      // Lock is represented by fixed ParameterDescriptor entries. It does not
      // create snapshot-restoration equations.
      {
        const auto status = geometryStatus(sketch, context,
                                           constraint.firstGeometry);
        if (status != EquationStatus::Valid)
          addInvalid(rows, status, EquationUnit::Dimensionless);
      }
      break;
  }
  for (auto& row : rows)
    if (!std::isfinite(row.residual.value) ||
        !std::isfinite(row.residual.derivative)) {
      row.residual = {1.0, 0.0};
      row.status = EquationStatus::NonFiniteInput;
    }
  return rows;
}

std::size_t matrixRank(std::vector<double> matrix, std::size_t rows,
                       std::size_t columns) {
  if (rows == 0 || columns == 0) return 0;
  double normInf = 0.0;
  for (std::size_t row = 0; row < rows; ++row) {
    double rowSum = 0.0;
    for (std::size_t column = 0; column < columns; ++column)
      rowSum += std::abs(matrix[row * columns + column]);
    normInf = std::max(normInf, rowSum);
  }
  if (normInf == 0.0) return 0;
  const double tolerance = std::numeric_limits<double>::epsilon() *
                           static_cast<double>(std::max(rows, columns)) *
                           normInf * 64.0;
  std::size_t rank = 0;
  for (std::size_t column = 0; column < columns && rank < rows; ++column) {
    std::size_t pivot = rank;
    for (std::size_t row = rank + 1; row < rows; ++row)
      if (std::abs(matrix[row * columns + column]) >
          std::abs(matrix[pivot * columns + column]))
        pivot = row;
    if (std::abs(matrix[pivot * columns + column]) <= tolerance) continue;
    for (std::size_t c = 0; c < columns; ++c)
      std::swap(matrix[pivot * columns + c], matrix[rank * columns + c]);
    const double divisor = matrix[rank * columns + column];
    for (std::size_t c = column; c < columns; ++c)
      matrix[rank * columns + c] /= divisor;
    for (std::size_t row = 0; row < rows; ++row) {
      if (row == rank) continue;
      const double factor = matrix[row * columns + column];
      if (std::abs(factor) <= tolerance) continue;
      for (std::size_t c = column; c < columns; ++c)
        matrix[row * columns + c] -= factor * matrix[rank * columns + c];
    }
    ++rank;
  }
  return rank;
}

constexpr std::array kRules{
    ConstraintEquationRule{ConstraintType::Horizontal, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Vertical, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Coincident, 2, 2, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::PointOnLine, 2, 2, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Distance, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Length, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Radius, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Diameter, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Parallel, 1, 1, EquationUnit::Angle, false},
    ConstraintEquationRule{ConstraintType::Perpendicular, 1, 1, EquationUnit::Angle, false},
    ConstraintEquationRule{ConstraintType::Equal, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Angle, 1, 1, EquationUnit::Angle, false},
    ConstraintEquationRule{ConstraintType::DistanceX, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::DistanceY, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::PointOnCircle, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Tangent, 1, 3, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::LineDistance, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Lock, 0, 0, EquationUnit::Dimensionless, true},
    ConstraintEquationRule{ConstraintType::PointOnArc, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::Midpoint, 2, 2, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::PointOnXAxis, 1, 1, EquationUnit::Length, false},
    ConstraintEquationRule{ConstraintType::PointOnYAxis, 1, 1, EquationUnit::Length, false},
};

}  // namespace

ConstraintGraph ConstraintGraph::build(const Sketch& sketch) {
  ConstraintGraph graph;
  std::vector<GeometryId> ids;
  ids.reserve(sketch.lines().size() + sketch.circles().size() +
              sketch.arcs().size() + sketch.beziers().size());
  for (std::size_t i = 0; i < sketch.lines().size(); ++i) ids.push_back(sketch.lineId(i));
  for (std::size_t i = 0; i < sketch.circles().size(); ++i) ids.push_back(sketch.circleId(i));
  for (std::size_t i = 0; i < sketch.arcs().size(); ++i) ids.push_back(sketch.arcId(i));
  for (std::size_t i = 0; i < sketch.beziers().size(); ++i) ids.push_back(sketch.bezierId(i));
  std::erase(ids, kInvalidGeometryId);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::unordered_map<GeometryId, std::size_t> index;
  index.reserve(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i) index.emplace(ids[i], i);
  UnionFind groups(ids.size());
  std::map<std::size_t, GeometryId> firstCompositeMember;
  for (std::size_t i = 0; i < sketch.lines().size(); ++i) {
    const auto elementId = sketch.lines()[i].elementId;
    const auto id = sketch.lineId(i);
    if (elementId == 0 || !index.contains(id)) continue;
    const auto [found, inserted] = firstCompositeMember.emplace(elementId, id);
    if (!inserted) groups.unite(index.at(found->second), index.at(id));
  }
  std::vector<std::pair<const Constraint*, std::vector<GeometryId>>> references;
  references.reserve(sketch.constraints().size());
  for (const auto& constraint : sketch.constraints()) {
    auto referenced = referencedGeometry(sketch, constraint);
    std::erase_if(referenced, [&](GeometryId id) { return !index.contains(id); });
    for (std::size_t i = 1; i < referenced.size(); ++i)
      groups.unite(index.at(referenced.front()), index.at(referenced[i]));
    references.emplace_back(&constraint, std::move(referenced));
  }
  std::map<std::size_t, ConstraintGraphComponent> byRoot;
  for (const auto id : ids)
    byRoot[groups.find(index.at(id))].geometryIds.push_back(id);
  std::vector<ConstraintId> orphanConstraintIds;
  for (const auto& [constraint, referenced] : references) {
    if (referenced.empty()) {
      if (constraint->mode == ConstraintMode::Driving)
        orphanConstraintIds.push_back(constraint->id);
      continue;
    }
    byRoot[groups.find(index.at(referenced.front()))].constraintIds.push_back(
        constraint->id);
  }
  for (auto& [root, component] : byRoot) {
    static_cast<void>(root);
    std::sort(component.geometryIds.begin(), component.geometryIds.end());
    std::sort(component.constraintIds.begin(), component.constraintIds.end());
    component.constraintIds.erase(
        std::unique(component.constraintIds.begin(), component.constraintIds.end()),
        component.constraintIds.end());
    graph.components_.push_back(std::move(component));
  }
  if (!orphanConstraintIds.empty()) {
    std::sort(orphanConstraintIds.begin(), orphanConstraintIds.end());
    orphanConstraintIds.erase(
        std::unique(orphanConstraintIds.begin(), orphanConstraintIds.end()),
        orphanConstraintIds.end());
    graph.components_.push_back({{}, std::move(orphanConstraintIds)});
  }
  std::sort(graph.components_.begin(), graph.components_.end(),
            [](const auto& first, const auto& second) {
              if (first.geometryIds.empty() != second.geometryIds.empty())
                return !first.geometryIds.empty();
              if (first.geometryIds.empty())
                return first.constraintIds < second.constraintIds;
              return first.geometryIds.front() < second.geometryIds.front();
            });
  return graph;
}

const ConstraintGraphComponent* ConstraintGraph::componentContaining(
    GeometryId id) const noexcept {
  const auto found = std::find_if(
      components_.begin(), components_.end(), [id](const auto& component) {
        return std::binary_search(component.geometryIds.begin(),
                                  component.geometryIds.end(), id);
      });
  return found == components_.end() ? nullptr : &*found;
}

ParameterLayout ParameterLayout::build(
    const Sketch& sketch, std::span<const GeometryId> geometryIds,
    std::span<const ConstraintId> ignoredConstraintIds) {
  ParameterLayout result;
  std::vector<GeometryId> ids(geometryIds.begin(), geometryIds.end());
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::set<GeometryId> locked;
  std::vector<ConstraintId> ignored(ignoredConstraintIds.begin(),
                                    ignoredConstraintIds.end());
  std::sort(ignored.begin(), ignored.end());
  ignored.erase(std::unique(ignored.begin(), ignored.end()), ignored.end());
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.mode != ConstraintMode::Driving ||
        static_cast<ConstraintType>(constraint.type) != ConstraintType::Lock ||
        std::binary_search(ignored.begin(), ignored.end(), constraint.id))
      continue;
    const auto id = static_cast<GeometryId>(constraint.firstGeometry);
    locked.insert(id);
    if (const auto lineIndex = sketch.lineIndex(id)) {
      const auto elementId = sketch.lines()[*lineIndex].elementId;
      if (elementId != 0)
        for (std::size_t index = 0; index < sketch.lines().size(); ++index)
          if (sketch.lines()[index].elementId == elementId)
            locked.insert(sketch.lineId(index));
    }
  }
  struct ScaleAnchor {
    double x{};
    double y{};
    double radius{};
  };
  std::vector<ScaleAnchor> scaleAnchors;
  const auto includePoint = [&](double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    scaleAnchors.push_back({x, y, 0.0});
  };
  const auto includeRadial = [&](double x, double y, double radius) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius))
      return;
    scaleAnchors.push_back({x, y, std::abs(radius)});
  };
  const auto append = [&](GeometryId id, ParameterRole role, double value) {
    const bool fixed = locked.contains(id);
    ParameterDescriptor descriptor{{id, role}, value, fixed, std::nullopt};
    if (!fixed) descriptor.variableIndex = result.variableCount_++;
    result.parameters_.push_back(descriptor);
    result.values_.push_back(value);
  };
  for (const auto id : ids) {
    if (const auto lineIndex = sketch.lineIndex(id)) {
      const auto& line = sketch.lines()[*lineIndex];
      append(id, ParameterRole::LineStartX, line.start.xMm);
      append(id, ParameterRole::LineStartY, line.start.yMm);
      append(id, ParameterRole::LineEndX, line.end.xMm);
      append(id, ParameterRole::LineEndY, line.end.yMm);
      includePoint(line.start.xMm, line.start.yMm);
      includePoint(line.end.xMm, line.end.yMm);
    } else if (const auto circleIndex = sketch.circleIndex(id)) {
      const auto& circle = sketch.circles()[*circleIndex];
      append(id, ParameterRole::CircleCenterX, circle.center.xMm);
      append(id, ParameterRole::CircleCenterY, circle.center.yMm);
      append(id, ParameterRole::CircleRadius, circle.radiusMm);
      includeRadial(circle.center.xMm, circle.center.yMm, circle.radiusMm);
    } else if (const auto arcIndex = sketch.arcIndex(id)) {
      const auto& arc = sketch.arcs()[*arcIndex];
      append(id, ParameterRole::ArcCenterX, arc.center.xMm);
      append(id, ParameterRole::ArcCenterY, arc.center.yMm);
      append(id, ParameterRole::ArcRadius, arc.radiusMm);
      append(id, ParameterRole::ArcStartAngle, arc.startAngleRad);
      append(id, ParameterRole::ArcSweepAngle, arc.sweepAngleRad);
      includeRadial(arc.center.xMm, arc.center.yMm, arc.radiusMm);
    } else if (const auto bezierIndex = sketch.bezierIndex(id)) {
      constexpr std::array xRoles{
          ParameterRole::BezierPoint0X, ParameterRole::BezierPoint1X,
          ParameterRole::BezierPoint2X, ParameterRole::BezierPoint3X};
      constexpr std::array yRoles{
          ParameterRole::BezierPoint0Y, ParameterRole::BezierPoint1Y,
          ParameterRole::BezierPoint2Y, ParameterRole::BezierPoint3Y};
      for (std::size_t point = 0; point < 4; ++point) {
        const auto& value = sketch.beziers()[*bezierIndex].points[point];
        append(id, xRoles[point], value.xMm);
        append(id, yRoles[point], value.yMm);
        includePoint(value.xMm, value.yMm);
      }
    }
  }
  if (!scaleAnchors.empty()) {
    double centerX = 0.0;
    double centerY = 0.0;
    for (const auto& anchor : scaleAnchors) {
      centerX += anchor.x;
      centerY += anchor.y;
    }
    centerX /= static_cast<double>(scaleAnchors.size());
    centerY /= static_cast<double>(scaleAnchors.size());
    double extent = 0.0;
    for (const auto& anchor : scaleAnchors)
      extent = std::max(extent,
                        std::hypot(anchor.x - centerX, anchor.y - centerY) +
                            anchor.radius);
    // Diameter of a centroid-centered enclosing radius is invariant under
    // rigid transforms, scales linearly, and is O(number of parameters).
    result.sketchScale_ = std::max(1e-9, 2.0 * extent);
  }
  return result;
}

std::optional<std::size_t> ParameterLayout::parameterIndex(
    GeometryId geometryId, ParameterRole role) const noexcept {
  const ParameterProvenance key{geometryId, role};
  const auto found = std::lower_bound(
      parameters_.begin(), parameters_.end(), key,
      [](const ParameterDescriptor& descriptor, const ParameterProvenance& value) {
        if (descriptor.provenance.geometryId != value.geometryId)
          return descriptor.provenance.geometryId < value.geometryId;
        return static_cast<int>(descriptor.provenance.role) <
               static_cast<int>(value.role);
      });
  if (found == parameters_.end() || found->provenance != key) return std::nullopt;
  return static_cast<std::size_t>(std::distance(parameters_.begin(), found));
}

std::optional<ParameterLayout> ParameterLayout::withVariableValues(
    std::span<const double> candidate) const {
  if (candidate.size() != variableCount_) return std::nullopt;
  ParameterLayout result = *this;
  for (std::size_t index = 0; index < result.parameters_.size(); ++index) {
    auto& descriptor = result.parameters_[index];
    if (!descriptor.variableIndex) continue;
    const double value = candidate[*descriptor.variableIndex];
    descriptor.value = value;
    result.values_[index] = value;
  }
  return result;
}

EquationSystem::EquationSystem(ConstraintGraphComponent component,
                               ParameterLayout layout,
                               std::vector<EquationRow> rows,
                               std::vector<double> normalizedJacobian,
                               EquationStatus status)
    : component_(std::move(component)),
      layout_(std::move(layout)),
      rows_(std::move(rows)),
      jacobian_(std::move(normalizedJacobian)),
      status_(status) {}

std::size_t EquationSystem::nonZeroJacobianCount(double threshold) const noexcept {
  return static_cast<std::size_t>(std::count_if(
      jacobian_.begin(), jacobian_.end(),
      [threshold](double value) { return std::abs(value) > threshold; }));
}

std::size_t EquationSystem::ownedBytes() const noexcept {
  return component_.geometryIds.capacity() * sizeof(GeometryId) +
         component_.constraintIds.capacity() * sizeof(ConstraintId) +
         layout_.parameters().capacity() * sizeof(ParameterDescriptor) +
         layout_.values().capacity() * sizeof(double) +
         rows_.capacity() * sizeof(EquationRow) +
         jacobian_.capacity() * sizeof(double);
}

std::uint64_t EquationSystem::semanticHash() const noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const auto& parameter : layout_.parameters()) {
    mix(parameter.provenance.geometryId);
    mix(static_cast<std::uint64_t>(parameter.provenance.role));
    mix(static_cast<std::uint64_t>(parameter.fixed));
    mix(std::bit_cast<std::uint64_t>(parameter.value));
  }
  for (const auto& row : rows_) {
    mix(row.provenance.constraintId);
    mix(static_cast<std::uint64_t>(row.provenance.constraintType));
    mix(row.provenance.scalarIndex);
    mix(std::bit_cast<std::uint64_t>(row.normalizedResidual));
    mix(static_cast<std::uint64_t>(row.status));
  }
  for (const auto value : jacobian_) mix(std::bit_cast<std::uint64_t>(value));
  return hash;
}

namespace {

EquationSystem assembleWithLayout(const Sketch& sketch,
                                  const ConstraintGraphComponent& component,
                                  ParameterLayout layout,
                                  std::span<const ConstraintId>
                                      ignoredConstraintIds = {}) {
  std::vector<const Constraint*> constraints;
  constraints.reserve(component.constraintIds.size());
  for (const auto id : component.constraintIds)
    if (!std::binary_search(ignoredConstraintIds.begin(),
                            ignoredConstraintIds.end(), id))
      if (const auto index = sketch.constraintIndex(id))
      constraints.push_back(&sketch.constraints()[*index]);
  std::sort(constraints.begin(), constraints.end(),
            [](const Constraint* first, const Constraint* second) {
              return first->id < second->id;
            });
  std::vector<EquationRow> rows;
  std::vector<double> jacobian;
  EquationStatus systemStatus = EquationStatus::Valid;
  const EvaluationContext validationContext(sketch, layout, std::nullopt);
  for (const auto id : component.geometryIds)
    systemStatus = worseStatus(
        systemStatus, geometryStatus(sketch, validationContext, id));
  const std::size_t columns = layout.parameters().size();
  for (const auto* constraint : constraints) {
    const auto base = evaluateConstraint(sketch, layout, *constraint, std::nullopt);
    const std::size_t rowOffset = rows.size();
    for (const auto& evaluated : base) {
      const double scale = evaluated.unit == EquationUnit::Length
                               ? layout.sketchScale()
                               : 1.0;
      const double tolerance = evaluated.unit == EquationUnit::Length
                                   ? std::max(1e-9, scale * 1e-8)
                                   : 1e-8;
      rows.push_back({evaluated.residual.value,
                      evaluated.residual.value / scale,
                      scale,
                      tolerance,
                      evaluated.unit,
                      evaluated.status,
                      {constraint->id,
                       static_cast<ConstraintType>(constraint->type),
                       evaluated.scalarIndex}});
      systemStatus = worseStatus(systemStatus, evaluated.status);
    }
    jacobian.resize(rows.size() * columns, 0.0);
    if (base.empty()) continue;
    const auto dependencies = referencedGeometry(sketch, *constraint);
    for (std::size_t column = 0; column < columns; ++column) {
      const auto& parameter = layout.parameters()[column];
      if (parameter.fixed ||
          !std::binary_search(dependencies.begin(), dependencies.end(),
                              parameter.provenance.geometryId))
        continue;
      const auto seeded = evaluateConstraint(sketch, layout, *constraint, column);
      if (seeded.size() != base.size()) continue;
      for (std::size_t localRow = 0; localRow < seeded.size(); ++localRow) {
        const auto& output = seeded[localRow];
        const double scale = rows[rowOffset + localRow].scale;
        jacobian[(rowOffset + localRow) * columns + column] =
            output.status == EquationStatus::Valid
                ? output.residual.derivative / scale
                : 0.0;
      }
    }
  }
  return EquationSystem(component, std::move(layout), std::move(rows),
                        std::move(jacobian), systemStatus);
}

}  // namespace

EquationSystem ConstraintEquationRegistry::assemble(
    const Sketch& sketch, const ConstraintGraphComponent& component) {
  return assembleWithLayout(
      sketch, component, ParameterLayout::build(sketch, component.geometryIds));
}

EquationSystem ConstraintEquationRegistry::assemble(
    const Sketch& sketch, const ConstraintGraphComponent& component,
    std::span<const ConstraintId> ignoredConstraintIds) {
  std::vector<ConstraintId> ignored(ignoredConstraintIds.begin(),
                                    ignoredConstraintIds.end());
  std::sort(ignored.begin(), ignored.end());
  ignored.erase(std::unique(ignored.begin(), ignored.end()), ignored.end());
  return assembleWithLayout(
      sketch, component,
      ParameterLayout::build(sketch, component.geometryIds, ignored),
      ignored);
}

std::optional<EquationSystem> ConstraintEquationRegistry::assemble(
    const Sketch& sketch, const ConstraintGraphComponent& component,
    std::span<const double> candidate) {
  const auto layout = ParameterLayout::build(sketch, component.geometryIds)
                          .withVariableValues(candidate);
  if (!layout) return std::nullopt;
  return assembleWithLayout(sketch, component, std::move(*layout));
}

std::optional<EquationSystem> ConstraintEquationRegistry::assemble(
    const Sketch& sketch, const ConstraintGraphComponent& component,
    std::span<const double> candidate,
    std::span<const ConstraintId> ignoredConstraintIds) {
  std::vector<ConstraintId> ignored(ignoredConstraintIds.begin(),
                                    ignoredConstraintIds.end());
  std::sort(ignored.begin(), ignored.end());
  ignored.erase(std::unique(ignored.begin(), ignored.end()), ignored.end());
  const auto layout = ParameterLayout::build(
                          sketch, component.geometryIds, ignored)
                          .withVariableValues(candidate);
  if (!layout) return std::nullopt;
  return assembleWithLayout(sketch, component, std::move(*layout), ignored);
}

const ConstraintEquationRule& ConstraintEquationRegistry::rule(
    ConstraintType type) noexcept {
  const auto found = std::find_if(kRules.begin(), kRules.end(),
                                  [type](const auto& item) {
                                    return item.type == type;
                                  });
  return found == kRules.end() ? kRules.front() : *found;
}

std::span<const ConstraintEquationRule> ConstraintEquationRegistry::rules()
    noexcept {
  return kRules;
}

EquationDiagnosticSummary EquationDiagnostics::analyze(
    const EquationSystem& system, bool computeRank) {
  EquationDiagnosticSummary result;
  result.status = system.status();
  for (const auto& row : system.rows())
    result.maxNormalizedResidual =
        std::max(result.maxNormalizedResidual,
                 std::abs(row.residual) / std::max(row.tolerance, 1e-15));
  std::vector<std::size_t> variableColumns;
  for (std::size_t column = 0; column < system.layout().parameters().size();
       ++column)
    if (!system.layout().parameters()[column].fixed)
      variableColumns.push_back(column);
  if (computeRank && !system.rows().empty() && !variableColumns.empty()) {
    std::vector<double> variableJacobian(system.rows().size() *
                                         variableColumns.size());
    for (std::size_t row = 0; row < system.rows().size(); ++row)
      for (std::size_t column = 0; column < variableColumns.size(); ++column)
        variableJacobian[row * variableColumns.size() + column] =
            system.jacobian()[row * system.parameterCount() +
                              variableColumns[column]];
    result.equationRank = matrixRank(std::move(variableJacobian),
                                     system.rows().size(),
                                     variableColumns.size());
  }
  result.degreesOfFreedom = system.variableCount() > result.equationRank
                                ? system.variableCount() - result.equationRank
                                : 0;
  return result;
}

}  // namespace solidar::sketch

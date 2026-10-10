#include <array>
#include "ui/SketchCanvas.h"
#include "model/TopologyReferenceResolver.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"
#include "ui/ThemeManager.h"

#include <QKeySequence>
#include <QKeyEvent>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSignalBlocker>
#include <QToolTip>
#include <QTimer>
#include <QVariant>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace solidar {
namespace {
SketchHitPoint hitPoint(QPointF point) noexcept {
  return {point.x(), point.y()};
}

SketchScreenSegment hitSegment(QPointF first, QPointF second) noexcept {
  return {hitPoint(first), hitPoint(second)};
}

SketchInteractionTool interactionTool(SketchCanvas::Tool tool) noexcept {
  using CanvasTool = SketchCanvas::Tool;
  switch (tool) {
    case CanvasTool::Select: return SketchInteractionTool::Select;
    case CanvasTool::Line: return SketchInteractionTool::Line;
    case CanvasTool::Rectangle: return SketchInteractionTool::Rectangle;
    case CanvasTool::Circle: return SketchInteractionTool::Circle;
    case CanvasTool::Arc: return SketchInteractionTool::Arc;
    case CanvasTool::Projection: return SketchInteractionTool::Projection;
    case CanvasTool::AutoDimension:
      return SketchInteractionTool::AutoDimension;
    case CanvasTool::LockConstraint:
      return SketchInteractionTool::LockConstraint;
    case CanvasTool::OrthogonalConstraint:
      return SketchInteractionTool::OrthogonalConstraint;
    case CanvasTool::CoincidentConstraint:
      return SketchInteractionTool::CoincidentConstraint;
    case CanvasTool::PerpendicularConstraint:
      return SketchInteractionTool::PerpendicularConstraint;
    case CanvasTool::ParallelConstraint:
      return SketchInteractionTool::ParallelConstraint;
    case CanvasTool::EqualConstraint:
      return SketchInteractionTool::EqualConstraint;
    case CanvasTool::TangentConstraint:
      return SketchInteractionTool::TangentConstraint;
    case CanvasTool::Mirror: return SketchInteractionTool::Mirror;
    case CanvasTool::Trim: return SketchInteractionTool::Trim;
  }
  return SketchInteractionTool::Select;
}

SketchCanvas::Tool canvasTool(SketchInteractionTool tool) noexcept {
  using CanvasTool = SketchCanvas::Tool;
  switch (tool) {
    case SketchInteractionTool::Select: return CanvasTool::Select;
    case SketchInteractionTool::Line: return CanvasTool::Line;
    case SketchInteractionTool::Rectangle: return CanvasTool::Rectangle;
    case SketchInteractionTool::Circle: return CanvasTool::Circle;
    case SketchInteractionTool::Arc: return CanvasTool::Arc;
    case SketchInteractionTool::Projection: return CanvasTool::Projection;
    case SketchInteractionTool::AutoDimension: return CanvasTool::AutoDimension;
    case SketchInteractionTool::LockConstraint: return CanvasTool::LockConstraint;
    case SketchInteractionTool::OrthogonalConstraint:
      return CanvasTool::OrthogonalConstraint;
    case SketchInteractionTool::CoincidentConstraint:
      return CanvasTool::CoincidentConstraint;
    case SketchInteractionTool::PerpendicularConstraint:
      return CanvasTool::PerpendicularConstraint;
    case SketchInteractionTool::ParallelConstraint:
      return CanvasTool::ParallelConstraint;
    case SketchInteractionTool::EqualConstraint: return CanvasTool::EqualConstraint;
    case SketchInteractionTool::TangentConstraint:
      return CanvasTool::TangentConstraint;
    case SketchInteractionTool::Mirror: return CanvasTool::Mirror;
    case SketchInteractionTool::Trim: return CanvasTool::Trim;
  }
  return CanvasTool::Select;
}

QString pointDimensionModeName(SketchPointDimensionMode mode) {
  switch (mode) {
    case SketchPointDimensionMode::X: return QStringLiteral("x");
    case SketchPointDimensionMode::Y: return QStringLiteral("y");
    case SketchPointDimensionMode::Aligned: return QStringLiteral("aligned");
  }
  return QStringLiteral("aligned");
}

SketchPointDimensionMode pointDimensionMode(const QString& name) noexcept {
  if (name == QStringLiteral("x")) return SketchPointDimensionMode::X;
  if (name == QStringLiteral("y")) return SketchPointDimensionMode::Y;
  return SketchPointDimensionMode::Aligned;
}

sketch::PointReference& ensurePointReference(
    std::optional<sketch::PointReference>& reference) {
  if (!reference) reference.emplace();
  return *reference;
}

struct SketchClipboardElement {
  enum class Kind { Line, Rectangle, Circle };

  Kind kind{Kind::Line};
  sketch::Point lineStart{};
  sketch::Point lineEnd{};
  std::array<sketch::Point, 4> rectanglePoints{};
  sketch::Point circleCenter{};
  double circleRadiusMm{0.0};
};

struct SketchClipboardData {
  std::vector<SketchClipboardElement> elements;
  int pasteGeneration{0};

  [[nodiscard]] bool empty() const noexcept { return elements.empty(); }

  void clear() {
    elements.clear();
    pasteGeneration = 0;
  }
};

SketchClipboardData gSketchClipboard;


constexpr double kRulerTop = 30.0;
constexpr double kRulerLeft = 44.0;

Point3d cubeViewingDirection(CameraOrientation camera) {
  const double yaw = camera.yaw * std::numbers::pi / 180.0;
  const double pitch = camera.pitch * std::numbers::pi / 180.0;
  return {std::sin(yaw) * std::sin(pitch),
          std::cos(yaw) * std::sin(pitch), std::cos(pitch)};
}

Point3d sketchViewingDirection(double yawDeg, double pitchDeg) {
  const double yaw = yawDeg * std::numbers::pi / 180.0;
  const double pitch = pitchDeg * std::numbers::pi / 180.0;
  // The sketch projection rotates the scene, while ViewCube describes the
  // camera.  Their vertical motion has opposite signs: when the visible model
  // tilts upward, the cube must tilt upward as well instead of mirroring it.
  return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
          std::cos(yaw) * std::cos(pitch)};
}

Point3d localDirectionToWorld(const SketchPlacement& placement,
                              Point3d local) {
  const Vector3d normal = placement.normal();
  return {
      placement.xDirection.x * local.x +
          placement.yDirection.x * local.y + normal.x * local.z,
      placement.xDirection.y * local.x +
          placement.yDirection.y * local.y + normal.y * local.z,
      placement.xDirection.z * local.x +
          placement.yDirection.z * local.y + normal.z * local.z};
}

Point3d worldDirectionToLocal(const SketchPlacement& placement,
                              Point3d world) {
  const Vector3d normal = placement.normal();
  const auto component = [world](Vector3d axis) {
    const double lengthSquared = axis.x * axis.x + axis.y * axis.y +
                                 axis.z * axis.z;
    return lengthSquared > 1e-12
               ? (world.x * axis.x + world.y * axis.y +
                  world.z * axis.z) /
                     std::sqrt(lengthSquared)
               : 0.0;
  };
  return {component(placement.xDirection),
          component(placement.yDirection), component(normal)};
}

double initialViewRotation(const SketchPlacement& placement,
                           Vector3d preferredUp) {
  const Vector3d normal = placement.normal();
  const auto vectorLength = [](Vector3d value) {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
  };
  // MainWindow supplies the current 3D camera's screen-up vector so entering
  // Sketcher preserves what the user saw as the top of the part.  Standalone
  // canvases use a stable world-axis fallback.  The resulting roll is snapped
  // to a quarter turn: Sketcher must open with horizontal/vertical axes, while
  // still choosing the canonical orientation closest to the 3D view.
  if (vectorLength(preferredUp) <= 1e-12)
    preferredUp = std::abs(normal.z) > 0.9
                      ? Vector3d{0.0, 1.0, 0.0}
                      : Vector3d{0.0, 0.0, 1.0};
  const auto component = [preferredUp, &vectorLength](Vector3d axis) {
    const double length = vectorLength(axis);
    return length > 1e-12
               ? (preferredUp.x * axis.x + preferredUp.y * axis.y +
                  preferredUp.z * axis.z) /
                     length
               : 0.0;
  };
  const double localX = component(placement.xDirection);
  const double localY = component(placement.yDirection);
  if (std::hypot(localX, localY) <= 1e-9) {
    const Vector3d fallbackUp = std::abs(normal.z) > 0.9
                                    ? Vector3d{0.0, 1.0, 0.0}
                                    : Vector3d{0.0, 0.0, 1.0};
    const auto fallbackComponent = [fallbackUp, &vectorLength](Vector3d axis) {
      const double length = vectorLength(axis);
      return length > 1e-12
                 ? (fallbackUp.x * axis.x + fallbackUp.y * axis.y +
                    fallbackUp.z * axis.z) /
                       length
                 : 0.0;
    };
    const double angle =
        std::atan2(-fallbackComponent(placement.xDirection),
                   fallbackComponent(placement.yDirection)) *
        180.0 / std::numbers::pi;
    return std::remainder(std::round(angle / 90.0) * 90.0, 360.0);
  }
  const double angle =
      std::atan2(-localX, localY) * 180.0 / std::numbers::pi;
  return std::remainder(std::round(angle / 90.0) * 90.0, 360.0);
}

CameraOrientation sketchOrientationForDirection(Point3d direction) {
  const double length = std::sqrt(direction.x * direction.x +
                                  direction.y * direction.y +
                                  direction.z * direction.z);
  if (length <= 1e-12) return {};
  const double x = direction.x / length;
  const double y = std::clamp(direction.y / length, -1.0, 1.0);
  const double z = direction.z / length;
  return {static_cast<float>(std::atan2(-x, z) * 180.0 /
                                        std::numbers::pi),
          static_cast<float>(std::clamp(
              -std::asin(y) * 180.0 / std::numbers::pi, -89.9, 89.9))};
}

double niceRulerStep(double pixelsPerMm) {
  const double targetMm = 75.0 / std::max(0.01, pixelsPerMm);
  const double magnitude = std::pow(10.0, std::floor(std::log10(targetMm)));
  const double normalized = targetMm / magnitude;
  const double factor = normalized <= 1.0 ? 1.0 :
                        normalized <= 2.0 ? 2.0 :
                        normalized <= 5.0 ? 5.0 : 10.0;
  return factor * magnitude;
}

double lineAngleDegrees(const sketch::Line& first,
                        const sketch::Line& second) {
  const auto samePoint = [](sketch::Point a, sketch::Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };

  double ax = first.end.xMm - first.start.xMm;
  double ay = first.end.yMm - first.start.yMm;
  double bx = second.end.xMm - second.start.xMm;
  double by = second.end.yMm - second.start.yMm;

  // For connected segments, an angular dimension must describe the two
  // visible rays leaving their common CAD vertex, regardless of each line's
  // internal start/end order.
  if (samePoint(first.start, second.start)) {
    ax = first.end.xMm - first.start.xMm;
    ay = first.end.yMm - first.start.yMm;
    bx = second.end.xMm - second.start.xMm;
    by = second.end.yMm - second.start.yMm;
  } else if (samePoint(first.start, second.end)) {
    ax = first.end.xMm - first.start.xMm;
    ay = first.end.yMm - first.start.yMm;
    bx = second.start.xMm - second.end.xMm;
    by = second.start.yMm - second.end.yMm;
  } else if (samePoint(first.end, second.start)) {
    ax = first.start.xMm - first.end.xMm;
    ay = first.start.yMm - first.end.yMm;
    bx = second.end.xMm - second.start.xMm;
    by = second.end.yMm - second.start.yMm;
  } else if (samePoint(first.end, second.end)) {
    ax = first.start.xMm - first.end.xMm;
    ay = first.start.yMm - first.end.yMm;
    bx = second.start.xMm - second.end.xMm;
    by = second.start.yMm - second.end.yMm;
  }

  const double al = std::hypot(ax, ay);
  const double bl = std::hypot(bx, by);
  if (al <= 1e-9 || bl <= 1e-9) return 0.0;

  const double cosine =
      std::clamp((ax * bx + ay * by) / (al * bl), -1.0, 1.0);
  return std::acos(cosine) * 180.0 / 3.14159265358979323846;
}

template <typename MapPointFn>
std::optional<QPointF> lineIntersectionScreen(const sketch::Line& first,
                                              const sketch::Line& second,
                                              MapPointFn&& mapPointFn) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double determinant = ax * by - ay * bx;
  if (std::abs(determinant) <= 1e-9) return std::nullopt;

  const double dx = second.start.xMm - first.start.xMm;
  const double dy = second.start.yMm - first.start.yMm;
  const double t = (dx * by - dy * bx) / determinant;

  return mapPointFn(sketch::Point{
      first.start.xMm + t * ax,
      first.start.yMm + t * ay});
}
// ANGLE DIMENSION RAY TOWARD SEGMENT
QPointF angleRayTowardSegment(QPointF intersection,
                              QPointF start,
                              QPointF end) {
  const QPointF toStart = start - intersection;
  const QPointF toEnd = end - intersection;

  const double startLength =
      std::hypot(toStart.x(), toStart.y());
  const double endLength =
      std::hypot(toEnd.x(), toEnd.y());

  constexpr double epsilon = 1e-6;

  if (startLength <= epsilon && endLength <= epsilon)
    return {};
  if (startLength <= epsilon)
    return toEnd;
  if (endLength <= epsilon)
    return toStart;

  const double dot = QPointF::dotProduct(toStart, toEnd);

  if (dot >= 0.0)
    return startLength <= endLength ? toStart : toEnd;

  return startLength >= endLength ? toStart : toEnd;
}

// VISIBLE ANGLE BETWEEN FINITE SEGMENTS
double visibleLineAngleDegrees(const sketch::Line& first,
                               const sketch::Line& second) {
  const auto samePoint = [](sketch::Point a, sketch::Point b) {
    return std::hypot(a.xMm - b.xMm,
                      a.yMm - b.yMm) <= 1e-7;
  };

  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;

  const double determinant = ax * by - ay * bx;
  if (std::abs(determinant) <= 1e-9)
    return 0.0;

  const double dx = second.start.xMm - first.start.xMm;
  const double dy = second.start.yMm - first.start.yMm;
  const double t = (dx * by - dy * bx) / determinant;

  const sketch::Point intersection{
      first.start.xMm + t * ax,
      first.start.yMm + t * ay};

  const auto rayTowardSegment =
      [intersection](sketch::Point start,
                     sketch::Point end) {
        QPointF toStart(start.xMm - intersection.xMm,
                        start.yMm - intersection.yMm);
        QPointF toEnd(end.xMm - intersection.xMm,
                      end.yMm - intersection.yMm);

        const double startLength =
            std::hypot(toStart.x(), toStart.y());
        const double endLength =
            std::hypot(toEnd.x(), toEnd.y());

        constexpr double epsilon = 1e-9;

        if (startLength <= epsilon)
          return toEnd;
        if (endLength <= epsilon)
          return toStart;

        const double dot =
            QPointF::dotProduct(toStart, toEnd);

        if (dot >= 0.0)
          return startLength <= endLength ? toStart : toEnd;

        return startLength >= endLength ? toStart : toEnd;
      };

  QPointF firstDirection =
      rayTowardSegment(first.start, first.end);
  QPointF secondDirection =
      rayTowardSegment(second.start, second.end);

  if (samePoint(first.start, second.start)) {
    firstDirection =
        QPointF(first.end.xMm - first.start.xMm,
                first.end.yMm - first.start.yMm);
    secondDirection =
        QPointF(second.end.xMm - second.start.xMm,
                second.end.yMm - second.start.yMm);
  } else if (samePoint(first.start, second.end)) {
    firstDirection =
        QPointF(first.end.xMm - first.start.xMm,
                first.end.yMm - first.start.yMm);
    secondDirection =
        QPointF(second.start.xMm - second.end.xMm,
                second.start.yMm - second.end.yMm);
  } else if (samePoint(first.end, second.start)) {
    firstDirection =
        QPointF(first.start.xMm - first.end.xMm,
                first.start.yMm - first.end.yMm);
    secondDirection =
        QPointF(second.end.xMm - second.start.xMm,
                second.end.yMm - second.start.yMm);
  } else if (samePoint(first.end, second.end)) {
    firstDirection =
        QPointF(first.start.xMm - first.end.xMm,
                first.start.yMm - first.end.yMm);
    secondDirection =
        QPointF(second.start.xMm - second.end.xMm,
                second.start.yMm - second.end.yMm);
  }

  const double firstLength =
      std::hypot(firstDirection.x(), firstDirection.y());
  const double secondLength =
      std::hypot(secondDirection.x(), secondDirection.y());

  if (firstLength <= 1e-9 || secondLength <= 1e-9)
    return 0.0;

  const double cosine =
      std::clamp(
          QPointF::dotProduct(firstDirection, secondDirection) /
              (firstLength * secondLength),
          -1.0,
          1.0);

  return std::acos(cosine) *
         180.0 / 3.14159265358979323846;
}

// ANGLE SECTOR RAY DIRECTIONS
//
// Returns the two unit ray directions (screen space) that define the angle
// sector between two segments, oriented away from their shared vertex along
// each finite segment. Returns nullopt when the segments are degenerate or
// share no screen vertex. A negative offsetMm flips both rays so the angle
// arc is drawn in the opposite vertical sector (the side the user placed it).
template <typename MapPointFn>
std::optional<std::pair<QPointF, QPointF>> angleSectorRays(
    const sketch::Line& firstLine,
    const sketch::Line& secondLine,
    QPointF center,
    double offsetMm,
    MapPointFn&& mapPointFn) {
  QPointF firstDirection = angleRayTowardSegment(
      center, mapPointFn(firstLine.start), mapPointFn(firstLine.end));
  QPointF secondDirection = angleRayTowardSegment(
      center, mapPointFn(secondLine.start), mapPointFn(secondLine.end));

  const auto sameScreenVertex = [&mapPointFn](sketch::Point a,
                                              sketch::Point b) {
    return QLineF(mapPointFn(a), mapPointFn(b)).length() <= 0.5;
  };

  if (sameScreenVertex(firstLine.start, secondLine.start)) {
    firstDirection =
        mapPointFn(firstLine.end) - mapPointFn(firstLine.start);
    secondDirection =
        mapPointFn(secondLine.end) - mapPointFn(secondLine.start);
  } else if (sameScreenVertex(firstLine.start, secondLine.end)) {
    firstDirection =
        mapPointFn(firstLine.end) - mapPointFn(firstLine.start);
    secondDirection =
        mapPointFn(secondLine.start) - mapPointFn(secondLine.end);
  } else if (sameScreenVertex(firstLine.end, secondLine.start)) {
    firstDirection =
        mapPointFn(firstLine.start) - mapPointFn(firstLine.end);
    secondDirection =
        mapPointFn(secondLine.end) - mapPointFn(secondLine.start);
  } else if (sameScreenVertex(firstLine.end, secondLine.end)) {
    firstDirection =
        mapPointFn(firstLine.start) - mapPointFn(firstLine.end);
    secondDirection =
        mapPointFn(secondLine.start) - mapPointFn(secondLine.end);
  }

  const double firstLength =
      std::hypot(firstDirection.x(), firstDirection.y());
  const double secondLength =
      std::hypot(secondDirection.x(), secondDirection.y());
  if (firstLength <= 1.0 || secondLength <= 1.0)
    return std::nullopt;

  firstDirection /= firstLength;
  secondDirection /= secondLength;

  if (offsetMm < 0.0) {
    firstDirection = -firstDirection;
    secondDirection = -secondDirection;
  }

  return std::pair{firstDirection, secondDirection};
}

std::optional<QPointF> nearestSegmentEndpointTo(
    QPointF point, QPointF start, QPointF end) {
  const double firstDistance = QLineF(point, start).length();
  const double secondDistance = QLineF(point, end).length();

  if (firstDistance <= 0.5 || secondDistance <= 0.5)
    return std::nullopt;

  return firstDistance <= secondDistance
             ? std::optional<QPointF>{start}
             : std::optional<QPointF>{end};
}
double pointSegmentDistance(QPointF point, QPointF start, QPointF end) {
  const QPointF segment = end - start;
  const double lengthSquared = QPointF::dotProduct(segment, segment);
  if (lengthSquared == 0.0) return QLineF(point, start).length();
  const double t = std::clamp(
      QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
  return QLineF(point, start + segment * t).length();
}

constexpr double kTrimTwoPi = 6.28318530717958647692;

double normalizedAngle(double angle) {
  angle = std::fmod(angle, kTrimTwoPi);
  if (angle < 0.0) angle += kTrimTwoPi;
  return angle;
}

std::optional<double> arcParameterAtPoint(const sketch::Arc& arc,
                                          sketch::Point point,
                                          double toleranceMm = 1e-6) {
  const double dx = point.xMm - arc.center.xMm;
  const double dy = point.yMm - arc.center.yMm;
  if (std::abs(std::hypot(dx, dy) - arc.radiusMm) > toleranceMm)
    return std::nullopt;
  const double offset = normalizedAngle(std::atan2(dy, dx) -
                                        arc.startAngleRad);
  if (offset > arc.sweepAngleRad + 1e-8) return std::nullopt;
  return std::clamp(offset / arc.sweepAngleRad, 0.0, 1.0);
}

std::vector<sketch::Point> segmentCircleIntersections(
    const sketch::Line& line, sketch::Point center, double radiusMm) {
  const double dx = line.end.xMm - line.start.xMm;
  const double dy = line.end.yMm - line.start.yMm;
  const double fx = line.start.xMm - center.xMm;
  const double fy = line.start.yMm - center.yMm;
  const double a = dx * dx + dy * dy;
  if (a <= 1e-18 || radiusMm <= 0.0) return {};
  const double b = 2.0 * (fx * dx + fy * dy);
  const double c = fx * fx + fy * fy - radiusMm * radiusMm;
  const double discriminant = b * b - 4.0 * a * c;
  if (discriminant < -1e-10) return {};
  const double root = std::sqrt(std::max(0.0, discriminant));
  std::vector<sketch::Point> result;
  for (const double t : {(-b - root) / (2.0 * a),
                         (-b + root) / (2.0 * a)}) {
    if (t < -1e-8 || t > 1.0 + 1e-8) continue;
    const double clamped = std::clamp(t, 0.0, 1.0);
    const sketch::Point point{line.start.xMm + dx * clamped,
                              line.start.yMm + dy * clamped};
    if (result.empty() ||
        std::hypot(result.back().xMm - point.xMm,
                   result.back().yMm - point.yMm) > 1e-7)
      result.push_back(point);
  }
  return result;
}

std::vector<sketch::Point> circleCircleIntersections(
    sketch::Point firstCenter, double firstRadius,
    sketch::Point secondCenter, double secondRadius) {
  const double dx = secondCenter.xMm - firstCenter.xMm;
  const double dy = secondCenter.yMm - firstCenter.yMm;
  const double distance = std::hypot(dx, dy);
  if (distance <= 1e-10 ||
      distance > firstRadius + secondRadius + 1e-8 ||
      distance < std::abs(firstRadius - secondRadius) - 1e-8)
    return {};
  const double along =
      (firstRadius * firstRadius - secondRadius * secondRadius +
       distance * distance) /
      (2.0 * distance);
  const double heightSquared =
      std::max(0.0, firstRadius * firstRadius - along * along);
  const double height = std::sqrt(heightSquared);
  const double ux = dx / distance;
  const double uy = dy / distance;
  const sketch::Point base{firstCenter.xMm + along * ux,
                           firstCenter.yMm + along * uy};
  const sketch::Point first{base.xMm - uy * height,
                            base.yMm + ux * height};
  if (height <= 1e-8) return {first};
  return {first,
          {base.xMm + uy * height, base.yMm - ux * height}};
}

std::optional<sketch::Point> intersectLines(const sketch::Line& first,
                                             const sketch::Line& second) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double determinant = ax * by - ay * bx;
  if (std::abs(determinant) < 1e-9) return std::nullopt;
  const double dx = second.start.xMm - first.start.xMm;
  const double dy = second.start.yMm - first.start.yMm;
  const double t = (dx * by - dy * bx) / determinant;
  return sketch::Point{first.start.xMm + t * ax,
                       first.start.yMm + t * ay};
}

std::optional<std::pair<sketch::Point, double>> circleThroughThreePoints(
    sketch::Point first, sketch::Point second, sketch::Point third) {
  const double determinant = 2.0 *
      (first.xMm * (second.yMm - third.yMm) +
       second.xMm * (third.yMm - first.yMm) +
       third.xMm * (first.yMm - second.yMm));
  if (std::abs(determinant) < 1e-9) return std::nullopt;
  const double a = first.xMm * first.xMm + first.yMm * first.yMm;
  const double b = second.xMm * second.xMm + second.yMm * second.yMm;
  const double c = third.xMm * third.xMm + third.yMm * third.yMm;
  sketch::Point center{
      (a * (second.yMm - third.yMm) + b * (third.yMm - first.yMm) +
       c * (first.yMm - second.yMm)) / determinant,
      (a * (third.xMm - second.xMm) + b * (first.xMm - third.xMm) +
       c * (second.xMm - first.xMm)) / determinant};
  return std::pair{center, std::hypot(center.xMm - first.xMm,
                                      center.yMm - first.yMm)};
}

std::optional<sketch::Arc> arcThroughThreePoints(
    sketch::Point first, sketch::Point through, sketch::Point last) {
  const auto circle = circleThroughThreePoints(first, through, last);
  if (!circle || circle->second <= 1e-9)
    return std::nullopt;

  constexpr double kTwoPi = 6.28318530717958647692;
  const auto normalize = [](double angle) {
    constexpr double twoPi = 6.28318530717958647692;
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0) angle += twoPi;
    return angle;
  };

  const auto& center = circle->first;
  const double start = std::atan2(first.yMm - center.yMm,
                                  first.xMm - center.xMm);
  const double middle = std::atan2(through.yMm - center.yMm,
                                   through.xMm - center.xMm);
  const double end = std::atan2(last.yMm - center.yMm,
                                last.xMm - center.xMm);

  const double ccwSweep = normalize(end - start);
  const double middleFromStart = normalize(middle - start);

  if (ccwSweep > 1e-9 &&
      middleFromStart <= ccwSweep + 1e-9) {
    return sketch::Arc{center, circle->second, normalize(start), ccwSweep,
                       false};
  }

  const double reverseSweep = normalize(start - end);
  if (reverseSweep <= 1e-9 || reverseSweep >= kTwoPi - 1e-9)
    return std::nullopt;

  return sketch::Arc{center, circle->second, normalize(end), reverseSweep,
                     false};
}

sketch::Point arcSagittaPoint(sketch::Point first,
                              sketch::Point last,
                              double signedSagittaMm) {
  const double dx = last.xMm - first.xMm;
  const double dy = last.yMm - first.yMm;
  const double chord = std::hypot(dx, dy);
  if (chord <= 1e-9) return first;

  const sketch::Point middle{(first.xMm + last.xMm) * 0.5,
                             (first.yMm + last.yMm) * 0.5};
  const double nx = -dy / chord;
  const double ny = dx / chord;
  return {middle.xMm + nx * signedSagittaMm,
          middle.yMm + ny * signedSagittaMm};
}

double signedArcSagitta(sketch::Point first,
                        sketch::Point last,
                        sketch::Point point) {
  const double dx = last.xMm - first.xMm;
  const double dy = last.yMm - first.yMm;
  const double chord = std::hypot(dx, dy);
  if (chord <= 1e-9) return 0.0;

  const sketch::Point middle{(first.xMm + last.xMm) * 0.5,
                             (first.yMm + last.yMm) * 0.5};
  const double nx = -dy / chord;
  const double ny = dx / chord;
  return (point.xMm - middle.xMm) * nx +
         (point.yMm - middle.yMm) * ny;
}

std::optional<sketch::Arc> arcFromChordSagitta(
    sketch::Point first, sketch::Point last, double signedSagittaMm) {
  if (std::hypot(last.xMm - first.xMm, last.yMm - first.yMm) <= 1e-9 ||
      std::abs(signedSagittaMm) <= 1e-9)
    return std::nullopt;
  return arcThroughThreePoints(
      first, arcSagittaPoint(first, last, signedSagittaMm), last);
}

double pointLineDistance(sketch::Point point, const sketch::Line& line) {
  const QPointF p(point.xMm, point.yMm);
  return pointSegmentDistance(p, QPointF(line.start.xMm, line.start.yMm),
                              QPointF(line.end.xMm, line.end.yMm));
}

double infiniteLineDistance(sketch::Point point, const sketch::Line& line) {
  const double dx = line.end.xMm - line.start.xMm;
  const double dy = line.end.yMm - line.start.yMm;
  const double length = std::hypot(dx, dy);
  if (length < 1e-9) return std::numeric_limits<double>::max();
  return std::abs(dy * point.xMm - dx * point.yMm +
                  line.end.xMm * line.start.yMm -
                  line.end.yMm * line.start.xMm) / length;
}

struct TwoTangentCirclePreview {
  sketch::Point center;
  double radiusMm{};
};

std::optional<TwoTangentCirclePreview>
twoTangentCircleForRadius(
    const sketch::Line& first,
    const sketch::Line& second,
    double radiusMm,
    sketch::Point hint) {
  if (!std::isfinite(radiusMm) ||
      radiusMm <= 1e-6)
    return std::nullopt;

  const double firstDx =
      first.end.xMm - first.start.xMm;
  const double firstDy =
      first.end.yMm - first.start.yMm;
  const double secondDx =
      second.end.xMm - second.start.xMm;
  const double secondDy =
      second.end.yMm - second.start.yMm;

  const double firstLengthSquared =
      firstDx * firstDx + firstDy * firstDy;
  const double secondLengthSquared =
      secondDx * secondDx + secondDy * secondDy;

  if (firstLengthSquared <= 1e-12 ||
      secondLengthSquared <= 1e-12)
    return std::nullopt;

  const double firstLength =
      std::sqrt(firstLengthSquared);
  const double secondLength =
      std::sqrt(secondLengthSquared);

  const double n1x = -firstDy / firstLength;
  const double n1y = firstDx / firstLength;
  const double n2x = -secondDy / secondLength;
  const double n2y = secondDx / secondLength;

  const double determinant =
      n1x * n2y - n1y * n2x;

  if (std::abs(determinant) <= 1e-10)
    return std::nullopt;

  const double c1 =
      n1x * first.start.xMm +
      n1y * first.start.yMm;
  const double c2 =
      n2x * second.start.xMm +
      n2y * second.start.yMm;

  std::optional<TwoTangentCirclePreview> best;
  double bestMovement =
      std::numeric_limits<double>::max();

  for (const double s1 : {-1.0, 1.0}) {
    for (const double s2 : {-1.0, 1.0}) {
      const double r1 = c1 + s1 * radiusMm;
      const double r2 = c2 + s2 * radiusMm;

      const sketch::Point center{
          (r1 * n2y - n1y * r2) / determinant,
          (n1x * r2 - r1 * n2x) / determinant};

      if (!std::isfinite(center.xMm) ||
          !std::isfinite(center.yMm))
        continue;

      // TWO-TANGENT FINITE SEGMENT DIAMETER LIMIT
      const double firstT =
          ((center.xMm - first.start.xMm) * firstDx +
           (center.yMm - first.start.yMm) * firstDy) /
          firstLengthSquared;

      const double secondT =
          ((center.xMm - second.start.xMm) * secondDx +
           (center.yMm - second.start.yMm) * secondDy) /
          secondLengthSquared;

      constexpr double finiteTolerance = 1e-8;

      if (firstT < -finiteTolerance ||
          firstT > 1.0 + finiteTolerance ||
          secondT < -finiteTolerance ||
          secondT > 1.0 + finiteTolerance)
        continue;

      const double movement =
          std::hypot(
              center.xMm - hint.xMm,
              center.yMm - hint.yMm);

      if (movement < bestMovement) {
        bestMovement = movement;
        best = TwoTangentCirclePreview{
            center,
            radiusMm};
      }
    }
  }

  return best;
}

std::optional<TwoTangentCirclePreview>
clampedTwoTangentCircleForRadius(
    const sketch::Line& first,
    const sketch::Line& second,
    double requestedRadiusMm,
    sketch::Point hint) {
  const double requested =
      std::max(0.01, requestedRadiusMm);

  if (const auto exact =
          twoTangentCircleForRadius(
              first,
              second,
              requested,
              hint))
    return exact;

  double low = 0.01;
  double high = requested;

  std::optional<TwoTangentCirclePreview> best =
      twoTangentCircleForRadius(
          first,
          second,
          low,
          hint);

  if (!best)
    return std::nullopt;

  for (int iteration = 0;
       iteration < 48;
       ++iteration) {
    const double mid =
        (low + high) * 0.5;

    const auto candidate =
        twoTangentCircleForRadius(
            first,
            second,
            mid,
            hint);

    if (candidate) {
      low = mid;
      best = candidate;
    } else {
      high = mid;
    }
  }

  return best;
}
std::optional<TwoTangentCirclePreview>
twoTangentCircleFromCursor(
    const sketch::Line& first,
    const sketch::Line& second,
    sketch::Point cursor) {
  const double firstDx =
      first.end.xMm - first.start.xMm;
  const double firstDy =
      first.end.yMm - first.start.yMm;
  const double secondDx =
      second.end.xMm - second.start.xMm;
  const double secondDy =
      second.end.yMm - second.start.yMm;

  const double firstLength =
      std::hypot(firstDx, firstDy);
  const double secondLength =
      std::hypot(secondDx, secondDy);

  if (firstLength <= 1e-9 ||
      secondLength <= 1e-9)
    return std::nullopt;

  const double n1x = -firstDy / firstLength;
  const double n1y = firstDx / firstLength;
  const double n2x = -secondDy / secondLength;
  const double n2y = secondDx / secondLength;

  const double determinant =
      n1x * n2y - n1y * n2x;

  if (std::abs(determinant) <= 1e-10)
    return std::nullopt;

  const double c1 =
      n1x * first.start.xMm +
      n1y * first.start.yMm;
  const double c2 =
      n2x * second.start.xMm +
      n2y * second.start.yMm;

  const sketch::Point base{
      (c1 * n2y - n1y * c2) /
          determinant,
      (n1x * c2 - c1 * n2x) /
          determinant};

  std::optional<TwoTangentCirclePreview> best;
  double bestScore =
      std::numeric_limits<double>::max();

  for (const double s1 : {-1.0, 1.0}) {
    for (const double s2 : {-1.0, 1.0}) {
      const sketch::Point direction{
          (s1 * n2y - n1y * s2) /
              determinant,
          (n1x * s2 - s1 * n2x) /
              determinant};

      const double directionSquared =
          direction.xMm * direction.xMm +
          direction.yMm * direction.yMm;

      if (directionSquared <= 1e-12)
        continue;

      const double cursorX =
          cursor.xMm - base.xMm;
      const double cursorY =
          cursor.yMm - base.yMm;

      double radiusMm =
          (cursorX * direction.xMm +
           cursorY * direction.yMm) /
          directionSquared;

      radiusMm =
          std::max(0.01, radiusMm);

      const sketch::Point center{
          base.xMm +
              direction.xMm * radiusMm,
          base.yMm +
              direction.yMm * radiusMm};

      double score =
          std::hypot(
              center.xMm - cursor.xMm,
              center.yMm - cursor.yMm);

      const auto finitePenalty =
          [center](const sketch::Line& line) {
            const double dx =
                line.end.xMm - line.start.xMm;
            const double dy =
                line.end.yMm - line.start.yMm;
            const double lengthSquared =
                dx * dx + dy * dy;

            if (lengthSquared <= 1e-12)
              return 1000000.0;

            const double t =
                ((center.xMm -
                      line.start.xMm) *
                     dx +
                 (center.yMm -
                      line.start.yMm) *
                     dy) /
                lengthSquared;

            if (t < 0.0)
              return -t *
                     std::sqrt(lengthSquared);

            if (t > 1.0)
              return (t - 1.0) *
                     std::sqrt(lengthSquared);

            return 0.0;
          };

      score +=
          0.25 *
          (finitePenalty(first) +
           finitePenalty(second));

      if (score < bestScore) {
        bestScore = score;
        best =
            TwoTangentCirclePreview{
                center,
                radiusMm};
      }
    }
  }

  return best;
}

bool parallelLinePair(const sketch::Line& first,
                      const sketch::Line& second) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double al = std::hypot(ax, ay);
  const double bl = std::hypot(bx, by);
  return al > 1e-9 && bl > 1e-9 &&
         std::abs(ax * by - ay * bx) / (al * bl) <= 1e-6;
}

std::optional<std::pair<sketch::Point, sketch::Point>>
parallelLineDistanceWitness(const sketch::Line& first,
                            const sketch::Line& second) {
  if (!parallelLinePair(first, second)) return std::nullopt;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double length2 = bx * bx + by * by;
  if (length2 <= 1e-12) return std::nullopt;
  const sketch::Point middle{
      (first.start.xMm + first.end.xMm) * 0.5,
      (first.start.yMm + first.end.yMm) * 0.5};
  const double t =
      ((middle.xMm - second.start.xMm) * bx +
       (middle.yMm - second.start.yMm) * by) / length2;
  return std::pair{
      middle,
      sketch::Point{second.start.xMm + bx * t,
                    second.start.yMm + by * t}};
}

double parallelLineDistanceMm(const sketch::Line& first,
                              const sketch::Line& second) {
  const auto witness = parallelLineDistanceWitness(first, second);
  return witness
             ? std::hypot(witness->second.xMm - witness->first.xMm,
                          witness->second.yMm - witness->first.yMm)
             : 0.0;
}

std::size_t lineElementMemberCount(const sketch::Sketch& geometry,
                                   sketch::GeometryId id) {
  const auto index = geometry.lineIndex(id);
  if (!index) return 0;
  const auto element = geometry.lines()[*index].elementId;
  return static_cast<std::size_t>(std::count_if(
      geometry.lines().begin(), geometry.lines().end(),
      [element](const sketch::Line& line) {
        return line.elementId == element;
      }));
}

// TWO-TANGENT LIVE DIAMETER PREVIEW

}  // namespace

SketchCanvas::SketchCanvas(QWidget* parent)
    : QWidget(parent),
      referenceBodyMesh_(std::make_shared<BodyRenderMesh>()),
      referenceFaceMesh_(std::make_shared<BodyRenderMesh>()) {
  setMinimumSize(560, 380);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  viewCubeAnimation_ = new QVariantAnimation(this);
  viewCubeAnimation_->setObjectName(
      QStringLiteral("sketchViewOrientationTransition"));
  viewCubeAnimation_->setDuration(200);
  viewCubeAnimation_->setEasingCurve(QEasingCurve::InOutCubic);
  auto makeDimension = [this]() {
    auto* input = new QDoubleSpinBox(this);
    input->setRange(-100000.0, 100000.0);
    input->setDecimals(2);
    input->setSuffix(QString::fromUtf8(" мм"));
    input->setFixedWidth(122);
    // The overlay dimension editor follows the application theme via the global
    // QDoubleSpinBox rule; a local light stylesheet would leave a white field in
    // Dark mode.
    input->installEventFilter(this);
    input->hide();
    return input;
  };
  primaryDimension_ = makeDimension();
  secondaryDimension_ = makeDimension();
  primaryDimension_->setObjectName(QStringLiteral("primaryDimension"));
  secondaryDimension_->setObjectName(QStringLiteral("secondaryDimension"));
  constraintDiagnosticsTimer_ = new QTimer(this);
  constraintDiagnosticsTimer_->setSingleShot(true);
  constraintDiagnosticsTimer_->setInterval(35);
  connect(constraintDiagnosticsTimer_, &QTimer::timeout, this,
          &SketchCanvas::runConstraintDiagnostics);
}

void SketchCanvas::setRectangle(double widthMm, double heightMm) {
  pushUndoState();
  const auto result = executeCommand(SetSketchRectangleCommand{widthMm, heightMm});
  if (!result.accepted) {
    cancelPendingUndo();
    return;
  }
  static_cast<void>(finalizeUndoState());
  clearGeometrySelection();
  interaction_.cancelGesture();
  markCommittedRenderSceneDirty();
  update();
}

void SketchCanvas::setTool(Tool tool) {
  interaction_.switchTool(interactionTool(tool));
  hoveredProjectionEdge_.reset();
  constructionHover_.reset();
  hideDimensionEditor();
  setCursor(tool == Tool::Select
                ? Qt::ArrowCursor
                : tool == Tool::Projection
                      ? Qt::PointingHandCursor
                      : Qt::CrossCursor);
  if (tool == Tool::Projection)
    emit selectionChanged(
        QString::fromUtf8("Проекция: выберите ребро существующей геометрии"));
  else if (tool == Tool::Mirror)
    emit selectionChanged(QString::fromUtf8(
        "Зеркало: один клик — объект, двойной — замкнутый контур"));
  else if (tool == Tool::Trim)
    emit selectionChanged(QString::fromUtf8(
        "Ножницы: щёлкните по подсвеченному участку"));
  emit toolChanged(tool);
  update();
}

void SketchCanvas::setRectangleMode(RectangleMode mode) {
  rectangleMode_ = mode;
  interaction_.cancelGesture();
  hideDimensionEditor();
  update();
}

void SketchCanvas::setCircleMode(CircleMode mode) {
  circleMode_ = mode;
  interaction_.cancelGesture();
  hideDimensionEditor();
  update();
}

void SketchCanvas::setCircleDiameter(double diameterMm) {
  circleDiameterMm_ = std::max(0.01, diameterMm);
  if (tool() == Tool::Circle && circleMode_ == CircleMode::CenterRadius &&
      interactionState().creation.anchor && primaryDimension_->isVisible())
    setPrimaryDimension(circleDiameterMm_);
}

SketchCanvas::Tool SketchCanvas::tool() const noexcept {
  return canvasTool(interaction_.tool());
}
const sketch::Sketch& SketchCanvas::sketch() const noexcept { return sketch_; }

const SketchToolState& SketchCanvas::interactionState() const noexcept {
  return interaction_.snapshot();
}

bool SketchCanvas::hasActiveInteraction() const noexcept {
  return interaction_.hasActiveGesture();
}

void SketchCanvas::selectDimension(std::size_t index) noexcept {
  interaction_.selectDimension(dimensionReference(index));
}

std::vector<SketchCanvas::ConstraintPanelEntry>
SketchCanvas::selectedConstraintPanelEntries() const {
  sketch::GeometryId selectedId = sketch::kInvalidGeometryId;
  if (selectionKind_ == SelectionKind::Line)
    selectedId = selectionLineId_;
  else if (selectionKind_ == SelectionKind::Circle)
    selectedId = selectionCircleId_;
  else if (selectionKind_ == SelectionKind::Arc)
    selectedId = selectionArcId_;

  if (selectedId == sketch::kInvalidGeometryId) return {};

  const auto typeName = [](sketch::ConstraintType type) -> QString {
    switch (type) {
      case sketch::ConstraintType::Horizontal:
        return QString::fromUtf8("Горизонтально");
      case sketch::ConstraintType::Vertical:
        return QString::fromUtf8("Вертикально");
      case sketch::ConstraintType::Coincident:
        return QString::fromUtf8("Совпадение");
      case sketch::ConstraintType::PointOnLine:
        return QString::fromUtf8("Принадлежность");
      case sketch::ConstraintType::Distance:
        return QString::fromUtf8("Расстояние");
      case sketch::ConstraintType::DistanceX:
        return QString::fromUtf8("Расстояние X");
      case sketch::ConstraintType::DistanceY:
        return QString::fromUtf8("Расстояние Y");
      case sketch::ConstraintType::Length:
        return QString::fromUtf8("Длина");
      case sketch::ConstraintType::Radius:
        return QString::fromUtf8("Радиус");
      case sketch::ConstraintType::Diameter:
        return QString::fromUtf8("Диаметр");
      case sketch::ConstraintType::Parallel:
        return QString::fromUtf8("Параллельно");
      case sketch::ConstraintType::Perpendicular:
        return QString::fromUtf8("Перпендикулярно");
      case sketch::ConstraintType::Equal:
        return QString::fromUtf8("Равенство");
      case sketch::ConstraintType::Angle:
        return QString::fromUtf8("Угол");
      case sketch::ConstraintType::Tangent:
        return QString::fromUtf8("\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xBE");
      case sketch::ConstraintType::Lock:
        return QString::fromUtf8("Замок");
      case sketch::ConstraintType::PointOnArc:
        return QString::fromUtf8("Принадлежность дуге");
      case sketch::ConstraintType::Midpoint:
        return QString::fromUtf8("Центр");
      case sketch::ConstraintType::PointOnXAxis:
        return QString::fromUtf8("На оси X");
      case sketch::ConstraintType::PointOnYAxis:
        return QString::fromUtf8("На оси Y");
    }
    return QString::fromUtf8("Ограничение");
  };

  const auto samePoint = [](sketch::PointReference first,
                            sketch::PointReference second) {
    // CRASH-FREE 05: COMPLETE POINTREFERENCE IDENTITY
    if (first.origin || second.origin)
      return first.origin && second.origin;
    if (first.elementCenterId != 0 ||
        second.elementCenterId != 0) {
      return first.elementCenterId != 0 &&
             second.elementCenterId != 0 &&
             first.elementCenterId == second.elementCenterId;
    }

    if (first.circleId != sketch::kInvalidGeometryId ||
        second.circleId != sketch::kInvalidGeometryId) {
      return first.circleId != sketch::kInvalidGeometryId &&
             second.circleId != sketch::kInvalidGeometryId &&
             first.circleId == second.circleId;
    }

    if (first.lineId == sketch::kInvalidGeometryId ||
        second.lineId == sketch::kInvalidGeometryId)
      return false;

    return first.lineId == second.lineId &&
           first.start == second.start;
  };

  const auto samePointPair = [&samePoint](
                                 const sketch::Constraint& constraint,
                                 const sketch::Dimension& dimension) {
    const bool sameOrder =
        samePoint(constraint.firstPoint, dimension.firstPoint) &&
        samePoint(constraint.secondPoint, dimension.secondPoint);
    const bool reverseOrder =
        samePoint(constraint.firstPoint, dimension.secondPoint) &&
        samePoint(constraint.secondPoint, dimension.firstPoint);
    return sameOrder || reverseOrder;
  };

  const auto constraintMatchesDimension =
      [&samePointPair](const sketch::Constraint& constraint,
                       const sketch::Dimension& dimension) {
        switch (dimension.kind) {
          case sketch::DimensionKind::LineLength:
            return constraint.type == sketch::ConstraintType::Length &&
                   constraint.firstGeometry == dimension.geometryId;
          case sketch::DimensionKind::CircleDiameter:
            return constraint.type == sketch::ConstraintType::Diameter &&
                   constraint.firstGeometry == dimension.geometryId;
          case sketch::DimensionKind::PointDistance:
            return constraint.type == sketch::ConstraintType::Distance &&
                   samePointPair(constraint, dimension);
          case sketch::DimensionKind::PointDistanceX:
            return constraint.type == sketch::ConstraintType::DistanceX &&
                   samePointPair(constraint, dimension);
          case sketch::DimensionKind::PointDistanceY:
            return constraint.type == sketch::ConstraintType::DistanceY &&
                   samePointPair(constraint, dimension);
          case sketch::DimensionKind::LineAngle: {
            if (constraint.type != sketch::ConstraintType::Angle) return false;
            const bool sameOrder =
                constraint.firstGeometry == dimension.geometryId &&
                constraint.secondGeometry == dimension.secondPoint.lineId;
            const bool reverseOrder =
                constraint.firstGeometry == dimension.secondPoint.lineId &&
                constraint.secondGeometry == dimension.geometryId;
            return sameOrder || reverseOrder;
          }
          case sketch::DimensionKind::LineDistance:
            return constraint.type == sketch::ConstraintType::LineDistance &&
                   ((constraint.firstGeometry == dimension.geometryId &&
                     constraint.secondGeometry == dimension.secondPoint.lineId) ||
                    (constraint.firstGeometry == dimension.secondPoint.lineId &&
                     constraint.secondGeometry == dimension.geometryId));
        }
        return false;
      };

  const auto dimensionReferencesSelected =
      [selectedId](const sketch::Dimension& dimension) {
        if (dimension.kind == sketch::DimensionKind::LineLength ||
            dimension.kind == sketch::DimensionKind::CircleDiameter)
          return dimension.geometryId == selectedId;
        if (dimension.kind == sketch::DimensionKind::LineAngle ||
            dimension.kind == sketch::DimensionKind::LineDistance)
          return dimension.geometryId == selectedId ||
                 dimension.secondPoint.lineId == selectedId;

        return dimension.firstPoint.lineId == selectedId ||
               dimension.secondPoint.lineId == selectedId ||
               dimension.firstPoint.circleId == selectedId ||
               dimension.secondPoint.circleId == selectedId;
      };

  const auto currentDimensionValue =
      [this](const sketch::Dimension& dimension) -> std::optional<double> {
        if (dimension.kind == sketch::DimensionKind::CircleDiameter) {
          const auto index = sketch_.circleIndex(dimension.geometryId);
          if (!index) return std::nullopt;
          return sketch_.circles()[*index].radiusMm * 2.0;
        }

        if (dimension.kind == sketch::DimensionKind::LineAngle) {
          const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
          const auto secondIndex =
              sketch_.lineIndex(dimension.secondPoint.lineId);
          if (!firstIndex || !secondIndex) return std::nullopt;
          return lineAngleDegrees(sketch_.lines()[*firstIndex],
                                  sketch_.lines()[*secondIndex]);
        }
        if (dimension.kind == sketch::DimensionKind::LineDistance) {
          const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
          const auto secondIndex =
              sketch_.lineIndex(dimension.secondPoint.lineId);
          if (!firstIndex || !secondIndex) return std::nullopt;
          return parallelLineDistanceMm(sketch_.lines()[*firstIndex],
                                        sketch_.lines()[*secondIndex]);
        }

        sketch::Point first;
        sketch::Point second;

        if (dimension.kind == sketch::DimensionKind::LineLength) {
          const auto index = sketch_.lineIndex(dimension.geometryId);
          if (!index) return std::nullopt;
          first = sketch_.lines()[*index].start;
          second = sketch_.lines()[*index].end;
        } else {
          const auto firstPoint = sketch_.referencedPoint(dimension.firstPoint);
          const auto secondPoint =
              sketch_.referencedPoint(dimension.secondPoint);
          if (!firstPoint || !secondPoint) return std::nullopt;
          first = *firstPoint;
          second = *secondPoint;
        }

        if (dimension.kind == sketch::DimensionKind::PointDistanceX)
          return std::abs(second.xMm - first.xMm);
        if (dimension.kind == sketch::DimensionKind::PointDistanceY)
          return std::abs(second.yMm - first.yMm);

        return std::hypot(second.xMm - first.xMm, second.yMm - first.yMm);
      };

  std::vector<ConstraintPanelEntry> result;
  std::vector<sketch::ConstraintId> dimensionConstraintIds;

  for (std::size_t dimensionIndex = 0;
       dimensionIndex < sketch_.dimensions().size(); ++dimensionIndex) {
    const auto& dimension = sketch_.dimensions()[dimensionIndex];
    if (!dimensionReferencesSelected(dimension)) continue;

    sketch::ConstraintId activeConstraint = sketch::kInvalidConstraintId;
    for (const auto& constraint : sketch_.constraints()) {
      if (constraintMatchesDimension(constraint, dimension)) {
        activeConstraint = constraint.id;
        dimensionConstraintIds.push_back(constraint.id);
        break;
      }
    }

    QString name;
    switch (dimension.kind) {
      case sketch::DimensionKind::LineLength:
        name = typeName(sketch::ConstraintType::Length);
        break;
      case sketch::DimensionKind::CircleDiameter:
        name = typeName(sketch::ConstraintType::Diameter);
        break;
      case sketch::DimensionKind::PointDistance:
        name = typeName(sketch::ConstraintType::Distance);
        break;
      case sketch::DimensionKind::PointDistanceX:
        name = typeName(sketch::ConstraintType::DistanceX);
        break;
      case sketch::DimensionKind::PointDistanceY:
        name = typeName(sketch::ConstraintType::DistanceY);
        break;
      case sketch::DimensionKind::LineAngle:
        name = typeName(sketch::ConstraintType::Angle);
        break;
      case sketch::DimensionKind::LineDistance:
        name = QString::fromUtf8("Р Р°СЃСЃС‚РѕСЏРЅРёРµ");
        break;
    }

    if (const auto value = currentDimensionValue(dimension)) {
      if (dimension.kind == sketch::DimensionKind::LineAngle)
        name += QStringLiteral(": %1").arg(*value, 0, 'f', 2) + QChar(0x00B0);
      else
      name += QString::fromUtf8(": %1 мм").arg(*value, 0, 'f', 2);
    }
    ConstraintPanelEntry entry;
    entry.description = name;
    entry.constraintId = activeConstraint;
    entry.dimensionIndex = dimensionIndex;
    entry.checked = activeConstraint != sketch::kInvalidConstraintId;
    result.push_back(std::move(entry));
  }

  for (const auto& constraint : sketch_.constraints()) {
    if (std::find(dimensionConstraintIds.begin(), dimensionConstraintIds.end(),
                  constraint.id) != dimensionConstraintIds.end())
      continue;

    const bool referencesSelected =
        constraint.firstGeometry == selectedId ||
        constraint.secondGeometry == selectedId ||
        constraint.firstPoint.lineId == selectedId ||
        constraint.secondPoint.lineId == selectedId ||
        constraint.firstPoint.circleId == selectedId ||
        constraint.secondPoint.circleId == selectedId;
    if (!referencesSelected) continue;

    QString text = typeName(constraint.type);
    if ((constraint.type == sketch::ConstraintType::Distance ||
         constraint.type == sketch::ConstraintType::DistanceX ||
         constraint.type == sketch::ConstraintType::DistanceY ||
         constraint.type == sketch::ConstraintType::Length ||
         constraint.type == sketch::ConstraintType::Radius ||
         constraint.type == sketch::ConstraintType::Diameter) &&
        constraint.value > 0.0)
      text += QString::fromUtf8(": %1 мм").arg(constraint.value, 0, 'f', 2);
    else if (constraint.type == sketch::ConstraintType::Angle &&
             constraint.value != 0.0)
      text += QString::fromUtf8(": %1°").arg(constraint.value, 0, 'f', 2);

    ConstraintPanelEntry entry;
    entry.description = text;
    entry.constraintId = constraint.id;
    entry.checked = true;
    result.push_back(std::move(entry));
  }

  return result;
}

bool SketchCanvas::setDimensionDriving(std::size_t dimensionIndex,
                                       bool driving) {
  if (dimensionIndex >= sketch_.dimensions().size()) return false;
  const auto id = sketch_.dimensions()[dimensionIndex].id;
  pushUndoState();
  const auto result = executeCommand(SetDimensionDrivingCommand{id, driving});
  if (!result.accepted) {
    cancelPendingUndo();
    emit undoAvailable(canUndo());
    emit redoAvailable(canRedo());
    if (result.error == SketchCommandError::Conflict)
      emit constraintStatusChanged(QString::fromUtf8(
          "Размер не включён: более ранние зависимости имеют приоритет"));
    update();
    return false;
  }
  notifyGeometryChanged();
  update();
  return true;
}
bool SketchCanvas::removeConstraintById(sketch::ConstraintId id) {
  if (id == sketch::kInvalidConstraintId) return false;

  const auto found = std::find_if(
      sketch_.constraints().begin(), sketch_.constraints().end(),
      [id](const auto& constraint) { return constraint.id == id; });
  if (found == sketch_.constraints().end()) return false;

  pushUndoState();
  if (!executeCommand(RemoveConstraintCommand{id}).accepted) {
    cancelPendingUndo();
    return false;
  }

  emit selectionChanged(QString::fromUtf8("Ограничение удалено"));
  notifyGeometryChanged();
  update();
  return true;
}
void SketchCanvas::setReferenceBody(BoxParameters box,
                                    const SketchPlacement& placement,
                                    bool visible) {
  referenceBox_ = box;
  referencePlacement_ = placement;
  referenceBodyVisible_ = visible;
  markCommittedRenderSceneDirty();
  update();
}

void SketchCanvas::setInitialViewUp(Vector3d worldUp) noexcept {
  preferredViewUp_ = worldUp;
}

void SketchCanvas::setSketchEditContext(const SketchEditContext& context) {
  clearSketchEditContext();
  referencePlacement_ = context.placement;
  initialViewRotationDeg_ =
      initialViewRotation(referencePlacement_, preferredViewUp_);
  viewRotationDeg_ = initialViewRotationDeg_;
  markCommittedRenderSceneDirty();
  if (!context.supportShape || context.supportShape->IsNull() ||
      !context.supportTopologyIndex || !context.supportFace)
    return;
  if (!context.supportTopologyIndex->shape() ||
      context.supportTopologyIndex->shape().get() !=
          context.supportShape.get())
    return;

  auto referenceBodyMesh = std::make_shared<BodyRenderMesh>();
  referenceBodyMesh->rebuild(*context.supportShape);
  referenceBodyMesh_ = std::move(referenceBodyMesh);
  const auto resolved = context.supportTopologyIndex->resolveFace(
      context.supportFace->topology());
  if (resolved) {
    auto referenceFaceMesh = std::make_shared<BodyRenderMesh>();
    referenceFaceMesh->rebuild(*resolved.subshape);
    referenceFaceMesh_ = std::move(referenceFaceMesh);
  }
  realReferenceBodyVisible_ = !referenceBodyMesh_->triangles().empty();
  markCommittedRenderSceneDirty();
  if (!realReferenceBodyVisible_) return;

  if (resolved && context.autoProjectSupportFace && sketch_.lines().empty() &&
      sketch_.circles().empty()) {
    bool projected = false;
    for (const auto& edge : referenceFaceMesh_->edges())
      projected = appendProjectedEdge(edge, false, false) || projected;
    if (projected) {
      clearGeometrySelection();
      notifyGeometryChanged();
    }
  }

  fitReferenceGeometry();
  update();
}

void SketchCanvas::setSceneReferences(
    SketchPlacement activePlacement,
    const std::vector<ShapeFeature::ShapePtr>& bodyShapes,
    std::vector<SketchSceneReference> sketches) {
  referencePlacement_ = activePlacement;
  initialViewRotationDeg_ =
      initialViewRotation(referencePlacement_, preferredViewUp_);
  viewRotationDeg_ = initialViewRotationDeg_;
  viewYawDeg_ = 0.0;
  viewPitchDeg_ = 0.0;
  sceneBodyMeshes_.clear();
  sceneBodyMeshes_.reserve(bodyShapes.size());
  for (const auto& shape : bodyShapes) {
    if (!shape || shape->IsNull()) continue;
    BodyRenderMesh mesh;
    mesh.rebuild(*shape);
    if (!mesh.triangles().empty() || !mesh.edges().empty())
      sceneBodyMeshes_.push_back(
          std::make_shared<BodyRenderMesh>(std::move(mesh)));
  }
  sceneSketches_ = std::move(sketches);
  hoveredProjectionEdge_.reset();
  markCommittedRenderSceneDirty();
  fitReferenceGeometry();
  update();
}

void SketchCanvas::fitReferenceGeometry() {
  double minU = std::numeric_limits<double>::max();
  double minV = std::numeric_limits<double>::max();
  double maxU = std::numeric_limits<double>::lowest();
  double maxV = std::numeric_limits<double>::lowest();
  const auto includePoint = [&](Point3d point) {
    const auto local = referencePlacement_.toLocal(point);
    const Vector3d normal = referencePlacement_.normal();
    const Vector3d delta{point.x - referencePlacement_.origin.x,
                         point.y - referencePlacement_.origin.y,
                         point.z - referencePlacement_.origin.z};
    const double localZ = delta.x * normal.x + delta.y * normal.y +
                          delta.z * normal.z;
    const auto projected = projectLocalPoint(local.x, local.y, localZ);
    minU = std::min(minU, projected.xMm);
    minV = std::min(minV, projected.yMm);
    maxU = std::max(maxU, projected.xMm);
    maxV = std::max(maxV, projected.yMm);
  };
  const auto includeMesh = [&](const BodyRenderMesh& mesh) {
    for (const auto& edge : mesh.edges())
      for (const auto& point : edge.points) includePoint(point);
    if (mesh.edges().empty())
      for (const auto& triangle : mesh.triangles()) {
        includePoint(triangle.a);
        includePoint(triangle.b);
        includePoint(triangle.c);
      }
  };
  includeMesh(*referenceBodyMesh_);
  for (const auto& mesh : sceneBodyMeshes_)
    if (mesh) includeMesh(*mesh);
  for (const auto& reference : sceneSketches_) {
    const auto includeSketchPoint = [&](sketch::Point point) {
      includePoint(reference.placement.toWorld(point.xMm, point.yMm));
    };
    for (const auto& line : reference.geometry.lines()) {
      includeSketchPoint(line.start);
      includeSketchPoint(line.end);
    }
    for (const auto& circle : reference.geometry.circles()) {
      includeSketchPoint({circle.center.xMm - circle.radiusMm,
                          circle.center.yMm - circle.radiusMm});
      includeSketchPoint({circle.center.xMm + circle.radiusMm,
                          circle.center.yMm + circle.radiusMm});
    }
    for (const auto& arc : reference.geometry.arcs()) {
      for (int step = 0; step <= 24; ++step) {
        const double angle = arc.startAngleRad +
                             arc.sweepAngleRad * step / 24.0;
        includeSketchPoint(
            {arc.center.xMm + arc.radiusMm * std::cos(angle),
             arc.center.yMm + arc.radiusMm * std::sin(angle)});
      }
    }
  }
  if (minU <= maxU && minV <= maxV) {
    const double spanU = std::max(1.0, maxU - minU);
    const double spanV = std::max(1.0, maxV - minV);
    const double availableWidth = std::max(100.0, width() - kRulerLeft - 50.0);
    const double availableHeight = std::max(100.0, height() - kRulerTop - 50.0);
    pixelsPerMm_ = std::clamp(0.82 * std::min(availableWidth / spanU,
                                             availableHeight / spanV),
                              0.05, 50.0);
    interaction_.setCameraPan(-(minU + maxU) * 0.5 * pixelsPerMm_,
                              (minV + maxV) * 0.5 * pixelsPerMm_);
  }
}

void SketchCanvas::clearSketchEditContext() {
  if (viewCubeAnimation_) viewCubeAnimation_->stop();
  cubePressed_ = {};
  cubeHover_ = {};
  QToolTip::hideText();
  referenceBodyMesh_ = std::make_shared<BodyRenderMesh>();
  referenceFaceMesh_ = std::make_shared<BodyRenderMesh>();
  sceneBodyMeshes_.clear();
  sceneSketches_.clear();
  realReferenceBodyVisible_ = false;
  hoveredProjectionEdge_.reset();
  initialViewRotationDeg_ = 0.0;
  viewRotationDeg_ = 0.0;
  viewYawDeg_ = 0.0;
  viewPitchDeg_ = 0.0;
  interaction_.cancelGesture();
  markCommittedRenderSceneDirty();
  update();
}

void SketchCanvas::setReferenceProfile(const sketch::Sketch& profile,
                                       bool visible) {
  referenceProfile_ = profile;
  referenceProfileVisible_ = visible;
  markCommittedRenderSceneDirty();
  update();
}

void SketchCanvas::clearSketch() {
  if (sketch_.lines().empty() && sketch_.circles().empty() &&
      sketch_.arcs().empty()) {
    interaction_.cancelGesture();
    hideDimensionEditor();
    return;
  }
  pushUndoState();
  const auto result = executeCommand(ClearSketchCommand{});
  if (!result.accepted) {
    cancelPendingUndo();
    return;
  }
  interaction_.clearDimensionLabels();
  clearGeometrySelection();
  emit lineStyleSelectionChanged(false, false);
  interaction_.cancelGesture();
  notifyGeometryChanged();
}

void SketchCanvas::resetSketch() {
  if (viewCubeAnimation_) viewCubeAnimation_->stop();
  cubePressed_ = {};
  cubeHover_ = {};
  QToolTip::hideText();
  cancelPendingUndo();
  if (!commandController_.invalidateTransactions(sketch_, sketchGeneration_))
    return;
  pendingUndoTransaction_.reset();
  commandSequenceFailed_ = false;
  if (!executeCommand(ClearSketchCommand{}).accepted) return;
  if (sketchGeneration_ == std::numeric_limits<std::uint64_t>::max())
    sketchGeneration_ = 1;
  else
    ++sketchGeneration_;
  interaction_.resetForProject();
  undoStack_.clear();
  redoStack_.clear();
  pendingUndoTransaction_.reset();
  undoRetainedBytes_ = 0;
  redoRetainedBytes_ = 0;
  clearGeometrySelection();
  emit lineStyleSelectionChanged(false, false);
  preferredViewUp_ = {};
  initialViewRotationDeg_ = 0.0;
  viewRotationDeg_ = 0.0;
  viewYawDeg_ = 0.0;
  viewPitchDeg_ = 0.0;
  hideDimensionEditor();
  setCursor(Qt::ArrowCursor);
  emit toolChanged(Tool::Select);
  emit undoAvailable(false);
  emit redoAvailable(false);
  notifyGeometryChanged();
}

void SketchCanvas::loadSketch(const sketch::Sketch& sketch) {
  if (viewCubeAnimation_) viewCubeAnimation_->stop();
  cubePressed_ = {};
  cubeHover_ = {};
  QToolTip::hideText();
  hideDimensionEditor();
  cancelPendingUndo();
  if (!commandController_.invalidateTransactions(sketch_, sketchGeneration_))
    return;
  pendingUndoTransaction_.reset();
  commandSequenceFailed_ = false;
  const auto replacementGeneration =
      sketchGeneration_ == std::numeric_limits<std::uint64_t>::max()
          ? std::uint64_t{1}
          : sketchGeneration_ + 1;
  if (!commandController_.replaceSketch(sketch_, sketch, sketchGeneration_,
                                        replacementGeneration))
    return;
  sketchGeneration_ = replacementGeneration;
  interaction_.resetForProject();
  undoStack_.clear();
  redoStack_.clear();
  pendingUndoTransaction_.reset();
  undoRetainedBytes_ = 0;
  redoRetainedBytes_ = 0;
  clearGeometrySelection();
  viewRotationDeg_ = initialViewRotationDeg_;
  viewYawDeg_ = 0.0;
  viewPitchDeg_ = 0.0;
  setCursor(Qt::ArrowCursor);
  emit toolChanged(Tool::Select);
  notifyGeometryChanged();
  emit undoAvailable(false);
  emit redoAvailable(false);
  update();
}

bool SketchCanvas::canUndo() const noexcept { return !undoStack_.empty(); }
bool SketchCanvas::canRedo() const noexcept { return !redoStack_.empty(); }
std::size_t SketchCanvas::undoHistorySize() const noexcept {
  return undoStack_.size();
}
std::size_t SketchCanvas::undoHistoryRetainedBytes() const noexcept {
  return undoRetainedBytes_;
}

std::size_t SketchCanvas::committedRenderSceneBuildCount() const noexcept {
  return renderSceneCache_.buildCount();
}

bool SketchCanvas::hasRealReferenceBody() const noexcept {
  return realReferenceBodyVisible_;
}

std::size_t SketchCanvas::referenceFaceEdgeCount() const noexcept {
  return referenceFaceMesh_ ? referenceFaceMesh_->edges().size() : 0;
}

std::size_t SketchCanvas::referenceBodyEdgeCount() const noexcept {
  std::size_t count =
      referenceBodyMesh_ ? referenceBodyMesh_->edges().size() : 0;
  for (const auto& mesh : sceneBodyMeshes_)
    if (mesh) count += mesh->edges().size();
  return count;
}

std::size_t SketchCanvas::sceneBodyCount() const noexcept {
  return sceneBodyMeshes_.size() + (realReferenceBodyVisible_ ? 1U : 0U);
}

std::size_t SketchCanvas::sceneSketchCount() const noexcept {
  return sceneSketches_.size();
}

void SketchCanvas::undo() {
  static_cast<void>(finalizeUndoState());
  if (undoStack_.empty()) return;
  const auto delta = undoStack_.back();
  if (!executeCommand(ApplySketchDeltaCommand{&delta, false}).accepted) return;
  redoStack_.push_back(delta);
  interaction_.clearDimensionLabels();
  undoStack_.pop_back();
  undoRetainedBytes_ = 0;
  for (const auto& state : undoStack_) undoRetainedBytes_ += state.retainedBytes;
  redoRetainedBytes_ = 0;
  for (const auto& state : redoStack_) redoRetainedBytes_ += state.retainedBytes;
  clearGeometrySelection();
  emit lineStyleSelectionChanged(false, false);
  interaction_.cancelGesture();
  hideDimensionEditor();
  emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  emit undoAvailable(canUndo());
  emit redoAvailable(canRedo());
  notifyGeometryChanged();
}

void SketchCanvas::redo() {
  if (redoStack_.empty()) return;
  const auto delta = redoStack_.back();
  if (!executeCommand(ApplySketchDeltaCommand{&delta, true}).accepted) return;
  undoStack_.push_back(delta);
  redoStack_.pop_back();
  undoRetainedBytes_ = 0;
  for (const auto& state : undoStack_) undoRetainedBytes_ += state.retainedBytes;
  redoRetainedBytes_ = 0;
  for (const auto& state : redoStack_) redoRetainedBytes_ += state.retainedBytes;
  interaction_.clearDimensionLabels();
  clearGeometrySelection();
  emit lineStyleSelectionChanged(false, false);
  interaction_.cancelGesture();
  hideDimensionEditor();
  emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  emit undoAvailable(canUndo());
  emit redoAvailable(canRedo());
  notifyGeometryChanged();
}

void SketchCanvas::deleteSelection() {
  DeleteSelectionCommand command;
  command.geometryIds = selectedLineIds_;
  command.geometryIds.insert(command.geometryIds.end(),
                             selectedCircleIds_.begin(), selectedCircleIds_.end());
  command.geometryIds.insert(command.geometryIds.end(),
                             selectedArcIds_.begin(), selectedArcIds_.end());
  command.elementIds = selectedElementIds_;
  if (command.geometryIds.empty() && command.elementIds.empty()) {
    if (selectionKind_ == SelectionKind::Line)
      command.geometryIds.push_back(selectionLineId_);
    else if (selectionKind_ == SelectionKind::Circle)
      command.geometryIds.push_back(selectionCircleId_);
    else if (selectionKind_ == SelectionKind::Arc)
      command.geometryIds.push_back(selectionArcId_);
  }
  if (command.geometryIds.empty() && command.elementIds.empty()) return;

  pushUndoState();
  const auto result = executeCommand(command);
  if (!result.accepted) {
    cancelPendingUndo();
    return;
  }
  clearGeometrySelection();
  emit lineStyleSelectionChanged(false, false);
  emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  notifyGeometryChanged();
}
sketch::Point SketchCanvas::rotateForView(
    sketch::Point point) const noexcept {
  const double angle = viewRotationDeg_ * std::numbers::pi / 180.0;
  const double cosine = std::cos(angle);
  const double sine = std::sin(angle);
  return {cosine * point.xMm + sine * point.yMm,
          -sine * point.xMm + cosine * point.yMm};
}

sketch::Point SketchCanvas::rotateFromView(
    sketch::Point point) const noexcept {
  const double angle = viewRotationDeg_ * std::numbers::pi / 180.0;
  const double cosine = std::cos(angle);
  const double sine = std::sin(angle);
  return {cosine * point.xMm - sine * point.yMm,
          sine * point.xMm + cosine * point.yMm};
}

bool SketchCanvas::screenToSketchMappingAvailable() const noexcept {
  const auto origin = projectLocalPoint(0.0, 0.0, 0.0);
  const auto xAxis = projectLocalPoint(1.0, 0.0, 0.0);
  const auto yAxis = projectLocalPoint(0.0, 1.0, 0.0);
  const double xx = xAxis.xMm - origin.xMm;
  const double xy = xAxis.yMm - origin.yMm;
  const double yx = yAxis.xMm - origin.xMm;
  const double yy = yAxis.yMm - origin.yMm;
  // In an orthographic view a plane seen exactly edge-on collapses to a line,
  // so a screen point has no unique Sketch coordinate. Every other camera
  // orientation remains editable through the same affine inverse used by
  // unmapPoint().
  return std::abs(xx * yy - xy * yx) > 1e-6;
}

QString SketchCanvas::pointDimensionModeForScreenAxis(
    bool horizontalDimensionLine, int viewQuarterTurns) {
  const int normalized = (viewQuarterTurns % 4 + 4) % 4;
  const bool odd = (normalized & 1) != 0;
  // Even turns: screen horizontal == Sketch X. Odd turns swap the axes.
  const bool sketchX = horizontalDimensionLine != odd;
  return sketchX ? QStringLiteral("x") : QStringLiteral("y");
}

QString SketchCanvas::resolvePointDimensionMode(
    bool horizontalLine, bool verticalLine,
    double deltaX, double deltaY, int viewQuarterTurns) {
  constexpr double projectionEpsilonMm = 1e-6;
  if (horizontalLine || verticalLine) {
    const QString mode = pointDimensionModeForScreenAxis(
        horizontalLine, viewQuarterTurns);
    const bool sketchX = mode == QStringLiteral("x");
    const bool separated = sketchX ? deltaX > projectionEpsilonMm
                                   : deltaY > projectionEpsilonMm;
    if (separated)
      return mode;
  }
  return QStringLiteral("aligned");
}

std::pair<sketch::Point, sketch::Point>
SketchCanvas::pointDimensionWitness(sketch::Point first,
                                    sketch::Point second,
                                    sketch::DimensionKind kind) {
  switch (kind) {
    case sketch::DimensionKind::PointDistanceX:
      return {first, {second.xMm, first.yMm}};
    case sketch::DimensionKind::PointDistanceY:
      return {first, {first.xMm, second.yMm}};
    default:
      return {first, second};
  }
}

double SketchCanvas::angularDimensionRadiusPx(
    double offsetMm, double pixelsPerMm, double viewportExtentPx) noexcept {
  const double safeScale = std::isfinite(pixelsPerMm)
                               ? std::max(0.0, pixelsPerMm)
                               : 0.0;
  const double extent = std::isfinite(viewportExtentPx)
                            ? std::max(0.0, viewportExtentPx)
                            : 0.0;
  const double maximum = std::clamp(extent * 0.22, 48.0, 180.0);
  const double requested = std::isfinite(offsetMm)
                               ? std::abs(offsetMm) * safeScale
                               : 16.0;
  return std::clamp(requested, 16.0, maximum);
}

void SketchCanvas::rotateViewClockwise() {
  if (viewCubeAnimation_) viewCubeAnimation_->stop();
  viewRotationDeg_ = std::remainder(viewRotationDeg_ + 90.0, 360.0);
  hideDimensionEditor();
  update();
}

void SketchCanvas::rotateViewCounterClockwise() {
  if (viewCubeAnimation_) viewCubeAnimation_->stop();
  viewRotationDeg_ = std::remainder(viewRotationDeg_ - 90.0, 360.0);
  hideDimensionEditor();
  update();
}

void SketchCanvas::orbitView(double yawDeltaDeg, double pitchDeltaDeg) {
  if (viewCubeAnimation_) viewCubeAnimation_->stop();
  setViewOrientation(viewYawDeg_ + yawDeltaDeg,
                     viewPitchDeg_ + pitchDeltaDeg);
}

void SketchCanvas::setViewOrientation(double yawDeg, double pitchDeg) {
  if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg)) return;
  viewYawDeg_ = std::remainder(yawDeg, 360.0);
  viewPitchDeg_ = std::clamp(pitchDeg, -89.9, 89.9);
  hideDimensionEditor();
  constructionHover_.reset();
  interaction_.updateTrim({});
  hoveredProjectionEdge_.reset();
  update();
}

void SketchCanvas::resetViewRotation() {
  if (viewCubeAnimation_) viewCubeAnimation_->stop();
  viewRotationDeg_ = initialViewRotationDeg_;
  viewYawDeg_ = 0.0;
  viewPitchDeg_ = 0.0;
  interaction_.endCameraGesture();
  cubePressed_ = {};
  clearViewCubeHover();
  hideDimensionEditor();
  fitReferenceGeometry();
  update();
}

int SketchCanvas::viewQuarterTurns() const noexcept {
  const int turns = qRound(viewRotationDeg_ / 90.0);
  return (turns % 4 + 4) % 4;
}

double SketchCanvas::viewRotationDegrees() const noexcept {
  return viewRotationDeg_;
}

double SketchCanvas::viewYawDegrees() const noexcept { return viewYawDeg_; }

double SketchCanvas::viewPitchDegrees() const noexcept {
  return viewPitchDeg_;
}

bool SketchCanvas::viewAlignedToSketchPlane() const noexcept {
  return std::abs(viewYawDeg_) <= 1e-9 &&
         std::abs(viewPitchDeg_) <= 1e-9;
}

CameraOrientation SketchCanvas::viewCubeCamera() const noexcept {
  // ViewCube labels are global (Front/Top/Right), while the Sketcher orbits
  // in the active plane's local frame.  Transform the eye direction through
  // SketchPlacement before feeding the shared 3D cube; otherwise an XZ face
  // is incorrectly labelled as Top and pitch appears mirrored.
  return orientationForDirection(localDirectionToWorld(
      referencePlacement_,
      sketchViewingDirection(viewYawDeg_, viewPitchDeg_)));
}

void SketchCanvas::animateViewToDirection(Point3d direction) {
  if (!viewCubeAnimation_) return;
  viewCubeAnimation_->stop();
  disconnect(viewCubeAnimation_, nullptr, this, nullptr);
  const CameraOrientation start{static_cast<float>(viewYawDeg_),
                                static_cast<float>(viewPitchDeg_)};
  const CameraOrientation target = sketchOrientationForDirection(
      worldDirectionToLocal(referencePlacement_, direction));
  viewRotationDeg_ = initialViewRotationDeg_;
  hideDimensionEditor();
  constructionHover_.reset();
  interaction_.updateTrim({});
  hoveredProjectionEdge_.reset();
  connect(viewCubeAnimation_, &QVariantAnimation::valueChanged, this,
          [this, start, target](const QVariant& value) {
            const auto camera = interpolateOrientation(
                start, target, value.toFloat());
            viewYawDeg_ = std::remainder(
                static_cast<double>(camera.yaw), 360.0);
            viewPitchDeg_ = std::clamp(
                static_cast<double>(camera.pitch), -89.9, 89.9);
            update();
          });
  viewCubeAnimation_->setStartValue(0.0F);
  viewCubeAnimation_->setEndValue(1.0F);
  viewCubeAnimation_->start();
}

void SketchCanvas::animateViewRotationBy(double deltaDeg) {
  if (!viewCubeAnimation_ || !std::isfinite(deltaDeg)) return;
  viewCubeAnimation_->stop();
  disconnect(viewCubeAnimation_, nullptr, this, nullptr);
  const double start = viewRotationDeg_;
  hideDimensionEditor();
  constructionHover_.reset();
  interaction_.updateTrim({});
  hoveredProjectionEdge_.reset();
  connect(viewCubeAnimation_, &QVariantAnimation::valueChanged, this,
          [this, start, deltaDeg](const QVariant& value) {
            viewRotationDeg_ = std::remainder(
                start + deltaDeg * value.toDouble(), 360.0);
            update();
          });
  viewCubeAnimation_->setStartValue(0.0);
  viewCubeAnimation_->setEndValue(1.0);
  viewCubeAnimation_->start();
}

void SketchCanvas::clearViewCubeHover() {
  if (!cubeHover_) return;
  cubeHover_ = {};
  QToolTip::hideText();
  setCursor(screenToSketchMappingAvailable()
                ? (tool() == Tool::Select ? Qt::ArrowCursor
                                         : Qt::CrossCursor)
                : Qt::OpenHandCursor);
  update();
}

std::optional<SketchProjectionEdgeToken> SketchCanvas::referenceEdgeAt(
    QPointF position) const {
  if (referenceBodyEdgeCount() == 0) return std::nullopt;

  SketchHitScene scene;
  std::size_t order = 0;
  const auto appendMesh = [this, &scene, &order](
                              const BodyRenderMesh& mesh,
                              SketchProjectionSource source,
                              std::size_t sceneBodySlot) {
    for (std::size_t edgeSlot = 0; edgeSlot < mesh.edges().size();
         ++edgeSlot) {
      const auto& edge = mesh.edges()[edgeSlot];
      if (edge.points.size() < 2) continue;
      SketchPickCandidate candidate;
      candidate.target = SketchProjectionEdgeToken{
          source, mesh.revision(), edgeSlot, sceneBodySlot};
      candidate.tolerancePx = SketchHitTolerancePolicy{}.projectionPx;
      candidate.priority = 0;
      candidate.stableOrder = order++;
      candidate.segments.reserve(edge.points.size() - 1);
      for (std::size_t pointIndex = 1;
           pointIndex < edge.points.size(); ++pointIndex) {
        const auto first = referencePlacement_.toLocal(
            edge.points[pointIndex - 1]);
        const auto second =
            referencePlacement_.toLocal(edge.points[pointIndex]);
        candidate.segments.push_back(hitSegment(
            mapPoint({first.x, first.y}), mapPoint({second.x, second.y})));
      }
      scene.candidates.push_back(std::move(candidate));
    }
  };
  if (referenceBodyMesh_)
    appendMesh(*referenceBodyMesh_, SketchProjectionSource::ReferenceBody, 0);
  for (std::size_t sceneBodySlot = 0;
       sceneBodySlot < sceneBodyMeshes_.size(); ++sceneBodySlot)
    if (sceneBodyMeshes_[sceneBodySlot])
      appendMesh(*sceneBodyMeshes_[sceneBodySlot],
                 SketchProjectionSource::SceneBody, sceneBodySlot);

  SketchPickFilter filter;
  filter.entities = false;
  filter.points = false;
  filter.datums = false;
  filter.dimensions = false;
  const auto hit = SketchHitTester::pick(scene, hitPoint(position), filter);
  if (!hit || !hit->projection()) return std::nullopt;
  // Revalidate immediately: the token may never outlive the source revision.
  return referenceEdge(*hit->projection())
             ? std::optional{*hit->projection()}
             : std::nullopt;
}

std::optional<SketchProjectionEdgeToken>
SketchCanvas::projectionTokenForFlatEdge(
    std::size_t edgeVectorIndex) const noexcept {
  if (referenceBodyMesh_ &&
      edgeVectorIndex < referenceBodyMesh_->edges().size())
    return SketchProjectionEdgeToken{
        SketchProjectionSource::ReferenceBody, referenceBodyMesh_->revision(),
        edgeVectorIndex, 0};
  edgeVectorIndex -=
      referenceBodyMesh_ ? referenceBodyMesh_->edges().size() : 0;
  for (std::size_t sceneBodySlot = 0;
       sceneBodySlot < sceneBodyMeshes_.size(); ++sceneBodySlot) {
    const auto& mesh = sceneBodyMeshes_[sceneBodySlot];
    if (!mesh) continue;
    if (edgeVectorIndex < mesh->edges().size())
      return SketchProjectionEdgeToken{
          SketchProjectionSource::SceneBody, mesh->revision(),
          edgeVectorIndex, sceneBodySlot};
    edgeVectorIndex -= mesh->edges().size();
  }
  return std::nullopt;
}

const RenderEdge* SketchCanvas::referenceEdge(
    std::size_t edgeVectorIndex) const noexcept {
  const auto token = projectionTokenForFlatEdge(edgeVectorIndex);
  return token ? referenceEdge(*token) : nullptr;
}

const RenderEdge* SketchCanvas::referenceEdge(
    const SketchProjectionEdgeToken& token) const noexcept {
  const BodyRenderMesh* mesh = nullptr;
  switch (token.source) {
    case SketchProjectionSource::ReferenceBody:
      mesh = referenceBodyMesh_.get();
      break;
    case SketchProjectionSource::SceneBody:
      if (token.sceneBodySlot >= sceneBodyMeshes_.size()) return nullptr;
      mesh = sceneBodyMeshes_[token.sceneBodySlot].get();
      break;
    case SketchProjectionSource::ReferenceFace:
      mesh = referenceFaceMesh_.get();
      break;
  }
  if (!mesh || mesh->revision() != token.meshRevision ||
      token.edgeSlot >= mesh->edges().size())
    return nullptr;
  return &mesh->edges()[token.edgeSlot];
}

bool SketchCanvas::projectReferenceEdge(std::size_t edgeVectorIndex) {
  const auto token = projectionTokenForFlatEdge(edgeVectorIndex);
  return token && projectReferenceEdge(*token);
}

bool SketchCanvas::projectReferenceEdge(
    const SketchProjectionEdgeToken& token) {
  // Apply only after a same-turn source/revision/slot revalidation.
  const auto* edge = referenceEdge(token);
  if (!edge) return false;

  return appendProjectedEdge(*edge, true, true);
}

bool SketchCanvas::appendProjectedCircularEdge(
    const RenderEdge& edge, bool recordUndo, bool reportStatus) {
  constexpr double kTwoPi = 6.28318530717958647692;
  const bool isCircle = edge.kind == RenderEdge::Kind::Circle;
  const Point2d localCenter = referencePlacement_.toLocal(edge.center);
  const sketch::Point center{localCenter.x, localCenter.y};

  ProjectGeometryCommand command;
  if (isCircle) {
    command.geometry = ProjectCircle{center, edge.radius};
  } else {
    const Point2d localStart = referencePlacement_.toLocal(edge.arcStart);
    const Point2d localEnd = referencePlacement_.toLocal(edge.arcEnd);
    const sketch::Point start{localStart.x, localStart.y};
    const sketch::Point end{localEnd.x, localEnd.y};
    const double startAngle =
        std::atan2(start.yMm - center.yMm, start.xMm - center.xMm);
    const double endAngle =
        std::atan2(end.yMm - center.yMm, end.xMm - center.xMm);
    double ccwSweep = endAngle - startAngle;
    while (ccwSweep < 0.0) ccwSweep += kTwoPi;
    while (ccwSweep >= kTwoPi) ccwSweep -= kTwoPi;
    bool counterClockwise = true;
    if (edge.points.size() >= 3) {
      const Point2d localMid =
          referencePlacement_.toLocal(edge.points[edge.points.size() / 2]);
      double midOffset =
          std::atan2(localMid.y - center.yMm, localMid.x - center.xMm) -
          startAngle;
      while (midOffset < 0.0) midOffset += kTwoPi;
      while (midOffset >= kTwoPi) midOffset -= kTwoPi;
      counterClockwise = midOffset <= ccwSweep;
    }
    command.geometry = ProjectArc{
        center, edge.radius, counterClockwise ? startAngle : endAngle,
        counterClockwise ? ccwSweep : kTwoPi - ccwSweep};
  }

  if (recordUndo) pushUndoState();
  const auto result = executeCommand(command);
  if (!result.accepted) {
    if (recordUndo) cancelPendingUndo();
    if (reportStatus)
      emit selectionChanged(result.error == SketchCommandError::Duplicate
                                ? QString::fromUtf8("Это ребро уже спроецировано")
                                : QString::fromUtf8(
                                      "Не удалось зафиксировать проекцию"));
    update();
    return false;
  }
  if (reportStatus) {
    clearGeometrySelection();
    notifyGeometryChanged();
    emit selectionChanged(QString::fromUtf8("Проекция ребра добавлена"));
  }
  update();
  return true;
}

bool SketchCanvas::appendProjectedEdge(const RenderEdge& edge, bool recordUndo,
                                       bool reportStatus) {
  if (edge.points.size() < 2) return false;
  if (edge.kind == RenderEdge::Kind::Circle ||
      edge.kind == RenderEdge::Kind::Arc)
    return appendProjectedCircularEdge(edge, recordUndo, reportStatus);

  ProjectLineChain chain;
  chain.segments.reserve(edge.points.size() - 1);
  for (std::size_t index = 1; index < edge.points.size(); ++index) {
    const auto first = referencePlacement_.toLocal(edge.points[index - 1]);
    const auto second = referencePlacement_.toLocal(edge.points[index]);
    chain.segments.push_back({{first.x, first.y}, {second.x, second.y}});
  }
  if (recordUndo) pushUndoState();
  const auto result = executeCommand(ProjectGeometryCommand{std::move(chain)});
  if (!result.accepted) {
    if (recordUndo) cancelPendingUndo();
    if (reportStatus)
      emit selectionChanged(result.error == SketchCommandError::Duplicate
                                ? QString::fromUtf8("Это ребро уже спроецировано")
                                : QString::fromUtf8(
                                      "Не удалось зафиксировать проекцию"));
    update();
    return false;
  }
  if (reportStatus) {
    clearGeometrySelection();
    notifyGeometryChanged();
    emit selectionChanged(QString::fromUtf8("Проекция ребра добавлена"));
  }
  update();
  return true;
}

SketchCanvas::ProjectedLocalPoint SketchCanvas::projectLocalPoint(
    double xMm, double yMm, double zMm) const noexcept {
  const double yaw = viewYawDeg_ * std::numbers::pi / 180.0;
  const double pitch = viewPitchDeg_ * std::numbers::pi / 180.0;
  const double cosYaw = std::cos(yaw);
  const double sinYaw = std::sin(yaw);
  const double cosPitch = std::cos(pitch);
  const double sinPitch = std::sin(pitch);

  // Orbit in the sketch's local 3D frame. The zero orientation is the exact
  // orthographic sketch plane used for editing; yaw/pitch are view-only and
  // never alter SketchPlacement or stored geometry.
  const double yawX = cosYaw * xMm + sinYaw * zMm;
  const double yawZ = -sinYaw * xMm + cosYaw * zMm;
  const double pitchY = cosPitch * yMm - sinPitch * yawZ;
  const double depth = sinPitch * yMm + cosPitch * yawZ;
  const auto rolled = rotateForView({yawX, pitchY});
  return {rolled.xMm, rolled.yMm, depth};
}

QPointF SketchCanvas::mapWorldPoint(Point3d point) const {
  const auto local = referencePlacement_.toLocal(point);
  const Vector3d normal = referencePlacement_.normal();
  const Vector3d delta{point.x - referencePlacement_.origin.x,
                       point.y - referencePlacement_.origin.y,
                       point.z - referencePlacement_.origin.z};
  const double localZ = delta.x * normal.x + delta.y * normal.y +
                        delta.z * normal.z;
  const auto projected = projectLocalPoint(local.x, local.y, localZ);
  const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5 +
                         interactionState().camera.panX;
  const double centerY = kRulerTop + (height() - kRulerTop) * 0.5 +
                         interactionState().camera.panY;
  return {centerX + projected.xMm * pixelsPerMm_,
          centerY - projected.yMm * pixelsPerMm_};
}

double SketchCanvas::worldPointDepth(Point3d point) const noexcept {
  const auto local = referencePlacement_.toLocal(point);
  const Vector3d normal = referencePlacement_.normal();
  const Vector3d delta{point.x - referencePlacement_.origin.x,
                       point.y - referencePlacement_.origin.y,
                       point.z - referencePlacement_.origin.z};
  const double localZ = delta.x * normal.x + delta.y * normal.y +
                        delta.z * normal.z;
  return projectLocalPoint(local.x, local.y, localZ).depthMm;
}

QPointF SketchCanvas::mapPoint(sketch::Point point) const {
  const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5 +
                         interactionState().camera.panX;
  const double centerY = kRulerTop + (height() - kRulerTop) * 0.5 +
                         interactionState().camera.panY;
  const auto viewPoint = projectLocalPoint(point.xMm, point.yMm, 0.0);
  return {centerX + viewPoint.xMm * pixelsPerMm_,
          centerY - viewPoint.yMm * pixelsPerMm_};
}

sketch::Point SketchCanvas::unmapPoint(QPointF point) const {
  const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5 +
                         interactionState().camera.panX;
  const double centerY = kRulerTop + (height() - kRulerTop) * 0.5 +
                         interactionState().camera.panY;
  const sketch::Point viewPoint{(point.x() - centerX) / pixelsPerMm_,
                                (centerY - point.y()) / pixelsPerMm_};
  const auto origin = projectLocalPoint(0.0, 0.0, 0.0);
  const auto xAxis = projectLocalPoint(1.0, 0.0, 0.0);
  const auto yAxis = projectLocalPoint(0.0, 1.0, 0.0);
  const double xx = xAxis.xMm - origin.xMm;
  const double xy = xAxis.yMm - origin.yMm;
  const double yx = yAxis.xMm - origin.xMm;
  const double yy = yAxis.yMm - origin.yMm;
  const double determinant = xx * yy - xy * yx;
  if (std::abs(determinant) <= 1e-9)
    return rotateFromView(viewPoint);
  return {(viewPoint.xMm * yy - viewPoint.yMm * yx) / determinant,
          (xx * viewPoint.yMm - xy * viewPoint.xMm) / determinant};
}

QPolygonF SketchCanvas::circlePolyline(sketch::Point center, double radiusMm,
                                       double startAngleRad,
                                       double sweepAngleRad,
                                       int segmentCount) const {
  QPolygonF result;
  segmentCount = std::max(8, segmentCount);
  result.reserve(segmentCount + 1);
  for (int segment = 0; segment <= segmentCount; ++segment) {
    const double t = static_cast<double>(segment) / segmentCount;
    const double angle = startAngleRad + sweepAngleRad * t;
    result << mapPoint({center.xMm + radiusMm * std::cos(angle),
                        center.yMm + radiusMm * std::sin(angle)});
  }
  return result;
}

SketchHitScene SketchCanvas::hitScene(
    const SketchHitTolerancePolicy& tolerance, bool includeDatums) const {
  SketchHitSceneOptions options;
  options.tolerance = tolerance;
  if (includeDatums) {
    options.origin = hitPoint(mapPoint({0.0, 0.0}));
    const auto topLeft = unmapPoint(QPointF(0.0, 0.0));
    const auto bottomRight =
        unmapPoint(QPointF(static_cast<double>(width()),
                           static_cast<double>(height())));
    const double extent = std::max(
        {std::abs(topLeft.xMm), std::abs(topLeft.yMm),
         std::abs(bottomRight.xMm), std::abs(bottomRight.yMm), 1.0}) *
        2.0;
    options.xAxis = hitSegment(mapPoint({-extent, 0.0}),
                               mapPoint({extent, 0.0}));
    options.yAxis = hitSegment(mapPoint({0.0, -extent}),
                               mapPoint({0.0, extent}));
  }
  return SketchHitSceneAdapter::build(
      sketch_, [this](sketch::Point point) {
        return hitPoint(mapPoint(point));
      }, options);
}

std::optional<SketchPickEntityRef> SketchCanvas::geometryAt(
    QPointF position, double tolerancePx,
    const SketchPickFilter& requestedFilter,
    sketch::GeometryId excludedGeometry) const {
  auto filter = requestedFilter;
  filter.entities = true;
  filter.points = false;
  filter.datums = false;
  filter.projections = false;
  filter.dimensions = false;
  SketchHitTolerancePolicy tolerance;
  tolerance.entityPx = tolerancePx;
  auto scene = hitScene(tolerance);
  if (excludedGeometry != sketch::kInvalidGeometryId) {
    for (auto& candidate : scene.candidates) {
      const auto* entity = std::get_if<SketchPickEntityRef>(&candidate.target);
      if (entity && entity->geometryId == excludedGeometry)
        candidate.enabled = false;
    }
  }
  const auto hit = SketchHitTester::pick(scene, hitPoint(position), filter);
  if (!hit || !hit->entity()) return std::nullopt;
  const auto reference = *hit->entity();
  switch (reference.kind) {
    case SketchPickEntityKind::Line:
      if (!sketch_.lineIndex(reference.geometryId)) return std::nullopt;
      break;
    case SketchPickEntityKind::Circle:
      if (!sketch_.circleIndex(reference.geometryId)) return std::nullopt;
      break;
    case SketchPickEntityKind::Arc:
      if (!sketch_.arcIndex(reference.geometryId)) return std::nullopt;
      break;
  }
  return reference;
}

std::optional<SketchPickPointRef> SketchCanvas::pointAt(
    QPointF position, double tolerancePx,
    const SketchPickFilter& requestedFilter,
    sketch::GeometryId excludedGeometry,
    std::size_t excludedElement) const {
  auto filter = requestedFilter;
  filter.entities = false;
  filter.points = true;
  filter.datums = false;
  filter.projections = false;
  filter.dimensions = false;
  filter.lineMidpoints = false;
  SketchHitTolerancePolicy tolerance;
  tolerance.pointPx = tolerancePx;
  tolerance.endpointPx = tolerancePx;
  auto scene = hitScene(tolerance);
  for (auto& candidate : scene.candidates) {
    const auto* point = std::get_if<SketchPickPointRef>(&candidate.target);
    if (!point) continue;
    if ((excludedGeometry != sketch::kInvalidGeometryId &&
         point->carrierId == excludedGeometry) ||
        (excludedElement != 0 && point->elementId == excludedElement))
      candidate.enabled = false;
  }
  const auto hit = SketchHitTester::pick(scene, hitPoint(position), filter);
  if (!hit || !hit->point()) return std::nullopt;
  const auto reference = *hit->point();
  if (!sketch_.referencedPoint(reference.reference)) return std::nullopt;
  return reference;
}

std::optional<std::vector<sketch::Line>>
SketchCanvas::resolvedCircleGuideLines() const {
  std::vector<sketch::Line> result;
  result.reserve(interactionState().creation.circleGuideIds.size());
  for (const auto id : interactionState().creation.circleGuideIds) {
    const auto index = sketch_.lineIndex(id);
    if (!index) return std::nullopt;
    result.push_back(sketch_.lines()[*index]);
  }
  return result;
}

sketch::Point SketchCanvas::snappedPoint(QPointF point) const {
  auto result = unmapPoint(point);
  if (snapEnabled_) {
    result.xMm = std::round(result.xMm / snapStepMm_) * snapStepMm_;
    result.yMm = std::round(result.yMm / snapStepMm_) * snapStepMm_;
  }
  return result;
}

SketchCanvas::ConstructionSnap SketchCanvas::constructionSnapAt(
    QPointF position) const {
  ConstructionSnap result;
  result.point = snappedPoint(position);

  const bool creationTool =
      tool() == Tool::Line ||
      tool() == Tool::Rectangle ||
      tool() == Tool::Circle ||
      tool() == Tool::Arc;

  sketch::GeometryId draggedGeometryId = sketch::kInvalidGeometryId;
  std::size_t draggedElementId = 0;
  if (const auto* lineTarget =
          std::get_if<SketchLineEndpointDrag>(&interactionState().pointDrag)) {
    draggedGeometryId = lineTarget->lineId;
    if (const auto index = sketch_.lineIndex(draggedGeometryId))
      draggedElementId = sketch_.lines()[*index].elementId;
  } else if (const auto* arcTarget =
                 std::get_if<SketchArcEndpointDrag>(
                     &interactionState().pointDrag)) {
    draggedGeometryId = arcTarget->arcId;
  } else if (const auto* circleTarget =
                 std::get_if<SketchCircleCenterDrag>(
                     &interactionState().pointDrag)) {
    draggedGeometryId = circleTarget->circleId;
  } else if (const auto* centerTarget =
                 std::get_if<SketchElementCenterDrag>(
                     &interactionState().pointDrag)) {
    draggedElementId = centerTarget->elementId;
  }
  const bool pointDrag = draggedGeometryId != sketch::kInvalidGeometryId ||
                         draggedElementId != 0;

  const bool tangentCircleMode =
      tool() == Tool::Circle &&
      (circleMode_ == CircleMode::ThreeTangents ||
       circleMode_ == CircleMode::TwoTangentsRadius);

  // Grid snapping and CAD inference are independent. Disabling the grid must
  // never disable endpoint, midpoint or point-on-geometry inference.
  if (!creationTool && !pointDrag)
    return result;

  // Tangent-circle tools select carrier lines, not construction points.
  // Keep the exact cursor position for nearestLine() and for the live
  // two-tangent side/radius preview instead of projecting it onto geometry.
  if (tangentCircleMode) {
    result.point = unmapPoint(position);
    return result;
  }

  // A body snap is only advertised when the current construction stage can
  // persist or deliberately consume that relationship.
  const bool allowLineBody =
      tool() == Tool::Line ||
      tool() == Tool::Rectangle ||
      tool() == Tool::Arc ||
      pointDrag ||
      tangentCircleMode ||
      (tool() == Tool::Circle &&
       circleMode_ == CircleMode::CenterRadius &&
       !interactionState().creation.anchor);

  const bool allowCircleBody =
      tool() == Tool::Line ||
      tool() == Tool::Rectangle ||
      tool() == Tool::Arc ||
      pointDrag ||
      (tool() == Tool::Circle &&
       circleMode_ == CircleMode::CenterRadius &&
       !interactionState().creation.anchor);

  const bool allowArcBody =
      tool() == Tool::Line ||
      tool() == Tool::Rectangle ||
      tool() == Tool::Circle ||
      tool() == Tool::Arc ||
      pointDrag;

  SketchHitTolerancePolicy tolerance;
  tolerance.entityPx = 10.0;
  tolerance.pointPx = 10.0;
  tolerance.endpointPx = 10.0;
  auto scene = hitScene(tolerance, true);
  for (auto& candidate : scene.candidates) {
    if (const auto* entity =
            std::get_if<SketchPickEntityRef>(&candidate.target)) {
      const bool dragged = pointDrag &&
          (entity->geometryId == draggedGeometryId ||
           (draggedElementId != 0 &&
            entity->elementId == draggedElementId));
      candidate.enabled = !dragged;
    } else if (const auto* point =
                   std::get_if<SketchPickPointRef>(&candidate.target)) {
      const bool dragged = pointDrag &&
          (point->carrierId == draggedGeometryId ||
           (draggedElementId != 0 && point->elementId == draggedElementId));
      candidate.enabled = !dragged;
    }
  }
  SketchPickFilter filter;
  filter.projections = false;
  filter.dimensions = false;
  filter.lines = allowLineBody;
  filter.circles = allowCircleBody;
  filter.arcs = allowArcBody;
  filter.lineMidpoints = !tangentCircleMode;
  const auto hit = SketchHitTester::pick(scene, hitPoint(position), filter);
  if (!hit) return result;

  if (const auto* point = hit->point()) {
    result.point = point->point;
    result.geometryId = point->carrierId;
    result.elementId = point->elementId;
    result.pointReference = point->reference;
    switch (point->kind) {
      case SketchPickPointKind::LineEndpoint:
      case SketchPickPointKind::ArcEndpoint:
        result.kind = ConstructionSnapKind::LinePoint;
        break;
      case SketchPickPointKind::CircleCenter:
        result.kind = ConstructionSnapKind::CircleCenter;
        break;
      case SketchPickPointKind::ElementCenter:
        result.kind = ConstructionSnapKind::ElementCenter;
        break;
      case SketchPickPointKind::LineMidpoint:
        result.kind = ConstructionSnapKind::LineMidpoint;
        result.pointReference = {};
        break;
    }
    return result;
  }
  if (const auto* datum = hit->datum()) {
    const auto cursor = unmapPoint(position);
    switch (datum->kind) {
      case SketchPickDatumKind::Origin:
        result.point = {0.0, 0.0};
        result.kind = ConstructionSnapKind::Origin;
        break;
      case SketchPickDatumKind::XAxis:
        result.point = {cursor.xMm, 0.0};
        result.kind = ConstructionSnapKind::XAxis;
        break;
      case SketchPickDatumKind::YAxis:
        result.point = {0.0, cursor.yMm};
        result.kind = ConstructionSnapKind::YAxis;
        break;
    }
    return result;
  }
  const auto* entity = hit->entity();
  if (!entity) return result;
  result.geometryId = entity->geometryId;
  const auto cursor = unmapPoint(position);
  if (entity->kind == SketchPickEntityKind::Line) {
    const auto index = sketch_.lineIndex(entity->geometryId);
    if (!index) return {};
    const auto& line = sketch_.lines()[*index];
    const double dx = line.end.xMm - line.start.xMm;
    const double dy = line.end.yMm - line.start.yMm;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 1e-18) return {};
    const double parameter = std::clamp(
        ((cursor.xMm - line.start.xMm) * dx +
         (cursor.yMm - line.start.yMm) * dy) / lengthSquared,
        0.0, 1.0);
    result.point = {line.start.xMm + dx * parameter,
                    line.start.yMm + dy * parameter};
    result.kind = ConstructionSnapKind::LineBody;
    return result;
  }
  const auto centerAndRadius =
      entity->kind == SketchPickEntityKind::Circle
          ? [&]() -> std::optional<std::pair<sketch::Point, double>> {
              const auto index = sketch_.circleIndex(entity->geometryId);
              if (!index) return std::nullopt;
              const auto& circle = sketch_.circles()[*index];
              return std::pair{circle.center, circle.radiusMm};
            }()
          : [&]() -> std::optional<std::pair<sketch::Point, double>> {
              const auto index = sketch_.arcIndex(entity->geometryId);
              if (!index) return std::nullopt;
              const auto& arc = sketch_.arcs()[*index];
              return std::pair{arc.center, arc.radiusMm};
            }();
  if (!centerAndRadius) return {};
  const double dx = cursor.xMm - centerAndRadius->first.xMm;
  const double dy = cursor.yMm - centerAndRadius->first.yMm;
  const double length = std::hypot(dx, dy);
  if (length <= 1e-18) return {};
  result.point = {centerAndRadius->first.xMm +
                      centerAndRadius->second * dx / length,
                  centerAndRadius->first.yMm +
                      centerAndRadius->second * dy / length};
  result.kind = ConstructionSnapKind::CircleBody;
  return result;
}

bool SketchCanvas::commitDraggedPointSnap(
    sketch::PointReference movingPoint, const ConstructionSnap& snap) {
  BindPointCommand command;
  command.movingPoint = movingPoint;
  command.targetPoint = snap.pointReference;
  command.targetGeometry = snap.geometryId;
  switch (snap.kind) {
    case ConstructionSnapKind::LinePoint:
    case ConstructionSnapKind::CircleCenter:
    case ConstructionSnapKind::ElementCenter:
      command.kind = SketchPointBindingKind::Coincident;
      break;
    case ConstructionSnapKind::LineMidpoint:
      command.kind = SketchPointBindingKind::Midpoint;
      break;
    case ConstructionSnapKind::LineBody:
      command.kind = SketchPointBindingKind::PointOnLine;
      break;
    case ConstructionSnapKind::CircleBody:
      command.kind = sketch_.circleIndex(snap.geometryId)
                         ? SketchPointBindingKind::PointOnCircle
                         : SketchPointBindingKind::PointOnArc;
      break;
    case ConstructionSnapKind::XAxis:
      command.kind = SketchPointBindingKind::XAxis;
      break;
    case ConstructionSnapKind::YAxis:
      command.kind = SketchPointBindingKind::YAxis;
      break;
    case ConstructionSnapKind::Origin:
      command.kind = SketchPointBindingKind::Origin;
      break;
    case ConstructionSnapKind::None:
      return false;
  }
  return executeCommand(command).accepted;
}

SketchRenderSnapshot SketchCanvas::renderSnapshot() const {
  SketchRenderSnapshot snapshot;
  snapshot.viewportSize = size();
  snapshot.palette = sketchRenderPalette(ThemeManager::instance().colors(),
                                         palette());
  snapshot.scene = renderSceneCache_.resolve(
      renderSceneRevision_, sketch_, referenceProfile_, sceneSketches_,
      referenceBodyMesh_, referenceFaceMesh_, sceneBodyMeshes_,
      referencePlacement_, referenceBox_.widthMm, referenceBox_.depthMm,
      referenceBox_.heightMm, referenceBodyVisible_, realReferenceBodyVisible_,
      referenceProfileVisible_);
  const auto& interaction = interactionState();
  snapshot.interaction.tool = interaction.tool;
  snapshot.interaction.dimension = interaction.dimension;
  snapshot.interaction.autoDimension = interaction.autoDimension;
  snapshot.interaction.constraint = interaction.constraint;
  snapshot.interaction.creation = interaction.creation;
  snapshot.interaction.trim = interaction.trim;
  snapshot.interaction.camera = interaction.camera;
  snapshot.interaction.selectionBox = interaction.selectionBox;
  snapshot.interaction.twoTangentRadiusPreviewActive =
      interaction.twoTangentRadiusPreviewActive;
  switch (selectionKind_) {
    case SelectionKind::None:
      snapshot.selectionKind = SketchRenderSelectionKind::None;
      break;
    case SelectionKind::Line:
      snapshot.selectionKind = SketchRenderSelectionKind::Line;
      break;
    case SelectionKind::Circle:
      snapshot.selectionKind = SketchRenderSelectionKind::Circle;
      break;
    case SelectionKind::Arc:
      snapshot.selectionKind = SketchRenderSelectionKind::Arc;
      break;
  }
  snapshot.selectionCircleId = selectionCircleId_;
  snapshot.selectionLineId = selectionLineId_;
  snapshot.selectionArcId = selectionArcId_;
  snapshot.selectionElementId = selectionElementId_;
  snapshot.selectedLineIds = selectedLineIds_;
  snapshot.selectedElementIds = selectedElementIds_;
  snapshot.selectedCircleIds = selectedCircleIds_;
  snapshot.selectedArcIds = selectedArcIds_;
  snapshot.hoverPoint = hoverPoint_;
  if (constructionHover_) {
    SketchRenderSnap renderSnap;
    renderSnap.point = constructionHover_->point;
    renderSnap.geometryId = constructionHover_->geometryId;
    renderSnap.elementId = constructionHover_->elementId;
    renderSnap.pointReference = constructionHover_->pointReference;
    switch (constructionHover_->kind) {
      case ConstructionSnapKind::None:
        renderSnap.kind = SketchRenderSnapKind::None;
        break;
      case ConstructionSnapKind::LinePoint:
        renderSnap.kind = SketchRenderSnapKind::LinePoint;
        break;
      case ConstructionSnapKind::LineMidpoint:
        renderSnap.kind = SketchRenderSnapKind::LineMidpoint;
        break;
      case ConstructionSnapKind::CircleCenter:
        renderSnap.kind = SketchRenderSnapKind::CircleCenter;
        break;
      case ConstructionSnapKind::ElementCenter:
        renderSnap.kind = SketchRenderSnapKind::ElementCenter;
        break;
      case ConstructionSnapKind::LineBody:
        renderSnap.kind = SketchRenderSnapKind::LineBody;
        break;
      case ConstructionSnapKind::CircleBody:
        renderSnap.kind = SketchRenderSnapKind::CircleBody;
        break;
      case ConstructionSnapKind::XAxis:
        renderSnap.kind = SketchRenderSnapKind::XAxis;
        break;
      case ConstructionSnapKind::YAxis:
        renderSnap.kind = SketchRenderSnapKind::YAxis;
        break;
      case ConstructionSnapKind::Origin:
        renderSnap.kind = SketchRenderSnapKind::Origin;
        break;
    }
    snapshot.constructionHover = renderSnap;
  }
  snapshot.hoveredProjectionEdge = hoveredProjectionEdge_;
  snapshot.pixelsPerMm = pixelsPerMm_;
  snapshot.snapStepMm = snapStepMm_;
  snapshot.snapEnabled = snapEnabled_;
  snapshot.gridVisible = gridVisible_;
  snapshot.viewRotationDeg = viewRotationDeg_;
  snapshot.viewYawDeg = viewYawDeg_;
  snapshot.viewPitchDeg = viewPitchDeg_;
  switch (circleMode_) {
    case CircleMode::CenterRadius:
      snapshot.circleMode = SketchRenderCircleMode::CenterRadius;
      break;
    case CircleMode::TwoPoints:
      snapshot.circleMode = SketchRenderCircleMode::TwoPoints;
      break;
    case CircleMode::ThreePoints:
      snapshot.circleMode = SketchRenderCircleMode::ThreePoints;
      break;
    case CircleMode::ThreeTangents:
      snapshot.circleMode = SketchRenderCircleMode::ThreeTangents;
      break;
    case CircleMode::TwoTangentsRadius:
      snapshot.circleMode = SketchRenderCircleMode::TwoTangentsRadius;
      break;
  }
  snapshot.circleDiameterMm = circleDiameterMm_;
  switch (rectangleMode_) {
    case RectangleMode::TwoPoints:
      snapshot.rectangleMode = SketchRenderRectangleMode::TwoPoints;
      break;
    case RectangleMode::ThreePoints:
      snapshot.rectangleMode = SketchRenderRectangleMode::ThreePoints;
      break;
    case RectangleMode::FromCenter:
      snapshot.rectangleMode = SketchRenderRectangleMode::FromCenter;
      break;
  }
  snapshot.primaryDimensionVisible = primaryDimension_->isVisible();
  snapshot.primaryDimensionValue = primaryDimension_->value();
  return snapshot;
}

void SketchCanvas::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  renderer_.render(painter, renderer_.buildFrame(renderSnapshot()));

  // The Sketcher and the 3D viewport intentionally share one navigation
  // cube: the same hit zones, labels, theme and 45-degree edge views.
  const ThemeColors& theme = ThemeManager::instance().colors();
  const CameraOrientation cubeCamera = viewCubeCamera();
  paintViewCube(painter, viewCubeGeometry(size(), cubeCamera), cubeCamera,
                cubeHover_, cubePressed_,
                ViewCubeStyle{theme.cubeTop, theme.cubeFront, theme.cubeSide,
                              theme.cubeOutline, theme.cubeText,
                              theme.cubeBevel, theme.cubeHover,
                              theme.cubePressed, theme.cubeActive,
                              theme.cubeAccent, theme.cubeShadow});
}

void SketchCanvas::mousePressEvent(QMouseEvent* event) {
  setFocus();
  if (event->button() == Qt::MiddleButton) {
    if (viewCubeAnimation_) viewCubeAnimation_->stop();
    const bool orbit = event->modifiers().testFlag(Qt::ShiftModifier);
    interaction_.beginCameraGesture(
        orbit ? SketchCameraGestureState::Kind::Orbit
              : SketchCameraGestureState::Kind::Pan,
        SketchCameraGestureState::Button::Middle,
        event->position().x(), event->position().y());
    setCursor(orbit ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
    event->accept();
    return;
  }
  if (event->button() == Qt::RightButton) {
    if (viewCubeAnimation_) viewCubeAnimation_->stop();
    // Match the 3D viewport: dragging the right mouse button freely orbits the
    // camera. A press/release without a drag retains the established Sketcher
    // cancellation behavior (handled in mouseReleaseEvent).
    interaction_.beginCameraGesture(
        SketchCameraGestureState::Kind::Orbit,
        SketchCameraGestureState::Button::Right,
        event->position().x(), event->position().y());
    setCursor(Qt::SizeAllCursor);
    event->accept();
    return;
  }
  if (event->button() != Qt::LeftButton) return;

  const ViewCubeHit cubeHit =
      viewCubeGeometry(size(), viewCubeCamera()).hitTest(event->position());
  if (cubeHit) {
    if (viewCubeAnimation_) viewCubeAnimation_->stop();
    cubePressed_ = cubeHit;
    hideDimensionEditor();
    setCursor(Qt::PointingHandCursor);
    update();
    event->accept();
    return;
  }

  // Any non-degenerate orthographic view can be inverted back to the active
  // sketch plane. Only a plane seen exactly edge-on is ambiguous.
  if (!screenToSketchMappingAvailable()) {
    emit selectionChanged(QString::fromUtf8(
        "Плоскость эскиза видна строго сбоку: слегка поверните камеру"));
    event->accept();
    return;
  }

  if (tool() == Tool::Trim) {
    if (!trimAt(event->position()))
      emit selectionChanged(QString::fromUtf8(
          "Ножницы: наведите курсор на линию, дугу или окружность"));
    event->accept();
    return;
  }

  if (tool() == Tool::Mirror) {
    if (interactionState().mirror.source.empty()) {
      const auto source = mirrorGeometryAt(event->position());
      if (!source) {
        emit selectionChanged(QString::fromUtf8(
            "Зеркало: выберите прямую, дугу или окружность"));
      } else {
        setMirrorSourceSelection({*source});
        emit selectionChanged(QString::fromUtf8(
            "Зеркало: объект выбран, теперь выберите прямую-ось"));
      }
    } else {
      const auto axis = lineAt(event->position());
      if (!axis || !mirrorContourAboutLine(*axis))
        emit selectionChanged(QString::fromUtf8(
            "Зеркало: выберите отдельную прямую как ось симметрии"));
    }
    event->accept();
    return;
  }

  if (tool() == Tool::Projection) {
    const auto edge = referenceEdgeAt(event->position());
    if (edge)
      (void)projectReferenceEdge(*edge);
    else
      emit selectionChanged(
          QString::fromUtf8("Проекция: наведите курсор на ребро модели"));
    event->accept();
    return;
  }

  if (tool() == Tool::AutoDimension && primaryDimension_->isVisible() &&
      interactionState().autoDimension.target !=
          SketchAutoDimensionTarget::None) {
    const auto directLineId = static_cast<sketch::GeometryId>(
        interactionState().autoDimension.directLineId.value_or(sketch::kInvalidGeometryId));

    if (directLineId != sketch::kInvalidGeometryId &&
        interactionState().autoDimension.target != SketchAutoDimensionTarget::Angle) {
      std::optional<std::size_t> secondLineIndex;
      SketchPickFilter lineFilter;
      lineFilter.circles = false;
      lineFilter.arcs = false;
      if (const auto second = geometryAt(event->position(), 9.0,
                                         lineFilter);
          second && second->geometryId != directLineId)
        secondLineIndex = sketch_.lineIndex(second->geometryId);

      if (secondLineIndex) {
        const auto firstIndex = sketch_.lineIndex(directLineId);
        if (firstIndex) {
          const auto secondId = sketch_.lineId(*secondLineIndex);
          const auto& firstLine = sketch_.lines()[*firstIndex];
          const auto& secondLine = sketch_.lines()[*secondLineIndex];

          if (parallelLinePair(firstLine, secondLine)) {
            const double distance =
                parallelLineDistanceMm(firstLine, secondLine);
            if (distance > 1e-9) {
              auto autoDimension = interactionState().autoDimension;
              autoDimension.target = SketchAutoDimensionTarget::LineDistance;
              autoDimension.distanceFirstLine = directLineId;
              autoDimension.distanceSecondLine = secondId;
              autoDimension.offsetMm = 0.0;
              interaction_.updateAutoDimension(std::move(autoDimension));
              primaryDimension_->setPrefix(QString());
              primaryDimension_->setSuffix(QString::fromUtf8(" РјРј"));
              primaryDimension_->setRange(0.01, 100000.0);
              primaryDimension_->setValue(distance);
              primaryDimension_->move(
                  (event->position() + QPointF(16, 16)).toPoint());
              primaryDimension_->show();
              primaryDimension_->setFocus();
              primaryDimension_->selectAll();
              update();
              event->accept();
              return;
            }
          }

          const double angle =
              visibleLineAngleDegrees(firstLine, secondLine);

          if (angle > 1e-6 && angle < 180.0 - 1e-6) {
            auto autoDimension = interactionState().autoDimension;
            autoDimension.target = SketchAutoDimensionTarget::Angle;
            autoDimension.angleFirstLine = directLineId;
            autoDimension.angleSecondLine = secondId;
            autoDimension.offsetMm = 12.0;
            interaction_.updateAutoDimension(std::move(autoDimension));

            primaryDimension_->setPrefix(QString());
            primaryDimension_->setSuffix(QString::fromUtf8("°"));
            primaryDimension_->setRange(0.01, 179.99);
            primaryDimension_->setValue(angle);
            primaryDimension_->move(
                (event->position() + QPointF(16, 16)).toPoint());
            primaryDimension_->show();
            primaryDimension_->setFocus();
            primaryDimension_->selectAll();

            update();
            event->accept();
            return;
          }
        }
      }
    }

    commitAutoDimension();
    event->accept();
    return;
  }
  // Installed dimensions are selectable/draggable only in Select mode.
  // While AutoDimension or a constraint tool is active, dimensions are
  // transparent to mouse hit-testing so geometry interaction cannot be
  // stolen by an existing annotation.
  if (tool() == Tool::Select &&
      !event->modifiers().testFlag(Qt::ControlModifier) &&
      beginDimensionLabelDrag(event->position())) {
    event->accept();
    return;
  }
  if (tool() == Tool::Select &&
      !event->modifiers().testFlag(Qt::ControlModifier) &&
      beginDimensionLineDrag(event->position())) {
    event->accept();
    return;
  }
  interaction_.selectDimension(std::nullopt);
  if (tool() == Tool::AutoDimension) {
    handleAutoDimensionClick(event->position());
  } else if (tool() == Tool::LockConstraint) {
    handleLockConstraintClick(event->position());
  } else if (tool() == Tool::OrthogonalConstraint) {
    handleOrthogonalConstraintClick(event->position());
  } else if (tool() == Tool::CoincidentConstraint) {
    handleCoincidentConstraintClick(event->position());
  } else if (tool() == Tool::PerpendicularConstraint) {
    handlePerpendicularConstraintClick(event->position());
  } else if (tool() == Tool::ParallelConstraint) {
    handleParallelConstraintClick(event->position());
  } else if (tool() == Tool::EqualConstraint) {
    handleEqualConstraintClick(event->position());
  } else if (tool() == Tool::TangentConstraint) {
    handleTangentConstraintClick(event->position());
  } else if (tool() == Tool::Select) {
    const bool additive =
        event->modifiers().testFlag(Qt::ControlModifier);

    // LMB drag on empty canvas starts the selection rectangle. A click on
    // existing geometry keeps the normal select/move behaviour.
    constexpr double geometryHitTolerance = 9.0;
    const bool geometryHit =
        geometryAt(event->position(), geometryHitTolerance).has_value() ||
        pointAt(event->position(), geometryHitTolerance).has_value();

    if (!geometryHit) {
      interaction_.beginSelectionBox(
          {event->position().x(), event->position().y()}, additive);

      if (!additive) {
        clearGeometrySelection();
        emit lineStyleSelectionChanged(false, false);
        emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
      }

      setCursor(Qt::CrossCursor);
      event->accept();
      update();
      return;
    }

    if (additive) {
      // Ctrl+LMB toggles complete CAD objects. Endpoint editing is deliberately
      // bypassed here so Ctrl always means selection-set modification.
      interaction_.cancelPointDrag();
      selectAt(event->position(), true, false);
      event->accept();
      return;
    }

    constexpr double endpointTolerance = 9.0;
    std::optional<sketch::PointReference> endpoint;
    std::size_t endpointElementId = 0;
    bool endpointDashed = false;
    bool endpointIsArc = false;
    sketch::GeometryId endpointArcId = sketch::kInvalidGeometryId;
    bool endpointArcStart = false;
    bool endpointIsCircleCenter = false;
    sketch::GeometryId endpointCircleId = sketch::kInvalidGeometryId;
    bool endpointIsElementCenter = false;
    std::size_t endpointCenterElementId = 0;

    if (const auto hit = pointAt(event->position(), endpointTolerance)) {
      endpointElementId = hit->elementId;
      endpointDashed = false;
      if (hit->carrierId != sketch::kInvalidGeometryId) {
        if (const auto line = sketch_.lineIndex(hit->carrierId))
          endpointDashed = sketch_.lines()[*line].dashed;
        else if (const auto circle = sketch_.circleIndex(hit->carrierId))
          endpointDashed = sketch_.circles()[*circle].dashed;
        else if (const auto arc = sketch_.arcIndex(hit->carrierId))
          endpointDashed = sketch_.arcs()[*arc].dashed;
      }
      switch (hit->kind) {
        case SketchPickPointKind::LineEndpoint:
          endpoint = hit->reference;
          break;
        case SketchPickPointKind::CircleCenter:
          endpointIsCircleCenter = true;
          endpointCircleId = hit->carrierId;
          break;
        case SketchPickPointKind::ArcEndpoint:
          endpointIsArc = true;
          endpointArcId = hit->carrierId;
          endpointArcStart = hit->reference.start;
          break;
        case SketchPickPointKind::ElementCenter:
          endpointIsElementCenter = true;
          endpointCenterElementId = hit->elementId;
          break;
        case SketchPickPointKind::LineMidpoint:
          break;
      }
    }

    const bool hasSeveralSelected =
        selectedLineIds_.size() + selectedElementIds_.size() +
            selectedCircleIds_.size() +
            selectedArcIds_.size() > 1;

    // Once several objects are selected, clicking any selected object means
    // "move the selection". Endpoint editing still works normally for a
    // single object.
    const bool endpointSelected =
        endpointIsArc
            ? arcSelected(endpointArcId)
            : endpointIsCircleCenter
                  ? circleSelected(endpointCircleId)
                  : endpointIsElementCenter
                        ? lineElementSelected(endpointCenterElementId)
                        : endpoint && lineSelected(endpoint->lineId);

    if ((endpoint || endpointIsArc || endpointIsCircleCenter ||
         endpointIsElementCenter) &&
        !(hasSeveralSelected &&
          endpointSelected)) {
      clearGeometrySelection();
      if (endpointIsArc) {
        selectedArcIds_.push_back(endpointArcId);
        selectionKind_ = SelectionKind::Arc;
        selectionArcId_ = endpointArcId;
        selectionLineId_ = sketch::kInvalidGeometryId;
        selectionCircleId_ = sketch::kInvalidGeometryId;
        selectionElementId_ = 0;

        interaction_.beginPointDrag(
            SketchArcEndpointDrag{endpointArcId, endpointArcStart});
      } else if (endpointIsCircleCenter) {
        selectedCircleIds_.push_back(endpointCircleId);
        selectionKind_ = SelectionKind::Circle;
        selectionCircleId_ = endpointCircleId;
        selectionLineId_ = sketch::kInvalidGeometryId;
        selectionArcId_ = sketch::kInvalidGeometryId;
        selectionElementId_ = 0;
        interaction_.beginPointDrag(
            SketchCircleCenterDrag{endpointCircleId});
      } else if (endpointIsElementCenter) {
        selectedElementIds_.push_back(endpointCenterElementId);
        selectionKind_ = SelectionKind::Line;
        selectionElementId_ = endpointCenterElementId;
        selectionLineId_ = sketch::kInvalidGeometryId;
        for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
          if (sketch_.lines()[index].elementId == endpointCenterElementId) {
            selectionLineId_ = sketch_.lineId(index);
            endpointDashed = sketch_.lines()[index].dashed;
            break;
          }
        }
        selectionCircleId_ = sketch::kInvalidGeometryId;
        selectionArcId_ = sketch::kInvalidGeometryId;
        interaction_.beginPointDrag(
            SketchElementCenterDrag{endpointCenterElementId});
      } else {
        selectedLineIds_.push_back(endpoint->lineId);
        selectionKind_ = SelectionKind::Line;
        selectionLineId_ = endpoint->lineId;
        selectionElementId_ = endpointElementId;
        selectionCircleId_ = sketch::kInvalidGeometryId;
        selectionArcId_ = sketch::kInvalidGeometryId;

        interaction_.beginPointDrag(
            SketchLineEndpointDrag{endpoint->lineId, endpoint->start});
      }

      pushUndoState();
      interaction_.updatePointDrag(snappedPoint(event->position()));
      constructionHover_.reset();

      emit lineStyleSelectionChanged(true, endpointDashed);
      emit selectionChanged(
          endpointIsArc
              ? QString::fromUtf8("Выбрана точка дуги")
              : endpointIsCircleCenter
                    ? QString::fromUtf8("Выбран центр окружности")
                    : endpointIsElementCenter
                          ? QString::fromUtf8("Выбран центр объекта")
                          : QString::fromUtf8("Выбрана точка линии"));
      update();
    } else {
      interaction_.cancelPointDrag();

      selectAt(event->position(), false, true);
      if (selectionKind_ != SelectionKind::None) {
        pushUndoState();
        interaction_.beginPointDrag(std::monostate{},
                                    snappedPoint(event->position()));
      }
    }  } else {
    const auto constructionSnap =
        constructionSnapAt(event->position());
    const sketch::Point constructionPoint =
        constructionSnap.point;

    if (constructionSnap.kind != ConstructionSnapKind::None)
      constructionHover_ = constructionSnap;
    else
      constructionHover_.reset();

    // Preserve the existing direct PointOnLine path for Line. Other creation
    // tools are persisted by autoCoincidentNewGeometry() after their geometry
    // has been materialized.
    if (tool() == Tool::Line) {
      auto lineCreation = interactionState().lineCreation;
      auto& carrier = interactionState().creation.anchor ? lineCreation.endLineCarrier
                             : lineCreation.startLineCarrier;
      auto& midpoint = interactionState().creation.anchor ? lineCreation.endMidpointCarrier
                              : lineCreation.startMidpointCarrier;
      carrier.reset();
      midpoint.reset();

      if (constructionSnap.kind == ConstructionSnapKind::LineBody &&
          constructionSnap.geometryId != sketch::kInvalidGeometryId) {
        carrier = constructionSnap.geometryId;
      } else if (constructionSnap.kind == ConstructionSnapKind::LineMidpoint &&
                 constructionSnap.geometryId != sketch::kInvalidGeometryId) {
        midpoint = constructionSnap.geometryId;
      }

      auto& arcCarrier = interactionState().creation.anchor ? lineCreation.endArcCarrier
                                : lineCreation.startArcCarrier;
      arcCarrier.reset();

      // constructionSnapAt() reuses CircleBody for curved carrier bodies.
      // GeometryId tells us whether the hit belongs to a Circle or an Arc.
      if (constructionSnap.kind == ConstructionSnapKind::CircleBody &&
          constructionSnap.geometryId != sketch::kInvalidGeometryId &&
          sketch_.arcIndex(constructionSnap.geometryId)) {
        arcCarrier = constructionSnap.geometryId;
      }
      interaction_.updateLineCreation(std::move(lineCreation));
    }

    commitPoint(constructionPoint);
  }
}

void SketchCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }

  const auto dimensionHit = dimensionAt(event->position());
  if (tool() == Tool::Mirror) {
    const auto seed = mirrorGeometryAt(event->position());
    const auto contour = seed ? closedMirrorContour(*seed)
                              : std::vector<MirrorGeometryRef>{};
    if (!contour.empty()) {
      setMirrorSourceSelection(contour);
      emit selectionChanged(QString::fromUtf8(
          "Зеркало: контур выбран, теперь выберите прямую-ось"));
      event->accept();
      update();
      return;
    }
    emit selectionChanged(QString::fromUtf8(
        "Зеркало: выбранная цепочка не образует однозначный замкнутый контур"));
    event->accept();
    return;
  }

  if (tool() == Tool::Select && !dimensionHit.has_value()) {
    const auto seed = lineAt(event->position());
    const auto contour = seed ? closedLineContour(*seed)
                              : std::vector<sketch::GeometryId>{};
    if (!contour.empty()) {
      clearGeometrySelection();
      selectedLineIds_ = contour;
      selectionKind_ = SelectionKind::Line;
      selectionLineId_ = *seed;
      if (const auto index = sketch_.lineIndex(*seed)) {
        selectionElementId_ = sketch_.lines()[*index].elementId;
        emit lineStyleSelectionChanged(true,
                                       sketch_.lines()[*index].dashed);
      }
      emit selectionChanged(QString::fromUtf8(
          "Выбран замкнутый контур: %1 линий").arg(contour.size()));
      event->accept();
      update();
      return;
    }
  }

  const auto found = dimensionHit;

  if (!found ||
      *found >= sketch_.dimensions().size()) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }

  const auto dimension =
      sketch_.dimensions()[*found];

  setTool(Tool::AutoDimension);

  const auto reference = dimensionReference(*found);
  if (!reference) return;
  interaction_.beginDimensionEdit(*reference);

  // CRASH-FREE 13: KEEP EDITED DIMENSION SELECTED
  auto autoDimension = interactionState().autoDimension;
  autoDimension.offsetMm = dimension.offsetMm;
  autoDimension.angleRad = dimension.angleRad;

  if (dimension.kind ==
      sketch::DimensionKind::LineLength) {
    autoDimension.target = SketchAutoDimensionTarget::Line;
    autoDimension.geometryId = dimension.geometryId;
    primaryDimension_->setPrefix(QString());

  } else if (dimension.kind ==
             sketch::DimensionKind::CircleDiameter) {
    autoDimension.target = SketchAutoDimensionTarget::Circle;
    autoDimension.geometryId = dimension.geometryId;
    primaryDimension_->setPrefix(
        QString::fromUtf8("Г: "));

  } else if (dimension.kind ==
             sketch::DimensionKind::LineAngle) {
    autoDimension.target = SketchAutoDimensionTarget::Angle;
    autoDimension.angleFirstLine = dimension.geometryId;
    autoDimension.angleSecondLine = dimension.secondPoint.lineId;
    primaryDimension_->setPrefix(QString());

  } else {
    autoDimension.target = SketchAutoDimensionTarget::Points;

    if (dimension.kind ==
        sketch::DimensionKind::PointDistanceX)
      autoDimension.pointMode = SketchPointDimensionMode::X;
    else if (dimension.kind ==
             sketch::DimensionKind::PointDistanceY)
      autoDimension.pointMode = SketchPointDimensionMode::Y;
    else
      autoDimension.pointMode = SketchPointDimensionMode::Aligned;

    autoDimension.firstPoint = dimension.firstPoint;
    autoDimension.secondPoint = dimension.secondPoint;
    autoDimension.datumModeFixed =
        dimension.firstPoint.origin || dimension.secondPoint.origin;

    primaryDimension_->setPrefix(QString());
  }

  interaction_.updateAutoDimension(std::move(autoDimension));

  if (dimension.kind ==
      sketch::DimensionKind::LineAngle) {
    primaryDimension_->setSuffix(
        QString(QChar(0x00B0)));
    primaryDimension_->setRange(0.01, 179.99);

    const auto firstIndex =
        sketch_.lineIndex(dimension.geometryId);
    const auto secondIndex =
        sketch_.lineIndex(
            dimension.secondPoint.lineId);

    if (firstIndex && secondIndex) {
      primaryDimension_->setValue(
          visibleLineAngleDegrees(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex]));
    } else {
      primaryDimension_->setValue(
          dimension.valueMm);
    }
  } else {
    primaryDimension_->setSuffix(QString::fromUtf8("\xD0\xBC\xD0\xBC"));
    primaryDimension_->setRange(
        0.01, 100000.0);
    primaryDimension_->setValue(
        dimension.valueMm);
  }

  secondaryDimension_->hide();

  primaryDimension_->move(
      (event->position() +
       QPointF(16, 16)).toPoint());

  primaryDimension_->show();
  primaryDimension_->setFocus();
  primaryDimension_->selectAll();

  event->accept();
  update();
}

void SketchCanvas::mouseMoveEvent(QMouseEvent* event) {
  if (cubePressed_) {
    event->accept();
    return;
  }

  if (interactionState().camera.kind ==
      SketchCameraGestureState::Kind::Orbit) {
    const auto dragButton = interactionState().camera.button ==
                                    SketchCameraGestureState::Button::Right
                                ? Qt::RightButton
                                : Qt::MiddleButton;
    if (event->buttons().testFlag(dragButton)) {
      const QPointF current = event->position();
      const QPointF last(interactionState().camera.lastX,
                         interactionState().camera.lastY);
      const QPointF delta = current - last;
      interaction_.updateCameraGesture(
          current.x(), current.y(),
          dragButton == Qt::RightButton &&
              (std::abs(delta.x()) >= 0.5 || std::abs(delta.y()) >= 0.5));
      orbitView(delta.x() * 0.55, delta.y() * 0.55);
      event->accept();
      return;
    }
  }

  if (interactionState().selectionBox.active &&
      (event->buttons() & Qt::LeftButton)) {
    interaction_.updateSelectionBox(
        {event->position().x(), event->position().y()});
    update();
    event->accept();
    return;
  }

  if (interactionState().camera.kind ==
          SketchCameraGestureState::Kind::Pan &&
      (event->buttons() & Qt::MiddleButton)) {
    const QPointF current = event->position();
    const QPointF last(interactionState().camera.lastX,
                       interactionState().camera.lastY);
    const QPointF delta = current - last;
    interaction_.panCameraBy(delta.x(), delta.y(), current.x(), current.y());
    update();
    event->accept();
    return;
  }
  if (event->buttons() == Qt::NoButton) {
    const ViewCubeHit hit =
        viewCubeGeometry(size(), viewCubeCamera()).hitTest(event->position());
    if (hit) {
      if (!(cubeHover_ == hit))
        QToolTip::showText(event->globalPosition().toPoint(),
                           viewCubeToolTip(hit), this);
      cubeHover_ = hit;
      setCursor(Qt::PointingHandCursor);
      update();
      event->accept();
      return;
    }
    clearViewCubeHover();
  }

  if (event->buttons() == Qt::NoButton &&
      !screenToSketchMappingAvailable()) {
    constructionHover_.reset();
    interaction_.updateTrim({});
    hoveredProjectionEdge_.reset();
    setCursor(Qt::OpenHandCursor);
    event->accept();
    return;
  }

  if (event->buttons() == Qt::NoButton &&
      tool() == Tool::Projection) {
    const auto previous = hoveredProjectionEdge_;
    hoveredProjectionEdge_ = referenceEdgeAt(event->position());
    setCursor(hoveredProjectionEdge_
                  ? Qt::PointingHandCursor
                  : Qt::CrossCursor);
    if (previous != hoveredProjectionEdge_) update();
    event->accept();
    return;
  }

  if (event->buttons() == Qt::NoButton && tool() == Tool::Trim) {
    interaction_.updateTrim({trimPreviewAt(event->position())});
    setCursor(interactionState().trim.preview ? Qt::PointingHandCursor : Qt::CrossCursor);
    update();
    event->accept();
    return;
  }

  const bool constraintTool =
      (tool() == Tool::AutoDimension &&
       interactionState().autoDimension.target ==
           SketchAutoDimensionTarget::None) ||
      tool() == Tool::LockConstraint ||
      tool() == Tool::OrthogonalConstraint ||
      tool() == Tool::CoincidentConstraint ||
      tool() == Tool::PerpendicularConstraint ||
      tool() == Tool::ParallelConstraint ||
      tool() == Tool::EqualConstraint ||
      tool() == Tool::TangentConstraint;

  if (event->buttons() == Qt::NoButton && constraintTool) {
    constexpr double hitTolerance = 9.0;
    int hoverKind = 0;  // 1 line, 2 circle, 3 arc, 4 point, 5/6 datum axes
    sketch::GeometryId hoverId = sketch::kInvalidGeometryId;
    sketch::Point hoverGeometryPoint{};

    const bool lineAllowed =
        tool() != Tool::TangentConstraint ||
        !interactionState().constraint.tangentFirst ||
        interactionState().constraint.tangentFirst->kind !=
            SketchGeometryOperandKind::Line;
    const bool circleAllowed =
        tool() == Tool::AutoDimension ||
        tool() == Tool::LockConstraint ||
        tool() == Tool::CoincidentConstraint ||
        tool() == Tool::EqualConstraint ||
        (tool() == Tool::TangentConstraint &&
         (!interactionState().constraint.tangentFirst ||
          interactionState().constraint.tangentFirst->kind ==
              SketchGeometryOperandKind::Line));
    const bool arcAllowed =
        tool() == Tool::LockConstraint ||
        tool() == Tool::CoincidentConstraint ||
        (tool() == Tool::TangentConstraint &&
         (!interactionState().constraint.tangentFirst ||
          interactionState().constraint.tangentFirst->kind ==
              SketchGeometryOperandKind::Line));

    SketchHitTolerancePolicy tolerance;
    tolerance.entityPx = hitTolerance;
    tolerance.pointPx = hitTolerance;
    tolerance.endpointPx = hitTolerance;
    auto scene = hitScene(tolerance, tool() == Tool::AutoDimension ||
                                         tool() == Tool::CoincidentConstraint);
    sketch::GeometryId excludedLine = sketch::kInvalidGeometryId;
    if (tool() == Tool::ParallelConstraint &&
        interactionState().constraint.parallelFirstLine)
      excludedLine = *interactionState().constraint.parallelFirstLine;
    else if (tool() == Tool::PerpendicularConstraint &&
             interactionState().constraint.perpendicularFirstLine)
      excludedLine = *interactionState().constraint.perpendicularFirstLine;
    for (auto& candidate : scene.candidates) {
      const auto* entity = std::get_if<SketchPickEntityRef>(&candidate.target);
      if (entity && entity->geometryId == excludedLine)
        candidate.enabled = false;
    }
    SketchPickFilter filter;
    filter.lines = lineAllowed;
    filter.circles = circleAllowed;
    filter.arcs = arcAllowed;
    filter.points = tool() == Tool::CoincidentConstraint ||
                    tool() == Tool::AutoDimension;
    filter.lineMidpoints = false;
    filter.datums = tool() == Tool::AutoDimension ||
                    tool() == Tool::CoincidentConstraint;
    filter.projections = false;
    filter.dimensions = false;
    if (const auto hit = SketchHitTester::pick(
            scene, hitPoint(event->position()), filter)) {
      if (const auto* point = hit->point()) {
        hoverKind = 4;
        hoverGeometryPoint = point->point;
      } else if (const auto* entity = hit->entity()) {
        hoverId = entity->geometryId;
        hoverKind = entity->kind == SketchPickEntityKind::Line
                        ? 1
                    : entity->kind == SketchPickEntityKind::Circle ? 2
                                                                    : 3;
      } else if (const auto* datum = hit->datum()) {
        switch (datum->kind) {
          case SketchPickDatumKind::Origin:
            hoverKind = 4;
            hoverGeometryPoint = {0.0, 0.0};
            break;
          case SketchPickDatumKind::XAxis: hoverKind = 5; break;
          case SketchPickDatumKind::YAxis: hoverKind = 6; break;
        }
      }
    }

    auto constraint = interactionState().constraint;
    if (hoverKind == 0) {
      constraint.hoverOperand.reset();
      constraint.hoverPoint.reset();
    } else {
      const auto kind = hoverKind == 1
                            ? SketchGeometryOperandKind::Line
                        : hoverKind == 2
                            ? SketchGeometryOperandKind::Circle
                        : hoverKind == 3
                            ? SketchGeometryOperandKind::Arc
                        : hoverKind == 4
                            ? SketchGeometryOperandKind::Point
                        : hoverKind == 5
                            ? SketchGeometryOperandKind::XAxis
                            : SketchGeometryOperandKind::YAxis;
      constraint.hoverOperand = SketchGeometryOperand{kind, hoverId, 0};
      if (hoverKind == 4) {
        constraint.hoverPoint = hoverGeometryPoint;
      } else {
        constraint.hoverPoint.reset();
      }
    }
    interaction_.updateConstraintOperands(std::move(constraint));
    setCursor(hoverKind != 0 ? Qt::PointingHandCursor : Qt::CrossCursor);
    update();
    event->accept();
    return;
  }

  if (!constraintTool && interactionState().constraint.hoverOperand) {
    auto constraint = interactionState().constraint;
    constraint.hoverOperand.reset();
    constraint.hoverPoint.reset();
    interaction_.updateConstraintOperands(std::move(constraint));
  }

  const bool creationTool =
      tool() == Tool::Line ||
      tool() == Tool::Rectangle ||
      tool() == Tool::Circle ||
      tool() == Tool::Arc;

  if (creationTool) {
    const auto constructionSnap =
        constructionSnapAt(event->position());
    hoverPoint_ = constructionSnap.point;

    if (constructionSnap.kind != ConstructionSnapKind::None)
      constructionHover_ = constructionSnap;
    else
      constructionHover_.reset();

    // Hover highlighting is part of the construction preview even before the
    // first point has been committed.
    update();
  } else {
    constructionHover_.reset();
    hoverPoint_ = snappedPoint(event->position());
  }

  // ARC CHORD / SAGITTA LIVE DRIVE
  //
  // Stage 1: first point is fixed; cursor drives the chord endpoint and L.
  // Stage 2: both endpoints are fixed; cursor drives signed sagitta and H.
  if (tool() == Tool::Arc && !interactionState().creation.arcPoints.empty()) {
    primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    primaryDimension_->setDecimals(2);
    primaryDimension_->setRange(0.01, 100000.0);
    secondaryDimension_->hide();

    if (!primaryDimension_->isVisible()) {
      primaryDimension_->show();
      primaryDimension_->raise();
    }

    if (interactionState().creation.arcPoints.size() == 1) {
      primaryDimension_->setPrefix(QString::fromUtf8("L: "));

      const auto first = interactionState().creation.arcPoints[0];
      double dx = hoverPoint_.xMm - first.xMm;
      double dy = hoverPoint_.yMm - first.yMm;
      double length = std::hypot(dx, dy);

      if (length > 1e-9) {
        auto arc = interactionState().arc;
        arc.chordAngleRad = std::atan2(dy, dx);
        interaction_.updateArcCreation(std::move(arc));
      }

      if (interactionState().arc.dimensionKeyboardEdit) {
        const double angle = interactionState().arc.chordAngleRad.has_value()
                                 ? interactionState().arc.chordAngleRad.value_or(0.0)
                                 : 0.0;
        const double requested =
            std::max(0.01, primaryDimension_->value());
        hoverPoint_ = {first.xMm + requested * std::cos(angle),
                       first.yMm + requested * std::sin(angle)};
      } else if (length > 1e-9) {
        const QSignalBlocker blocker(primaryDimension_);
        primaryDimension_->setValue(length);
        primaryDimension_->move(
            (event->position() + QPointF(18.0, 18.0)).toPoint());
        if (primaryDimension_->hasFocus())
          primaryDimension_->selectAll();
      }
    } else if (interactionState().creation.arcPoints.size() == 2) {
      primaryDimension_->setPrefix(QString::fromUtf8("H: "));

      const auto first = interactionState().creation.arcPoints[0];
      const auto last = interactionState().creation.arcPoints[1];

      if (interactionState().arc.dimensionKeyboardEdit) {
        const double sign =
            interactionState().arc.sagittaSign.has_value()
                ? interactionState().arc.sagittaSign.value_or(1.0)
                : 1.0;
        hoverPoint_ = arcSagittaPoint(
            first, last, sign * std::max(0.01, primaryDimension_->value()));
      } else {
        double signedSagitta = signedArcSagitta(first, last, hoverPoint_);
        double sign = signedSagitta < 0.0 ? -1.0 : 1.0;
        if (std::abs(signedSagitta) <= 1e-9 &&
            interactionState().arc.sagittaSign.has_value())
          sign = interactionState().arc.sagittaSign.value_or(1.0);

        const double magnitude = std::max(0.01, std::abs(signedSagitta));
        auto arc = interactionState().arc;
        arc.sagittaSign = sign;
        interaction_.updateArcCreation(std::move(arc));
        hoverPoint_ = arcSagittaPoint(first, last, sign * magnitude);

        const QSignalBlocker blocker(primaryDimension_);
        primaryDimension_->setValue(magnitude);
        primaryDimension_->move(
            (event->position() + QPointF(18.0, 18.0)).toPoint());
        if (primaryDimension_->hasFocus())
          primaryDimension_->selectAll();
      }
    }

    update();
  }

  // TWO-TANGENT MOUSE DIAMETER DRIVE
  if (tool() == Tool::Circle &&
      circleMode_ ==
          CircleMode::TwoTangentsRadius &&
      interactionState().creation.circleGuideIds.size() == 2 &&
      interactionState().twoTangentRadiusPreviewActive) {
    const auto circleGuides = resolvedCircleGuideLines();
    if (!circleGuides || circleGuides->size() != 2) {
      interaction_.clearCircleGuides();
      interaction_.setTwoTangentPreview(false);
      update();
      return;
    }
    primaryDimension_->setPrefix(
        QString::fromUtf8("Ø "));
    primaryDimension_->setSuffix(
        QString::fromUtf8(" мм"));
    primaryDimension_->setRange(
        0.02, 100000.0);

    if (!primaryDimension_->isVisible()) {
      primaryDimension_->show();
      primaryDimension_->raise();
    }

    if (!primaryDimension_->hasFocus()) {
      const auto cursorPreview =
          twoTangentCircleFromCursor(
              (*circleGuides)[0], (*circleGuides)[1],
              hoverPoint_);

      if (cursorPreview) {
        const auto finitePreview =
            clampedTwoTangentCircleForRadius(
                (*circleGuides)[0], (*circleGuides)[1],
                cursorPreview->radiusMm,
                hoverPoint_);

        if (finitePreview) {
          circleDiameterMm_ =
              finitePreview->radiusMm * 2.0;

          const QSignalBlocker blocker(
              primaryDimension_);

          primaryDimension_->setValue(
              circleDiameterMm_);

          primaryDimension_->move(
              (event->position() +
               QPointF(18.0, 18.0)).toPoint());

          emit primaryDimensionChanged(
              circleDiameterMm_);
        }
      }
    } else {
      const auto finitePreview =
          clampedTwoTangentCircleForRadius(
              (*circleGuides)[0], (*circleGuides)[1],
              primaryDimension_->value() * 0.5,
              hoverPoint_);

      if (finitePreview) {
        const double actualDiameter =
            finitePreview->radiusMm * 2.0;

        circleDiameterMm_ =
            actualDiameter;

        if (std::abs(
                primaryDimension_->value() -
                actualDiameter) > 1e-6) {
          const QSignalBlocker blocker(
              primaryDimension_);
          primaryDimension_->setValue(
              actualDiameter);
          primaryDimension_->selectAll();
        }
      }
    }

    update();
  }
  if (interactionState().dimension.draggingLabel.has_value() &&
      (event->buttons() & Qt::LeftButton)) {
    const auto index = dimensionIndex(
        *interactionState().dimension.draggingLabel)
                           .value_or(static_cast<std::size_t>(-1));

    if (index < sketch_.dimensions().size() &&
        sketch_.dimensions()[index].kind ==
            sketch::DimensionKind::LineAngle) {
      const auto& dimension = sketch_.dimensions()[index];
      const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);

      if (firstIndex && secondIndex) {
        const auto& firstLine = sketch_.lines()[*firstIndex];
        const auto& secondLine = sketch_.lines()[*secondIndex];
        const auto center = lineIntersectionScreen(
            firstLine, secondLine,
            [this](sketch::Point point) { return mapPoint(point); });

        if (center) {
          const auto rays = angleSectorRays(
              firstLine, secondLine, *center, dimension.offsetMm,
              [this](sketch::Point point) { return mapPoint(point); });

          if (rays) {
            const QPointF firstDirection = rays->first;
            const QPointF secondDirection = rays->second;

            double startDeg =
                -std::atan2(firstDirection.y(), firstDirection.x()) *
                180.0 / 3.14159265358979323846;
            double endDeg =
                -std::atan2(secondDirection.y(), secondDirection.x()) *
                180.0 / 3.14159265358979323846;
            double spanDeg = endDeg - startDeg;
            while (spanDeg <= -180.0) spanDeg += 360.0;
            while (spanDeg > 180.0) spanDeg -= 360.0;

            const double radius = angularDimensionRadiusPx(
                dimension.offsetMm, pixelsPerMm_,
                std::min(width(), height()));
            const double midDeg = startDeg + spanDeg * 0.5;
            const double midRad =
                midDeg * 3.14159265358979323846 / 180.0;

            const QPointF defaultLabelCenter =
                *center +
                QPointF(std::cos(midRad), -std::sin(midRad)) *
                    (radius + 18.0);

            const QPointF delta =
                event->position() - defaultLabelCenter;

            interaction_.setDimensionLabelPosition(
                index, delta.x() / pixelsPerMm_,
                delta.y() / pixelsPerMm_);
            update();
          }
        }
      }

      event->accept();
      return;
    }

    QPointF first;
    QPointF second;
    if (dimensionSegment(index, first, second)) {
      QPointF direction = second - first;
      const double length = std::hypot(direction.x(), direction.y());
      if (length > 1.0) {
        direction /= length;
        const QPointF normal(-direction.y(), direction.x());
        const QPointF delta = event->position() - (first + second) * 0.5;
        interaction_.setDimensionLabelPosition(
            index, QPointF::dotProduct(delta, direction) / pixelsPerMm_,
            QPointF::dotProduct(delta, normal) / pixelsPerMm_);
        update();
      }
    }
    event->accept();
    return;
  }
  if (interactionState().dimension.draggingLine.has_value() &&
      (event->buttons() & Qt::LeftButton)) {
    const auto index = dimensionIndex(
        *interactionState().dimension.draggingLine)
                           .value_or(static_cast<std::size_t>(-1));
    if (index < sketch_.dimensions().size()) {
      const auto& dimension = sketch_.dimensions()[index];
      if (dimension.kind == sketch::DimensionKind::LineAngle) {
        const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
        const auto secondIndex =
            sketch_.lineIndex(dimension.secondPoint.lineId);

        if (firstIndex && secondIndex) {
          const auto center = lineIntersectionScreen(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex],
              [this](sketch::Point point) { return mapPoint(point); });

          if (center) {
            const double radiusPixels =
                QLineF(*center, event->position()).length();
            const double sideSign =
                dimension.offsetMm < 0.0 ? -1.0 : 1.0;
            if (!executeLiveCommand(SetDimensionPlacementCommand{
                     dimension.id,
                     sideSign * std::max(3.0, radiusPixels / pixelsPerMm_),
                     dimension.angleRad})
                     .accepted) {
              event->accept();
              return;
            }
          }
        }
      } else if (dimension.kind == sketch::DimensionKind::CircleDiameter) {
        const auto circleIndex = sketch_.circleIndex(dimension.geometryId);
        if (circleIndex) {
          const QPointF center =
              mapPoint(sketch_.circles()[*circleIndex].center);
          const QPointF delta = event->position() - center;
          if (std::hypot(delta.x(), delta.y()) > 2.0)
            if (!executeLiveCommand(SetDimensionPlacementCommand{
                     dimension.id, dimension.offsetMm,
                     std::atan2(-delta.y(), delta.x())})
                     .accepted) {
              event->accept();
              return;
            }
        }
      } else {
        QPointF first;
        QPointF second;
        if (dimensionSegment(index, first, second)) {
          QPointF direction = second - first;
          const double length = std::hypot(direction.x(), direction.y());
          if (length > 1.0) {
            direction /= length;
            const QPointF normal(-direction.y(), direction.x());
            const QPointF rawFirst =
                first - normal * dimension.offsetMm * pixelsPerMm_;
            const double signedPixels =
                QPointF::dotProduct(event->position() - rawFirst, normal);
            if (!executeLiveCommand(SetDimensionPlacementCommand{
                     dimension.id, signedPixels / pixelsPerMm_,
                     dimension.angleRad})
                     .accepted) {
              event->accept();
              return;
            }
          }
        }
      }
      update();
    }
    event->accept();
    return;
  }
  if (tool() == Tool::AutoDimension && primaryDimension_->isVisible() &&
      interactionState().autoDimension.target != SketchAutoDimensionTarget::None) {
    // CRASH-FREE 13: EXISTING DIMENSION EDIT LOCK
    //
    // Double-click restoration already sets the exact stored dimension kind:
    // aligned / X / Y / line / circle / angle. Cursor movement must not run
    // the new-dimension heuristic again, must not move the editor and must not
    // alter annotation placement. Only the typed numeric value is editable.
    if (interactionState().dimension.editing.has_value()) {
      update();
    } else {
    auto automatic = interactionState().autoDimension;
    const auto target = interactionState().autoDimension.target;

    if (target == SketchAutoDimensionTarget::Angle) {
      const auto firstId = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.angleFirstLine.value_or(sketch::kInvalidGeometryId));
      const auto secondId = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.angleSecondLine.value_or(sketch::kInvalidGeometryId));
      const auto firstIndex = sketch_.lineIndex(firstId);
      const auto secondIndex = sketch_.lineIndex(secondId);

      if (firstIndex && secondIndex) {
        const auto center = lineIntersectionScreen(
            sketch_.lines()[*firstIndex], sketch_.lines()[*secondIndex],
            [this](sketch::Point point) { return mapPoint(point); });
        if (center) {
          const double radiusPixels =
              QLineF(*center, event->position()).length();

          // Preserve the side the user places the dimension on: the sign of
          // offsetMm flips the angle arc between the two vertical sectors.
          const auto rays = angleSectorRays(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex],
              *center, 0.0,
              [this](sketch::Point point) { return mapPoint(point); });
          double sideSign = 1.0;
          if (rays) {
            const QPointF bisector =
                rays->first + rays->second;
            const QPointF cursorDir = event->position() - *center;
            if (QPointF::dotProduct(cursorDir, bisector) < 0.0)
              sideSign = -1.0;
          }

          automatic.offsetMm =
              sideSign * std::max(3.0, radiusPixels / pixelsPerMm_);
          primaryDimension_->move(
              (event->position() + QPointF(16, 16)).toPoint());
        }
      }
    } else if (target == SketchAutoDimensionTarget::Circle) {
      const auto id = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.geometryId.value_or(sketch::kInvalidGeometryId));
      const auto index = sketch_.circleIndex(id);
      if (index) {
        const auto center =
            mapPoint(sketch_.circles()[*index].center);
        const QPointF delta = event->position() - center;
        if (std::hypot(delta.x(), delta.y()) > 2.0)
          automatic.angleRad = std::atan2(-delta.y(), delta.x());
      }
    } else if (target == SketchAutoDimensionTarget::Line) {
      const auto id = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.geometryId.value_or(sketch::kInvalidGeometryId));
      const auto index = sketch_.lineIndex(id);
      if (index) {
        const QPointF first =
            mapPoint(sketch_.lines()[*index].start);
        const QPointF second =
            mapPoint(sketch_.lines()[*index].end);

        QPointF direction = second - first;
        const double length =
            std::hypot(direction.x(), direction.y());
        if (length > 1.0) {
          direction /= length;
          const QPointF normal(-direction.y(), direction.x());
          const double signedPixels =
              QPointF::dotProduct(event->position() - first,
                                  normal);
          automatic.offsetMm = signedPixels / pixelsPerMm_;
        }
      }
    } else if (target == SketchAutoDimensionTarget::Points) {
      sketch::PointReference firstReference{
          static_cast<sketch::GeometryId>(
              interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).lineId),
          interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).start,
          static_cast<sketch::GeometryId>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).circleId),
        static_cast<std::size_t>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).elementCenterId)};
      firstReference.origin =
          interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).origin;
      sketch::PointReference secondReference{
          static_cast<sketch::GeometryId>(
              interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).lineId),
          interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).start,
          static_cast<sketch::GeometryId>(
            interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).circleId),
        static_cast<std::size_t>(
            interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).elementCenterId)};
      secondReference.origin =
          interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).origin;

      const auto firstPoint =
          sketch_.referencedPoint(firstReference);
      const auto secondPoint =
          sketch_.referencedPoint(secondReference);

      if (firstPoint && secondPoint) {
        const QPointF first = mapPoint(*firstPoint);
        const QPointF second = mapPoint(*secondPoint);
        const QPointF cursor = event->position();
        const QPointF midpoint = (first + second) * 0.5;
        const QPointF fromMid = cursor - midpoint;

        constexpr double axisBias = 2.20;

        const double deltaX =
            std::abs(secondPoint->xMm - firstPoint->xMm);
        const double deltaY =
            std::abs(secondPoint->yMm - firstPoint->yMm);

        // The user selects the dimension orientation visually (a horizontal
        // dimension line vs a vertical one), but x/y constraints store
        // Sketch-axis semantics. Resolve the gesture through the single
        // screen -> Sketch mapping; the mapped Sketch axis must have a
        // non-zero separation, otherwise the dimension stays aligned.
        const bool horizontalLine =
            std::abs(fromMid.y()) >
            std::abs(fromMid.x()) * axisBias;
        const bool verticalLine =
            std::abs(fromMid.x()) >
            std::abs(fromMid.y()) * axisBias;

        const bool datumModeFixed =
            interactionState().autoDimension.datumModeFixed;
        const double quarterTurns = viewRotationDeg_ / 90.0;
        const int nearestQuarterTurn = qRound(quarterTurns);
        const bool screenAxesMatchSketchAxes =
            std::abs(quarterTurns - nearestQuarterTurn) <= 1e-9;
        const QString mode =
            datumModeFixed
                ? pointDimensionModeName(interactionState().autoDimension.pointMode)
                : screenAxesMatchSketchAxes
                      ? resolvePointDimensionMode(
                            horizontalLine, verticalLine, deltaX, deltaY,
                            nearestQuarterTurn)
                      : QStringLiteral("aligned");

        automatic.pointMode = pointDimensionMode(mode);

        const auto witness = pointDimensionWitness(
            *firstPoint, *secondPoint,
            mode == QStringLiteral("x")
                ? sketch::DimensionKind::PointDistanceX
                : mode == QStringLiteral("y")
                    ? sketch::DimensionKind::PointDistanceY
                    : sketch::DimensionKind::PointDistance);
        const QPointF baseFirst = mapPoint(witness.first);
        const QPointF baseSecond = mapPoint(witness.second);

        if (mode == QStringLiteral("x")) {
          primaryDimension_->setValue(deltaX);
        } else if (mode == QStringLiteral("y")) {
          primaryDimension_->setValue(deltaY);
        } else {
          primaryDimension_->setValue(
              std::hypot(secondPoint->xMm - firstPoint->xMm,
                         secondPoint->yMm - firstPoint->yMm));
        }

        QPointF direction = baseSecond - baseFirst;
        const double length =
            std::hypot(direction.x(), direction.y());

        if (length > 1.0) {
          direction /= length;
          const QPointF normal(-direction.y(), direction.x());
          const double signedPixels =
              QPointF::dotProduct(cursor - baseFirst, normal);
          automatic.offsetMm = signedPixels / pixelsPerMm_;
        }

        // Keep the editor close to the cursor while previewing.
        primaryDimension_->move(
            (event->position() + QPointF(16, 16)).toPoint());
      }
    }

    interaction_.updateAutoDimension(std::move(automatic));
    update();
      }
}  if (interactionState().drag.active && (event->buttons() & Qt::LeftButton)) {
    sketch::Point current = snappedPoint(event->position());
    const bool pointDrag =
        !std::holds_alternative<std::monostate>(interactionState().pointDrag);
    if (pointDrag) {
      const auto snap = constructionSnapAt(event->position());
      current = snap.point;
      if (snap.kind != ConstructionSnapKind::None)
        constructionHover_ = snap;
      else
        constructionHover_.reset();
    } else {
      constructionHover_.reset();
    }
    const double dx = current.xMm - interactionState().drag.current.xMm;
    const double dy = current.yMm - interactionState().drag.current.yMm;
    bool moved = false;

    if (const auto* circleTarget = std::get_if<SketchCircleCenterDrag>(
            &interactionState().pointDrag)) {
      sketch::PointReference reference;
      reference.circleId = circleTarget->circleId;
      moved = executeLiveCommand(TranslatePointCommand{reference, dx, dy})
                  .accepted;
    } else if (const auto* centerTarget = std::get_if<SketchElementCenterDrag>(
                   &interactionState().pointDrag)) {
      sketch::PointReference reference;
      reference.elementCenterId = centerTarget->elementId;
      moved = executeLiveCommand(TranslatePointCommand{reference, dx, dy})
                  .accepted;
    } else if (const auto* arcTarget = std::get_if<SketchArcEndpointDrag>(
                   &interactionState().pointDrag)) {
      moved = executeLiveCommand(MoveArcEndpointCommand{
          arcTarget->arcId, arcTarget->start, current}).accepted;
    } else if (const auto* lineTarget = std::get_if<SketchLineEndpointDrag>(
                   &interactionState().pointDrag)) {
      const sketch::PointReference reference{lineTarget->lineId,
                                             lineTarget->start};
      moved = executeLiveCommand(TranslatePointCommand{reference, dx, dy})
                  .accepted;
    } else if (!selectedLineIds_.empty()) {
      moved = executeLiveCommand(
          TranslateLinesCommand{selectedLineIds_, dx, dy}).accepted;
    } else if (!selectedElementIds_.empty() ||
               !selectedCircleIds_.empty() ||
               !selectedArcIds_.empty()) {
      moved = executeLiveCommand(TranslateSelectionCommand{
          selectedElementIds_, selectedCircleIds_, selectedArcIds_, dx, dy})
                  .accepted;
    } else if (selectionKind_ == SelectionKind::Line) {
      moved = executeLiveCommand(
          TranslateLinesCommand{{selectionLineId_}, dx, dy}).accepted;
    } else if (selectionKind_ == SelectionKind::Circle) {
      moved = executeLiveCommand(
          TranslateCircleCommand{selectionCircleId_, dx, dy}).accepted;
    } else if (selectionKind_ == SelectionKind::Arc) {
      moved = executeLiveCommand(
          TranslateArcCommand{selectionArcId_, dx, dy}).accepted;
    }

    if (!moved) {
      event->accept();
      return;
    }
    interaction_.updatePointDrag(current);
    update();
  } else if (interactionState().creation.anchor) {
    updateDimensionEditor();
    update();
  } else if (tool() == Tool::Circle && circleMode_ != CircleMode::CenterRadius) {
    if (circleMode_ == CircleMode::TwoPoints && interactionState().creation.circlePoints.size() == 1) {
      const auto first = interactionState().creation.circlePoints.front();
      emit primaryDimensionChanged(std::hypot(hoverPoint_.xMm - first.xMm,
                                               hoverPoint_.yMm - first.yMm));
    } else if (circleMode_ == CircleMode::ThreePoints &&
               interactionState().creation.circlePoints.size() == 2) {
      const auto preview = circleThroughThreePoints(
          interactionState().creation.circlePoints[0], interactionState().creation.circlePoints[1], hoverPoint_);
      if (preview) emit primaryDimensionChanged(preview->second * 2.0);
    }
    update();
  } else if (tool() == Tool::Rectangle &&
             rectangleMode_ == RectangleMode::ThreePoints &&
             !interactionState().creation.rectanglePoints.empty()) {
    update();
  }
}

void SketchCanvas::leaveEvent(QEvent* event) {
  clearViewCubeHover();
  if (interactionState().constraint.hoverOperand) {
    auto constraint = interactionState().constraint;
    constraint.hoverOperand.reset();
    constraint.hoverPoint.reset();
    interaction_.updateConstraintOperands(std::move(constraint));
    update();
  }
  if (interactionState().trim.preview) {
    interaction_.updateTrim({});
    update();
  }
  QWidget::leaveEvent(event);
}

void SketchCanvas::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && cubePressed_) {
    const ViewCubeHit released =
        viewCubeGeometry(size(), viewCubeCamera()).hitTest(event->position());
    const ViewCubeHit pressed = cubePressed_;
    cubePressed_ = {};

    if (released == pressed) {
      if (released.zone == ViewCubeZone::Fit) {
        fitReferenceGeometry();
      } else if (released.zone == ViewCubeZone::Home) {
        animateViewToDirection(
            cubeViewingDirection(orientationFor(StandardView::Isometric)));
      } else if (released.zone == ViewCubeZone::RotateCounterClockwise) {
        animateViewRotationBy(-45.0);
      } else if (released.zone == ViewCubeZone::RotateClockwise) {
        animateViewRotationBy(45.0);
      } else {
        animateViewToDirection(released.direction);
      }
    }

    clearViewCubeHover();
    setCursor(screenToSketchMappingAvailable()
                  ? (tool() == Tool::Select ? Qt::ArrowCursor
                                           : Qt::CrossCursor)
                  : Qt::OpenHandCursor);
    event->accept();
    update();
    return;
  }

  if (interactionState().camera.kind ==
          SketchCameraGestureState::Kind::Orbit &&
      event->button() ==
          (interactionState().camera.button ==
                   SketchCameraGestureState::Button::Right
               ? Qt::RightButton
               : Qt::MiddleButton)) {
    const bool cancelInteraction =
        event->button() == Qt::RightButton &&
        !interactionState().camera.moved;
    if (cancelInteraction) {
      interaction_.cancelGesture();
      hideDimensionEditor();
    } else {
      interaction_.endCameraGesture();
    }

    setCursor(screenToSketchMappingAvailable() ?
                  (tool() == Tool::Select ? Qt::ArrowCursor
                                         : Qt::CrossCursor)
                                           : Qt::OpenHandCursor);
    update();
    event->accept();
    return;
  }

  if (event->button() == Qt::LeftButton && interactionState().selectionBox.active) {
    const auto selectionBox = interactionState().selectionBox;
    const QRectF selectionRect(
        QPointF(selectionBox.start.x, selectionBox.start.y),
        event->position());
    const bool additive = selectionBox.additive;
    interaction_.completeSelectionBox();
    setCursor(tool() == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);

    if (selectionRect.normalized().width() >= 3.0 ||
        selectionRect.normalized().height() >= 3.0) {
      selectInRect(selectionRect.normalized(), additive);
    } else if (!additive) {
      clearGeometrySelection();
      emit lineStyleSelectionChanged(false, false);
      emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
    }

    event->accept();
    update();
    return;
  }

  if (event->button() == Qt::MiddleButton &&
      interactionState().camera.kind ==
          SketchCameraGestureState::Kind::Pan) {
    interaction_.endCameraGesture();
    setCursor(screenToSketchMappingAvailable()
                  ? (tool() == Tool::Select ? Qt::ArrowCursor
                                           : Qt::CrossCursor)
                  : Qt::OpenHandCursor);
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && interactionState().drag.active) {
    std::optional<sketch::PointReference> draggedPoint;
    if (const auto* lineTarget = std::get_if<SketchLineEndpointDrag>(
            &interactionState().pointDrag)) {
      draggedPoint =
          sketch::PointReference{lineTarget->lineId, lineTarget->start};
    } else if (const auto* arcTarget = std::get_if<SketchArcEndpointDrag>(
                   &interactionState().pointDrag)) {
      sketch::PointReference endpoint;
      endpoint.arcId = arcTarget->arcId;
      endpoint.start = arcTarget->start;
      draggedPoint = endpoint;
    } else if (const auto* circleTarget = std::get_if<SketchCircleCenterDrag>(
                   &interactionState().pointDrag)) {
      sketch::PointReference center;
      center.circleId = circleTarget->circleId;
      draggedPoint = center;
    } else if (const auto* centerTarget = std::get_if<SketchElementCenterDrag>(
                   &interactionState().pointDrag)) {
      sketch::PointReference center;
      center.elementCenterId = centerTarget->elementId;
      draggedPoint = center;
    }

    // Resolve the release position again. Windows can deliver the final mouse
    // button event at an endpoint without a preceding move at that exact
    // pixel; using the stale body hover would then create PointOnLine instead
    // of the intended endpoint Coincident relation.
    std::optional<ConstructionSnap> snap;
    const auto releaseSnap = constructionSnapAt(event->position());
    if (releaseSnap.kind != ConstructionSnapKind::None)
      snap = releaseSnap;
    interaction_.completePointDrag();
    constructionHover_.reset();

    if (draggedPoint && snap) {
      const bool bound = commitDraggedPointSnap(*draggedPoint, *snap);
      emit selectionChanged(
          bound
              ? QString::fromUtf8("Точка привязана к геометрии")
              : QString::fromUtf8(
                    "Привязка не создана: конфликт зависимостей"));
    }
    notifyGeometryChanged();
    update();
  }
  if (event->button() == Qt::LeftButton &&
      interactionState().dimension.draggingLabel.has_value()) {
    interaction_.endDimensionLabelDrag();
    setCursor(tool() == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    event->accept();
  }
  if (event->button() == Qt::LeftButton &&
      interactionState().dimension.draggingLine.has_value()) {
    interaction_.endDimensionLineDrag();
    setCursor(tool() == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    notifyGeometryChanged();
    event->accept();
  }
  flushConstraintDiagnostics();
}

void SketchCanvas::keyPressEvent(QKeyEvent* event) {
  const auto copyCurrentSelection = [this]() -> bool {
    gSketchClipboard.clear();

    std::vector<sketch::GeometryId> lineIds = selectedLineIds_;
    std::vector<std::size_t> elementIds = selectedElementIds_;
    std::vector<sketch::GeometryId> circleIds = selectedCircleIds_;

    if (lineIds.empty() && elementIds.empty() && circleIds.empty()) {
      if (selectionKind_ == SelectionKind::Line &&
          selectionLineId_ != sketch::kInvalidGeometryId) {
        lineIds.push_back(selectionLineId_);
      } else if (selectionKind_ == SelectionKind::Circle &&
                 selectionCircleId_ != sketch::kInvalidGeometryId) {
        circleIds.push_back(selectionCircleId_);
      }
    }

    for (const auto lineId : lineIds) {
      const auto index = sketch_.lineIndex(lineId);
      if (!index) continue;
      const auto& line = sketch_.lines()[*index];
      SketchClipboardElement item;
      item.kind = SketchClipboardElement::Kind::Line;
      item.lineStart = line.start;
      item.lineEnd = line.end;
      gSketchClipboard.elements.push_back(item);
    }

    for (const auto elementId : elementIds) {
      std::vector<std::size_t> indices;

      for (std::size_t index = 0;
           index < sketch_.lines().size();
           ++index) {
        if (sketch_.lines()[index].elementId == elementId)
          indices.push_back(index);
      }

      if (indices.empty()) continue;

      if (indices.size() == 4) {
        SketchClipboardElement item;
        item.kind = SketchClipboardElement::Kind::Rectangle;

        for (std::size_t side = 0; side < 4; ++side)
          item.rectanglePoints[side] =
              sketch_.lines()[indices[side]].start;

        gSketchClipboard.elements.push_back(item);
        continue;
      }

      for (const auto index : indices) {
        const auto& line = sketch_.lines()[index];

        SketchClipboardElement item;
        item.kind = SketchClipboardElement::Kind::Line;
        item.lineStart = line.start;
        item.lineEnd = line.end;
        gSketchClipboard.elements.push_back(item);
      }
    }

    for (const auto circleId : circleIds) {
      const auto index = sketch_.circleIndex(circleId);
      if (!index) continue;

      SketchClipboardElement item;
      item.kind = SketchClipboardElement::Kind::Circle;
      item.circleCenter = sketch_.circles()[*index].center;
      item.circleRadiusMm = sketch_.circles()[*index].radiusMm;
      gSketchClipboard.elements.push_back(item);
    }

    gSketchClipboard.pasteGeneration = 0;
    return !gSketchClipboard.empty();
  };

  const auto pasteClipboard = [this]() -> bool {
    if (gSketchClipboard.empty()) return false;

    pushUndoState();
    ++gSketchClipboard.pasteGeneration;

    const double offsetMm =
        6.0 * static_cast<double>(gSketchClipboard.pasteGeneration);

    const std::size_t oldLineCount = sketch_.lines().size();
    const std::size_t oldCircleCount = sketch_.circles().size();

    const auto shifted =
        [offsetMm](sketch::Point point) {
          point.xMm += offsetMm;
          point.yMm -= offsetMm;
          return point;
        };

    AddPrimitiveBatchCommand batch;
    batch.primitives.reserve(gSketchClipboard.elements.size());
    for (const auto& item : gSketchClipboard.elements) {
      switch (item.kind) {
        case SketchClipboardElement::Kind::Line:
          batch.primitives.emplace_back(AddLineCommand{
              shifted(item.lineStart), shifted(item.lineEnd), std::nullopt});
          break;
        case SketchClipboardElement::Kind::Rectangle:
          batch.primitives.emplace_back(AddRectangleCommand{
              shifted(item.rectanglePoints[0]),
              shifted(item.rectanglePoints[1]),
              shifted(item.rectanglePoints[2]),
              shifted(item.rectanglePoints[3]), false});
          break;
        case SketchClipboardElement::Kind::Circle:
          batch.primitives.emplace_back(AddCircleCommand{
              shifted(item.circleCenter), item.circleRadiusMm, false});
          break;
      }
    }
    if (!executeCommand(batch).accepted) {
      cancelPendingUndo();
      return false;
    }

    clearGeometrySelection();

    for (std::size_t index = oldLineCount;
         index < sketch_.lines().size();
         ++index) {
      const auto elementId = sketch_.lines()[index].elementId;

      if (std::find(selectedElementIds_.begin(),
                    selectedElementIds_.end(),
                    elementId) == selectedElementIds_.end())
        selectedElementIds_.push_back(elementId);
    }

    for (std::size_t index = oldCircleCount;
         index < sketch_.circles().size();
         ++index) {
      const auto id = sketch_.circleId(index);
      if (id != sketch::kInvalidGeometryId)
        selectedCircleIds_.push_back(id);
    }

    if (!selectedElementIds_.empty()) {
      const auto elementId = selectedElementIds_.back();

      for (std::size_t index = sketch_.lines().size();
           index > 0;
           --index) {
        const auto current = index - 1;
        if (sketch_.lines()[current].elementId != elementId)
          continue;

        selectionKind_ = SelectionKind::Line;
        selectionElementId_ = elementId;
        selectionLineId_ = sketch_.lineId(current);
        selectionCircleId_ = sketch::kInvalidGeometryId;
        break;
      }

      emit lineStyleSelectionChanged(true, false);
    } else if (!selectedCircleIds_.empty()) {
      selectionKind_ = SelectionKind::Circle;
      selectionCircleId_ = selectedCircleIds_.back();
      selectionLineId_ = sketch::kInvalidGeometryId;
      selectionElementId_ = 0;
      emit lineStyleSelectionChanged(false, false);
    }

    emit selectionChanged(QString::fromUtf8("Вставленные объекты"));
    notifyGeometryChanged();
    update();
    return true;
  };

  // StandardKey matching is layout-aware. Ctrl+A/C/X/V therefore works
  // with both English and Russian keyboard layouts.
  if (event->matches(QKeySequence::SelectAll)) {
    clearGeometrySelection();

    for (const auto& line : sketch_.lines()) {
      if (std::find(selectedElementIds_.begin(),
                    selectedElementIds_.end(),
                    line.elementId) == selectedElementIds_.end()) {
        selectedElementIds_.push_back(line.elementId);
      }
    }

    for (std::size_t index = 0;
         index < sketch_.circles().size();
         ++index) {
      const auto id = sketch_.circleId(index);
      if (id != sketch::kInvalidGeometryId)
        selectedCircleIds_.push_back(id);
    }

    for (std::size_t index = 0;
         index < sketch_.arcs().size();
         ++index) {
      const auto id = sketch_.arcId(index);
      if (id != sketch::kInvalidGeometryId)
        selectedArcIds_.push_back(id);
    }

    if (!selectedElementIds_.empty()) {
      const auto elementId = selectedElementIds_.back();

      for (std::size_t index = sketch_.lines().size();
           index > 0;
           --index) {
        const auto current = index - 1;
        if (sketch_.lines()[current].elementId != elementId)
          continue;

        selectionKind_ = SelectionKind::Line;
        selectionElementId_ = elementId;
        selectionLineId_ = sketch_.lineId(current);
        selectionCircleId_ = sketch::kInvalidGeometryId;
        selectionArcId_ = sketch::kInvalidGeometryId;
        break;
      }

      emit lineStyleSelectionChanged(true, false);
    } else if (!selectedCircleIds_.empty()) {
      selectionKind_ = SelectionKind::Circle;
      selectionCircleId_ = selectedCircleIds_.back();
      selectionLineId_ = sketch::kInvalidGeometryId;
      selectionArcId_ = sketch::kInvalidGeometryId;
      selectionElementId_ = 0;
      emit lineStyleSelectionChanged(false, false);
    } else if (!selectedArcIds_.empty()) {
      selectionKind_ = SelectionKind::Arc;
      selectionArcId_ = selectedArcIds_.back();
      selectionLineId_ = sketch::kInvalidGeometryId;
      selectionCircleId_ = sketch::kInvalidGeometryId;
      selectionElementId_ = 0;
      emit lineStyleSelectionChanged(false, false);
    } else {
      emit lineStyleSelectionChanged(false, false);
    }

    emit selectionChanged(QString::fromUtf8("Выбраны все объекты"));
    update();

    event->accept();
    return;
  }

  if (event->matches(QKeySequence::Copy)) {
    if (copyCurrentSelection())
      emit selectionChanged(QString::fromUtf8("Объекты скопированы"));

    event->accept();
    return;
  }

  if (event->matches(QKeySequence::Cut)) {
    if (copyCurrentSelection()) {
      deleteSelection();
      emit selectionChanged(QString::fromUtf8("Объекты вырезаны"));
    }

    event->accept();
    return;
  }

  if (event->matches(QKeySequence::Paste)) {
    (void)pasteClipboard();

    event->accept();
    return;
  }

  if (event->key() == Qt::Key_Escape) {
    setTool(Tool::Select);
  } else if (event->key() == Qt::Key_Delete) {
    if (interactionState().dimension.selected) {
      const auto index = dimensionIndex(
          *interactionState().dimension.selected)
                             .value_or(static_cast<std::size_t>(-1));

      if (index < sketch_.dimensions().size()) {
        const auto id = sketch_.dimensions()[index].id;
        pushUndoState();
        const auto result = executeCommand(RemoveDimensionCommand{id});
        if (!result.accepted) {
          cancelPendingUndo();
          event->ignore();
          return;
        }
        interaction_.eraseDimensionLabel(index);
        interaction_.completeDimensionInteraction();
        hideDimensionEditor();
        notifyGeometryChanged();
        update();
        event->accept();
        return;
      }

      interaction_.selectDimension(std::nullopt);
    }

    deleteSelection();
  } else {
    QWidget::keyPressEvent(event);
  }
}

bool SketchCanvas::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == primaryDimension_ || watched == secondaryDimension_) &&
      event->type() == QEvent::KeyPress) {
    const auto* keyEvent = static_cast<QKeyEvent*>(event);

    if (tool() == Tool::Arc &&
        watched == primaryDimension_ &&
        primaryDimension_->isVisible() &&
        keyEvent->key() != Qt::Key_Tab &&
        keyEvent->key() != Qt::Key_Backtab &&
        keyEvent->key() != Qt::Key_Return &&
        keyEvent->key() != Qt::Key_Enter &&
        keyEvent->key() != Qt::Key_Escape) {
      auto arc = interactionState().arc;
      arc.dimensionKeyboardEdit = true;
      interaction_.updateArcCreation(std::move(arc));
    }
    if (keyEvent->key() == Qt::Key_Tab ||
        keyEvent->key() == Qt::Key_Backtab) {
      if (secondaryDimension_->isVisible()) {
        auto* target = watched == primaryDimension_ ? secondaryDimension_
                                                    : primaryDimension_;
        target->setFocus();
        target->selectAll();
      } else {
        primaryDimension_->setFocus();
        primaryDimension_->selectAll();
      }
      return true;
    }
    if (keyEvent->key() == Qt::Key_Return ||
        keyEvent->key() == Qt::Key_Enter) {
      // ARC NUMERIC ENTER
      if (tool() == Tool::Arc && primaryDimension_->isVisible()) {
        if (interactionState().creation.arcPoints.size() == 1) {
          const auto first = interactionState().creation.arcPoints[0];
          const double angle = interactionState().arc.chordAngleRad.has_value()
                                   ? interactionState().arc.chordAngleRad.value_or(0.0)
                                   : 0.0;
          const double length =
              std::max(0.01, primaryDimension_->value());
          const sketch::Point endpoint{
              first.xMm + length * std::cos(angle),
              first.yMm + length * std::sin(angle)};
          commitArcPoint(endpoint);
          return true;
        }

        if (interactionState().creation.arcPoints.size() == 2) {
          const double sign =
              interactionState().arc.sagittaSign.has_value()
                  ? interactionState().arc.sagittaSign.value_or(1.0)
                  : 1.0;
          const double sagitta =
              sign * std::max(0.01, primaryDimension_->value());
          commitArcPoint(
              arcSagittaPoint(interactionState().creation.arcPoints[0], interactionState().creation.arcPoints[1], sagitta));
          return true;
        }
      }

      // TWO-TANGENT NUMERIC ENTER
      if (tool() == Tool::Circle &&
          circleMode_ ==
              CircleMode::TwoTangentsRadius &&
          interactionState().creation.circleGuideIds.size() == 2 &&
          interactionState().twoTangentRadiusPreviewActive) {
        const auto circleGuides = resolvedCircleGuideLines();
        if (!circleGuides || circleGuides->size() != 2) {
          interaction_.clearCircleGuides();
          interaction_.setTwoTangentPreview(false);
          return true;
        }
        const auto finitePreview =
            clampedTwoTangentCircleForRadius(
                (*circleGuides)[0], (*circleGuides)[1],
                primaryDimension_->value() * 0.5,
                hoverPoint_);

        if (finitePreview) {
          circleDiameterMm_ =
              finitePreview->radiusMm * 2.0;

          {
            const QSignalBlocker blocker(
                primaryDimension_);
            primaryDimension_->setValue(
                circleDiameterMm_);
          }

          commitCirclePoint(hoverPoint_);
        }
        return true;
      }

      if (tool() == Tool::AutoDimension)
        commitAutoDimension();
      else
        commitDimensionEditor();
      return true;
    }
    if (keyEvent->key() == Qt::Key_Escape) {
      setTool(Tool::Select);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

bool SketchCanvas::dimensionSegment(std::size_t index, QPointF& first,
                                    QPointF& second) const {
  if (index >= sketch_.dimensions().size()) return false;

  const auto& dimension = sketch_.dimensions()[index];
  const bool diameter =
      dimension.kind == sketch::DimensionKind::CircleDiameter;

  if (diameter) {
    const auto circleIndex = sketch_.circleIndex(dimension.geometryId);
    if (!circleIndex) return false;

    const auto& circle = sketch_.circles()[*circleIndex];
    const double dx = std::cos(dimension.angleRad) * circle.radiusMm;
    const double dy = std::sin(dimension.angleRad) * circle.radiusMm;

    first = mapPoint(
        {circle.center.xMm - dx, circle.center.yMm - dy});
    second = mapPoint(
        {circle.center.xMm + dx, circle.center.yMm + dy});
    return true;
  }

  sketch::Point firstPoint;
  sketch::Point secondPoint;

  if (dimension.kind == sketch::DimensionKind::LineDistance) {
    const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
    const auto secondIndex =
        sketch_.lineIndex(dimension.secondPoint.lineId);
    if (!firstIndex || !secondIndex) return false;
    const auto witness =
        parallelLineDistanceWitness(sketch_.lines()[*firstIndex],
                                    sketch_.lines()[*secondIndex]);
    if (!witness) return false;
    firstPoint = witness->first;
    secondPoint = witness->second;
  } else if (dimension.kind == sketch::DimensionKind::LineLength) {
    const auto lineIndex = sketch_.lineIndex(dimension.geometryId);
    if (!lineIndex) return false;

    const auto& line = sketch_.lines()[*lineIndex];
    firstPoint = line.start;
    secondPoint = line.end;
  } else {
    const auto referencedFirst =
        sketch_.referencedPoint(dimension.firstPoint);
    const auto referencedSecond =
        sketch_.referencedPoint(dimension.secondPoint);

    if (!referencedFirst || !referencedSecond) return false;

    firstPoint = *referencedFirst;
    secondPoint = *referencedSecond;
  }

  // Hit testing must use exactly the same projected base line that is
  // painted for the stored dimension: project in Sketch coordinates first,
  // then map, so viewport rotation stays purely visual.
  const auto witness =
      pointDimensionWitness(firstPoint, secondPoint, dimension.kind);
  first = mapPoint(witness.first);
  second = mapPoint(witness.second);

  QPointF direction = second - first;
  const double length = std::hypot(direction.x(), direction.y());
  if (length < 1.0) return false;

  direction /= length;
  const QPointF normal(-direction.y(), direction.x());
  const QPointF offset =
      normal * dimension.offsetMm * pixelsPerMm_;

  first += offset;
  second += offset;

  return true;
}
QPointF SketchCanvas::dimensionLabelCenter(std::size_t index, QPointF first,
                                           QPointF second) const {
  QPointF direction = second - first;
  const double length = std::hypot(direction.x(), direction.y());
  if (length < 1.0) return (first + second) * 0.5;
  direction /= length;
  const QPointF normal(-direction.y(), direction.x());
  const auto& alongValues = interactionState().dimension.labelAlongMm;
  const auto& offsetValues = interactionState().dimension.labelOffsetMm;
  const double along = index < static_cast<std::size_t>(alongValues.size())
                           ? alongValues[index]
                           : 0.0;
  const double offset = index < static_cast<std::size_t>(offsetValues.size())
                            ? offsetValues[index]
                            : 2.0;
  return (first + second) * 0.5 +
         direction * along * pixelsPerMm_ + normal * offset * pixelsPerMm_;
}

bool SketchCanvas::beginDimensionLabelDrag(QPointF position) {
  const auto hit = dimensionAt(position, SketchDimensionHitKind::Label);
  if (!hit) return false;
  const auto reference = dimensionReference(*hit);
  if (!reference) return false;
  interaction_.beginDimensionLabelDrag(*reference);
  setCursor(Qt::ClosedHandCursor);
  update();
  return true;
}

bool SketchCanvas::beginDimensionLineDrag(QPointF position) {
  const auto hit = dimensionAt(position);
  if (!hit) return false;
  const auto reference = dimensionReference(*hit);
  if (!reference) return false;
  pushUndoState();
  if (!pendingUndoTransaction_) return false;
  interaction_.beginDimensionLineDrag(*reference);
  setCursor(Qt::ClosedHandCursor);
  update();
  return true;
}

std::optional<SketchDimensionReference> SketchCanvas::dimensionReference(
    std::size_t index) const {
  if (index >= sketch_.dimensions().size()) return std::nullopt;
  const auto& dimension = sketch_.dimensions()[index];
  if (dimension.id == sketch::kInvalidDimensionId) return std::nullopt;
  return SketchDimensionReference{dimension.id};
}

std::optional<std::size_t> SketchCanvas::dimensionIndex(
    const SketchDimensionReference& reference) const {
  return sketch_.dimensionIndex(reference.id);
}

std::optional<std::size_t> SketchCanvas::dimensionAt(
    QPointF position,
    std::optional<SketchDimensionHitKind> requiredKind) const {
  constexpr double hitTolerance = 8.0;
  SketchHitScene scene;
  for (std::size_t index = 0; index < sketch_.dimensions().size(); ++index) {
    const auto& dimension = sketch_.dimensions()[index];
    const auto makeCandidate = [&](SketchDimensionHitKind hitKind) {
      SketchPickCandidate candidate;
      candidate.target = SketchDimensionToken{
          index, dimension.id, hitKind, dimension.kind, dimension.geometryId,
          dimension.firstPoint, dimension.secondPoint};
      candidate.tolerancePx = hitTolerance;
      // Existing behavior resolves overlapping dimensions newest-first.
      candidate.priority = static_cast<int>(
          sketch_.dimensions().size() - 1 - index);
      candidate.stableOrder = index * 2 +
                              (hitKind == SketchDimensionHitKind::Label ? 1 : 0);
      return candidate;
    };
    auto geometryCandidate = makeCandidate(SketchDimensionHitKind::Geometry);
    auto labelCandidate = makeCandidate(SketchDimensionHitKind::Label);

    if (dimension.kind == sketch::DimensionKind::LineAngle) {
      const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);
      if (!firstIndex || !secondIndex) continue;

      const auto& firstLine = sketch_.lines()[*firstIndex];
      const auto& secondLine = sketch_.lines()[*secondIndex];

      const auto center = lineIntersectionScreen(
          firstLine, secondLine,
          [this](sketch::Point point) {
            return mapPoint(point);
          });

      if (!center) continue;

      const auto rays = angleSectorRays(
          firstLine, secondLine, *center, dimension.offsetMm,
          [this](sketch::Point point) { return mapPoint(point); });
      if (!rays) continue;

      QPointF firstDirection = rays->first;
      QPointF secondDirection = rays->second;

      double startDeg =
          -std::atan2(firstDirection.y(),
                      firstDirection.x()) *
          180.0 / 3.14159265358979323846;

      double endDeg =
          -std::atan2(secondDirection.y(),
                      secondDirection.x()) *
          180.0 / 3.14159265358979323846;

      double spanDeg = endDeg - startDeg;

      while (spanDeg <= -180.0)
        spanDeg += 360.0;
      while (spanDeg > 180.0)
        spanDeg -= 360.0;

      const double radius = angularDimensionRadiusPx(
          dimension.offsetMm, pixelsPerMm_, std::min(width(), height()));

      const QPointF arcFirst =
          *center + firstDirection * radius;
      const QPointF arcSecond =
          *center + secondDirection * radius;
      geometryCandidate.segments.push_back(hitSegment(*center, arcFirst));
      geometryCandidate.segments.push_back(hitSegment(*center, arcSecond));
      constexpr int arcSamples = 36;
      QPointF previous = arcFirst;
      for (int sample = 1; sample <= arcSamples; ++sample) {
        const double angleDeg =
            startDeg + spanDeg * static_cast<double>(sample) / arcSamples;
        const double angleRad =
            angleDeg * 3.14159265358979323846 / 180.0;
        const QPointF current =
            *center + QPointF(std::cos(angleRad), -std::sin(angleRad)) *
                          radius;
        geometryCandidate.segments.push_back(hitSegment(previous, current));
        previous = current;
      }

      const double midRad =
          (startDeg + spanDeg * 0.5) *
          3.14159265358979323846 /
          180.0;

      QPointF labelCenter =
          *center +
          QPointF(std::cos(midRad),
                  -std::sin(midRad)) *
              (radius + 18.0);

      const auto& labelX = interactionState().dimension.labelAlongMm;
      const auto& labelY = interactionState().dimension.labelOffsetMm;

      if (index <
          static_cast<std::size_t>(labelX.size()))
        labelCenter.rx() +=
            labelX[index] *
            pixelsPerMm_;

      if (index <
          static_cast<std::size_t>(labelY.size()))
        labelCenter.ry() +=
            labelY[index] *
            pixelsPerMm_;
      labelCandidate.boxes.push_back(
          {{labelCenter.x() - 48.0, labelCenter.y() - 14.0},
           {labelCenter.x() + 48.0, labelCenter.y() + 14.0}});
      scene.candidates.push_back(std::move(geometryCandidate));
      scene.candidates.push_back(std::move(labelCandidate));
      continue;
    }

    QPointF first;
    QPointF second;

    if (!dimensionSegment(index, first, second))
      continue;

    const QPointF center =
        dimensionLabelCenter(index, first, second);
    const QPointF direction = second - first;

    double angle =
        std::atan2(direction.y(), direction.x());

    if (angle > 3.141592653589793 * 0.5 ||
        angle < -3.141592653589793 * 0.5)
      angle += 3.141592653589793;

    geometryCandidate.segments.push_back(hitSegment(first, second));
    labelCandidate.orientedBox = SketchScreenOrientedBox{
        hitPoint(center), 46.0, 13.0, angle};
    scene.candidates.push_back(std::move(geometryCandidate));
    scene.candidates.push_back(std::move(labelCandidate));
  }

  SketchPickFilter filter;
  filter.entities = false;
  filter.points = false;
  filter.datums = false;
  filter.projections = false;
  if (requiredKind) {
    filter.dimensionGeometry =
        *requiredKind == SketchDimensionHitKind::Geometry;
    filter.dimensionLabels = *requiredKind == SketchDimensionHitKind::Label;
  }
  const auto hit = SketchHitTester::pick(scene, hitPoint(position), filter);
  if (!hit || !hit->dimension()) return std::nullopt;
  const auto token = *hit->dimension();
  if (token.transientSlot >= sketch_.dimensions().size()) return std::nullopt;
  const auto& current = sketch_.dimensions()[token.transientSlot];
  const auto sameReference = [](const sketch::PointReference& first,
                                const sketch::PointReference& second) {
    return first.lineId == second.lineId && first.start == second.start &&
           first.circleId == second.circleId &&
           first.elementCenterId == second.elementCenterId &&
           first.arcId == second.arcId && first.origin == second.origin;
  };
  if (current.id != token.dimensionId || current.kind != token.kind ||
      current.geometryId != token.geometryId ||
      !sameReference(current.firstPoint, token.firstPoint) ||
      !sameReference(current.secondPoint, token.secondPoint))
    return std::nullopt;
  return token.transientSlot;
}
void SketchCanvas::handleCoincidentConstraintClick(QPointF position) {
  // POINT-ON-CIRCLE PRIORITY ROUTER V3
  //
  // Keep the requested workflow deliberately narrow and deterministic:
  //   line endpoint -> circle body
  //   circle body   -> line endpoint
  //
  // If neither side of this pair is involved, fall through untouched to the
  // existing Coincident / PointOnLine logic below.
  constexpr double pocEndpointTolerance = 11.0;
  constexpr double pocCircleBodyTolerance = 11.0;
  constexpr double pocCircleCenterExclusion = 12.0;

  const auto samePointReference =
      [](sketch::PointReference first, sketch::PointReference second) {
        if (first.elementCenterId != 0 || second.elementCenterId != 0)
          return first.elementCenterId != 0 &&
                 first.elementCenterId == second.elementCenterId;
        if (first.circleId != sketch::kInvalidGeometryId ||
            second.circleId != sketch::kInvalidGeometryId)
          return first.circleId != sketch::kInvalidGeometryId &&
                 first.circleId == second.circleId;
        if (first.arcId != sketch::kInvalidGeometryId ||
            second.arcId != sketch::kInvalidGeometryId)
          return first.arcId != sketch::kInvalidGeometryId &&
                 first.arcId == second.arcId && first.start == second.start;
        return first.lineId != sketch::kInvalidGeometryId &&
               first.lineId == second.lineId && first.start == second.start;
      };

  const auto findLineEndpoint =
      [this, position]() -> std::optional<sketch::PointReference> {
        SketchPickFilter filter;
        filter.circleCenters = false;
        filter.arcEndpoints = false;
        filter.elementCenters = false;
        const auto hit = pointAt(position, pocEndpointTolerance, filter);
        return hit ? std::optional{hit->reference} : std::nullopt;
      };

  const auto findCircleBody =
      [this, position]() -> sketch::GeometryId {
        SketchPickFilter centerFilter;
        centerFilter.lineEndpoints = false;
        centerFilter.arcEndpoints = false;
        centerFilter.elementCenters = false;
        if (pointAt(position, pocCircleCenterExclusion, centerFilter))
          return sketch::kInvalidGeometryId;
        SketchPickFilter circleFilter;
        circleFilter.lines = false;
        circleFilter.arcs = false;
        const auto hit = geometryAt(position, pocCircleBodyTolerance,
                                    circleFilter);
        return hit ? hit->geometryId : sketch::kInvalidGeometryId;
      };

  const auto createPointOnCircle =
      [this](sketch::GeometryId circleId,
             sketch::PointReference endpoint) {
        if (circleId == sketch::kInvalidGeometryId ||
            endpoint.lineId == sketch::kInvalidGeometryId)
          return false;

        // Do not create the same relation twice.
        for (const auto& existing : sketch_.constraints()) {
          if (existing.type !=
              sketch::ConstraintType::PointOnCircle)
            continue;

          if (existing.firstGeometry == circleId &&
              existing.secondPoint.lineId == endpoint.lineId &&
              existing.secondPoint.start == endpoint.start &&
              existing.secondPoint.circleId ==
                  sketch::kInvalidGeometryId &&
              existing.secondPoint.elementCenterId == 0)
            return true;
        }

        pushUndoState();

        sketch::Constraint constraint;
        constraint.type =
            sketch::ConstraintType::PointOnCircle;
        constraint.firstGeometry = circleId;
        constraint.secondPoint = endpoint;
        if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
          cancelPendingUndo();
          return false;
        }

        notifyGeometryChanged();
        return true;
      };

  // Circle was chosen first: now accept only a real line endpoint.
  if (interactionState().constraint.pointOnCircleCarrier.has_value()) {
    const auto carrier =
        static_cast<sketch::GeometryId>(
            interactionState().constraint.pointOnCircleCarrier.value_or(sketch::kInvalidGeometryId));

    const auto endpoint = findLineEndpoint();

    if (endpoint) {
      const bool created = createPointOnCircle(carrier, *endpoint);

      interaction_.setPointOnCircleCarrier(std::nullopt);
      interaction_.setCoincidentFirstPoint(std::nullopt);

      emit selectionChanged(QString::fromUtf8(
          created ? "Ограничение: конец линии на окружности"
                  : "Ограничение отклонено"));
      update();
      return;
    }

    // Clicking elsewhere cancels only this special pending pair and then lets
    // the normal Coincident tool handle the same click.
    interaction_.setPointOnCircleCarrier(std::nullopt);
  }

  const auto endpointHit = findLineEndpoint();
  const auto circleHit = findCircleBody();

  // Endpoint was chosen previously by the ordinary Coincident first-point
  // picker. Give a circle-body second click priority over PointOnLine.
  if (interactionState().constraint.coincidentFirstPoint &&
      interactionState().constraint.coincidentFirstPoint->lineId !=
          sketch::kInvalidGeometryId &&
      interactionState().constraint.coincidentFirstPoint->circleId ==
          sketch::kInvalidGeometryId &&
      interactionState().constraint.coincidentFirstPoint->elementCenterId == 0 &&
      circleHit != sketch::kInvalidGeometryId) {
    const auto endpoint = *interactionState().constraint.coincidentFirstPoint;

    const bool created = createPointOnCircle(circleHit, endpoint);

    interaction_.setCoincidentFirstPoint(std::nullopt);
    interaction_.setPointOnCircleCarrier(std::nullopt);

    emit selectionChanged(QString::fromUtf8(
        created ? "Ограничение: конец линии на окружности"
                : "Ограничение отклонено"));
    update();
    return;
  }

  // Circle body first. Store only the carrier and wait for an endpoint.
  //
  // Endpoint hit has priority so a point that already lies on/near the
  // circumference is still treated as a point rather than as circle body.
  if (!interactionState().constraint.coincidentFirstPoint &&
      !endpointHit &&
      circleHit != sketch::kInvalidGeometryId) {
    interaction_.setPointOnCircleCarrier(circleHit);

    emit selectionChanged(QString::fromUtf8(
        "Окружность выбрана: укажите конец линии"));
    update();
    return;
  }

  // MERGED POINT-ON-LINE MODE
  //
  // The existing Coincident tool now handles two related workflows:
  //   point -> point       = Coincident
  //   line body -> point   = PointOnLine
  //
  // Clicking near a line endpoint deliberately falls through to the original
  // point-selection logic below.
  constexpr double lineTolerance = 9.0;
  constexpr double endpointExclusion = 11.0;
  constexpr double pointTolerance = 10.0;

  // POINT-ON-CIRCLE: ACTIVE CARRIER
  const auto circleCarrierProperty =
      interactionState().constraint.pointOnCircleCarrier;

  if (circleCarrierProperty) {
    const auto carrierCircleId = *circleCarrierProperty;

    if (!sketch_.circleIndex(carrierCircleId)) {
      interaction_.setPointOnCircleCarrier(std::nullopt);
      return;
    }

    std::optional<sketch::PointReference> clickedPoint;
    if (const auto hit = pointAt(position, pointTolerance, {},
                                 carrierCircleId))
      clickedPoint = hit->reference;

    if (!clickedPoint) {
      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите конечную точку"));
      return;
    }

    for (const auto& constraint :
         sketch_.constraints()) {
      if (constraint.type !=
          sketch::ConstraintType::PointOnCircle)
        continue;

      if (constraint.firstGeometry == carrierCircleId &&
          samePointReference(constraint.secondPoint,
                             *clickedPoint)) {
        interaction_.setPointOnCircleCarrier(std::nullopt);
        emit selectionChanged(QString::fromUtf8(
            "Эта точка уже принадлежит выбранной окружности"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type =
        sketch::ConstraintType::PointOnCircle;
    constraint.firstGeometry = carrierCircleId;
    constraint.secondPoint = *clickedPoint;
    if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
      cancelPendingUndo();
      interaction_.setPointOnCircleCarrier(std::nullopt);
      interaction_.setCoincidentFirstPoint(std::nullopt);
      emit selectionChanged(QString::fromUtf8("Ограничение отклонено"));
      update();
      return;
    }

    interaction_.setPointOnCircleCarrier(std::nullopt);
    interaction_.setCoincidentFirstPoint(std::nullopt);

    emit selectionChanged(QString::fromUtf8(
        "Ограничение: точка на окружности"));
    notifyGeometryChanged();
    update();
    return;
  }
  const auto carrierProperty =
      interactionState().constraint.pointOnLineCarrier;

  if (carrierProperty) {
    const auto carrierId = *carrierProperty;

    if (!sketch_.lineIndex(carrierId)) {
      interaction_.setPointOnLineCarrier(std::nullopt);
      return;
    }

    std::optional<sketch::PointReference> clickedPoint;
    if (const auto hit = pointAt(position, pointTolerance, {}, carrierId))
      clickedPoint = hit->reference;
    if (!clickedPoint) {
      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите конечную точку или центр окружности"));
      return;
    }

    if (clickedPoint->elementCenterId == 0 &&
        clickedPoint->circleId == sketch::kInvalidGeometryId &&
        clickedPoint->lineId == carrierId) {
      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите точку другого объекта"));
      return;
    }

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type != sketch::ConstraintType::PointOnLine)
        continue;

      if (constraint.firstGeometry == carrierId &&
          samePointReference(constraint.secondPoint, *clickedPoint)) {
        interaction_.setPointOnLineCarrier(std::nullopt);

        emit selectionChanged(QString::fromUtf8(
            "Эта точка уже принадлежит выбранной линии"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type = sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = carrierId;
    constraint.secondPoint = *clickedPoint;
    if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
      cancelPendingUndo();
      interaction_.setPointOnLineCarrier(std::nullopt);
      emit selectionChanged(QString::fromUtf8("Ограничение отклонено"));
      update();
      return;
    }

    interaction_.setPointOnLineCarrier(std::nullopt);

    emit selectionChanged(
        QString::fromUtf8("Ограничение: Принадлежность"));
    notifyGeometryChanged();
    update();
    return;
  }

  // SECOND-CLICK POINT PRIORITY. A point may lie directly on a line or Arc
  // body. Resolve all explicit points before carrier-body workflows so a click
  // on an Arc endpoint creates Coincident, not PointOnLine for the line under
  // that endpoint.
  if (interactionState().constraint.coincidentFirstPoint) {
    std::optional<sketch::PointReference> secondPoint;
    if (const auto hit = pointAt(position, pointTolerance))
      secondPoint = hit->reference;

    if (secondPoint) {
      const auto firstPoint = *interactionState().constraint.coincidentFirstPoint;
      if (samePointReference(firstPoint, *secondPoint)) {
        interaction_.setCoincidentFirstPoint(std::nullopt);
        emit selectionChanged(QString::fromUtf8(
            "Выбрана одна и та же точка"));
        update();
        return;
      }
      bool duplicate = false;
      for (const auto& constraint : sketch_.constraints()) {
        if (constraint.type != sketch::ConstraintType::Coincident) continue;
        duplicate =
            (samePointReference(constraint.firstPoint, firstPoint) &&
             samePointReference(constraint.secondPoint, *secondPoint)) ||
            (samePointReference(constraint.firstPoint, *secondPoint) &&
             samePointReference(constraint.secondPoint, firstPoint));
        if (duplicate) break;
      }
      if (!duplicate) {
        pushUndoState();
        sketch::Constraint constraint;
        constraint.type = sketch::ConstraintType::Coincident;
        constraint.firstPoint = firstPoint;
        constraint.secondPoint = *secondPoint;
        if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
          cancelPendingUndo();
          interaction_.setCoincidentFirstPoint(std::nullopt);
          emit selectionChanged(QString::fromUtf8("Ограничение отклонено"));
          update();
          return;
        }
        notifyGeometryChanged();
      }
      interaction_.setCoincidentFirstPoint(std::nullopt);
      emit selectionChanged(QString::fromUtf8(
          duplicate ? "Эти точки уже имеют ограничение «Совпадение»"
                    : "Ограничение: Совпадение"));
      update();
      return;
    }
  }

  // POINT-FIRST -> LINE-BODY POINT-ON-LINE
  //
  // If a point was selected first, allow the second click to be the body of
  // a line. This makes the merged tool symmetrical:
  //   line -> point  and  point -> line.
  if (interactionState().constraint.coincidentFirstPoint) {
    SketchPickFilter lineFilter;
    lineFilter.circles = false;
    lineFilter.arcs = false;
    const auto carrierHit =
        pointAt(position, endpointExclusion)
            ? std::optional<SketchPickEntityRef>{}
            : geometryAt(position, lineTolerance, lineFilter);
    const auto bestCarrierIndex = carrierHit
                                      ? sketch_.lineIndex(
                                            carrierHit->geometryId)
                                      : std::nullopt;

    if (bestCarrierIndex) {
      const auto carrierId = sketch_.lineId(*bestCarrierIndex);
      const auto pointReference = *interactionState().constraint.coincidentFirstPoint;

      if (carrierId != sketch::kInvalidGeometryId &&
          !(pointReference.elementCenterId == 0 &&
            pointReference.circleId == sketch::kInvalidGeometryId &&
            pointReference.lineId == carrierId)) {
        bool duplicate = false;

        for (const auto& constraint : sketch_.constraints()) {
          if (constraint.type != sketch::ConstraintType::PointOnLine)
            continue;

          if (constraint.firstGeometry == carrierId &&
              samePointReference(constraint.secondPoint, pointReference)) {
            duplicate = true;
            break;
          }
        }

        if (!duplicate) {
          pushUndoState();

          sketch::Constraint constraint;
          constraint.type = sketch::ConstraintType::PointOnLine;
          constraint.firstGeometry = carrierId;
          constraint.secondPoint = pointReference;
          if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
            cancelPendingUndo();
            interaction_.setCoincidentFirstPoint(std::nullopt);
            interaction_.setPointOnLineCarrier(std::nullopt);
            emit selectionChanged(QString::fromUtf8("Ограничение отклонено"));
            update();
            return;
          }

          emit selectionChanged(
              QString::fromUtf8("Ограничение: Принадлежность"));
          notifyGeometryChanged();
        } else {
          emit selectionChanged(QString::fromUtf8(
              "Эта точка уже принадлежит выбранной линии"));
        }

        interaction_.setCoincidentFirstPoint(std::nullopt);
        interaction_.setPointOnLineCarrier(std::nullopt);
        update();
        return;
      }
    }
  }
  // POINT-FIRST -> ARC-BODY POINT-ON-ARC. Arc endpoints remain point hits;
  // only the finite curved body is accepted as the carrier here.
  if (interactionState().constraint.coincidentFirstPoint) {
    sketch::GeometryId carrierArcId = sketch::kInvalidGeometryId;
    SketchPickFilter arcFilter;
    arcFilter.lines = false;
    arcFilter.circles = false;
    if (!pointAt(position, endpointExclusion)) {
      const auto carrier = geometryAt(position, lineTolerance, arcFilter);
      if (carrier) carrierArcId = carrier->geometryId;
    }

    if (carrierArcId != sketch::kInvalidGeometryId) {
      const auto pointReference = *interactionState().constraint.coincidentFirstPoint;
      if (pointReference.arcId == carrierArcId) {
        emit selectionChanged(QString::fromUtf8(
            "Конец дуги нельзя привязать к этой же дуге"));
        return;
      }

      bool duplicate = false;
      for (const auto& constraint : sketch_.constraints()) {
        if (constraint.type == sketch::ConstraintType::PointOnArc &&
            constraint.firstGeometry == carrierArcId &&
            samePointReference(constraint.secondPoint, pointReference)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        pushUndoState();
        sketch::Constraint constraint;
        constraint.type = sketch::ConstraintType::PointOnArc;
        constraint.firstGeometry = carrierArcId;
        constraint.secondPoint = pointReference;
        if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
          cancelPendingUndo();
          interaction_.setCoincidentFirstPoint(std::nullopt);
          emit selectionChanged(QString::fromUtf8("Ограничение отклонено"));
          update();
          return;
        }
        notifyGeometryChanged();
      }
      interaction_.setCoincidentFirstPoint(std::nullopt);
      emit selectionChanged(QString::fromUtf8(
          "Ограничение: точка на дуге"));
      update();
      return;
    }
  }
  // POINT-ON-CIRCLE: POINT FIRST
  if (interactionState().constraint.coincidentFirstPoint) {
    constexpr double circleBodyTolerance = 9.0;
    constexpr double circleCenterExclusion = 11.0;

    sketch::GeometryId carrierCircleId =
        sketch::kInvalidGeometryId;
    SketchPickFilter circleCenterFilter;
    circleCenterFilter.lineEndpoints = false;
    circleCenterFilter.arcEndpoints = false;
    circleCenterFilter.elementCenters = false;
    SketchPickFilter circleFilter;
    circleFilter.lines = false;
    circleFilter.arcs = false;
    if (!pointAt(position, circleCenterExclusion, circleCenterFilter)) {
      const auto carrier = geometryAt(position, circleBodyTolerance,
                                      circleFilter);
      if (carrier) carrierCircleId = carrier->geometryId;
    }

    if (carrierCircleId !=
        sketch::kInvalidGeometryId) {
      const auto pointReference =
          *interactionState().constraint.coincidentFirstPoint;

      if (pointReference.circleId == carrierCircleId) {
        emit selectionChanged(QString::fromUtf8(
            "Центр окружности нельзя связать с её собственной окружностью"));
        return;
      }

      bool duplicate = false;

      for (const auto& constraint :
           sketch_.constraints()) {
        if (constraint.type !=
            sketch::ConstraintType::PointOnCircle)
          continue;

        if (constraint.firstGeometry != carrierCircleId)
          continue;

        const auto& existing = constraint.secondPoint;

        if (existing.elementCenterId != 0 ||
            pointReference.elementCenterId != 0) {
          duplicate =
              existing.elementCenterId != 0 &&
              existing.elementCenterId ==
                  pointReference.elementCenterId;
        } else if (
            existing.circleId != sketch::kInvalidGeometryId ||
            pointReference.circleId !=
                sketch::kInvalidGeometryId) {
          duplicate =
              existing.circleId != sketch::kInvalidGeometryId &&
              existing.circleId == pointReference.circleId;
        } else if (
            existing.arcId != sketch::kInvalidGeometryId ||
            pointReference.arcId != sketch::kInvalidGeometryId) {
          duplicate = existing.arcId != sketch::kInvalidGeometryId &&
                      existing.arcId == pointReference.arcId &&
                      existing.start == pointReference.start;
        } else {
          duplicate =
              existing.lineId == pointReference.lineId &&
              existing.start == pointReference.start;
        }

        if (duplicate) break;
      }

      if (!duplicate) {
        pushUndoState();

        sketch::Constraint constraint;
        constraint.type =
            sketch::ConstraintType::PointOnCircle;
        constraint.firstGeometry = carrierCircleId;
        constraint.secondPoint = pointReference;
        if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
          cancelPendingUndo();
          interaction_.setCoincidentFirstPoint(std::nullopt);
          interaction_.setPointOnCircleCarrier(std::nullopt);
          emit selectionChanged(QString::fromUtf8("Ограничение отклонено"));
          update();
          return;
        }

        notifyGeometryChanged();
      }

      interaction_.setCoincidentFirstPoint(std::nullopt);
      interaction_.setPointOnCircleCarrier(std::nullopt);

      emit selectionChanged(QString::fromUtf8(
          "Ограничение: точка на окружности"));
      update();
      return;
    }
  }
  // FIRST-CLICK POINT PRIORITY
  //
  // Endpoints and circle centres take precedence over a nearby segment body.
  // This prevents a point lying visually on/near another line from
  // accidentally starting line-first PointOnLine mode.
  if (!interactionState().constraint.coincidentFirstPoint &&
      !interactionState().constraint.pointOnLineCarrier.has_value()) {
    std::optional<sketch::PointReference> firstClickedPoint;
    if (const auto hit = pointAt(position, pointTolerance))
      firstClickedPoint = hit->reference;
    if (firstClickedPoint) {
      interaction_.setCoincidentFirstPoint(*firstClickedPoint);

      emit selectionChanged(QString::fromUtf8(
          "Совпадение / Принадлежность: выберите вторую точку или тело линии"));
      update();
      return;
    }
  }
  // No first point has been selected for ordinary Coincident yet:
  // a click on the interior of a segment starts PointOnLine mode.
  // POINT-ON-CIRCLE: CIRCLE FIRST
  if (!interactionState().constraint.coincidentFirstPoint) {
    constexpr double circleBodyTolerance = 9.0;
    constexpr double circleCenterExclusion = 11.0;

    sketch::GeometryId carrierCircleId =
        sketch::kInvalidGeometryId;
    SketchPickFilter centerFilter;
    centerFilter.lineEndpoints = false;
    centerFilter.arcEndpoints = false;
    centerFilter.elementCenters = false;
    SketchPickFilter circleFilter;
    circleFilter.lines = false;
    circleFilter.arcs = false;
    if (!pointAt(position, circleCenterExclusion, centerFilter)) {
      const auto carrier = geometryAt(position, circleBodyTolerance,
                                      circleFilter);
      if (carrier) carrierCircleId = carrier->geometryId;
    }

    if (carrierCircleId !=
        sketch::kInvalidGeometryId) {
      interaction_.setPointOnCircleCarrier(carrierCircleId);

      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите конечную точку"));
      update();
      return;
    }
  }
  if (!interactionState().constraint.coincidentFirstPoint) {
    SketchPickFilter lineFilter;
    lineFilter.circles = false;
    lineFilter.arcs = false;
    const auto carrier = pointAt(position, endpointExclusion)
                             ? std::optional<SketchPickEntityRef>{}
                             : geometryAt(position, lineTolerance,
                                          lineFilter);
    const auto bestLineIndex = carrier
                                   ? sketch_.lineIndex(carrier->geometryId)
                                   : std::nullopt;

    if (bestLineIndex) {
      const auto carrierId = sketch_.lineId(*bestLineIndex);

      if (carrierId != sketch::kInvalidGeometryId) {
        interaction_.setPointOnLineCarrier(carrierId);

        emit selectionChanged(QString::fromUtf8(
            "Принадлежность: выберите точку или центр окружности"));
        update();
        return;
      }
    }
  }

  constexpr double hitTolerance = 10.0;
  std::optional<sketch::PointReference> clickedPoint;
  if (const auto hit = pointAt(position, hitTolerance))
    clickedPoint = hit->reference;
  if (!clickedPoint) {
    emit selectionChanged(QString::fromUtf8(
        "Совпадение: выберите конец линии или центр окружности"));
    return;
  }

  if (!interactionState().constraint.coincidentFirstPoint) {
    interaction_.setCoincidentFirstPoint(*clickedPoint);
    emit selectionChanged(QString::fromUtf8(
        "Совпадение: выберите вторую точку или центр"));
    update();
    return;
  }

  const auto first = *interactionState().constraint.coincidentFirstPoint;
  const auto second = *clickedPoint;

  if (samePointReference(first, second)) {
    interaction_.setCoincidentFirstPoint(std::nullopt);
    emit selectionChanged(QString::fromUtf8(
        "Выбрана одна и та же точка"));
    update();
    return;
  }

  // Do not collapse a single line by coinciding its own two ends.
  if (first.elementCenterId == 0 &&
      second.elementCenterId == 0 &&
      first.circleId == sketch::kInvalidGeometryId &&
      second.circleId == sketch::kInvalidGeometryId &&
      first.arcId == sketch::kInvalidGeometryId &&
      second.arcId == sketch::kInvalidGeometryId &&
      first.lineId != sketch::kInvalidGeometryId &&
      first.lineId == second.lineId) {
    interaction_.setCoincidentFirstPoint(std::nullopt);
    emit selectionChanged(QString::fromUtf8(
        "Совпадение: выберите разные объекты"));
    update();
    return;
  }

  // Avoid duplicate Coincident constraints in either order.
  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Coincident) continue;

    const bool sameOrder =
        samePointReference(constraint.firstPoint, first) &&
        samePointReference(constraint.secondPoint, second);
    const bool reverseOrder =
        samePointReference(constraint.firstPoint, second) &&
        samePointReference(constraint.secondPoint, first);

    if (sameOrder || reverseOrder) {
      interaction_.setCoincidentFirstPoint(std::nullopt);
      emit selectionChanged(QString::fromUtf8(
          "Эти точки уже имеют ограничение «Совпадение»"));
      update();
      return;
    }
  }

  pushUndoState();

  sketch::Constraint constraint;
  constraint.type = sketch::ConstraintType::Coincident;
  constraint.firstPoint = first;
  constraint.secondPoint = second;
  if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
    cancelPendingUndo();
    interaction_.setCoincidentFirstPoint(std::nullopt);
    emit selectionChanged(QString::fromUtf8("Ограничение отклонено"));
    update();
    return;
  }

  interaction_.setCoincidentFirstPoint(std::nullopt);
  emit selectionChanged(QString::fromUtf8(
      "Ограничение: Совпадение"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleTangentConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;

  enum class HitKind {
    None = 0,
    Line = 1,
    Circle = 2,
    Arc = 3
  };

  HitKind hitKind = HitKind::None;
  sketch::GeometryId hitId = sketch::kInvalidGeometryId;
  SketchPickFilter tangentFilter;
  if (const auto first = interactionState().constraint.tangentFirst) {
    const bool firstIsLine =
        first->kind == SketchGeometryOperandKind::Line;
    tangentFilter.lines = !firstIsLine;
    tangentFilter.circles = firstIsLine;
    tangentFilter.arcs = firstIsLine;
  }
  if (const auto hit = geometryAt(position, hitTolerance, tangentFilter)) {
    hitId = hit->geometryId;
    hitKind = hit->kind == SketchPickEntityKind::Line
                  ? HitKind::Line
              : hit->kind == SketchPickEntityKind::Circle ? HitKind::Circle
                                                           : HitKind::Arc;
  }

  if (hitKind == HitKind::None ||
      hitId == sketch::kInvalidGeometryId)
    return;

  const auto operandKind = [](HitKind kind) {
    return kind == HitKind::Line
               ? SketchGeometryOperandKind::Line
           : kind == HitKind::Circle
               ? SketchGeometryOperandKind::Circle
               : SketchGeometryOperandKind::Arc;
  };
  const auto hitOperandKind = operandKind(hitKind);
  const auto firstOperand = interactionState().constraint.tangentFirst;

  if (!firstOperand) {
    interaction_.setTangentFirst(
        SketchGeometryOperand{hitOperandKind, hitId, 0});

    if (hitKind == HitKind::Line) {
      emit selectionChanged(
          QString::fromUtf8("Касательная: выберите окружность или дугу"));
    }
    else {
      emit selectionChanged(
          QString::fromUtf8(
              "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F: "
              "\xD0\xB2\xD1\x8B\xD0\xB1\xD0\xB5\xD1\x80\xD0\xB8\xD1\x82\xD0\xB5 "
              "\xD0\xBF\xD1\x80\xD1\x8F\xD0\xBC\xD1\x83\xD1\x8E"));
    }

    update();
    return;
  }

  const auto firstId = firstOperand->geometryId;
  const auto firstKind = firstOperand->kind == SketchGeometryOperandKind::Line
                             ? HitKind::Line
                         : firstOperand->kind ==
                                   SketchGeometryOperandKind::Circle
                             ? HitKind::Circle
                             : HitKind::Arc;

  if (firstKind == hitKind) {
    emit selectionChanged(
        firstKind == HitKind::Line
            ? QString::fromUtf8(
                  "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F: "
                  "\xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE "
                  "\xD0\xB2\xD1\x8B\xD0\xB1\xD1\x80\xD0\xB0\xD1\x82\xD1\x8C "
                  "\xD0\xBE\xD0\xBA\xD1\x80\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C")
            : QString::fromUtf8(
                  "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F: "
                  "\xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE "
                  "\xD0\xB2\xD1\x8B\xD0\xB1\xD1\x80\xD0\xB0\xD1\x82\xD1\x8C "
                  "\xD0\xBF\xD1\x80\xD1\x8F\xD0\xBC\xD1\x83\xD1\x8E"));
    update();
    return;
  }

  sketch::GeometryId lineId =
      sketch::kInvalidGeometryId;

  sketch::GeometryId curvedId =
      sketch::kInvalidGeometryId;

  if (firstKind == HitKind::Line &&
      (hitKind == HitKind::Circle || hitKind == HitKind::Arc)) {
    lineId = firstId;
    curvedId = hitId;
  }
  else if ((firstKind == HitKind::Circle || firstKind == HitKind::Arc) &&
           hitKind == HitKind::Line) {
    lineId = hitId;
    curvedId = firstId;
  }

  const auto resetState = [this]() {
    interaction_.setTangentFirst(std::nullopt);
  };

  if (lineId == sketch::kInvalidGeometryId ||
      curvedId == sketch::kInvalidGeometryId ||
      !sketch_.lineIndex(lineId) ||
      (!sketch_.circleIndex(curvedId) && !sketch_.arcIndex(curvedId))) {
    resetState();
    update();
    return;
  }

  for (const auto& constraint :
       sketch_.constraints()) {
    if (constraint.type !=
        sketch::ConstraintType::Tangent)
      continue;

    if (constraint.firstGeometry == lineId &&
        constraint.secondGeometry == curvedId) {
      resetState();

      emit selectionChanged(
          QString::fromUtf8(
              "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F "
              "\xD1\x83\xD0\xB6\xD0\xB5 "
              "\xD1\x83\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB0"));

      update();
      return;
    }
  }

  pushUndoState();

  sketch::Constraint constraint;
  constraint.type =
      sketch::ConstraintType::Tangent;
  constraint.firstGeometry = lineId;
  constraint.secondGeometry = curvedId;

  const bool added =
      executeCommand(AddConstraintCommand{constraint}).accepted;

  resetState();

  if (!added) {
    cancelPendingUndo();
    emit undoAvailable(canUndo());
    emit selectionChanged(QString::fromUtf8(
        "Касательная не добавлена: конфликт зависимостей"));
    update();
    return;
  }

  emit selectionChanged(
      QString::fromUtf8("Ограничение: Касательная к кривой"));

  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleEqualConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;

  enum class HitKind { None, Line, Rectangle, Circle };

  HitKind hitKind = HitKind::None;
  sketch::GeometryId hitId = sketch::kInvalidGeometryId;
  std::size_t hitElementId = 0;
  SketchPickFilter equalFilter;
  equalFilter.arcs = false;
  if (const auto hit = geometryAt(position, hitTolerance, equalFilter)) {
    hitId = hit->geometryId;
    hitElementId = hit->elementId;
    if (hit->kind == SketchPickEntityKind::Circle) {
      hitKind = HitKind::Circle;
    } else {
      const auto elementLineCount = static_cast<std::size_t>(std::count_if(
          sketch_.lines().begin(), sketch_.lines().end(),
          [hit](const sketch::Line& line) {
            return line.elementId == hit->elementId;
          }));
      hitKind = elementLineCount == 4 ? HitKind::Rectangle : HitKind::Line;
    }
  }

  if (hitKind == HitKind::None ||
      hitId == sketch::kInvalidGeometryId)
    return;

  const auto operandKind = [](HitKind kind) {
    switch (kind) {
      case HitKind::Line:
        return SketchGeometryOperandKind::Line;
      case HitKind::Rectangle:
        return SketchGeometryOperandKind::Rectangle;
      case HitKind::Circle:
        return SketchGeometryOperandKind::Circle;
      default:
        return SketchGeometryOperandKind::Line;
    }
  };

  const auto firstOperand = interactionState().constraint.equalFirst;

  if (!firstOperand) {
    interaction_.setEqualFirst(
        SketchGeometryOperand{operandKind(hitKind), hitId, hitElementId});

    clearGeometrySelection();

    if (hitKind == HitKind::Line ||
        hitKind == HitKind::Rectangle) {
      const auto index = sketch_.lineIndex(hitId);
      if (index) {
        selectedElementIds_.push_back(
            sketch_.lines()[*index].elementId);
        selectionKind_ = SelectionKind::Line;
        selectionElementId_ =
            sketch_.lines()[*index].elementId;
        selectionLineId_ = hitId;
        selectionCircleId_ = sketch::kInvalidGeometryId;

        emit lineStyleSelectionChanged(
            true, sketch_.lines()[*index].dashed);
      }
    } else {
      selectedCircleIds_.push_back(hitId);
      selectionKind_ = SelectionKind::Circle;
      selectionCircleId_ = hitId;
      selectionLineId_ = sketch::kInvalidGeometryId;
      emit lineStyleSelectionChanged(false, false);
    }

    emit selectionChanged(
        QString::fromUtf8("Эквивалентность: выберите второй объект"));
    update();
    return;
  }

  const auto resetEqualState = [this]() {
    interaction_.setEqualFirst(std::nullopt);
  };
  const auto rollbackConstraintAdd = [this]() {
    cancelPendingUndo();
    emit undoAvailable(canUndo());
  };

  const auto firstId = firstOperand->geometryId;
  const auto firstKind = firstOperand->kind;
  const auto secondKind = operandKind(hitKind);

  // CRASH-FREE 14: LINE-RECTANGLE EQUAL
  //
  // A click on a rectangle still resolves to one concrete perimeter line
  // (hitId). Equal between a standalone line and a rectangle therefore means
  // equality with THAT selected side. Rectangle + Rectangle keeps the
  // dedicated width/height mapping below.
  const bool firstLineLike =
      firstKind == SketchGeometryOperandKind::Line ||
      firstKind == SketchGeometryOperandKind::Rectangle;
  const bool secondLineLike =
      secondKind == SketchGeometryOperandKind::Line ||
      secondKind == SketchGeometryOperandKind::Rectangle;
  const bool mixedLineRectangle =
      firstLineLike &&
      secondLineLike &&
      firstKind != secondKind;

  if (firstKind != secondKind &&
      !mixedLineRectangle) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эквивалентность: выберите два объекта одного типа"));
    update();
    return;
  }

  if (firstKind == SketchGeometryOperandKind::Circle) {
    if (firstId == hitId ||
        !sketch_.circleIndex(firstId)) {
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность: выберите две разные окружности"));
      update();
      return;
    }

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type != sketch::ConstraintType::Equal)
        continue;

      const bool sameOrder =
          constraint.firstGeometry == firstId &&
          constraint.secondGeometry == hitId;
      const bool reverseOrder =
          constraint.firstGeometry == hitId &&
          constraint.secondGeometry == firstId;

      if (sameOrder || reverseOrder) {
        resetEqualState();
        emit selectionChanged(
            QString::fromUtf8("Эти окружности уже эквивалентны"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type = sketch::ConstraintType::Equal;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = hitId;
    if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
      rollbackConstraintAdd();
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность не добавлена: конфликт зависимостей"));
      update();
      return;
    }

    resetEqualState();

    clearGeometrySelection();
    selectedCircleIds_.push_back(hitId);
    selectionKind_ = SelectionKind::Circle;
    selectionCircleId_ = hitId;
    selectionLineId_ = sketch::kInvalidGeometryId;
    emit lineStyleSelectionChanged(false, false);

    emit selectionChanged(
        QString::fromUtf8("Ограничение: Эквивалентность"));
    notifyGeometryChanged();
    update();
    return;
  }

  if (mixedLineRectangle) {
    if (firstId == hitId ||
        !sketch_.lineIndex(firstId) ||
        !sketch_.lineIndex(hitId)) {
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность: выберите две разные линии"));
      update();
      return;
    }

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type !=
          sketch::ConstraintType::Equal)
        continue;

      const bool sameOrder =
          constraint.firstGeometry == firstId &&
          constraint.secondGeometry == hitId;
      const bool reverseOrder =
          constraint.firstGeometry == hitId &&
          constraint.secondGeometry == firstId;

      if (sameOrder || reverseOrder) {
        resetEqualState();
        emit selectionChanged(
            QString::fromUtf8(
                "Эти линии уже эквивалентны"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type =
        sketch::ConstraintType::Equal;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = hitId;
    if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
      rollbackConstraintAdd();
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность не добавлена: конфликт зависимостей"));
      update();
      return;
    }

    resetEqualState();

    const auto hitIndex =
        sketch_.lineIndex(hitId);

    clearGeometrySelection();

    if (hitIndex) {
      selectedElementIds_.push_back(
          sketch_.lines()[*hitIndex].elementId);
      selectionKind_ = SelectionKind::Line;
      selectionElementId_ =
          sketch_.lines()[*hitIndex].elementId;
      selectionLineId_ = hitId;
      selectionCircleId_ =
          sketch::kInvalidGeometryId;

      emit lineStyleSelectionChanged(
          true,
          sketch_.lines()[*hitIndex].dashed);
    }

    emit selectionChanged(
        QString::fromUtf8(
            "Ограничение: Эквивалентность"));

    notifyGeometryChanged();
    update();
    return;
  }
  if (firstKind == SketchGeometryOperandKind::Line) {
    if (firstId == hitId ||
        !sketch_.lineIndex(firstId) ||
        !sketch_.lineIndex(hitId)) {
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность: выберите две разные линии"));
      update();
      return;
    }

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type != sketch::ConstraintType::Equal)
        continue;

      const bool sameOrder =
          constraint.firstGeometry == firstId &&
          constraint.secondGeometry == hitId;
      const bool reverseOrder =
          constraint.firstGeometry == hitId &&
          constraint.secondGeometry == firstId;

      if (sameOrder || reverseOrder) {
        resetEqualState();
        emit selectionChanged(
            QString::fromUtf8("Эти линии уже эквивалентны"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type = sketch::ConstraintType::Equal;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = hitId;
    if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
      rollbackConstraintAdd();
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность не добавлена: конфликт зависимостей"));
      update();
      return;
    }

    resetEqualState();

    const auto hitIndex = sketch_.lineIndex(hitId);
    clearGeometrySelection();

    if (hitIndex) {
      selectedElementIds_.push_back(
          sketch_.lines()[*hitIndex].elementId);
      selectionKind_ = SelectionKind::Line;
      selectionElementId_ =
          sketch_.lines()[*hitIndex].elementId;
      selectionLineId_ = hitId;
      selectionCircleId_ = sketch::kInvalidGeometryId;

      emit lineStyleSelectionChanged(
          true, sketch_.lines()[*hitIndex].dashed);
    }

    emit selectionChanged(
        QString::fromUtf8("Ограничение: Эквивалентность"));
    notifyGeometryChanged();
    update();
    return;
  }

  // Rectangle + Rectangle.
  const auto firstElementId = firstOperand->elementId;
  const auto secondElementId = hitElementId;

  if (firstElementId == 0 ||
      secondElementId == 0 ||
      firstElementId == secondElementId) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эквивалентность: выберите два разных прямоугольника"));
    update();
    return;
  }

  std::vector<sketch::GeometryId> firstRectangleIds;
  std::vector<sketch::GeometryId> secondRectangleIds;

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];

    if (line.elementId == firstElementId)
      firstRectangleIds.push_back(sketch_.lineId(index));

    if (line.elementId == secondElementId)
      secondRectangleIds.push_back(sketch_.lineId(index));
  }

  if (firstRectangleIds.size() != 4 ||
      secondRectangleIds.size() != 4) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эквивалентность: составной объект не распознан как прямоугольник"));
    update();
    return;
  }

  const auto findIndex =
      [](const std::vector<sketch::GeometryId>& ids,
         sketch::GeometryId id) -> std::optional<std::size_t> {
        const auto found = std::find(ids.begin(), ids.end(), id);
        if (found == ids.end()) return std::nullopt;

        return static_cast<std::size_t>(
            std::distance(ids.begin(), found));
      };

  const auto firstSideIndex =
      findIndex(firstRectangleIds, firstId);

  if (!firstSideIndex) {
    resetEqualState();
    return;
  }

  const auto firstLineIndex = sketch_.lineIndex(firstId);
  if (!firstLineIndex) {
    resetEqualState();
    return;
  }

  const auto& firstSelectedLine =
      sketch_.lines()[*firstLineIndex];

  const double firstDx =
      firstSelectedLine.end.xMm - firstSelectedLine.start.xMm;
  const double firstDy =
      firstSelectedLine.end.yMm - firstSelectedLine.start.yMm;
  const double firstLength =
      std::hypot(firstDx, firstDy);

  if (firstLength <= 1e-9) {
    resetEqualState();
    return;
  }

  const double firstUx = firstDx / firstLength;
  const double firstUy = firstDy / firstLength;

  // The second click selects the rectangle, not the dimension mapping.
  // Match its side automatically by orientation so width cannot be
  // accidentally linked to height.
  std::optional<std::size_t> secondSideIndex;
  double bestParallelScore = -1.0;

  for (std::size_t sideIndex = 0;
       sideIndex < secondRectangleIds.size();
       ++sideIndex) {
    const auto candidateIndex =
        sketch_.lineIndex(secondRectangleIds[sideIndex]);
    if (!candidateIndex) continue;

    const auto& candidate =
        sketch_.lines()[*candidateIndex];

    const double dx =
        candidate.end.xMm - candidate.start.xMm;
    const double dy =
        candidate.end.yMm - candidate.start.yMm;
    const double length = std::hypot(dx, dy);
    if (length <= 1e-9) continue;

    const double ux = dx / length;
    const double uy = dy / length;

    // abs(dot) treats parallel and anti-parallel directions as equivalent.
    const double score =
        std::abs(firstUx * ux + firstUy * uy);

    if (score > bestParallelScore) {
      bestParallelScore = score;
      secondSideIndex = sideIndex;
    }
  }

  if (!secondSideIndex) {
    resetEqualState();
    return;
  }

  const auto matchedSecondId =
      secondRectangleIds[*secondSideIndex];

  const auto firstAdjacentId =
      firstRectangleIds[(*firstSideIndex + 1) % 4];
  const auto secondAdjacentId =
      secondRectangleIds[(*secondSideIndex + 1) % 4];

  const auto equalExists =
      [this](sketch::GeometryId first,
             sketch::GeometryId second) {
        return std::any_of(
            sketch_.constraints().begin(),
            sketch_.constraints().end(),
            [first, second](const sketch::Constraint& constraint) {
              if (constraint.type !=
                  sketch::ConstraintType::Equal)
                return false;

              return (constraint.firstGeometry == first &&
                      constraint.secondGeometry == second) ||
                     (constraint.firstGeometry == second &&
                      constraint.secondGeometry == first);
            });
      };

  const bool firstPairExists =
      equalExists(firstId, matchedSecondId);
  const bool secondPairExists =
      equalExists(firstAdjacentId, secondAdjacentId);

  if (firstPairExists && secondPairExists) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эти прямоугольники уже эквивалентны"));
    update();
    return;
  }

  pushUndoState();

  bool addedAll = true;
  if (!firstPairExists) {
    sketch::Constraint firstEqual;
    firstEqual.type = sketch::ConstraintType::Equal;
    firstEqual.firstGeometry = firstId;
    firstEqual.secondGeometry = matchedSecondId;
    addedAll = executeCommand(AddConstraintCommand{firstEqual}).accepted;
  }

  if (addedAll && !secondPairExists) {
    sketch::Constraint secondEqual;
    secondEqual.type = sketch::ConstraintType::Equal;
    secondEqual.firstGeometry = firstAdjacentId;
    secondEqual.secondGeometry = secondAdjacentId;
    addedAll = executeCommand(AddConstraintCommand{secondEqual}).accepted;
  }

  if (!addedAll) {
    rollbackConstraintAdd();
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эквивалентность не добавлена: конфликт зависимостей"));
    update();
    return;
  }

  resetEqualState();

  clearGeometrySelection();
  selectedElementIds_.push_back(secondElementId);
  selectionKind_ = SelectionKind::Line;
  selectionElementId_ = secondElementId;
  selectionLineId_ = hitId;
  selectionCircleId_ = sketch::kInvalidGeometryId;

  const auto hitIndex = sketch_.lineIndex(hitId);
  emit lineStyleSelectionChanged(
      true, hitIndex && sketch_.lines()[*hitIndex].dashed);

  emit selectionChanged(QString::fromUtf8(
      "Ограничение: Эквивалентность прямоугольников"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleParallelConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;
  const auto firstProperty =
      interactionState().constraint.parallelFirstLine;
  const auto firstSelectedId =
      firstProperty.value_or(sketch::kInvalidGeometryId);

  SketchPickFilter lineFilter;
  lineFilter.circles = false;
  lineFilter.arcs = false;
  const auto hit = geometryAt(position, hitTolerance, lineFilter,
                              firstSelectedId);
  const auto bestIndex =
      hit ? sketch_.lineIndex(hit->geometryId) : std::nullopt;

  if (!bestIndex) return;

  const auto clickedId = sketch_.lineId(*bestIndex);
  if (clickedId == sketch::kInvalidGeometryId) return;

  if (!firstProperty) {
    interaction_.setParallelFirstLine(clickedId);

    clearGeometrySelection();
    selectedElementIds_.push_back(
        sketch_.lines()[*bestIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*bestIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit selectionChanged(
        QString::fromUtf8("Параллельность: выберите вторую линию"));
    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*bestIndex].dashed);
    update();
    return;
  }

  const auto firstId = *firstProperty;

  if (firstId == sketch::kInvalidGeometryId ||
      !sketch_.lineIndex(firstId)) {
    interaction_.setParallelFirstLine(std::nullopt);
    return;
  }

  if (firstId == clickedId) {
    interaction_.setParallelFirstLine(std::nullopt);
    emit selectionChanged(
        QString::fromUtf8("Параллельность: выберите две разные линии"));
    update();
    return;
  }

  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Parallel) continue;

    const bool sameOrder =
        constraint.firstGeometry == firstId &&
        constraint.secondGeometry == clickedId;
    const bool reverseOrder =
        constraint.firstGeometry == clickedId &&
        constraint.secondGeometry == firstId;

    if (sameOrder || reverseOrder) {
      interaction_.setParallelFirstLine(std::nullopt);
      emit selectionChanged(
          QString::fromUtf8("Эти линии уже параллельны"));
      update();
      return;
    }
  }

  const auto orthogonalType =
      [this](sketch::GeometryId id)
          -> std::optional<sketch::ConstraintType> {
        for (const auto& constraint : sketch_.constraints()) {
          if (constraint.firstGeometry != id) continue;

          if (constraint.type == sketch::ConstraintType::Horizontal ||
              constraint.type == sketch::ConstraintType::Vertical)
            return constraint.type;
        }

        return std::nullopt;
      };

  const auto firstOrthogonal = orthogonalType(firstId);
  const auto secondOrthogonal = orthogonalType(clickedId);

  if (firstOrthogonal && secondOrthogonal &&
      *firstOrthogonal != *secondOrthogonal) {
    interaction_.setParallelFirstLine(std::nullopt);
    emit selectionChanged(QString::fromUtf8(
        "Конфликт: одна линия горизонтальна, другая вертикальна"));
    update();
    return;
  }

  pushUndoState();

  sketch::Constraint constraint;
  constraint.type = sketch::ConstraintType::Parallel;
  constraint.firstGeometry = firstId;
  constraint.secondGeometry = clickedId;
  const bool added =
      executeCommand(AddConstraintCommand{constraint}).accepted;

  if (!added) {
    cancelPendingUndo();
    emit undoAvailable(canUndo());
    interaction_.setParallelFirstLine(std::nullopt);
    emit selectionChanged(QString::fromUtf8(
        "Параллельность не добавлена: конфликт зависимостей"));
    update();
    return;
  }

  interaction_.setParallelFirstLine(std::nullopt);

  const auto clickedIndex = sketch_.lineIndex(clickedId);
  clearGeometrySelection();

  if (clickedIndex) {
    selectedElementIds_.push_back(
        sketch_.lines()[*clickedIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*clickedIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*clickedIndex].dashed);
  }

  emit selectionChanged(
      QString::fromUtf8("Ограничение: Параллельность"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handlePerpendicularConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;
  const auto firstProperty =
      interactionState().constraint.perpendicularFirstLine;
  const auto firstSelectedId =
      firstProperty.value_or(sketch::kInvalidGeometryId);

  SketchPickFilter lineFilter;
  lineFilter.circles = false;
  lineFilter.arcs = false;
  const auto hit = geometryAt(position, hitTolerance, lineFilter,
                              firstSelectedId);
  const auto bestIndex =
      hit ? sketch_.lineIndex(hit->geometryId) : std::nullopt;

  if (!bestIndex) return;

  const auto clickedId = sketch_.lineId(*bestIndex);
  if (clickedId == sketch::kInvalidGeometryId) return;

  if (!firstProperty) {
    interaction_.setPerpendicularFirstLine(clickedId);

    clearGeometrySelection();
    selectedElementIds_.push_back(sketch_.lines()[*bestIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*bestIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit selectionChanged(
        QString::fromUtf8("Перпендикулярность: выберите вторую линию"));
    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*bestIndex].dashed);
    update();
    return;
  }

  const auto firstId = *firstProperty;

  if (firstId == sketch::kInvalidGeometryId ||
      !sketch_.lineIndex(firstId)) {
    interaction_.setPerpendicularFirstLine(std::nullopt);
    return;
  }

  if (firstId == clickedId) {
    interaction_.setPerpendicularFirstLine(std::nullopt);
    emit selectionChanged(
        QString::fromUtf8("Перпендикулярность: выберите две разные линии"));
    update();
    return;
  }

  // Do not create a duplicate in either order.
  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Perpendicular) continue;

    const bool sameOrder =
        constraint.firstGeometry == firstId &&
        constraint.secondGeometry == clickedId;
    const bool reverseOrder =
        constraint.firstGeometry == clickedId &&
        constraint.secondGeometry == firstId;

    if (sameOrder || reverseOrder) {
      interaction_.setPerpendicularFirstLine(std::nullopt);
      emit selectionChanged(
          QString::fromUtf8("Эти линии уже перпендикулярны"));
      update();
      return;
    }
  }

  const auto orthogonalType =
      [this](sketch::GeometryId id)
          -> std::optional<sketch::ConstraintType> {
        for (const auto& constraint : sketch_.constraints()) {
          if (constraint.firstGeometry != id) continue;
          if (constraint.type == sketch::ConstraintType::Horizontal ||
              constraint.type == sketch::ConstraintType::Vertical)
            return constraint.type;
        }
        return std::nullopt;
      };

  const auto firstOrthogonal = orthogonalType(firstId);
  const auto secondOrthogonal = orthogonalType(clickedId);

  // Two H/V-fixed lines can only accept Perpendicular when their fixed
  // orientations are already opposite.
  if (firstOrthogonal && secondOrthogonal &&
      *firstOrthogonal == *secondOrthogonal) {
    interaction_.setPerpendicularFirstLine(std::nullopt);
    emit selectionChanged(QString::fromUtf8(
        "Конфликт: обе линии уже имеют одинаковую H/V-ориентацию"));
    update();
    return;
  }

  pushUndoState();

  sketch::Constraint constraint;
  constraint.type = sketch::ConstraintType::Perpendicular;
  constraint.firstGeometry = firstId;
  constraint.secondGeometry = clickedId;
  const bool added =
      executeCommand(AddConstraintCommand{constraint}).accepted;

  if (!added) {
    cancelPendingUndo();
    emit undoAvailable(canUndo());
    interaction_.setPerpendicularFirstLine(std::nullopt);
    emit selectionChanged(QString::fromUtf8(
        "Перпендикулярность не добавлена: конфликт зависимостей"));
    update();
    return;
  }

  // addConstraint() already runs the sketch solver. Do not run the full
  // sequential solver a second time here: with interconnected constraints
  // that duplicate pass can move geometry twice.

  interaction_.setPerpendicularFirstLine(std::nullopt);

  const auto clickedIndex = sketch_.lineIndex(clickedId);
  clearGeometrySelection();

  if (clickedIndex) {
    selectedElementIds_.push_back(
        sketch_.lines()[*clickedIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*clickedIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*clickedIndex].dashed);
  }

  emit selectionChanged(
      QString::fromUtf8("Ограничение: Перпендикулярность"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleOrthogonalConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;
  SketchPickFilter lineFilter;
  lineFilter.circles = false;
  lineFilter.arcs = false;
  const auto hit = geometryAt(position, hitTolerance, lineFilter);
  const auto bestIndex =
      hit ? sketch_.lineIndex(hit->geometryId) : std::nullopt;

  if (!bestIndex) return;

  const auto& line = sketch_.lines()[*bestIndex];
  const double dx = std::abs(line.end.xMm - line.start.xMm);
  const double dy = std::abs(line.end.yMm - line.start.yMm);
  const auto id = sketch_.lineId(*bestIndex);
  if (id == sketch::kInvalidGeometryId) return;

  const auto type = dy > dx ? sketch::ConstraintType::Vertical
                            : sketch::ConstraintType::Horizontal;

  // Avoid stacking duplicate H/V constraints on the same primitive.
  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.firstGeometry == id && constraint.type == type) {
      emit selectionChanged(
          type == sketch::ConstraintType::Vertical
              ? QString::fromUtf8("Линия уже вертикальна")
              : QString::fromUtf8("Линия уже горизонтальна"));
      return;
    }
  }

  pushUndoState();

  sketch::Constraint constraint;
  constraint.type = type;
  constraint.firstGeometry = id;
  const bool added =
      executeCommand(AddConstraintCommand{constraint}).accepted;

  if (!added) {
    cancelPendingUndo();
    emit undoAvailable(canUndo());
    emit selectionChanged(QString::fromUtf8(
        "Ограничение не добавлено: конфликт зависимостей"));
    update();
    return;
  }

  selectionKind_ = SelectionKind::Line;
  selectionElementId_ = line.elementId;
  selectionLineId_ = id;
  emit selectionChanged(
      type == sketch::ConstraintType::Vertical
          ? QString::fromUtf8("Ограничение: вертикально")
          : QString::fromUtf8("Ограничение: горизонтально"));
  notifyGeometryChanged();
  update();
}

void SketchCanvas::handleLockConstraintClick(
    QPointF position) {
  constexpr double hitTolerance = 9.0;
  const auto hit = geometryAt(position, hitTolerance);
  if (!hit) {
    emit selectionChanged(
        QString::fromUtf8(
            "Замок: выберите объект"));
    return;
  }

  const sketch::GeometryId target = hit->geometryId;

  if (target == sketch::kInvalidGeometryId)
    return;

  if (sketch_.isGeometryLocked(target)) {
    emit selectionChanged(
        QString::fromUtf8(
            "Объект уже зафиксирован"));
    return;
  }

  pushUndoState();

  sketch::Constraint lock;
  lock.type = sketch::ConstraintType::Lock;
  lock.firstGeometry = target;

  if (!executeCommand(AddConstraintCommand{lock}).accepted) {
    cancelPendingUndo();
    emit undoAvailable(canUndo());

    emit selectionChanged(
        QString::fromUtf8(
            "Не удалось зафиксировать объект"));
    update();
    return;
  }

  emit selectionChanged(
      QString::fromUtf8(
          "Ограничение: Замок"));
  notifyGeometryChanged();
  update();
}

void SketchCanvas::handleAutoDimensionClick(QPointF position) {
  // AutoDimension owns mouse movement while it is active. Never allow a
  // stale installed-dimension drag state to intercept its live preview.
  interaction_.endDimensionLineDrag();
  interaction_.endDimensionLabelDrag();

  constexpr double pointTolerance = 8.0;
  std::optional<sketch::PointReference> clickedPoint;
  std::optional<SketchDatumReference> clickedDatum;
  SketchHitTolerancePolicy tolerance;
  tolerance.pointPx = pointTolerance;
  tolerance.endpointPx = pointTolerance;
  auto scene = hitScene(tolerance, true);
  SketchPickFilter pointFilter;
  pointFilter.entities = false;
  pointFilter.lineMidpoints = false;
  pointFilter.projections = false;
  pointFilter.dimensions = false;
  if (const auto hit = SketchHitTester::pick(
          scene, hitPoint(position), pointFilter)) {
    if (const auto* point = hit->point()) {
      clickedPoint = point->reference;
    } else if (const auto* datum = hit->datum()) {
      clickedDatum = datum->kind == SketchPickDatumKind::XAxis
                         ? SketchDatumReference::XAxis
                     : datum->kind == SketchPickDatumKind::YAxis
                         ? SketchDatumReference::YAxis
                         : SketchDatumReference::Origin;
    }
  }

  const auto beginDatumDimension =
      [this, position](sketch::PointReference geometryPoint,
                       SketchDatumReference datum) {
        const auto point = sketch_.referencedPoint(geometryPoint);
        if (!point) return false;

        QString mode = QStringLiteral("aligned");
        double distance = std::hypot(point->xMm, point->yMm);
        bool fixedMode = false;
        if (datum == SketchDatumReference::XAxis) {
          mode = QStringLiteral("y");
          distance = std::abs(point->yMm);
          fixedMode = true;
        } else if (datum == SketchDatumReference::YAxis) {
          mode = QStringLiteral("x");
          distance = std::abs(point->xMm);
          fixedMode = true;
        }
        if (distance <= 1e-9) {
          emit selectionChanged(QString::fromUtf8(
              "Авторазмер: выбранная точка уже лежит на опорной оси"));
          return false;
        }

        auto autoDimension = interactionState().autoDimension;
        autoDimension.target = SketchAutoDimensionTarget::Points;
        autoDimension.pointMode = pointDimensionMode(mode);
        autoDimension.datumModeFixed = fixedMode;
        autoDimension.directLineId.reset();
        autoDimension.offsetMm = 4.0;

        sketch::PointReference origin;
        origin.origin = true;
        autoDimension.firstPoint = origin;
        autoDimension.secondPoint = geometryPoint;
        interaction_.updateAutoDimension(std::move(autoDimension));

        primaryDimension_->setPrefix(QString());
        primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
        primaryDimension_->setRange(0.01, 100000.0);
        primaryDimension_->setValue(distance);
        secondaryDimension_->hide();
        primaryDimension_->move((position + QPointF(16, 16)).toPoint());
        primaryDimension_->show();
        primaryDimension_->setFocus();
        primaryDimension_->selectAll();
        update();
        return true;
      };

  if (clickedPoint) {
    const auto firstDatum = interactionState().autoDimension.firstDatum;
    if (firstDatum) {
      (void)beginDatumDimension(*clickedPoint, *firstDatum);
      return;
    }
    if (!interactionState().autoDimension.firstPoint.has_value()) {
      auto automatic = interactionState().autoDimension;
      automatic.firstPoint = *clickedPoint;
      interaction_.updateAutoDimension(std::move(automatic));
      update();
      return;
    }
    sketch::PointReference first{
        static_cast<sketch::GeometryId>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).lineId),
        interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).start,
        static_cast<sketch::GeometryId>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).circleId),
        static_cast<std::size_t>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).elementCenterId)};
    const auto firstPoint = sketch_.referencedPoint(first);
    const auto secondPoint = sketch_.referencedPoint(*clickedPoint);
    if (!firstPoint || !secondPoint) return;
    const double distance = std::hypot(secondPoint->xMm - firstPoint->xMm,
                                       secondPoint->yMm - firstPoint->yMm);
    if (distance <= 1e-9) return;
    auto autoDimension = interactionState().autoDimension;
    autoDimension.target = SketchAutoDimensionTarget::Points;
    autoDimension.pointMode = SketchPointDimensionMode::Aligned;
    autoDimension.directLineId.reset();
    autoDimension.offsetMm = 4.0;
    autoDimension.secondPoint = *clickedPoint;
    interaction_.updateAutoDimension(std::move(autoDimension));
    primaryDimension_->setPrefix(QString());
    primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    primaryDimension_->setRange(0.01, 100000.0);
    primaryDimension_->setValue(distance);
    secondaryDimension_->hide();
    primaryDimension_->move((position + QPointF(16, 16)).toPoint());
    primaryDimension_->show();
    primaryDimension_->setFocus();
    primaryDimension_->selectAll();
    update();
    return;
  }

  if (clickedDatum) {
    if (!interactionState().autoDimension.firstPoint.has_value()) {
      auto automatic = interactionState().autoDimension;
      automatic.firstDatum = clickedDatum;
      interaction_.updateAutoDimension(std::move(automatic));
      emit selectionChanged(
          clickedDatum == SketchDatumReference::Origin
              ? QString::fromUtf8(
                    "Авторазмер: начало координат выбрано, укажите точку")
              : QString::fromUtf8(
                    "Авторазмер: ось выбрана, укажите точку фигуры"));
      update();
      return;
    }

    sketch::PointReference first{
        static_cast<sketch::GeometryId>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).lineId),
        interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).start,
        static_cast<sketch::GeometryId>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).circleId),
        static_cast<std::size_t>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).elementCenterId)};
    (void)beginDatumDimension(first, *clickedDatum);
    return;
  }

  SketchPickFilter dimensionFilter;
  dimensionFilter.arcs = false;
  const auto geometry = geometryAt(position, 9.0, dimensionFilter);
  if (!geometry) return;
  const auto lineIndex = geometry->kind == SketchPickEntityKind::Line
                             ? sketch_.lineIndex(geometry->geometryId)
                             : std::nullopt;
  const auto circleIndex = geometry->kind == SketchPickEntityKind::Circle
                               ? sketch_.circleIndex(geometry->geometryId)
                               : std::nullopt;
  if (!lineIndex && !circleIndex) return;
  auto automatic = interactionState().autoDimension;
  automatic.firstPoint.reset();
  secondaryDimension_->hide();
  primaryDimension_->setRange(0.01, 100000.0);
  if (lineIndex) {
    const auto& line = sketch_.lines()[*lineIndex];
    const auto lineId = sketch_.lineId(*lineIndex);

    // Treat a direct line click exactly like selecting its two endpoints.
    // This gives the line the same three interactive dimension modes:
    // aligned length, X projection and Y projection.
    automatic.target = SketchAutoDimensionTarget::Points;
    automatic.pointMode = SketchPointDimensionMode::Aligned;
    automatic.directLineId = lineId;
    automatic.offsetMm = 4.0;
    automatic.firstPoint = sketch::PointReference{lineId, true};
    automatic.secondPoint = sketch::PointReference{lineId, false};

    primaryDimension_->setPrefix(QString());
    primaryDimension_->setValue(
        std::hypot(line.end.xMm - line.start.xMm,
                   line.end.yMm - line.start.yMm));
  } else {
    automatic.target = SketchAutoDimensionTarget::Circle;
    automatic.geometryId = sketch_.circleId(*circleIndex);
    primaryDimension_->setPrefix(QString::fromUtf8("Ø: "));
    const QPointF center = mapPoint(sketch_.circles()[*circleIndex].center);
    const QPointF delta = position - center;
    automatic.angleRad = std::atan2(-delta.y(), delta.x());
    primaryDimension_->setValue(sketch_.circles()[*circleIndex].radiusMm * 2.0);
  }
  interaction_.updateAutoDimension(std::move(automatic));
  primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
  primaryDimension_->move((position + QPointF(16, 16)).toPoint());
  primaryDimension_->show();
  primaryDimension_->setFocus();
  primaryDimension_->selectAll();
  update();
}

void SketchCanvas::commitAutoDimension() {
  const auto& automatic = interactionState().autoDimension;
  const auto target = automatic.target;
  const double value = primaryDimension_->value();
  if (target == SketchAutoDimensionTarget::None || !std::isfinite(value) ||
      value <= 0.0)
    return;

  sketch::Dimension dimension;
  dimension.valueMm = value;
  dimension.offsetMm = automatic.offsetMm.value_or(4.0);
  dimension.angleRad = automatic.angleRad.value_or(0.0);
  sketch::Constraint constraint;
  bool ensureParallel = false;

  if (target == SketchAutoDimensionTarget::Angle) {
    const auto firstId = automatic.angleFirstLine.value_or(
        sketch::kInvalidGeometryId);
    const auto secondId = automatic.angleSecondLine.value_or(
        sketch::kInvalidGeometryId);
    const auto firstIndex = sketch_.lineIndex(firstId);
    const auto secondIndex = sketch_.lineIndex(secondId);
    if (!firstIndex || !secondIndex) return;
    double solverAngle = value;
    const double primitive = lineAngleDegrees(sketch_.lines()[*firstIndex],
                                              sketch_.lines()[*secondIndex]);
    const double visible = visibleLineAngleDegrees(
        sketch_.lines()[*firstIndex], sketch_.lines()[*secondIndex]);
    if (std::abs(visible - (180.0 - primitive)) <
        std::abs(visible - primitive))
      solverAngle = 180.0 - value;
    dimension.kind = sketch::DimensionKind::LineAngle;
    dimension.geometryId = firstId;
    dimension.secondPoint.lineId = secondId;
    constraint.type = sketch::ConstraintType::Angle;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = secondId;
    constraint.value = solverAngle;
  } else if (target == SketchAutoDimensionTarget::LineDistance) {
    auto firstId = automatic.distanceFirstLine.value_or(
        sketch::kInvalidGeometryId);
    auto secondId = automatic.distanceSecondLine.value_or(
        sketch::kInvalidGeometryId);
    const auto firstIndex = sketch_.lineIndex(firstId);
    const auto secondIndex = sketch_.lineIndex(secondId);
    if (!firstIndex || !secondIndex ||
        !parallelLinePair(sketch_.lines()[*firstIndex],
                          sketch_.lines()[*secondIndex]))
      return;
    if (lineElementMemberCount(sketch_, firstId) == 1 &&
        lineElementMemberCount(sketch_, secondId) > 1)
      std::swap(firstId, secondId);
    dimension.kind = sketch::DimensionKind::LineDistance;
    dimension.geometryId = firstId;
    dimension.secondPoint.lineId = secondId;
    constraint.type = sketch::ConstraintType::LineDistance;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = secondId;
    constraint.value = value;
    ensureParallel = true;
  } else if (target == SketchAutoDimensionTarget::Line) {
    const auto id = automatic.geometryId.value_or(sketch::kInvalidGeometryId);
    dimension.kind = sketch::DimensionKind::LineLength;
    dimension.geometryId = id;
    constraint.type = sketch::ConstraintType::Length;
    constraint.firstGeometry = id;
    constraint.value = value;
  } else if (target == SketchAutoDimensionTarget::Circle) {
    const auto id = automatic.geometryId.value_or(sketch::kInvalidGeometryId);
    dimension.kind = sketch::DimensionKind::CircleDiameter;
    dimension.geometryId = id;
    constraint.type = sketch::ConstraintType::Diameter;
    constraint.firstGeometry = id;
    constraint.value = value;
  } else if (target == SketchAutoDimensionTarget::Points) {
    if (!automatic.firstPoint || !automatic.secondPoint) return;
    dimension.firstPoint = *automatic.firstPoint;
    dimension.secondPoint = *automatic.secondPoint;
    switch (automatic.pointMode) {
      case SketchPointDimensionMode::X:
        dimension.kind = sketch::DimensionKind::PointDistanceX;
        constraint.type = sketch::ConstraintType::DistanceX;
        break;
      case SketchPointDimensionMode::Y:
        dimension.kind = sketch::DimensionKind::PointDistanceY;
        constraint.type = sketch::ConstraintType::DistanceY;
        break;
      case SketchPointDimensionMode::Aligned:
        dimension.kind = sketch::DimensionKind::PointDistance;
        constraint.type = sketch::ConstraintType::Distance;
        break;
    }
    constraint.firstPoint = dimension.firstPoint;
    constraint.secondPoint = dimension.secondPoint;
    constraint.value = value;
  } else {
    return;
  }

  std::optional<sketch::DimensionId> existing;
  if (interactionState().dimension.editing) {
    const auto index = dimensionIndex(*interactionState().dimension.editing);
    if (!index || *index >= sketch_.dimensions().size()) return;
    existing = sketch_.dimensions()[*index].id;
  }
  pushUndoState();
  const auto result = executeCommand(UpsertDrivingDimensionCommand{
      dimension, constraint, existing, ensureParallel});
  if (!result.accepted) {
    cancelPendingUndo();
    emit undoAvailable(canUndo());
    emit redoAvailable(canRedo());
    const auto message = existing
        ? QString::fromUtf8(
              "Размер не изменён: более ранние зависимости имеют приоритет")
        : target == SketchAutoDimensionTarget::Points
              ? QString::fromUtf8(
                    "Размер не добавлен: точки уже определены по X и Y")
              : QString::fromUtf8(
                    "Размер не добавлен: конфликт с более ранней зависимостью");
    emit selectionChanged(message);
    emit constraintStatusChanged(message);
    primaryDimension_->setToolTip(message);
    primaryDimension_->show();
    primaryDimension_->setFocus();
    primaryDimension_->selectAll();
    update();
    return;
  }
  if (!existing) interaction_.appendDimensionLabel(0.0, 2.0);
  interaction_.completeAutoDimension();
  interaction_.selectDimension(std::nullopt);
  hideDimensionEditor();
  setFocus();
  notifyGeometryChanged();
}

void SketchCanvas::wheelEvent(QWheelEvent* event) {
  pixelsPerMm_ = std::clamp(
      pixelsPerMm_ * (event->angleDelta().y() > 0 ? 1.12 : 0.89), 0.5, 30.0);
  update();
}

bool SketchCanvas::lineElementSelected(std::size_t elementId) const {
  return std::find(selectedElementIds_.begin(), selectedElementIds_.end(),
                   elementId) != selectedElementIds_.end();
}

bool SketchCanvas::lineSelected(sketch::GeometryId id) const {
  return std::find(selectedLineIds_.begin(), selectedLineIds_.end(), id) !=
         selectedLineIds_.end();
}

bool SketchCanvas::circleSelected(sketch::GeometryId id) const {
  return std::find(selectedCircleIds_.begin(), selectedCircleIds_.end(),
                   id) != selectedCircleIds_.end();
}

bool SketchCanvas::arcSelected(sketch::GeometryId id) const {
  return std::find(selectedArcIds_.begin(), selectedArcIds_.end(),
                   id) != selectedArcIds_.end();
}

void SketchCanvas::clearGeometrySelection() {
  selectedLineIds_.clear();
  selectedElementIds_.clear();
  selectedCircleIds_.clear();
  selectedArcIds_.clear();
  selectionKind_ = SelectionKind::None;
  selectionLineId_ = sketch::kInvalidGeometryId;
  selectionCircleId_ = sketch::kInvalidGeometryId;
  selectionArcId_ = sketch::kInvalidGeometryId;
  selectionElementId_ = 0;
}

std::optional<sketch::GeometryId> SketchCanvas::lineAt(
    QPointF position, double tolerancePx) const {
  SketchPickFilter filter;
  filter.circles = false;
  filter.arcs = false;
  const auto hit = geometryAt(position, tolerancePx, filter);
  return hit ? std::optional{hit->geometryId} : std::nullopt;
}

std::vector<sketch::GeometryId> SketchCanvas::closedLineContour(
    sketch::GeometryId seed) const {
  const auto seedIndex = sketch_.lineIndex(seed);
  if (!seedIndex) return {};
  constexpr double tolerance = 1e-6;
  const auto samePoint = [](sketch::Point first, sketch::Point second) {
    return std::hypot(first.xMm - second.xMm,
                      first.yMm - second.yMm) <= tolerance;
  };

  const auto walk = [&](bool reverseSeed) {
    std::vector<sketch::GeometryId> result{seed};
    std::vector<sketch::GeometryId> used{seed};
    const auto& firstLine = sketch_.lines()[*seedIndex];
    const sketch::Point origin = reverseSeed ? firstLine.end : firstLine.start;
    sketch::Point cursor = reverseSeed ? firstLine.start : firstLine.end;

    for (std::size_t guard = 0; guard <= sketch_.lines().size(); ++guard) {
      if (samePoint(cursor, origin))
        return result.size() >= 3 ? result : std::vector<sketch::GeometryId>{};

      std::optional<std::size_t> next;
      bool nextReversed = false;
      for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
        const auto id = sketch_.lineId(index);
        const auto& line = sketch_.lines()[index];
        if (std::find(used.begin(), used.end(), id) != used.end())
          continue;
        const bool startsHere = samePoint(line.start, cursor);
        const bool endsHere = samePoint(line.end, cursor);
        if (!startsHere && !endsHere) continue;
        if (next) return std::vector<sketch::GeometryId>{};
        next = index;
        nextReversed = endsHere;
      }
      if (!next) return std::vector<sketch::GeometryId>{};
      const auto id = sketch_.lineId(*next);
      result.push_back(id);
      used.push_back(id);
      const auto& line = sketch_.lines()[*next];
      cursor = nextReversed ? line.start : line.end;
    }
    return std::vector<sketch::GeometryId>{};
  };

  auto result = walk(false);
  if (result.empty()) result = walk(true);
  return result;
}

std::optional<SketchCanvas::MirrorGeometryRef>
SketchCanvas::mirrorGeometryAt(QPointF position,
                               double tolerancePx) const {
  const auto hit = geometryAt(position, tolerancePx);
  if (!hit) return std::nullopt;
  switch (hit->kind) {
    case SketchPickEntityKind::Line:
      return MirrorGeometryRef{MirrorGeometryKind::Line, hit->geometryId};
    case SketchPickEntityKind::Circle:
      return MirrorGeometryRef{MirrorGeometryKind::Circle, hit->geometryId};
    case SketchPickEntityKind::Arc:
      return MirrorGeometryRef{MirrorGeometryKind::Arc, hit->geometryId};
  }
  return std::nullopt;
}

std::vector<SketchCanvas::MirrorGeometryRef>
SketchCanvas::closedMirrorContour(MirrorGeometryRef seed) const {
  if (seed.kind == MirrorGeometryKind::Circle)
    return sketch_.circleIndex(seed.geometryId)
               ? std::vector<MirrorGeometryRef>{seed}
               : std::vector<MirrorGeometryRef>{};

  struct OpenGeometry {
    MirrorGeometryRef reference;
    sketch::Point start;
    sketch::Point end;
  };
  std::vector<OpenGeometry> geometry;
  geometry.reserve(sketch_.lines().size() + sketch_.arcs().size());
  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    geometry.push_back({{MirrorGeometryKind::Line, sketch_.lineId(index)},
                        line.start, line.end});
  }
  for (std::size_t index = 0; index < sketch_.arcs().size(); ++index) {
    const auto& arc = sketch_.arcs()[index];
    geometry.push_back({{MirrorGeometryKind::Arc, sketch_.arcId(index)},
                        sketch::arcStartPoint(arc),
                        sketch::arcEndPoint(arc)});
  }

  const auto sameReference = [](MirrorGeometryRef first,
                                MirrorGeometryRef second) {
    return first.kind == second.kind &&
           first.geometryId == second.geometryId;
  };
  const auto seedItem = std::find_if(
      geometry.begin(), geometry.end(),
      [seed, &sameReference](const OpenGeometry& item) {
        return sameReference(item.reference, seed);
      });
  if (seedItem == geometry.end()) return {};

  constexpr double tolerance = 1e-6;
  const auto samePoint = [](sketch::Point first, sketch::Point second) {
    return std::hypot(first.xMm - second.xMm,
                      first.yMm - second.yMm) <= tolerance;
  };
  const auto walk = [&](bool reverseSeed) {
    std::vector<MirrorGeometryRef> result{seed};
    std::vector<MirrorGeometryRef> used{seed};
    const sketch::Point origin = reverseSeed ? seedItem->end : seedItem->start;
    sketch::Point cursor = reverseSeed ? seedItem->start : seedItem->end;

    for (std::size_t guard = 0; guard <= geometry.size(); ++guard) {
      if (samePoint(cursor, origin))
        return result.size() >= 2 ? result
                                  : std::vector<MirrorGeometryRef>{};

      const OpenGeometry* next = nullptr;
      bool nextReversed = false;
      for (const auto& candidate : geometry) {
        if (std::any_of(
                used.begin(), used.end(),
                [&candidate, &sameReference](MirrorGeometryRef item) {
                  return sameReference(item, candidate.reference);
                }))
          continue;
        const bool startsHere = samePoint(candidate.start, cursor);
        const bool endsHere = samePoint(candidate.end, cursor);
        if (!startsHere && !endsHere) continue;
        if (next) return std::vector<MirrorGeometryRef>{};
        next = &candidate;
        nextReversed = endsHere;
      }
      if (!next) return std::vector<MirrorGeometryRef>{};
      result.push_back(next->reference);
      used.push_back(next->reference);
      cursor = nextReversed ? next->start : next->end;
    }
    return std::vector<MirrorGeometryRef>{};
  };

  auto result = walk(false);
  if (result.empty()) result = walk(true);
  return result;
}

void SketchCanvas::setMirrorSourceSelection(
    const std::vector<MirrorGeometryRef>& source) {
  interaction_.updateMirror({source});
  clearGeometrySelection();

  for (const auto item : source) {
    switch (item.kind) {
      case MirrorGeometryKind::Line:
        selectedLineIds_.push_back(item.geometryId);
        selectionKind_ = SelectionKind::Line;
        selectionLineId_ = item.geometryId;
        if (const auto index = sketch_.lineIndex(item.geometryId))
          selectionElementId_ = sketch_.lines()[*index].elementId;
        break;
      case MirrorGeometryKind::Circle:
        selectedCircleIds_.push_back(item.geometryId);
        selectionKind_ = SelectionKind::Circle;
        selectionCircleId_ = item.geometryId;
        break;
      case MirrorGeometryKind::Arc:
        selectedArcIds_.push_back(item.geometryId);
        selectionKind_ = SelectionKind::Arc;
        selectionArcId_ = item.geometryId;
        break;
    }
  }
  update();
}

bool SketchCanvas::mirrorContourAboutLine(sketch::GeometryId axisId) {
  MirrorGeometryCommand command;
  command.axisId = axisId;
  command.sourceIds.reserve(interactionState().mirror.source.size());
  for (const auto item : interactionState().mirror.source)
    command.sourceIds.push_back(item.geometryId);
  if (command.sourceIds.empty()) return false;

  pushUndoState();
  const auto result = executeCommand(command);
  if (!result.accepted) {
    cancelPendingUndo();
    return false;
  }
  std::vector<MirrorGeometryRef> created;
  created.reserve(result.changedGeometryIds.size());
  for (const auto id : result.changedGeometryIds) {
    const auto location = sketch_.geometryLocation(id);
    if (!location) continue;
    switch (location->kind) {
      case sketch::GeometryKind::Line:
        created.push_back({MirrorGeometryKind::Line, id});
        break;
      case sketch::GeometryKind::Circle:
        created.push_back({MirrorGeometryKind::Circle, id});
        break;
      case sketch::GeometryKind::Arc:
        created.push_back({MirrorGeometryKind::Arc, id});
        break;
    }
  }
  setMirrorSourceSelection(created);
  interaction_.updateMirror({});
  notifyGeometryChanged();
  emit selectionChanged(QString::fromUtf8(
      "Зеркало создано · один клик — объект, двойной — контур"));
  return true;
}

std::optional<SketchCanvas::TrimPreview> SketchCanvas::trimPreviewAt(
    QPointF position) const {
  TrimPreview result;
  SketchHitTolerancePolicy tolerance;
  tolerance.entityPx = tolerance.trimPx;
  auto scene = hitScene(tolerance);
  for (auto& candidate : scene.candidates) {
    const auto* entity = std::get_if<SketchPickEntityRef>(&candidate.target);
    if (!entity || sketch_.isGeometryLocked(entity->geometryId))
      candidate.enabled = false;
  }
  SketchPickFilter filter;
  filter.points = false;
  filter.datums = false;
  filter.projections = false;
  filter.dimensions = false;
  const auto hit = SketchHitTester::pick(scene, hitPoint(position), filter);
  if (!hit || !hit->entity()) return std::nullopt;
  const auto reference = *hit->entity();
  switch (reference.kind) {
    case SketchPickEntityKind::Line:
      result = {TrimGeometryKind::Line, reference.geometryId, 0.0, 1.0,
                false};
      break;
    case SketchPickEntityKind::Circle:
      result = {TrimGeometryKind::Circle, reference.geometryId, 0.0, 1.0,
                false};
      break;
    case SketchPickEntityKind::Arc:
      result = {TrimGeometryKind::Arc, reference.geometryId, 0.0, 1.0,
                false};
      break;
  }

  const auto cross = [](sketch::Point a, sketch::Point b) {
    return a.xMm * b.yMm - a.yMm * b.xMm;
  };
  const auto subtract = [](sketch::Point a, sketch::Point b) {
    return sketch::Point{a.xMm - b.xMm, a.yMm - b.yMm};
  };
  const auto segmentIntersection =
      [&cross, &subtract](const sketch::Line& first,
                          const sketch::Line& second)
      -> std::optional<sketch::Point> {
        const sketch::Point r = subtract(first.end, first.start);
        const sketch::Point s = subtract(second.end, second.start);
        const double denominator = cross(r, s);
        if (std::abs(denominator) <= 1e-12) return std::nullopt;
        const sketch::Point delta = subtract(second.start, first.start);
        const double t = cross(delta, s) / denominator;
        const double u = cross(delta, r) / denominator;
        if (t < -1e-8 || t > 1.0 + 1e-8 ||
            u < -1e-8 || u > 1.0 + 1e-8)
          return std::nullopt;
        return sketch::Point{
            first.start.xMm + (first.end.xMm - first.start.xMm) *
                                  std::clamp(t, 0.0, 1.0),
            first.start.yMm + (first.end.yMm - first.start.yMm) *
                                  std::clamp(t, 0.0, 1.0)};
      };

  std::vector<double> cuts;
  const sketch::Line* targetLine = nullptr;
  const sketch::Circle* targetCircle = nullptr;
  const sketch::Arc* targetArc = nullptr;
  if (result.kind == TrimGeometryKind::Line) {
    const auto index = sketch_.lineIndex(result.geometryId);
    if (!index) return std::nullopt;
    targetLine = &sketch_.lines()[*index];
    cuts = {0.0, 1.0};
  } else if (result.kind == TrimGeometryKind::Circle) {
    const auto index = sketch_.circleIndex(result.geometryId);
    if (!index) return std::nullopt;
    targetCircle = &sketch_.circles()[*index];
  } else {
    const auto index = sketch_.arcIndex(result.geometryId);
    if (!index) return std::nullopt;
    targetArc = &sketch_.arcs()[*index];
    cuts = {0.0, 1.0};
  }

  const auto addTargetPoint = [&](sketch::Point point) {
    std::optional<double> parameter;
    if (targetLine) {
      const double dx = targetLine->end.xMm - targetLine->start.xMm;
      const double dy = targetLine->end.yMm - targetLine->start.yMm;
      const double lengthSquared = dx * dx + dy * dy;
      if (lengthSquared <= 1e-18) return;
      const double value =
          ((point.xMm - targetLine->start.xMm) * dx +
           (point.yMm - targetLine->start.yMm) * dy) /
          lengthSquared;
      if (value >= -1e-8 && value <= 1.0 + 1e-8)
        parameter = std::clamp(value, 0.0, 1.0);
    } else if (targetCircle) {
      const double radius = std::hypot(point.xMm - targetCircle->center.xMm,
                                       point.yMm - targetCircle->center.yMm);
      if (std::abs(radius - targetCircle->radiusMm) <= 1e-6)
        parameter = normalizedAngle(std::atan2(
                        point.yMm - targetCircle->center.yMm,
                        point.xMm - targetCircle->center.xMm)) /
                    kTrimTwoPi;
    } else if (targetArc) {
      parameter = arcParameterAtPoint(*targetArc, point);
    }
    if (parameter) cuts.push_back(*parameter);
  };

  if (targetLine) {
    for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
      if (sketch_.lineId(index) == result.geometryId) continue;
      if (const auto point = segmentIntersection(*targetLine,
                                                 sketch_.lines()[index]))
        addTargetPoint(*point);
    }
    for (const auto& circle : sketch_.circles())
      for (const auto point : segmentCircleIntersections(
               *targetLine, circle.center, circle.radiusMm))
        addTargetPoint(point);
    for (const auto& arc : sketch_.arcs())
      for (const auto point : segmentCircleIntersections(
               *targetLine, arc.center, arc.radiusMm))
        if (arcParameterAtPoint(arc, point)) addTargetPoint(point);
  } else {
    const sketch::Point center =
        targetCircle ? targetCircle->center : targetArc->center;
    const double radius =
        targetCircle ? targetCircle->radiusMm : targetArc->radiusMm;
    for (const auto& line : sketch_.lines())
      for (const auto point : segmentCircleIntersections(line, center, radius))
        addTargetPoint(point);
    for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
      if (targetCircle && sketch_.circleId(index) == result.geometryId) continue;
      const auto& other = sketch_.circles()[index];
      for (const auto point : circleCircleIntersections(
               center, radius, other.center, other.radiusMm))
        addTargetPoint(point);
    }
    for (std::size_t index = 0; index < sketch_.arcs().size(); ++index) {
      if (targetArc && sketch_.arcId(index) == result.geometryId) continue;
      const auto& other = sketch_.arcs()[index];
      const bool coincidentCarrier =
          std::hypot(center.xMm - other.center.xMm,
                     center.yMm - other.center.yMm) <= 1e-7 &&
          std::abs(radius - other.radiusMm) <= 1e-7;
      if (coincidentCarrier) {
        addTargetPoint(sketch::arcStartPoint(other));
        addTargetPoint(sketch::arcEndPoint(other));
        continue;
      }
      for (const auto point : circleCircleIntersections(
               center, radius, other.center, other.radiusMm))
        if (arcParameterAtPoint(other, point)) addTargetPoint(point);
    }
  }

  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) {
               return std::abs(a - b) <= 1e-8;
             }), cuts.end());

  const sketch::Point cursor = unmapPoint(position);
  double cursorParameter = 0.0;
  if (targetLine) {
    const double dx = targetLine->end.xMm - targetLine->start.xMm;
    const double dy = targetLine->end.yMm - targetLine->start.yMm;
    cursorParameter = std::clamp(
        ((cursor.xMm - targetLine->start.xMm) * dx +
         (cursor.yMm - targetLine->start.yMm) * dy) /
            (dx * dx + dy * dy),
        0.0, 1.0);
  } else if (targetCircle) {
    cursorParameter = normalizedAngle(std::atan2(
                          cursor.yMm - targetCircle->center.yMm,
                          cursor.xMm - targetCircle->center.xMm)) /
                      kTrimTwoPi;
  } else {
    cursorParameter = arcParameterAtPoint(*targetArc, cursor, 2.0 / pixelsPerMm_)
                          .value_or(0.0);
  }

  if (targetCircle) {
    if (cuts.size() < 2) {
      result.fullGeometry = true;
      return result;
    }
    double best = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < cuts.size(); ++index) {
      const double first = cuts[index];
      const double second = index + 1 < cuts.size()
                                ? cuts[index + 1]
                                : cuts.front() + 1.0;
      double adjustedCursor = cursorParameter;
      if (adjustedCursor < first) adjustedCursor += 1.0;
      const double distance =
          std::abs(adjustedCursor - (first + second) * 0.5);
      if (distance < best) {
        best = distance;
        result.firstParameter = first;
        result.secondParameter = second;
      }
    }
    return result;
  }

  if (cuts.size() < 2) return std::nullopt;
  double best = std::numeric_limits<double>::max();
  for (std::size_t index = 0; index + 1 < cuts.size(); ++index) {
    const double distance =
        std::abs(cursorParameter - (cuts[index] + cuts[index + 1]) * 0.5);
    if (distance < best) {
      best = distance;
      result.firstParameter = cuts[index];
      result.secondParameter = cuts[index + 1];
    }
  }
  result.fullGeometry =
      result.firstParameter <= 1e-8 && result.secondParameter >= 1.0 - 1e-8;
  return result;
}

bool SketchCanvas::trimAt(QPointF position) {
  const auto preview = trimPreviewAt(position);
  if (!preview) return false;
  pushUndoState();
  const auto result = executeCommand(TrimGeometryCommand{
      preview->geometryId, preview->firstParameter,
      preview->secondParameter, preview->fullGeometry});
  if (!result.accepted) {
    cancelPendingUndo();
    return false;
  }
  clearGeometrySelection();
  interaction_.updateTrim({});
  for (const auto id : result.changedGeometryIds) {
    const auto location = sketch_.geometryLocation(id);
    if (!location) continue;
    if (location->kind == sketch::GeometryKind::Line) {
      selectedLineIds_.push_back(id);
      selectionKind_ = SelectionKind::Line;
      selectionLineId_ = id;
    } else if (location->kind == sketch::GeometryKind::Arc) {
      selectedArcIds_.push_back(id);
      selectionKind_ = SelectionKind::Arc;
      selectionArcId_ = id;
    }
  }
  notifyGeometryChanged();
  emit selectionChanged(QString::fromUtf8("Подсвеченный участок удалён"));
  update();
  return true;
}

void SketchCanvas::selectAt(QPointF position, bool additive,
                            bool preserveExistingIfHit) {
  SelectionKind hitKind = SelectionKind::None;
  std::size_t hitElementId = 0;
  sketch::GeometryId hitLineId = sketch::kInvalidGeometryId;
  sketch::GeometryId hitCircleId = sketch::kInvalidGeometryId;
  sketch::GeometryId hitArcId = sketch::kInvalidGeometryId;
  bool hitDashed = false;

  if (const auto hit = geometryAt(position)) {
    hitElementId = hit->elementId;
    hitDashed = hit->dashed;
    switch (hit->kind) {
      case SketchPickEntityKind::Line:
        hitKind = SelectionKind::Line;
        hitLineId = hit->geometryId;
        break;
      case SketchPickEntityKind::Circle:
        hitKind = SelectionKind::Circle;
        hitCircleId = hit->geometryId;
        break;
      case SketchPickEntityKind::Arc:
        hitKind = SelectionKind::Arc;
        hitArcId = hit->geometryId;
        break;
    }
  }

  if (hitKind == SelectionKind::None) {
    if (!additive) clearGeometrySelection();
    emit lineStyleSelectionChanged(false, false);
    emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
    update();
    return;
  }

  const bool alreadySelected =
      hitKind == SelectionKind::Line
          ? lineSelected(hitLineId)
          : hitKind == SelectionKind::Circle
                ? circleSelected(hitCircleId)
                : arcSelected(hitArcId);

  if (additive) {
    if (hitKind == SelectionKind::Line) {
      const auto found =
          std::find(selectedLineIds_.begin(), selectedLineIds_.end(),
                    hitLineId);
      if (found != selectedLineIds_.end())
        selectedLineIds_.erase(found);
      else
        selectedLineIds_.push_back(hitLineId);
    } else if (hitKind == SelectionKind::Circle) {
      const auto found =
          std::find(selectedCircleIds_.begin(), selectedCircleIds_.end(),
                    hitCircleId);
      if (found != selectedCircleIds_.end())
        selectedCircleIds_.erase(found);
      else
        selectedCircleIds_.push_back(hitCircleId);
    } else {
      const auto found =
          std::find(selectedArcIds_.begin(), selectedArcIds_.end(),
                    hitArcId);
      if (found != selectedArcIds_.end())
        selectedArcIds_.erase(found);
      else
        selectedArcIds_.push_back(hitArcId);
    }

    if (alreadySelected) {
      if (selectedLineIds_.empty() && selectedElementIds_.empty() &&
          selectedCircleIds_.empty() &&
          selectedArcIds_.empty()) {
        clearGeometrySelection();
        emit lineStyleSelectionChanged(false, false);
        emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
        update();
        return;
      }

      // Keep one remaining object as the active property-panel object.
      if (!selectedLineIds_.empty()) {
        const auto lineId = selectedLineIds_.back();
        const auto index = sketch_.lineIndex(lineId);
        if (index) {
          selectionKind_ = SelectionKind::Line;
          selectionLineId_ = lineId;
          selectionElementId_ = sketch_.lines()[*index].elementId;
          selectionCircleId_ = sketch::kInvalidGeometryId;
          selectionArcId_ = sketch::kInvalidGeometryId;
          hitDashed = sketch_.lines()[*index].dashed;
        }
      } else if (!selectedElementIds_.empty()) {
        const auto elementId = selectedElementIds_.back();
        const auto found = std::find_if(
            sketch_.lines().begin(), sketch_.lines().end(),
            [elementId](const auto& line) { return line.elementId == elementId; });

        if (found != sketch_.lines().end()) {
          const auto index =
              static_cast<std::size_t>(
                  std::distance(sketch_.lines().begin(), found));
          selectionKind_ = SelectionKind::Line;
          selectionElementId_ = elementId;
          selectionLineId_ = sketch_.lineId(index);
          selectionCircleId_ = sketch::kInvalidGeometryId;
          selectionArcId_ = sketch::kInvalidGeometryId;
          hitDashed = found->dashed;
        }
      } else if (!selectedCircleIds_.empty()) {
        selectionKind_ = SelectionKind::Circle;
        selectionCircleId_ = selectedCircleIds_.back();
        selectionLineId_ = sketch::kInvalidGeometryId;
        selectionArcId_ = sketch::kInvalidGeometryId;
        const auto index = sketch_.circleIndex(selectionCircleId_);
        hitDashed = index && sketch_.circles()[*index].dashed;
      } else {
        selectionKind_ = SelectionKind::Arc;
        selectionArcId_ = selectedArcIds_.back();
        selectionLineId_ = sketch::kInvalidGeometryId;
        selectionCircleId_ = sketch::kInvalidGeometryId;
        const auto index = sketch_.arcIndex(selectionArcId_);
        hitDashed = index && sketch_.arcs()[*index].dashed;
      }
    } else {
      selectionKind_ = hitKind;
      selectionElementId_ = hitElementId;
      selectionLineId_ = hitLineId;
      selectionCircleId_ = hitCircleId;
      selectionArcId_ = hitArcId;
    }
  } else {
    if (!(preserveExistingIfHit && alreadySelected)) {
      clearGeometrySelection();

      if (hitKind == SelectionKind::Line)
        selectedLineIds_.push_back(hitLineId);
      else if (hitKind == SelectionKind::Circle)
        selectedCircleIds_.push_back(hitCircleId);
      else
        selectedArcIds_.push_back(hitArcId);
    }

    selectionKind_ = hitKind;
    selectionElementId_ = hitElementId;
    selectionLineId_ = hitLineId;
    selectionCircleId_ = hitCircleId;
    selectionArcId_ = hitArcId;
  }

  const std::size_t selectedCount =
      selectedLineIds_.size() + selectedElementIds_.size() +
      selectedCircleIds_.size() +
      selectedArcIds_.size();

  emit selectionChanged(
      selectedCount > 1
          ? QString::fromUtf8("Выбрано объектов: %1").arg(selectedCount)
          : hitKind == SelectionKind::Line
                ? QString::fromUtf8("Объект: линия")
                : hitKind == SelectionKind::Circle
                      ? QString::fromUtf8("Объект: окружность")
                      : QString::fromUtf8("Объект: дуга"));

  emit lineStyleSelectionChanged(selectedCount > 0, hitDashed);
  update();
}

void SketchCanvas::selectInRect(const QRectF& rect, bool additive) {
  if (!additive) clearGeometrySelection();

  const auto addLine = [this](sketch::GeometryId lineId) {
    if (!lineSelected(lineId))
      selectedLineIds_.push_back(lineId);
  };

  SketchPickFilter filter;
  filter.points = false;
  filter.datums = false;
  filter.projections = false;
  filter.dimensions = false;
  const auto hits = SketchHitTester::pickInBox(
      hitScene(), hitPoint(rect.topLeft()), hitPoint(rect.bottomRight()),
      filter);
  for (const auto& target : hits) {
    const auto* hit = std::get_if<SketchPickEntityRef>(&target);
    if (!hit) continue;
    switch (hit->kind) {
      case SketchPickEntityKind::Line:
        if (sketch_.lineIndex(hit->geometryId)) addLine(hit->geometryId);
        break;
      case SketchPickEntityKind::Circle:
        if (sketch_.circleIndex(hit->geometryId) &&
            !circleSelected(hit->geometryId))
          selectedCircleIds_.push_back(hit->geometryId);
        break;
      case SketchPickEntityKind::Arc:
        if (sketch_.arcIndex(hit->geometryId) &&
            !arcSelected(hit->geometryId))
          selectedArcIds_.push_back(hit->geometryId);
        break;
    }
  }

  const std::size_t selectedCount =
      selectedLineIds_.size() + selectedElementIds_.size() +
      selectedCircleIds_.size() +
      selectedArcIds_.size();

  if (!selectedLineIds_.empty()) {
    const auto lineId = selectedLineIds_.back();
    const auto index = sketch_.lineIndex(lineId);
    if (index) {
      selectionKind_ = SelectionKind::Line;
      selectionLineId_ = lineId;
      selectionElementId_ = sketch_.lines()[*index].elementId;
      selectionCircleId_ = sketch::kInvalidGeometryId;
      selectionArcId_ = sketch::kInvalidGeometryId;
      emit lineStyleSelectionChanged(true, sketch_.lines()[*index].dashed);
    }
  } else if (!selectedElementIds_.empty()) {
    const auto elementId = selectedElementIds_.back();
    const auto found = std::find_if(
        sketch_.lines().begin(), sketch_.lines().end(),
        [elementId](const auto& line) { return line.elementId == elementId; });

    if (found != sketch_.lines().end()) {
      const auto index = static_cast<std::size_t>(
          std::distance(sketch_.lines().begin(), found));
      selectionKind_ = SelectionKind::Line;
      selectionElementId_ = elementId;
      selectionLineId_ = sketch_.lineId(index);
      selectionCircleId_ = sketch::kInvalidGeometryId;
      emit lineStyleSelectionChanged(true, found->dashed);
    }
  } else if (!selectedCircleIds_.empty()) {
    selectionKind_ = SelectionKind::Circle;
    selectionCircleId_ = selectedCircleIds_.back();
    selectionLineId_ = sketch::kInvalidGeometryId;
    const auto index = sketch_.circleIndex(selectionCircleId_);
    emit lineStyleSelectionChanged(
        true, index && sketch_.circles()[*index].dashed);
  } else if (!selectedArcIds_.empty()) {
    selectionKind_ = SelectionKind::Arc;
    selectionArcId_ = selectedArcIds_.back();
    selectionLineId_ = sketch::kInvalidGeometryId;
    selectionCircleId_ = sketch::kInvalidGeometryId;
    const auto index = sketch_.arcIndex(selectionArcId_);
    emit lineStyleSelectionChanged(
        true, index && sketch_.arcs()[*index].dashed);
  } else {
    clearGeometrySelection();
    emit lineStyleSelectionChanged(false, false);
  }

  emit selectionChanged(
      selectedCount > 0
          ? QString::fromUtf8("Выбрано объектов: %1").arg(selectedCount)
          : QString::fromUtf8("Ничего не выбрано"));

  update();
}
void SketchCanvas::commitPoint(sketch::Point point) {
  if (tool() == Tool::Rectangle && rectangleMode_ == RectangleMode::ThreePoints) {
    commitRectanglePoint(point);
    return;
  }
  if (tool() == Tool::Circle && circleMode_ != CircleMode::CenterRadius) {
    commitCirclePoint(point);
    return;
  }
  if (tool() == Tool::Arc) {
    commitArcPoint(point);
    return;
  }
  if (!interactionState().creation.anchor) {
    interaction_.setCreationAnchor(point);
    hoverPoint_ = point;
    showDimensionEditor(mapPoint(point).toPoint() + QPoint(18, 18));
    update();
    return;
  }
  const std::size_t oldLineCount = sketch_.lines().size();
  std::vector<sketch::GeometryId> newGeometryIds;

  if (tool() == Tool::Line)
    pushUndoState();
  else if (tool() == Tool::Rectangle)
    pushUndoState();
  else if (tool() == Tool::Circle)
    pushUndoState();
  if (tool() == Tool::Line) {
    auto lineCreation = interactionState().lineCreation;
    // Automatic CAD coincidence:
    // if the new line starts on an existing line endpoint, remember that
    // endpoint before creating the new primitive and store a real persistent
    // Coincident constraint to the new line start.
    std::optional<sketch::PointReference> connectedEndpoint;
    double bestEndpointDistanceMm =
        8.0 / std::max(0.001, pixelsPerMm_);

    for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
      const auto id = sketch_.lineId(index);
      if (id == sketch::kInvalidGeometryId) continue;

      const auto& existingLine = sketch_.lines()[index];

      for (const bool start : {true, false}) {
        const auto existingPoint =
            start ? existingLine.start : existingLine.end;
        const double distanceMm =
            std::hypot(interactionState().creation.anchor->xMm - existingPoint.xMm,
                       interactionState().creation.anchor->yMm - existingPoint.yMm);

        if (distanceMm < bestEndpointDistanceMm) {
          bestEndpointDistanceMm = distanceMm;
          connectedEndpoint = sketch::PointReference{id, start};
        }
      }
    }

    // Snap the new line start exactly onto the detected CAD endpoint.
    sketch::Point lineStart = *interactionState().creation.anchor;
    if (connectedEndpoint) {
      if (const auto endpointPoint =
              sketch_.referencedPoint(*connectedEndpoint))
        lineStart = *endpointPoint;
    }

    const std::size_t newLineIndex = sketch_.lines().size();
    const auto creationResult = executeCommand(
        AddLineCommand{lineStart, point, std::nullopt});
    if (!creationResult.accepted) {
      cancelPendingUndo();
      return;
    }
    newGeometryIds = creationResult.changedGeometryIds;

    if (connectedEndpoint &&
        newLineIndex < sketch_.lines().size()) {
      const auto newLineId = sketch_.lineId(newLineIndex);

      if (newLineId != sketch::kInvalidGeometryId) {
        sketch::Constraint constraint;
        constraint.type = sketch::ConstraintType::Coincident;
        constraint.firstPoint = *connectedEndpoint;
        constraint.secondPoint =
            sketch::PointReference{newLineId, true};
        if (!executeCommand(AddConstraintCommand{constraint, true}).accepted)
          return;
      }
    }

    // AUTO START POINT-ON-LINE CONSTRAINT
    //
    // The first click can lie on the body of an existing segment. Keep that
    // relation when the second click finally creates the new line, otherwise
    // a later dimension may detach the visually-snapped start point.
    const auto startCarrierProperty = lineCreation.startLineCarrier;

    if (startCarrierProperty &&
        newLineIndex < sketch_.lines().size()) {
      const auto carrierId = *startCarrierProperty;

      const auto newLineId =
          sketch_.lineId(newLineIndex);

      if (carrierId !=
              sketch::kInvalidGeometryId &&
          newLineId !=
              sketch::kInvalidGeometryId &&
          carrierId != newLineId &&
          sketch_.lineIndex(carrierId)) {
        sketch::Constraint constraint;
        constraint.type =
            sketch::ConstraintType::PointOnLine;
        constraint.firstGeometry = carrierId;
        constraint.secondPoint =
            sketch::PointReference{
                newLineId, true};

        if (!executeCommand(AddConstraintCommand{constraint, true}).accepted)
          return;
      }
    }

    // AUTO START MIDPOINT CONSTRAINT
    //
    // Clicking the midpoint of an existing segment ties the new start point
    // to that midpoint, not merely to the line body.
    const auto startMidpointProperty = lineCreation.startMidpointCarrier;

    if (startMidpointProperty &&
        newLineIndex < sketch_.lines().size()) {
      const auto carrierId = *startMidpointProperty;
      const auto newLineId = sketch_.lineId(newLineIndex);

      if (carrierId != sketch::kInvalidGeometryId &&
          newLineId != sketch::kInvalidGeometryId &&
          carrierId != newLineId &&
          sketch_.lineIndex(carrierId)) {
        sketch::Constraint constraint;
        constraint.type = sketch::ConstraintType::Midpoint;
        constraint.firstGeometry = carrierId;
        constraint.secondPoint = sketch::PointReference{newLineId, true};
        if (!executeCommand(AddConstraintCommand{constraint, true}).accepted)
          return;
      }
    }

    // AUTO POINT-ON-LINE CONSTRAINT
    //
    // The mouse path already projected the new endpoint onto this carrier.
    // Store the semantic relation so later edits keep the endpoint on it.
    const auto endCarrierProperty = lineCreation.endLineCarrier;

    if (endCarrierProperty &&
        newLineIndex < sketch_.lines().size()) {
      const auto carrierId = *endCarrierProperty;
      const auto newLineId = sketch_.lineId(newLineIndex);

      if (carrierId != sketch::kInvalidGeometryId &&
          newLineId != sketch::kInvalidGeometryId &&
          carrierId != newLineId &&
          sketch_.lineIndex(carrierId)) {
        const sketch::PointReference newEnd{newLineId, false};
        const bool duplicate = std::any_of(
            sketch_.constraints().begin(), sketch_.constraints().end(),
            [carrierId, newEnd](const sketch::Constraint& item) {
              return item.type == sketch::ConstraintType::PointOnLine &&
                     item.firstGeometry == carrierId &&
                     item.secondPoint.lineId == newEnd.lineId &&
                     item.secondPoint.start == newEnd.start;
            });

        if (!duplicate) {
          sketch::Constraint constraint;
          constraint.type = sketch::ConstraintType::PointOnLine;
          constraint.firstGeometry = carrierId;
          constraint.secondPoint = newEnd;
          if (!executeCommand(AddConstraintCommand{constraint, true}).accepted)
            return;
        }
      }
    }

    // AUTO MIDPOINT CONSTRAINT (end point)
    const auto endMidpointProperty = lineCreation.endMidpointCarrier;

    if (endMidpointProperty &&
        newLineIndex < sketch_.lines().size()) {
      const auto carrierId = *endMidpointProperty;
      const auto newLineId = sketch_.lineId(newLineIndex);

      if (carrierId != sketch::kInvalidGeometryId &&
          newLineId != sketch::kInvalidGeometryId &&
          carrierId != newLineId &&
          sketch_.lineIndex(carrierId)) {
        sketch::Constraint constraint;
        constraint.type = sketch::ConstraintType::Midpoint;
        constraint.firstGeometry = carrierId;
        constraint.secondPoint = sketch::PointReference{newLineId, false};
        if (!executeCommand(AddConstraintCommand{constraint, true}).accepted)
          return;
      }
    }

    const auto addPointOnArc =
        [this, newLineIndex](std::optional<sketch::GeometryId>& carrier,
                             bool start) {
          const auto carrierProperty = carrier;
          carrier.reset();

          if (!carrierProperty ||
              newLineIndex >= sketch_.lines().size())
            return;
          const auto carrierId = *carrierProperty;
          const auto newLineId = sketch_.lineId(newLineIndex);

          if (carrierId == sketch::kInvalidGeometryId ||
              newLineId == sketch::kInvalidGeometryId ||
              !sketch_.arcIndex(carrierId))
            return;

          const sketch::PointReference endpoint{newLineId, start};

          const bool duplicate = std::any_of(
              sketch_.constraints().begin(),
              sketch_.constraints().end(),
              [carrierId, endpoint](const sketch::Constraint& item) {
                return item.type ==
                           sketch::ConstraintType::PointOnArc &&
                       item.firstGeometry == carrierId &&
                       item.secondPoint.lineId == endpoint.lineId &&
                       item.secondPoint.start == endpoint.start;
              });

          if (duplicate)
            return;

          sketch::Constraint constraint;
          constraint.type = sketch::ConstraintType::PointOnArc;
          constraint.firstGeometry = carrierId;
          constraint.secondPoint = endpoint;
          if (!executeCommand(AddConstraintCommand{constraint, true}).accepted)
            return;
        };

    addPointOnArc(lineCreation.startArcCarrier, true);
    addPointOnArc(lineCreation.endArcCarrier, false);
  }
  else if (tool() == Tool::Rectangle) {
    if (rectangleMode_ == RectangleMode::FromCenter) {
      const double dx = point.xMm - interactionState().creation.anchor->xMm;
      const double dy = point.yMm - interactionState().creation.anchor->yMm;

      const auto creationResult = executeCommand(AddRectangleCommand{
          {interactionState().creation.anchor->xMm - dx,
           interactionState().creation.anchor->yMm - dy},
          {interactionState().creation.anchor->xMm + dx,
           interactionState().creation.anchor->yMm + dy},
          std::nullopt, std::nullopt, true});
      if (!creationResult.accepted) return;
      newGeometryIds = creationResult.changedGeometryIds;
    } else {
      const auto creationResult = executeCommand(AddRectangleCommand{
          *interactionState().creation.anchor, point, std::nullopt,
          std::nullopt, false});
      if (!creationResult.accepted) return;
      newGeometryIds = creationResult.changedGeometryIds;
    }
  }
  else if (tool() == Tool::Circle) {
    const double radius = std::hypot(point.xMm - interactionState().creation.anchor->xMm,
                                     point.yMm - interactionState().creation.anchor->yMm);
    const auto creationResult = executeCommand(AddCircleCommand{
        *interactionState().creation.anchor, radius, false});
    if (!creationResult.accepted) return;
    newGeometryIds = creationResult.changedGeometryIds;
    circleDiameterMm_ = 2.0 * radius;
    emit primaryDimensionChanged(circleDiameterMm_);
  }
  if (!newGeometryIds.empty() &&
      !executeCommand(AutoConstrainNewGeometryCommand{
           newGeometryIds, 8.0 / std::max(0.001, pixelsPerMm_)})
           .accepted) {
    cancelPendingUndo();
    return;
  }

  // AUTO ORTHOGONAL LINE CONSTRAINT V2
  //
  // A line that the user intentionally draws almost horizontal/vertical
  // receives a REAL persistent CAD constraint immediately.
  //
  // This runs after Coincident / PointOnLine / generic auto-coincidence.
  // Therefore Sketch::addConstraint() must preserve those older relations.
  // If H/V would break an existing relation, the transactional constraint
  // system rejects this automatic constraint instead of detaching geometry.
  if (tool() == Tool::Line &&
      oldLineCount < sketch_.lines().size()) {
    const std::size_t newLineIndex = oldLineCount;
    const auto newLineId = sketch_.lineId(newLineIndex);

    if (newLineId != sketch::kInvalidGeometryId) {
      const auto& newLine = sketch_.lines()[newLineIndex];
      const double dx =
          newLine.end.xMm - newLine.start.xMm;
      const double dy =
          newLine.end.yMm - newLine.start.yMm;
      const double length = std::hypot(dx, dy);

      if (length > 1e-9) {
        constexpr double kAutoOrthogonalAngleDeg = 2.0;
        constexpr double kPi = 3.14159265358979323846;
        const double tolerance =
            std::sin(kAutoOrthogonalAngleDeg * kPi / 180.0);

        sketch::ConstraintType inferredType =
            sketch::ConstraintType::Horizontal;
        bool shouldConstrain = false;

        if (std::abs(dy) / length <= tolerance) {
          inferredType = sketch::ConstraintType::Horizontal;
          shouldConstrain = true;
        } else if (std::abs(dx) / length <= tolerance) {
          inferredType = sketch::ConstraintType::Vertical;
          shouldConstrain = true;
        }

        if (shouldConstrain) {
          const bool duplicate =
              std::any_of(
                  sketch_.constraints().begin(),
                  sketch_.constraints().end(),
                  [newLineId, inferredType](
                      const sketch::Constraint& item) {
                    return item.type == inferredType &&
                           item.firstGeometry == newLineId;
                  });

          if (!duplicate) {
            sketch::Constraint orthogonal;
            orthogonal.type = inferredType;
            orthogonal.firstGeometry = newLineId;

            // No manual geometry mutation here. The transactional solver
            // either satisfies the COMPLETE system or rejects H/V.
            if (!executeCommand(AddConstraintCommand{orthogonal, true})
                     .accepted)
              return;
          }
        }
      }
    }
  }

  interaction_.completeCreation();
  hideDimensionEditor();
  notifyGeometryChanged();
}

void SketchCanvas::commitArcPoint(sketch::Point point) {
  if (interactionState().creation.arcPoints.empty()) {
    interaction_.appendArcPoint(point);
    hoverPoint_ = point;
    interaction_.updateArcCreation({0.0, 1.0, false});

    primaryDimension_->setPrefix(QString::fromUtf8("L: "));
    primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    primaryDimension_->setRange(0.01, 100000.0);
    primaryDimension_->setDecimals(2);
    primaryDimension_->setValue(0.01);
    secondaryDimension_->hide();
    primaryDimension_->move(
        (mapPoint(point) + QPointF(18.0, 18.0)).toPoint());
    primaryDimension_->show();
    primaryDimension_->raise();
    primaryDimension_->setFocus();
    primaryDimension_->selectAll();

    emit selectionChanged(
        QString::fromUtf8(
            "Дуга: задайте конец хорды мышью или введите L и нажмите Enter"));
    update();
    return;
  }

  if (interactionState().creation.arcPoints.size() == 1) {
    const auto first = interactionState().creation.arcPoints[0];
    const double chord =
        std::hypot(point.xMm - first.xMm, point.yMm - first.yMm);

    if (chord <= 1e-9) {
      emit selectionChanged(
          QString::fromUtf8("Дуга: длина хорды должна быть больше нуля"));
      return;
    }

    interaction_.appendArcPoint(point);
    const double sagitta = chord * 0.5;
    auto arc = interactionState().arc;
    arc.sagittaSign = 1.0;
    arc.dimensionKeyboardEdit = false;
    interaction_.updateArcCreation(std::move(arc));
    hoverPoint_ = arcSagittaPoint(first, point, sagitta);

    primaryDimension_->setPrefix(QString::fromUtf8("H: "));
    primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    primaryDimension_->setRange(0.01, 100000.0);
    primaryDimension_->setDecimals(2);
    primaryDimension_->setValue(sagitta);
    primaryDimension_->move(
        (mapPoint(hoverPoint_) + QPointF(18.0, 18.0)).toPoint());
    primaryDimension_->show();
    primaryDimension_->raise();
    primaryDimension_->setFocus();
    primaryDimension_->selectAll();

    emit selectionChanged(
        QString::fromUtf8(
            "Дуга: задайте изгиб мышью или введите H и нажмите Enter"));
    update();
    return;
  }

  const auto first = interactionState().creation.arcPoints[0];
  const auto last = interactionState().creation.arcPoints[1];
  double sagitta = signedArcSagitta(first, last, point);

  if (std::abs(sagitta) <= 1e-9) {
    const double sign = interactionState().arc.sagittaSign.has_value()
                            ? interactionState().arc.sagittaSign.value_or(1.0)
                            : 1.0;
    sagitta = sign * std::max(0.01, primaryDimension_->value());
  }

  const auto arc = arcFromChordSagitta(first, last, sagitta);

  if (!arc) {
    emit selectionChanged(
        QString::fromUtf8(
            "Дуга не построена: хорда или величина изгиба некорректны"));
    update();
    return;
  }

  pushUndoState();
  const auto creationResult = executeCommand(AddArcCommand{
      arc->center, arc->radiusMm, arc->startAngleRad, arc->sweepAngleRad,
      arc->dashed});
  if (!creationResult.accepted) {
    cancelPendingUndo();
    return;
  }

  if (!executeCommand(AutoConstrainNewGeometryCommand{
           creationResult.changedGeometryIds,
           8.0 / std::max(0.001, pixelsPerMm_)})
           .accepted) {
    cancelPendingUndo();
    return;
  }

  interaction_.completeCreation();
  hideDimensionEditor();
  setFocus();
  emit selectionChanged(QString::fromUtf8("Дуга создана"));
  notifyGeometryChanged();
  update();
}

void SketchCanvas::commitRectanglePoint(sketch::Point point) {
  interaction_.appendRectanglePoint(point);
  if (interactionState().creation.rectanglePoints.size() < 3) { update(); return; }
  const auto first = interactionState().creation.rectanglePoints[0];
  const auto second = interactionState().creation.rectanglePoints[1];
  const double dx = second.xMm - first.xMm;
  const double dy = second.yMm - first.yMm;
  const double length = std::hypot(dx, dy);
  if (length > 1e-9) {
    const double nx = -dy / length;
    const double ny = dx / length;
    const double height = (point.xMm-first.xMm)*nx +
                          (point.yMm-first.yMm)*ny;
    if (std::abs(height) > 1e-9) {
      const sketch::Point third{second.xMm+nx*height, second.yMm+ny*height};
      const sketch::Point fourth{first.xMm+nx*height, first.yMm+ny*height};
      pushUndoState();

      const auto creationResult = executeCommand(
          AddRectangleCommand{first, second, third, fourth, false});
      if (!creationResult.accepted) {
        cancelPendingUndo();
        return;
      }

      if (!executeCommand(AutoConstrainNewGeometryCommand{
               creationResult.changedGeometryIds,
               8.0 / std::max(0.001, pixelsPerMm_)})
               .accepted) {
        cancelPendingUndo();
        return;
      }

      interaction_.clearRectanglePoints();
      notifyGeometryChanged();
      return;
    }
  }
  interaction_.clearRectanglePoints();
  update();
}

void SketchCanvas::commitCirclePoint(sketch::Point point) {
  constexpr double minRadiusMm = 1e-6;

  // Resolve a picked construction point back to an existing CAD point.
  // This is used after the new circle is created to persist the relation
  // as PointOnCircle rather than merely keeping equal coordinates.
  const auto nearestExistingReference =
      [this](
          sketch::Point target)
          -> std::optional<sketch::PointReference> {
        SketchPickFilter filter;
        // Preserve the historical source set: line endpoints, circle centres
        // and composite-element centres (arc endpoints were not considered).
        filter.arcEndpoints = false;
        const auto hit = pointAt(mapPoint(target), 10.0, filter);
        return hit ? std::optional{hit->reference} : std::nullopt;
      };

  const auto sameReference =
      [](sketch::PointReference first,
         sketch::PointReference second) {
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId !=
                sketch::kInvalidGeometryId ||
            second.circleId !=
                sketch::kInvalidGeometryId) {
          return first.circleId !=
                     sketch::kInvalidGeometryId &&
                 first.circleId ==
                     second.circleId;
        }

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  const auto finishCircle =
      [this, minRadiusMm,
       &nearestExistingReference,
       &sameReference](
          sketch::Point center,
          double radius,
          const std::vector<sketch::Point>& definingPoints,
          const std::vector<sketch::GeometryId>& tangentLineIds,
          bool addDrivingDiameter) {
        if (!std::isfinite(radius) ||
            radius < minRadiusMm)
          return false;

        // Capture references BEFORE creating the new circle so its own
        // centre can never be mistaken for one of the source points.
        std::vector<sketch::PointReference>
            sourceReferences;

        for (const auto definingPoint :
             definingPoints) {
          const auto reference =
              nearestExistingReference(
                  definingPoint);

          if (!reference)
            continue;

          const bool duplicate =
              std::any_of(
                  sourceReferences.begin(),
                  sourceReferences.end(),
                  [&sameReference, &reference](
                      sketch::PointReference existing) {
                    return sameReference(
                        existing, *reference);
                  });

          if (!duplicate)
            sourceReferences.push_back(*reference);
        }

        pushUndoState();

        const auto creationResult =
            executeCommand(AddCircleCommand{center, radius, false});
        if (!creationResult.accepted) {
          cancelPendingUndo();
          return false;
        }
        if (creationResult.changedGeometryIds.size() != 1) {
          cancelPendingUndo();
          return false;
        }
        const auto newCircleId = creationResult.changedGeometryIds.front();

        // Center-based automatic coincidence is still useful if the
        // newly computed centre itself happens to be an existing CAD point.
        if (!executeCommand(AutoConstrainNewGeometryCommand{
                 creationResult.changedGeometryIds,
                 8.0 / std::max(0.001, pixelsPerMm_)})
                 .accepted) {
          cancelPendingUndo();
          return false;
        }

        // Two-point and three-point circles are defined by points on
        // their circumference. Persist those relations explicitly.
        if (newCircleId !=
            sketch::kInvalidGeometryId) {
          for (const auto source :
               sourceReferences) {
            bool duplicate = false;

            for (const auto& existing :
                 sketch_.constraints()) {
              if (existing.type !=
                  sketch::ConstraintType::PointOnCircle)
                continue;

              if (existing.firstGeometry !=
                  newCircleId)
                continue;

              if (sameReference(
                      existing.secondPoint,
                      source)) {
                duplicate = true;
                break;
              }
            }

            if (duplicate)
              continue;

            sketch::Constraint constraint;
            constraint.type =
                sketch::ConstraintType::PointOnCircle;
            constraint.firstGeometry =
                newCircleId;
            constraint.secondPoint = source;

            if (!executeCommand(AddConstraintCommand{constraint})
                     .accepted) {
              cancelPendingUndo();
              return false;
            }
          }
        }
        // AUTO TANGENT CONSTRAINTS
        if (newCircleId != sketch::kInvalidGeometryId) {
          for (const auto lineId : tangentLineIds) {
            if (lineId == sketch::kInvalidGeometryId ||
                !sketch_.lineIndex(lineId))
              continue;

            bool duplicate = false;

            for (const auto& existing : sketch_.constraints()) {
              if (existing.type != sketch::ConstraintType::Tangent)
                continue;

              if (existing.firstGeometry == lineId &&
                  existing.secondGeometry == newCircleId) {
                duplicate = true;
                break;
              }
            }

            if (duplicate)
              continue;

            sketch::Constraint tangent;
            tangent.type = sketch::ConstraintType::Tangent;
            tangent.firstGeometry = lineId;
            tangent.secondGeometry = newCircleId;
            if (!executeCommand(AddConstraintCommand{tangent}).accepted) {
              cancelPendingUndo();
              return false;
            }
          }
        }

        circleDiameterMm_ = 2.0 * radius;
        emit primaryDimensionChanged(
            circleDiameterMm_);

        if (addDrivingDiameter) {
          sketch::Dimension dimension;
          dimension.kind = sketch::DimensionKind::CircleDiameter;
          dimension.geometryId = newCircleId;
          dimension.valueMm = circleDiameterMm_;
          dimension.offsetMm = 0.0;
          dimension.angleRad = 0.0;
          if (!executeCommand(StoreDimensionCommand{dimension}).accepted) {
            cancelPendingUndo();
            return false;
          }
          sketch::Constraint constraint;
          constraint.type = sketch::ConstraintType::Diameter;
          constraint.firstGeometry = newCircleId;
          constraint.value = circleDiameterMm_;
          if (!executeCommand(AddConstraintCommand{constraint}).accepted) {
            cancelPendingUndo();
            return false;
          }
        }

        interaction_.clearCirclePoints();
        interaction_.clearCircleGuides();

        notifyGeometryChanged();
        update();
        return true;
      };

  if (circleMode_ ==
      CircleMode::TwoPoints) {
    interaction_.appendCirclePoint(point);

    if (interactionState().creation.circlePoints.size() < 2) {
      hoverPoint_ = point;
      update();
      return;
    }

    const auto first = interactionState().creation.circlePoints[0];
    const auto second = interactionState().creation.circlePoints[1];

    const double diameter =
        std::hypot(
            second.xMm - first.xMm,
            second.yMm - first.yMm);

    if (diameter <= minRadiusMm * 2.0) {
      interaction_.clearCirclePoints();
      update();
      return;
    }

    const sketch::Point center{
        (first.xMm + second.xMm) * 0.5,
        (first.yMm + second.yMm) * 0.5};

    (void)finishCircle(
        center,
        diameter * 0.5,
        {first, second},
        {}, false);

    return;
  }

  if (circleMode_ ==
      CircleMode::ThreePoints) {
    interaction_.appendCirclePoint(point);

    if (interactionState().creation.circlePoints.size() < 3) {
      hoverPoint_ = point;
      update();
      return;
    }

    const auto first = interactionState().creation.circlePoints[0];
    const auto second = interactionState().creation.circlePoints[1];
    const auto third = interactionState().creation.circlePoints[2];

    const auto result =
        circleThroughThreePoints(
            first, second, third);

    if (!result) {
      interaction_.clearCirclePoints();
      update();
      return;
    }

    (void)finishCircle(
        result->first,
        result->second,
        {first, second, third},
        {}, false);

    return;
  }

  const auto nearestLine =
      [this](sketch::Point target)
          -> std::optional<sketch::GeometryId> {
        SketchPickFilter filter;
        filter.circles = false;
        filter.arcs = false;
        filter.includeConstruction = false;
        filter.includeProjected = false;
        const auto hit = geometryAt(
            mapPoint(target), std::max(12.0, 3.0 * pixelsPerMm_), filter);
        if (!hit || hit->kind != SketchPickEntityKind::Line)
          return std::nullopt;
        return sketch_.lineIndex(hit->geometryId)
                   ? std::optional{hit->geometryId}
                   : std::nullopt;
      };

  if ((circleMode_ ==
           CircleMode::ThreeTangents &&
       interactionState().creation.circleGuideIds.size() < 3) ||
      (circleMode_ ==
           CircleMode::TwoTangentsRadius &&
       interactionState().creation.circleGuideIds.size() < 2)) {
    const auto lineId =
        nearestLine(point);

    if (!lineId)
      return;
    const bool duplicate =
        std::find(interactionState().creation.circleGuideIds.begin(),
                  interactionState().creation.circleGuideIds.end(), *lineId) !=
        interactionState().creation.circleGuideIds.end();

    if (!duplicate)
      interaction_.appendCircleGuide(*lineId);
    update();

    if (circleMode_ ==
        CircleMode::TwoTangentsRadius) {
      if (interactionState().creation.circleGuideIds.size() == 2) {
        // TWO-TANGENT LIVE PREVIEW START
        interaction_.setTwoTangentPreview(true);

        primaryDimension_->setPrefix(
            QString::fromUtf8("Ø "));
        primaryDimension_->setSuffix(
            QString::fromUtf8(" мм"));
        primaryDimension_->setRange(
            0.02, 100000.0);
        primaryDimension_->setDecimals(2);
        primaryDimension_->setValue(
            std::max(0.02, circleDiameterMm_));

        const auto guides = resolvedCircleGuideLines();
        if (!guides || guides->size() != 2) {
          interaction_.clearCircleGuides();
          interaction_.setTwoTangentPreview(false);
          return;
        }
        const auto intersection = intersectLines((*guides)[0], (*guides)[1]);

        const QPointF editorPoint =
            intersection
                ? mapPoint(*intersection)
                : mapPoint(point);

        primaryDimension_->move(
            (editorPoint +
             QPointF(18.0, 18.0)).toPoint());
        primaryDimension_->show();
        primaryDimension_->raise();
        primaryDimension_->clearFocus();

        emit selectionChanged(
            QString::fromUtf8(
                "Перемещайте мышь для изменения диаметра, "
                "щелкните для подтверждения или введите Ø и нажмите Enter"));
      }

      return;
    }
  }

  const auto circleGuides = resolvedCircleGuideLines();
  if ((circleMode_ == CircleMode::ThreeTangents &&
       interactionState().creation.circleGuideIds.size() == 3) ||
      (circleMode_ == CircleMode::TwoTangentsRadius &&
       interactionState().creation.circleGuideIds.size() == 2)) {
    if (!circleGuides ||
        circleGuides->size() !=
            interactionState().creation.circleGuideIds.size()) {
      interaction_.clearCircleGuides();
      interaction_.setTwoTangentPreview(false);
      update();
      return;
    }
  }

  if (circleMode_ ==
          CircleMode::ThreeTangents &&
      circleGuides && circleGuides->size() == 3) {
    // THREE TANGENTS FINITE-SEGMENT SOLUTION V2
    //
    // Three infinite lines have up to four tangent circles:
    // one incircle and three excircles. Select the solution that best
    // corresponds to the finite segments clicked by the user.

    const auto p0 =
        intersectLines(
            (*circleGuides)[0], (*circleGuides)[1]);

    const auto p1 =
        intersectLines(
            (*circleGuides)[1], (*circleGuides)[2]);

    const auto p2 =
        intersectLines(
            (*circleGuides)[2], (*circleGuides)[0]);

    if (!p0 || !p1 || !p2) {
      interaction_.clearCircleGuides();
      update();
      return;
    }

    const double a =
        std::hypot(
            p1->xMm - p2->xMm,
            p1->yMm - p2->yMm);

    const double b =
        std::hypot(
            p0->xMm - p2->xMm,
            p0->yMm - p2->yMm);

    const double c =
        std::hypot(
            p0->xMm - p1->xMm,
            p0->yMm - p1->yMm);

    const auto weightedCenter =
        [](sketch::Point first,
           sketch::Point second,
           sketch::Point third,
           double wa,
           double wb,
           double wc)
            -> std::optional<sketch::Point> {
          const double denominator = wa + wb + wc;

          if (std::abs(denominator) <= 1e-9)
            return std::nullopt;

          const sketch::Point center{
              (wa * first.xMm +
               wb * second.xMm +
               wc * third.xMm) /
                  denominator,
              (wa * first.yMm +
               wb * second.yMm +
               wc * third.yMm) /
                  denominator};

          if (!std::isfinite(center.xMm) ||
              !std::isfinite(center.yMm))
            return std::nullopt;

          return center;
        };

    std::vector<sketch::Point> candidates;

    const auto addCandidate =
        [&candidates](std::optional<sketch::Point> center) {
          if (!center)
            return;

          const bool duplicate =
              std::any_of(
                  candidates.begin(),
                  candidates.end(),
                  [&center](sketch::Point existing) {
                    return std::hypot(
                               existing.xMm - center->xMm,
                               existing.yMm - center->yMm) <= 1e-7;
                  });

          if (!duplicate)
            candidates.push_back(*center);
        };

    // Incenter.
    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            a, b, c));

    // Three excenters.
    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            -a, b, c));

    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            a, -b, c));

    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            a, b, -c));

    const auto tangentFootPenalty =
        [](sketch::Point center,
           const sketch::Line& line) {
          const double dx =
              line.end.xMm - line.start.xMm;
          const double dy =
              line.end.yMm - line.start.yMm;

          const double lengthSquared =
              dx * dx + dy * dy;

          if (lengthSquared <= 1e-12)
            return 1e12;

          const double t =
              ((center.xMm - line.start.xMm) * dx +
               (center.yMm - line.start.yMm) * dy) /
              lengthSquared;

          if (t >= 0.0 && t <= 1.0)
            return 0.0;

          const double segmentLength =
              std::sqrt(lengthSquared);

          const double outside =
              t < 0.0 ? -t : t - 1.0;

          return outside * outside *
                 segmentLength * segmentLength;
        };

    const auto endpointProximityScore =
        [](sketch::Point center,
           const sketch::Line& line) {
          const double d0 =
              std::hypot(
                  center.xMm - line.start.xMm,
                  center.yMm - line.start.yMm);

          const double d1 =
              std::hypot(
                  center.xMm - line.end.xMm,
                  center.yMm - line.end.yMm);

          return std::min(d0, d1);
        };

    std::optional<sketch::Point> bestCenter;
    double bestScore =
        std::numeric_limits<double>::max();

    for (const auto candidate : candidates) {
      const double r0 =
          infiniteLineDistance(
              candidate,
              (*circleGuides)[0]);

      const double r1 =
          infiniteLineDistance(
              candidate,
              (*circleGuides)[1]);

      const double r2 =
          infiniteLineDistance(
              candidate,
              (*circleGuides)[2]);

      if (!std::isfinite(r0) ||
          !std::isfinite(r1) ||
          !std::isfinite(r2))
        continue;

      const double radius =
          (r0 + r1 + r2) / 3.0;

      if (radius <= 1e-9)
        continue;

      const double residual =
          std::abs(r0 - radius) +
          std::abs(r1 - radius) +
          std::abs(r2 - radius);

      const double finitePenalty =
          tangentFootPenalty(
              candidate,
              (*circleGuides)[0]) +
          tangentFootPenalty(
              candidate,
              (*circleGuides)[1]) +
          tangentFootPenalty(
              candidate,
              (*circleGuides)[2]);

      // If several solutions have tangent feet on all finite segments,
      // prefer the geometrically nearer one instead of a huge remote circle.
      const double proximity =
          endpointProximityScore(
              candidate,
              (*circleGuides)[0]) +
          endpointProximityScore(
              candidate,
              (*circleGuides)[1]) +
          endpointProximityScore(
              candidate,
              (*circleGuides)[2]);

      const double score =
          finitePenalty * 1000000.0 +
          residual * 1000.0 +
          proximity;

      if (score < bestScore) {
        bestScore = score;
        bestCenter = candidate;
      }
    }

    if (!bestCenter) {
      interaction_.clearCircleGuides();
      update();
      return;
    }

    const double radius =
        infiniteLineDistance(
            *bestCenter,
            (*circleGuides)[0]);

    (void)finishCircle(
        *bestCenter,
        radius,
        {},
        interactionState().creation.circleGuideIds, false);

    return;
  }

  if (circleMode_ ==
          CircleMode::TwoTangentsRadius &&
      circleGuides && circleGuides->size() == 2) {
    interaction_.appendCirclePoint(point);

    const double requestedDiameter =
        primaryDimension_->isVisible()
            ? primaryDimension_->value()
            : circleDiameterMm_;

    circleDiameterMm_ =
        std::max(0.02, requestedDiameter);

    const auto finiteRequestedCircle =
        clampedTwoTangentCircleForRadius(
            (*circleGuides)[0], (*circleGuides)[1],
            circleDiameterMm_ * 0.5,
            point);

    if (!finiteRequestedCircle) {
      emit selectionChanged(
          QString::fromUtf8(
              "Для выбранных отрезков невозможно построить "
              "касательную окружность этого диаметра"));
      update();
      return;
    }

    circleDiameterMm_ =
        finiteRequestedCircle->radiusMm * 2.0;

    {
      const QSignalBlocker blocker(
          primaryDimension_);
      primaryDimension_->setValue(
          circleDiameterMm_);
    }

    const double radius =
        finiteRequestedCircle->radiusMm;

    struct Equation {
      double nx;
      double ny;
      double c;
    };

    Equation equations[2];

    for (int i = 0; i < 2; ++i) {
      const auto& line =
          (*circleGuides)[i];

      const double dx =
          line.end.xMm -
          line.start.xMm;

      const double dy =
          line.end.yMm -
          line.start.yMm;

      const double length =
          std::hypot(dx, dy);

      if (length <= 1e-9) {
        interaction_.clearCircleGuides();
        interaction_.clearCirclePoints();
        update();
        return;
      }

      equations[i] = {
          -dy / length,
          dx / length,
          0.0};

      equations[i].c =
          equations[i].nx *
              line.start.xMm +
          equations[i].ny *
              line.start.yMm;
    }

    double best =
        std::numeric_limits<double>::max();

    std::optional<sketch::Point> center;

    const double det =
        equations[0].nx *
            equations[1].ny -
        equations[0].ny *
            equations[1].nx;

    if (std::abs(det) > 1e-9) {
      for (double s0 :
           {-1.0, 1.0}) {
        for (double s1 :
             {-1.0, 1.0}) {
          const double r0 =
              equations[0].c +
              s0 * radius;

          const double r1 =
              equations[1].c +
              s1 * radius;

          const sketch::Point candidate{
              (r0 * equations[1].ny -
               equations[0].ny * r1) /
                  det,
              (equations[0].nx * r1 -
               r0 * equations[1].nx) /
                  det};

          const double distance =
              std::hypot(
                  candidate.xMm -
                      point.xMm,
                  candidate.yMm -
                      point.yMm);

          if (distance < best) {
            best = distance;
            center = candidate;
          }
        }
      }
    }

    if (finiteRequestedCircle) {

      const std::size_t circlesBefore =
          sketch_.circles().size();

      const bool created =
          finishCircle(
              finiteRequestedCircle->center,
              radius,
              {},
              interactionState().creation.circleGuideIds, true);
      if (!created || sketch_.circles().size() <= circlesBefore)
        emit constraintStatusChanged(
            QString::fromUtf8("Не удалось создать окружность и размер"));
    }

    interaction_.setTwoTangentPreview(false);
    hideDimensionEditor();
    interaction_.clearCirclePoints();
    update();
    return;
  }
}

void SketchCanvas::showDimensionEditor(QPoint position) {
  primaryDimension_->move(position);
  secondaryDimension_->move(position + QPoint(0, 42));
  primaryDimension_->setRange(0.01, 100000.0);
  secondaryDimension_->setRange(-360.0, 100000.0);
  if (tool() == Tool::Line) {
    primaryDimension_->setFixedWidth(122);
    secondaryDimension_->setFixedWidth(122);
    primaryDimension_->setFrame(true);
    secondaryDimension_->setFrame(true);
    primaryDimension_->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    secondaryDimension_->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    primaryDimension_->setAlignment(Qt::AlignLeft);
    secondaryDimension_->setAlignment(Qt::AlignLeft);
    primaryDimension_->setPrefix(QString::fromUtf8("L: "));
    secondaryDimension_->setPrefix(QString::fromUtf8("Угол: "));
    secondaryDimension_->setSuffix(QString::fromUtf8(" °"));
    secondaryDimension_->show();
  } else if (tool() == Tool::Rectangle) {
    primaryDimension_->setFixedWidth(88);
    secondaryDimension_->setFixedWidth(88);
    primaryDimension_->setFrame(false);
    secondaryDimension_->setFrame(false);
    primaryDimension_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    secondaryDimension_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    primaryDimension_->setAlignment(Qt::AlignCenter);
    secondaryDimension_->setAlignment(Qt::AlignCenter);
    primaryDimension_->setPrefix(QString());
    secondaryDimension_->setPrefix(QString());
    primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    secondaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    secondaryDimension_->setRange(0.01, 100000.0);
    secondaryDimension_->show();
  } else {
    primaryDimension_->setFixedWidth(122);
    primaryDimension_->setFrame(true);
    primaryDimension_->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    primaryDimension_->setAlignment(Qt::AlignLeft);
    primaryDimension_->setPrefix(QString::fromUtf8("Ø: "));
    secondaryDimension_->hide();
  }
  primaryDimension_->show();
  updateDimensionEditor();
  primaryDimension_->setFocus();
  primaryDimension_->selectAll();
}

void SketchCanvas::updateDimensionEditor() {
  if (!interactionState().creation.anchor || !primaryDimension_->isVisible()) return;
  const double dx = hoverPoint_.xMm - interactionState().creation.anchor->xMm;
  const double dy = hoverPoint_.yMm - interactionState().creation.anchor->yMm;
  const bool primaryFocused = primaryDimension_->hasFocus();
  const bool secondaryFocused = secondaryDimension_->hasFocus();
  if (tool() == Tool::Line)
    primaryDimension_->setValue(std::max(0.01, std::hypot(dx, dy)));
  else if (tool() == Tool::Rectangle)
    primaryDimension_->setValue(std::max(
        0.01, std::abs(dx) *
                  (rectangleMode_ == RectangleMode::FromCenter ? 2.0 : 1.0)));
  else
    primaryDimension_->setValue(std::max(0.01, 2.0 * std::hypot(dx, dy)));
  if (secondaryDimension_->isVisible()) {
    secondaryDimension_->setValue(tool() == Tool::Line
                                      ? std::atan2(dy, dx) * 180.0 / 3.141592653589793
                                      : std::max(
                                            0.01, std::abs(dy) *
                                                      (rectangleMode_ == RectangleMode::FromCenter
                                                           ? 2.0 : 1.0)));
  }
  positionDimensionEditor();
  emit primaryDimensionChanged(primaryDimension_->value());
  // QDoubleSpinBox clears its text selection whenever setValue() changes the
  // displayed number. Keep the active dimension ready for immediate typing.
  if (primaryFocused)
    primaryDimension_->selectAll();
  else if (secondaryFocused)
    secondaryDimension_->selectAll();
}

void SketchCanvas::positionDimensionEditor() {
  if (!interactionState().creation.anchor || !primaryDimension_->isVisible()) return;
  const QPointF anchorPosition = mapPoint(*interactionState().creation.anchor);
  const QPointF tipPosition = mapPoint(hoverPoint_);

  if (tool() == Tool::Rectangle) {
    const QRectF rectangle = rectangleMode_ == RectangleMode::FromCenter
                                 ? QRectF(anchorPosition -
                                              (tipPosition - anchorPosition),
                                          anchorPosition +
                                              (tipPosition - anchorPosition))
                                       .normalized()
                                 : QRectF(anchorPosition, tipPosition).normalized();
    constexpr double offset = 24.0;
    const QPoint primaryPosition(
        qRound(rectangle.center().x() - primaryDimension_->width() * 0.5),
        qRound(rectangle.bottom() + offset - primaryDimension_->height() * 0.5));
    const QPoint secondaryPosition(
        qRound(rectangle.right() + offset - secondaryDimension_->width() * 0.5),
        qRound(rectangle.center().y() - secondaryDimension_->height() * 0.5));
    const auto clamped = [this](QPoint position, const QWidget* editor) {
      position.setX(std::clamp(position.x(), static_cast<int>(kRulerLeft + 6),
                               std::max(static_cast<int>(kRulerLeft + 6),
                                        width() - editor->width() - 8)));
      position.setY(std::clamp(position.y(), static_cast<int>(kRulerTop + 6),
                               std::max(static_cast<int>(kRulerTop + 6),
                                        height() - editor->height() - 8)));
      return position;
    };
    primaryDimension_->move(clamped(primaryPosition, primaryDimension_));
    secondaryDimension_->move(clamped(secondaryPosition, secondaryDimension_));
    return;
  }

  QPointF direction = tipPosition - anchorPosition;
  const double length = std::hypot(direction.x(), direction.y());
  direction = length < 1.0 ? QPointF(1.0, 0.5) : direction / length;

  const int editorHeight = secondaryDimension_->isVisible() ? 82 : 40;
  const int editorWidth = std::max(primaryDimension_->width(),
                                   secondaryDimension_->width());
  const QPointF normal{-direction.y(), direction.x()};
  const QPointF midpoint = (anchorPosition + tipPosition) * 0.5;
  const double normalDistance = editorHeight * 0.5 + 20.0;
  const std::array<QPointF, 6> centers{
      tipPosition + normal * normalDistance,
      tipPosition - normal * normalDistance,
      midpoint + normal * normalDistance,
      midpoint - normal * normalDistance,
      anchorPosition + normal * normalDistance,
      anchorPosition - normal * normalDistance};

  const auto clamped = [this, editorWidth, editorHeight](QPoint position) {
    position.setX(std::clamp(position.x(), static_cast<int>(kRulerLeft + 6),
                             std::max(static_cast<int>(kRulerLeft + 6),
                                      width() - editorWidth - 8)));
    position.setY(std::clamp(position.y(), static_cast<int>(kRulerTop + 6),
                             std::max(static_cast<int>(kRulerTop + 6),
                                      height() - editorHeight - 8)));
    return position;
  };
  const auto segmentTouchesRect = [](QPointF first, QPointF second,
                                     const QRectF& rect) {
    if (rect.contains(first) || rect.contains(second)) return true;
    const QLineF segment(first, second);
    const std::array<QLineF, 4> borders{
        QLineF(rect.topLeft(), rect.topRight()),
        QLineF(rect.topRight(), rect.bottomRight()),
        QLineF(rect.bottomRight(), rect.bottomLeft()),
        QLineF(rect.bottomLeft(), rect.topLeft())};
    QPointF intersection;
    return std::any_of(borders.begin(), borders.end(),
                       [&segment, &intersection](const QLineF& border) {
                         return segment.intersects(border, &intersection) ==
                                QLineF::BoundedIntersection;
                       });
  };

  QPoint bestPosition;
  double bestScore = std::numeric_limits<double>::max();
  for (const auto center : centers) {
    const QPoint candidate = clamped(
        (center - QPointF(editorWidth * 0.5, editorHeight * 0.5)).toPoint());
    const QRectF occupied(QPointF(candidate),
                          QSizeF(editorWidth, editorHeight));
    double score = QLineF(center, tipPosition).length();
    if (segmentTouchesRect(anchorPosition, tipPosition,
                           occupied.adjusted(-8, -8, 8, 8)))
      score += 100000.0;
    for (const auto& line : sketch_.lines()) {
      if (segmentTouchesRect(mapPoint(line.start), mapPoint(line.end),
                             occupied.adjusted(-6, -6, 6, 6)))
        score += 1000.0;
    }
    if (score < bestScore) {
      bestScore = score;
      bestPosition = candidate;
    }
  }
  primaryDimension_->move(bestPosition);
  secondaryDimension_->move(bestPosition + QPoint(0, 42));
}

void SketchCanvas::setPrimaryDimension(double value) {
  if (!interactionState().creation.anchor || !primaryDimension_->isVisible() || value <= 0.0) return;
  const double dx = hoverPoint_.xMm - interactionState().creation.anchor->xMm;
  const double dy = hoverPoint_.yMm - interactionState().creation.anchor->yMm;
  const double angle = std::atan2(dy, dx);
  if (tool() == Tool::Circle) {
    hoverPoint_ = {interactionState().creation.anchor->xMm + value * 0.5 * std::cos(angle),
                   interactionState().creation.anchor->yMm + value * 0.5 * std::sin(angle)};
  } else if (tool() == Tool::Line) {
    hoverPoint_ = {interactionState().creation.anchor->xMm + value * std::cos(angle),
                   interactionState().creation.anchor->yMm + value * std::sin(angle)};
  } else if (tool() == Tool::Rectangle) {
    const double extent = rectangleMode_ == RectangleMode::FromCenter
                              ? value * 0.5 : value;
    hoverPoint_.xMm = interactionState().creation.anchor->xMm + (dx < 0.0 ? -extent : extent);
  }
  {
    const QSignalBlocker blocker(primaryDimension_);
    primaryDimension_->setValue(value);
  }
  positionDimensionEditor();
  update();
}

void SketchCanvas::setSelectedDashed(bool dashed) {
  SetSelectionDashedCommand command;
  command.lineIds = selectedLineIds_;
  command.elementIds = selectedElementIds_;
  command.circleIds = selectedCircleIds_;
  command.arcIds = selectedArcIds_;
  command.dashed = dashed;
  if (command.lineIds.empty() && command.elementIds.empty() &&
      command.circleIds.empty() && command.arcIds.empty()) {
    if (selectionKind_ == SelectionKind::Line)
      command.lineIds.push_back(selectionLineId_);
    else if (selectionKind_ == SelectionKind::Circle)
      command.circleIds.push_back(selectionCircleId_);
    else if (selectionKind_ == SelectionKind::Arc)
      command.arcIds.push_back(selectionArcId_);
  }
  if (command.lineIds.empty() && command.elementIds.empty() &&
      command.circleIds.empty() && command.arcIds.empty())
    return;
  pushUndoState();
  const auto result = executeCommand(command);
  if (!result.accepted) {
    cancelPendingUndo();
    return;
  }
  emit lineStyleSelectionChanged(true, dashed);
  notifyGeometryChanged();
}

void SketchCanvas::commitCurrentDimension() { commitDimensionEditor(); }

void SketchCanvas::setGridVisible(bool visible) {
  gridVisible_ = visible;
  update();
}

void SketchCanvas::setSnapEnabled(bool enabled) {
  snapEnabled_ = enabled;
  update();
}

void SketchCanvas::commitDimensionEditor() {
  if (!interactionState().creation.anchor) return;
  pushUndoState();

  const std::size_t oldLineCount = sketch_.lines().size();
  std::vector<sketch::GeometryId> newGeometryIds;

  const auto start = *interactionState().creation.anchor;
  if (tool() == Tool::Line) {
    const double angle = secondaryDimension_->value() * 3.141592653589793 / 180.0;
    const auto creationResult = executeCommand(AddLineCommand{
        start,
        {start.xMm + primaryDimension_->value() * std::cos(angle),
         start.yMm + primaryDimension_->value() * std::sin(angle)},
        std::nullopt});
    if (!creationResult.accepted) return;
    newGeometryIds = creationResult.changedGeometryIds;
  } else if (tool() == Tool::Rectangle) {
    const double sx = hoverPoint_.xMm < start.xMm ? -1.0 : 1.0;
    const double sy = hoverPoint_.yMm < start.yMm ? -1.0 : 1.0;
    if (rectangleMode_ == RectangleMode::FromCenter) {
      const double halfWidth = primaryDimension_->value() * 0.5;
      const double halfHeight = secondaryDimension_->value() * 0.5;
      const auto creationResult = executeCommand(AddRectangleCommand{
          {start.xMm - sx * halfWidth, start.yMm - sy * halfHeight},
          {start.xMm + sx * halfWidth, start.yMm + sy * halfHeight},
          std::nullopt, std::nullopt, true});
      if (!creationResult.accepted) return;
      newGeometryIds = creationResult.changedGeometryIds;
    } else {
      const auto creationResult = executeCommand(AddRectangleCommand{
          start,
          {start.xMm + sx * primaryDimension_->value(),
           start.yMm + sy * secondaryDimension_->value()},
          std::nullopt, std::nullopt, false});
      if (!creationResult.accepted) return;
      newGeometryIds = creationResult.changedGeometryIds;
    }

    // Enter confirms explicit driving dimensions. A rectangle finished with
    // the mouse remains free of dimensional constraints and annotations.
    if (sketch_.lines().size() == oldLineCount + 4) {
      std::size_t horizontalIndex = oldLineCount;
      std::size_t verticalIndex = oldLineCount + 1;
      double lowestY = std::numeric_limits<double>::max();
      double rightmostX = std::numeric_limits<double>::lowest();

      for (std::size_t index = oldLineCount;
           index < oldLineCount + 4; ++index) {
        const auto& line = sketch_.lines()[index];
        const double dx = std::abs(line.end.xMm - line.start.xMm);
        const double dy = std::abs(line.end.yMm - line.start.yMm);
        if (dx >= dy) {
          const double y = (line.start.yMm + line.end.yMm) * 0.5;
          if (y < lowestY) {
            lowestY = y;
            horizontalIndex = index;
          }
        } else {
          const double x = (line.start.xMm + line.end.xMm) * 0.5;
          if (x > rightmostX) {
            rightmostX = x;
            verticalIndex = index;
          }
        }
      }

      QPointF rectangleCenter;
      for (std::size_t index = oldLineCount;
           index < oldLineCount + 4; ++index)
        rectangleCenter += mapPoint(sketch_.lines()[index].start);
      rectangleCenter /= 4.0;
      const auto outwardOffset = [this, rectangleCenter](
                                     const sketch::Line& line) {
        const QPointF first = mapPoint(line.start);
        const QPointF second = mapPoint(line.end);
        QPointF direction = second - first;
        const double length = std::hypot(direction.x(), direction.y());
        if (length <= 1e-9) return 4.0;
        direction /= length;
        const QPointF normal{-direction.y(), direction.x()};
        const QPointF midpoint = (first + second) * 0.5;
        return QPointF::dotProduct(normal, midpoint - rectangleCenter) >= 0.0
                   ? 4.0
                   : -4.0;
      };

      const auto addDrivingDimension =
          [this](std::size_t lineIndex,
                 sketch::ConstraintType constraintType,
                 sketch::DimensionKind dimensionKind,
                 double valueMm, double offsetMm) -> bool {
            const auto lineId = sketch_.lineId(lineIndex);
            if (lineId == sketch::kInvalidGeometryId) return false;

            sketch::Constraint constraint;
            constraint.type = constraintType;
            constraint.firstPoint = {lineId, true};
            constraint.secondPoint = {lineId, false};
            constraint.value = valueMm;
            if (!executeCommand(AddConstraintCommand{constraint}).accepted)
              return false;

            sketch::Dimension dimension;
            dimension.kind = dimensionKind;
            dimension.firstPoint = constraint.firstPoint;
            dimension.secondPoint = constraint.secondPoint;
            dimension.valueMm = valueMm;
            dimension.offsetMm = offsetMm;
            if (!executeCommand(StoreDimensionCommand{dimension}).accepted)
              return false;
            return true;
          };

      const bool widthAdded = addDrivingDimension(
          horizontalIndex, sketch::ConstraintType::DistanceX,
          sketch::DimensionKind::PointDistanceX,
          primaryDimension_->value(),
          outwardOffset(sketch_.lines()[horizontalIndex]));
      const bool heightAdded = widthAdded && addDrivingDimension(
          verticalIndex, sketch::ConstraintType::DistanceY,
          sketch::DimensionKind::PointDistanceY,
          secondaryDimension_->value(),
          outwardOffset(sketch_.lines()[verticalIndex]));
      if (!heightAdded) {
        // Numeric rectangle creation is one transaction: never leave a
        // rectangle with only one of its two promised driving dimensions.
        cancelPendingUndo();
        emit undoAvailable(canUndo());
        emit redoAvailable(canRedo());
        interaction_.cancelCreation();
        hideDimensionEditor();
        emit constraintStatusChanged(
            QString::fromUtf8("Не удалось создать размеры прямоугольника"));
        update();
        return;
      }
    }
  } else if (tool() == Tool::Circle) {
    const auto creationResult = executeCommand(AddCircleCommand{
        start, primaryDimension_->value() * 0.5, false});
    if (!creationResult.accepted) return;
    newGeometryIds = creationResult.changedGeometryIds;
    circleDiameterMm_ = primaryDimension_->value();
    emit primaryDimensionChanged(circleDiameterMm_);
  }
  if (!newGeometryIds.empty() &&
      !executeCommand(AutoConstrainNewGeometryCommand{
           newGeometryIds, 8.0 / std::max(0.001, pixelsPerMm_)})
           .accepted) {
    cancelPendingUndo();
    return;
  }

  interaction_.completeCreation();
  hideDimensionEditor();
  setFocus();
  notifyGeometryChanged();
}

void SketchCanvas::hideDimensionEditor() {
  primaryDimension_->hide();
  secondaryDimension_->hide();
}

void SketchCanvas::notifyGeometryChanged() {
  if (commandSequenceFailed_) {
    cancelPendingUndo();
    commandSequenceFailed_ = false;
    update();
    return;
  }
  markCommittedRenderSceneDirty();
  const bool hadPendingTransaction = pendingUndoTransaction_.has_value();
  const bool committed = finalizeUndoState();
  if (hadPendingTransaction && !committed) {
    update();
    return;
  }
  emit geometryChanged(sketch_.widthMm(), sketch_.heightMm());
  update();

  // Geometry repaint and the lightweight component solve stay synchronous.
  // The numerical Jacobian/rank diagnostics are coalesced across bursts of UI
  // updates and explicitly flushed at interaction boundaries.
  if (constraintDiagnosticsTimer_) constraintDiagnosticsTimer_->start();
}

SketchCommandResult SketchCanvas::executeCommand(const SketchCommand& command) {
  if (commandSequenceFailed_)
    return SketchCommandResult{
        false, SketchCommandError::TransactionMismatch};
  auto result = commandController_.execute(sketch_, command);
  if (result.accepted) {
    applyCommandEffects(result.effects);
  } else if (pendingUndoTransaction_) {
    const auto token = *pendingUndoTransaction_;
    if (commandController_.cancelTransaction(sketch_, token,
                                             sketchGeneration_) ||
        !commandController_.hasActiveTransaction())
      pendingUndoTransaction_.reset();
    commandSequenceFailed_ = true;
  }
  return result;
}

SketchCommandResult SketchCanvas::executeLiveCommand(
    const SketchLiveCommand& command) {
  if (commandSequenceFailed_ || !pendingUndoTransaction_)
    return SketchCommandResult{
        false, SketchCommandError::TransactionMismatch};
  auto result = commandController_.executeInTransaction(
      sketch_, *pendingUndoTransaction_, sketchGeneration_, command);
  if (result.accepted) {
    applyCommandEffects(result.effects);
  } else {
    const auto token = *pendingUndoTransaction_;
    if (commandController_.cancelTransaction(sketch_, token,
                                             sketchGeneration_) ||
        !commandController_.hasActiveTransaction())
      pendingUndoTransaction_.reset();
    commandSequenceFailed_ = true;
  }
  return result;
}

void SketchCanvas::applyCommandEffects(const SketchCommandEffects& effects) {
  if (effects.committedRenderSceneDirty) markCommittedRenderSceneDirty();
  if (effects.selectionMayBeStale) clearGeometrySelection();
  if (effects.diagnosticsRequired && constraintDiagnosticsTimer_)
    constraintDiagnosticsTimer_->start();
}

void SketchCanvas::markCommittedRenderSceneDirty() noexcept {
  renderSceneCache_.invalidate();
  if (renderSceneRevision_ == std::numeric_limits<std::uint64_t>::max())
    renderSceneRevision_ = 1;
  else
    ++renderSceneRevision_;
}

void SketchCanvas::flushConstraintDiagnostics() {
  if (!constraintDiagnosticsTimer_ || !constraintDiagnosticsTimer_->isActive())
    return;
  constraintDiagnosticsTimer_->stop();
  runConstraintDiagnostics();
}

std::size_t SketchCanvas::fullDiagnosticsCount() const noexcept {
  return fullDiagnosticsCount_;
}

void SketchCanvas::runConstraintDiagnostics() {
  ++fullDiagnosticsCount_;

  const auto constraintState =
      sketch::analyzeConstraintSystem(
          sketch_, true);

  QString constraintText;

  if (sketch_.lines().empty() && sketch_.circles().empty() &&
      sketch_.arcs().empty()) {
    constraintText =
        QString::fromUtf8("Эскиз пуст");
  } else if (constraintState.conflicting) {
    constraintText =
        QString::fromUtf8(
            "Конфликт ограничений · нарушено: %1")
            .arg(
                constraintState.violations.size());
  } else if (!constraintState.components.empty() &&
             std::all_of(constraintState.components.begin(),
                         constraintState.components.end(),
                         [](const auto& component) {
                           return component.degreesOfFreedom == 0 &&
                                  !component.conflicting;
                         })) {
    constraintText =
        QString::fromUtf8(
            "Эскиз полностью определён · DOF: 0");
  } else {
    constraintText =
        QString::fromUtf8(
            "Эскиз недоопределён · DOF: %1")
            .arg(
                constraintState.degreesOfFreedom);
  }

  emit constraintStatusChanged(
      constraintText);
}

void SketchCanvas::pushUndoState() {
  if (pendingUndoTransaction_) return;
  commandSequenceFailed_ = false;
  pendingUndoTransaction_ =
      commandController_.beginTransaction(sketch_, sketchGeneration_);
}

bool SketchCanvas::finalizeUndoState() {
  if (commandSequenceFailed_) {
    cancelPendingUndo();
    return false;
  }
  if (!pendingUndoTransaction_) return false;
  const auto token = *pendingUndoTransaction_;
  auto delta = commandController_.finishTransaction(
      sketch_, token, sketchGeneration_);
  if (!delta) {
    if (!commandController_.hasActiveTransaction())
      pendingUndoTransaction_.reset();
    return false;
  }
  pendingUndoTransaction_.reset();
  if (delta->empty()) return false;
  redoStack_.clear();
  redoRetainedBytes_ = 0;
  undoRetainedBytes_ += delta->retainedBytes;
  undoStack_.push_back(std::move(*delta));
  constexpr std::size_t maxUndoSteps = 100;
  constexpr std::size_t maxRetainedBytes = 32U * 1024U * 1024U;
  while (undoStack_.size() > 1 &&
         (undoStack_.size() > maxUndoSteps ||
          undoRetainedBytes_ > maxRetainedBytes)) {
    undoRetainedBytes_ -= undoStack_.front().retainedBytes;
    undoStack_.erase(undoStack_.begin());
  }
  emit undoAvailable(true);
  emit redoAvailable(false);
  return true;
}

void SketchCanvas::cancelPendingUndo() {
  if (!pendingUndoTransaction_) return;
  const auto token = *pendingUndoTransaction_;
  if (commandController_.cancelTransaction(sketch_, token,
                                           sketchGeneration_) ||
      !commandController_.hasActiveTransaction())
    pendingUndoTransaction_.reset();
}

}  // namespace solidar

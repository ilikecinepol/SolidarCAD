#include "ui/ViewportRuler.h"

#include <QFontMetricsF>
#include <QLineF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "ui/ThemeColors.h"
#include "ui/ViewportPicking.h"

namespace solidar {
namespace {

constexpr double kRulerSnapRadiusPx = 11.0;

class ProjectionCache final {
 public:
  explicit ProjectionCache(const ViewportCameraState& camera)
      : camera_(camera), matrix_(camera.worldToClip()) {
    const double yaw = camera.yawDeg * std::numbers::pi / 180.0;
    const double pitch = camera.pitchDeg * std::numbers::pi / 180.0;
    sinYaw_ = std::sin(yaw);
    cosYaw_ = std::cos(yaw);
    sinPitch_ = std::sin(pitch);
    cosPitch_ = std::cos(pitch);
  }

  [[nodiscard]] ProjectedPoint project(Point3d point) const {
    const QVector4D clip =
        matrix_ * QVector4D(point.x, point.y, point.z, 1.0F);
    return {{(clip.x() + 1.0) * camera_.logicalSize.width() * 0.5,
             (1.0 - clip.y()) * camera_.logicalSize.height() * 0.5},
            (point.x * sinYaw_ + point.y * cosYaw_) * sinPitch_ +
                point.z * cosPitch_};
  }

 private:
  const ViewportCameraState& camera_;
  QMatrix4x4 matrix_;
  double sinYaw_{};
  double cosYaw_{};
  double sinPitch_{};
  double cosPitch_{};
};

Point3d interpolate(Point3d a, Point3d b, double t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
          a.z + (b.z - a.z) * t};
}

double pointDistance(Point3d a, Point3d b) {
  return std::hypot(std::hypot(b.x - a.x, b.y - a.y), b.z - a.z);
}

std::optional<std::array<double, 3>> barycentricAt(
    QPointF point, QPointF a, QPointF b, QPointF c) {
  const QPointF v0 = b - a;
  const QPointF v1 = c - a;
  const QPointF v2 = point - a;
  const double denominator = v0.x() * v1.y() - v1.x() * v0.y();
  if (std::abs(denominator) < 1e-12) return std::nullopt;
  const double bWeight =
      (v2.x() * v1.y() - v1.x() * v2.y()) / denominator;
  const double cWeight =
      (v0.x() * v2.y() - v2.x() * v0.y()) / denominator;
  const double aWeight = 1.0 - bWeight - cWeight;
  constexpr double tolerance = 1e-9;
  if (aWeight < -tolerance || bWeight < -tolerance ||
      cWeight < -tolerance)
    return std::nullopt;
  return std::array<double, 3>{aWeight, bWeight, cWeight};
}

Point3d interpolateTriangle(const RenderTriangle& triangle,
                            const std::array<double, 3>& weights) {
  return {triangle.a.x * weights[0] + triangle.b.x * weights[1] +
              triangle.c.x * weights[2],
          triangle.a.y * weights[0] + triangle.b.y * weights[1] +
              triangle.c.y * weights[2],
          triangle.a.z * weights[0] + triangle.b.z * weights[1] +
              triangle.c.z * weights[2]};
}

double frontDepthAt(const std::vector<RenderTriangle>& triangles,
                    const ProjectionCache& projection, QPointF point) {
  double depth = -std::numeric_limits<double>::max();
  for (const auto& triangle : triangles) {
    const ProjectedPoint a = projection.project(triangle.a);
    const ProjectedPoint b = projection.project(triangle.b);
    const ProjectedPoint c = projection.project(triangle.c);
    if (const auto candidate = triangleDepthAt(point, a, b, c))
      depth = std::max(depth, *candidate);
  }
  return depth;
}

QString formattedMillimetres(double value) {
  return QLocale().toString(value, 'f', 2) + QStringLiteral(" mm");
}

}  // namespace

void ViewportRuler::begin() noexcept {
  active_ = true;
  first_.reset();
  second_.reset();
  hover_.reset();
}

void ViewportRuler::cancel() noexcept {
  active_ = false;
  first_.reset();
  second_.reset();
  hover_.reset();
}

bool ViewportRuler::active() const noexcept { return active_; }

bool ViewportRuler::updateHover(const BodyRenderMesh& mesh,
                                const ViewportCameraState& camera,
                                QPointF cursor) {
  std::vector<RenderTriangle> triangles;
  triangles.reserve(mesh.triangleCount());
  for (const auto triangle : mesh.triangles()) triangles.push_back(triangle);
  const auto previous = hover_;
  hover_ = active_ ? pickPoint(triangles, mesh.edges(), camera, cursor)
                   : std::nullopt;
  if (previous.has_value() != hover_.has_value()) return true;
  if (!previous) return false;
  return QLineF(previous->screen, hover_->screen).length() > 0.01 ||
         pointDistance(previous->world, hover_->world) > 1e-9 ||
         previous->snap != hover_->snap;
}

bool ViewportRuler::updateHover(const ProjectedPickingScene& scene,
                                QPointF cursor, bool exact) {
  const auto previous = hover_;
  if (!active_) {
    hover_.reset();
  } else {
    const std::uint64_t uncertaintyBefore = scene.counters().uncertainVisible;
    const auto hit = scene.snapAt(
        cursor, kRulerSnapRadiusPx,
        exact ? PickingQueryPrecision::Exact
              : PickingQueryPrecision::Interactive);
    if (!exact && scene.counters().uncertainVisible != uncertaintyBefore)
      return false;
    if (hit) {
      const auto snap = hit->kind == PickingSnapKind::Vertex
                            ? RulerSnapKind::Vertex
                            : hit->kind == PickingSnapKind::Edge
                                  ? RulerSnapKind::Edge
                                  : RulerSnapKind::Surface;
      hover_ = RulerHit{hit->world, hit->screen, hit->depth, snap};
    } else {
      hover_.reset();
    }
  }
  if (previous.has_value() != hover_.has_value()) return true;
  if (!previous) return false;
  return QLineF(previous->screen, hover_->screen).length() > 0.01 ||
         pointDistance(previous->world, hover_->world) > 1e-9 ||
         previous->snap != hover_->snap;
}

void ViewportRuler::clearHover() noexcept { hover_.reset(); }

RulerClickResult ViewportRuler::commitHoveredPoint() noexcept {
  return hover_ ? commitPoint(*hover_) : RulerClickResult::Ignored;
}

RulerClickResult ViewportRuler::commitPoint(const RulerHit& hit) noexcept {
  if (!active_) return RulerClickResult::Ignored;
  if (!first_) {
    first_ = hit;
    second_.reset();
    return RulerClickResult::FirstPoint;
  }
  if (!second_) {
    second_ = hit;
    return RulerClickResult::Completed;
  }
  first_ = hit;
  second_.reset();
  return RulerClickResult::Restarted;
}

const std::optional<RulerHit>& ViewportRuler::firstPoint() const noexcept {
  return first_;
}

const std::optional<RulerHit>& ViewportRuler::secondPoint() const noexcept {
  return second_;
}

const std::optional<RulerHit>& ViewportRuler::hoverPoint() const noexcept {
  return hover_;
}

std::optional<double> ViewportRuler::measuredDistanceMm() const noexcept {
  if (!first_ || !second_) return std::nullopt;
  return pointDistance(first_->world, second_->world);
}

std::optional<double> ViewportRuler::previewDistanceMm() const noexcept {
  if (!first_) return std::nullopt;
  const auto& end = second_ ? second_ : hover_;
  if (!end) return std::nullopt;
  return pointDistance(first_->world, end->world);
}

std::optional<RulerHit> ViewportRuler::pickPoint(
    const std::vector<RenderTriangle>& triangles,
    const std::vector<RenderEdge>& edges,
    const ViewportCameraState& camera, QPointF cursor) {
  if (triangles.empty()) return std::nullopt;
  const double depthEpsilon =
      std::max(1.0, camera.depthExtent) * kDepthEpsilonScale;
  const ProjectionCache projection(camera);

  // Vertices are the most useful exact snap. Test visibility at the projected
  // vertex itself so a close cursor on an adjacent face cannot hide it.
  std::optional<RulerHit> vertexHit;
  double bestVertexDistance = kRulerSnapRadiusPx;
  for (const auto& triangle : triangles) {
    for (const Point3d point : {triangle.a, triangle.b, triangle.c}) {
      const ProjectedPoint projected = projection.project(point);
      const QPointF screen = projected.screen;
      const double distance = QLineF(cursor, screen).length();
      if (distance > bestVertexDistance) continue;
      const double depth = projected.depth;
      if (depth + depthEpsilon < frontDepthAt(triangles, projection, screen))
        continue;
      bestVertexDistance = distance;
      vertexHit = RulerHit{point, screen, depth, RulerSnapKind::Vertex};
    }
  }
  if (vertexHit) return vertexHit;

  std::optional<RulerHit> edgeHit;
  double bestEdgeDistance = kRulerSnapRadiusPx;
  for (const auto& edge : edges) {
    for (std::size_t index = 1; index < edge.points.size(); ++index) {
      const Point3d aWorld = edge.points[index - 1];
      const Point3d bWorld = edge.points[index];
      const ProjectedPoint a = projection.project(aWorld);
      const ProjectedPoint b = projection.project(bWorld);
      const SegmentHit segment = closestSegmentHit(cursor, a, b);
      if (segment.distance > bestEdgeDistance) continue;
      const QPointF screen = a.screen + (b.screen - a.screen) * segment.parameter;
      if (segment.depth + depthEpsilon <
          frontDepthAt(triangles, projection, screen))
        continue;
      bestEdgeDistance = segment.distance;
      edgeHit = RulerHit{interpolate(aWorld, bWorld, segment.parameter), screen,
                         segment.depth, RulerSnapKind::Edge};
    }
  }
  if (edgeHit) return edgeHit;

  std::optional<RulerHit> surfaceHit;
  double bestDepth = -std::numeric_limits<double>::max();
  for (const auto& triangle : triangles) {
    const QPointF a = projection.project(triangle.a).screen;
    const QPointF b = projection.project(triangle.b).screen;
    const QPointF c = projection.project(triangle.c).screen;
    const auto weights = barycentricAt(cursor, a, b, c);
    if (!weights) continue;
    const Point3d world = interpolateTriangle(triangle, *weights);
    const double depth = projection.project(world).depth;
    if (depth <= bestDepth) continue;
    bestDepth = depth;
    surfaceHit =
        RulerHit{world, cursor, depth, RulerSnapKind::Surface};
  }
  return surfaceHit;
}

void ViewportRuler::paint(QPainter& painter,
                          const ViewportCameraState& camera,
                          const ThemeColors& theme) const {
  if (!active_) return;

  const auto paintMarker = [&](const RulerHit& hit, bool emphasized) {
    const QPointF point = camera.worldToScreen(hit.world);
    const double radius = emphasized ? 5.5 : 4.5;
    painter.setBrush(theme.viewportBackground);
    painter.setPen(QPen(emphasized ? theme.accentHover : theme.accent,
                        emphasized ? 2.8 : 2.2));
    if (hit.snap == RulerSnapKind::Vertex) {
      painter.drawRect(QRectF(point.x() - radius, point.y() - radius,
                              radius * 2.0, radius * 2.0));
    } else if (hit.snap == RulerSnapKind::Edge) {
      QPainterPath diamond;
      diamond.moveTo(point + QPointF(0.0, -radius));
      diamond.lineTo(point + QPointF(radius, 0.0));
      diamond.lineTo(point + QPointF(0.0, radius));
      diamond.lineTo(point + QPointF(-radius, 0.0));
      diamond.closeSubpath();
      painter.drawPath(diamond);
    } else {
      painter.drawEllipse(point, radius, radius);
    }
  };

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  if (first_) {
    const auto& end = second_ ? second_ : hover_;
    if (end) {
      const QPointF startScreen = camera.worldToScreen(first_->world);
      const QPointF endScreen = camera.worldToScreen(end->world);
      painter.setPen(QPen(theme.viewportBackground, 5.5, Qt::SolidLine,
                          Qt::RoundCap));
      painter.drawLine(startScreen, endScreen);
      painter.setPen(QPen(theme.accent, 2.2,
                          second_ ? Qt::SolidLine : Qt::DashLine,
                          Qt::RoundCap));
      painter.drawLine(startScreen, endScreen);

      const double distance = pointDistance(first_->world, end->world);
      const double dx = end->world.x - first_->world.x;
      const double dy = end->world.y - first_->world.y;
      const double dz = end->world.z - first_->world.z;
      const QString value = formattedMillimetres(distance);
      const QString components =
          QStringLiteral("ΔX %1   ΔY %2   ΔZ %3")
              .arg(QLocale().toString(dx, 'f', 2),
                   QLocale().toString(dy, 'f', 2),
                   QLocale().toString(dz, 'f', 2));
      QFont valueFont = painter.font();
      const QFont detailFont = painter.font();
      valueFont.setBold(true);
      const QFontMetricsF valueMetrics(valueFont);
      const QFontMetricsF detailMetrics(painter.font());
      const double width =
          std::max(valueMetrics.horizontalAdvance(value),
                   detailMetrics.horizontalAdvance(components)) +
          20.0;
      const double height = valueMetrics.height() + detailMetrics.height() + 13.0;
      QPointF labelPosition = (startScreen + endScreen) * 0.5 + QPointF(12, -height - 8);
      labelPosition.setX(std::clamp(labelPosition.x(), 8.0,
                                    std::max(8.0, camera.logicalSize.width() - width - 8.0)));
      labelPosition.setY(std::clamp(labelPosition.y(), 8.0,
                                    std::max(8.0, camera.logicalSize.height() - height - 8.0)));
      const QRectF labelRect(labelPosition, QSizeF(width, height));
      painter.setPen(QPen(theme.borderStrong, 1.0));
      painter.setBrush(theme.elevated);
      painter.drawRoundedRect(labelRect, 5.0, 5.0);
      painter.setPen(theme.textPrimary);
      painter.setFont(valueFont);
      painter.drawText(labelRect.adjusted(10, 4, -10, 0),
                       Qt::AlignLeft | Qt::AlignTop, value);
      painter.setFont(detailFont);
      painter.setPen(theme.textSecondary);
      painter.drawText(labelRect.adjusted(10, valueMetrics.height() + 3, -10, -3),
                       Qt::AlignLeft | Qt::AlignTop, components);
    }
    paintMarker(*first_, true);
    if (second_) paintMarker(*second_, true);
  }
  if (hover_ && (!second_ || pointDistance(hover_->world, second_->world) > 1e-9))
    paintMarker(*hover_, false);
  painter.restore();
}

}  // namespace solidar

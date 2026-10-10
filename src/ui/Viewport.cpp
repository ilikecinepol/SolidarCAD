#include "ui/Viewport.h"
#include "ui/ThemeManager.h"
#include "ui/WorldGrid.h"
#include <QVariantAnimation>
#include <QToolTip>
#include <QTimer>
#include <QOpenGLContext>

#include "ui/EdgeSelectionState.h"
#include "ui/ManipulatorLayout.h"
#include "ui/ViewportCamera.h"
#include "ui/ViewportPicking.h"
#include "ui/tools/ToolParameterHud.h"
#include "ui/ExtrusionPreviewGeometry.h"
#include "model/TopologyReferenceResolver.h"

#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt.hxx>

#include <QLineF>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QImageReader>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QSignalBlocker>
#include <QSurfaceFormat>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <numbers>

namespace solidar {
namespace {

struct Point3 {
  float x;
  float y;
  float z;
};

ProjectedPoint projectBodyPoint(Point3d point, Point3d center, const QSize& size,
                                float yaw, float pitch, float zoom) {
  // Keep the solid in the same world-space projection as sketches and
  // construction geometry. Fit All supplies the screen-space centering.
  ViewportCameraState camera{yaw, pitch, zoom, {}, size, 1.0F, center, 1.0};
  return {camera.worldToScreen(point), camera.cameraDepth(point)};
}

QRectF projectedBodyBounds(const BodyRenderMesh& mesh, const QSize& size,
                           float yaw, float pitch, float zoom) {
  QRectF bounds;
  bool first = true;
  for (const auto& vertex : mesh.vertices()) {
    const QPointF screen = projectBodyPoint(vertex.position, mesh.center(), size,
                                            yaw, pitch, zoom).screen;
    if (first) {
      bounds = QRectF(screen, QSizeF());
      first = false;
    } else {
      bounds |= QRectF(screen, QSizeF());
    }
  }
  return bounds;
}

Vector3d cross(Vector3d a, Vector3d b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

Vector3d normalized(Vector3d value) {
  const double length =
      std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  if (length < 1e-12) return {0.0, 0.0, 1.0};
  return {value.x / length, value.y / length, value.z / length};
}

std::pair<Vector3d, Vector3d> angularBasis(Vector3d axis) {
  axis = normalized(axis);
  const Vector3d reference =
      std::abs(axis.z) < 0.85 ? Vector3d{0.0, 0.0, 1.0}
                              : Vector3d{0.0, 1.0, 0.0};
  const Vector3d u = normalized(cross(axis, reference));
  return {u, normalized(cross(axis, u))};
}

Point3d offsetPoint(Point3d origin, Vector3d u, double uScale,
                    Vector3d v = {}, double vScale = 0.0) {
  return {origin.x + u.x * uScale + v.x * vScale,
          origin.y + u.y * uScale + v.y * vScale,
          origin.z + u.z * uScale + v.z * vScale};
}

QPointF project(Point3 point, const QSize& size, float yaw, float pitch,
                float zoom) {
  const float yawRadians = yaw * std::numbers::pi_v<float> / 180.0F;
  const float pitchRadians = pitch * std::numbers::pi_v<float> / 180.0F;
  const float x1 = point.x * std::cos(yawRadians) - point.y * std::sin(yawRadians);
  const float y1 = point.x * std::sin(yawRadians) + point.y * std::cos(yawRadians);
  const float y2 = y1 * std::cos(pitchRadians) - point.z * std::sin(pitchRadians);
  const float scale = std::min(size.width(), size.height()) * 0.008F * zoom;
  return {size.width() * 0.5F + x1 * scale,
          size.height() * 0.52F + y2 * scale};
}

Point3 pointOnPlacement(sketch::Point point,
                        const SketchPlacement& placement,
                        float offsetX, float offsetY) {
  const auto world = placement.toWorld(point.xMm, point.yMm);
  return {static_cast<float>(world.x) + offsetX,
          static_cast<float>(world.y) + offsetY,
          static_cast<float>(world.z)};
}

Point3 placementNormal(const SketchPlacement& placement) {
  const auto normal = placement.normal();
  return {static_cast<float>(normal.x), static_cast<float>(normal.y),
          static_cast<float>(normal.z)};
}

Point3 translated(Point3 point, Point3 direction, float distance) {
  return {point.x + direction.x * distance,
          point.y + direction.y * distance,
          point.z + direction.z * distance};
}

double signedArea(const QPolygonF& polygon) {
  double area = 0.0;
  for (qsizetype index = 0; index < polygon.size(); ++index) {
    const QPointF& current = polygon[index];
    const QPointF& next = polygon[(index + 1) % polygon.size()];
    area += current.x() * next.y() - next.x() * current.y();
  }
  return area * 0.5;
}

bool isFrontFacing(const QPolygonF& polygon) {
  // The screen Y axis points down, therefore outward, front-facing polygons
  // have clockwise winding after projection.
  return polygon.size() >= 3 && signedArea(polygon) < -0.01;
}


struct SketchFaceHalfEdge {
  int from{-1};
  int to{-1};
  int twin{-1};
  bool used{false};
  bool userGeometry{true};
};

double sketchCross(sketch::Point a, sketch::Point b) {
  return a.xMm * b.yMm - a.yMm * b.xMm;
}

sketch::Point sketchSubtract(sketch::Point a, sketch::Point b) {
  return {a.xMm - b.xMm, a.yMm - b.yMm};
}

std::vector<std::vector<sketch::Point>> planarSketchLineFaces(
    const sketch::Sketch& geometry) {
  struct SourceSegment {
    sketch::Point a;
    sketch::Point b;
    std::vector<double> cuts{0.0, 1.0};
    bool userGeometry{true};
  };

  std::vector<SourceSegment> source;
  for (std::size_t index = 0; index < geometry.lines().size(); ++index) {
    const auto& line = geometry.lines()[index];
    // Face-supported sketches contain the support boundary as locked dashed
    // projection geometry. It is not an independently extrudable profile, but
    // it must close regions cut by ordinary user lines (for example a diagonal
    // from one corner to the middle of the opposite side).
    const bool lockedProjection =
        line.dashed && geometry.isGeometryLocked(geometry.lineId(index));
    if (line.dashed && !lockedProjection) continue;
    if (std::hypot(line.end.xMm - line.start.xMm,
                   line.end.yMm - line.start.yMm) <= 1e-9)
      continue;
    source.push_back({line.start, line.end, {0.0, 1.0}, !line.dashed});
  }

  // A click made with grid snapping disabled can land a fraction of a
  // millimetre away from a visible carrier even though it is visually on the
  // line. CAD inference normally prevents that for new geometry; retain a
  // small profile-only healing tolerance for older sketches so T-junctions
  // still form selectable bounded regions. This does not mutate the document.
  constexpr double profileInferenceToleranceMm = 0.25;
  for (std::size_t index = 0; index < source.size(); ++index) {
    for (const bool firstEndpoint : {true, false}) {
      auto& endpoint = firstEndpoint ? source[index].a : source[index].b;
      sketch::Point best = endpoint;
      double bestDistance = profileInferenceToleranceMm;
      for (std::size_t carrierIndex = 0; carrierIndex < source.size();
           ++carrierIndex) {
        if (carrierIndex == index) continue;
        const auto& carrier = source[carrierIndex];
        const double dx = carrier.b.xMm - carrier.a.xMm;
        const double dy = carrier.b.yMm - carrier.a.yMm;
        const double lengthSquared = dx * dx + dy * dy;
        if (lengthSquared <= 1e-12) continue;
        const double parameter =
            ((endpoint.xMm - carrier.a.xMm) * dx +
             (endpoint.yMm - carrier.a.yMm) * dy) /
            lengthSquared;
        if (parameter < -1e-8 || parameter > 1.0 + 1e-8) continue;
        const sketch::Point projection{
            carrier.a.xMm + dx * std::clamp(parameter, 0.0, 1.0),
            carrier.a.yMm + dy * std::clamp(parameter, 0.0, 1.0)};
        const double distance = std::hypot(endpoint.xMm - projection.xMm,
                                           endpoint.yMm - projection.yMm);
        if (distance >= bestDistance) continue;
        bestDistance = distance;
        best = projection;
      }
      endpoint = best;
    }
  }

  constexpr double parameterTolerance = 1e-8;
  for (std::size_t i = 0; i < source.size(); ++i) {
    const sketch::Point p = source[i].a;
    const sketch::Point r = sketchSubtract(source[i].b, source[i].a);

    for (std::size_t j = i + 1; j < source.size(); ++j) {
      const sketch::Point q = source[j].a;
      const sketch::Point s = sketchSubtract(source[j].b, source[j].a);
      const sketch::Point qp = sketchSubtract(q, p);
      const double denominator = sketchCross(r, s);

      if (std::abs(denominator) <= 1e-12) {
        // A user edge may deliberately repeat part of the locked projected
        // support boundary (for example three sides of a rectangle touching
        // the face perimeter). Split both collinear carriers at every overlap
        // endpoint; the identical pieces are collapsed below. Without this,
        // duplicate half-edges hide the bounded cell from face traversal.
        const double rLengthSquared =
            r.xMm * r.xMm + r.yMm * r.yMm;
        const double sLengthSquared =
            s.xMm * s.xMm + s.yMm * s.yMm;
        if (rLengthSquared <= 1e-12 || sLengthSquared <= 1e-12)
          continue;
        const double distanceFromFirst =
            std::abs(sketchCross(qp, r)) / std::sqrt(rLengthSquared);
        if (distanceFromFirst > 1e-7) continue;

        const auto addCutForPoint = [](SourceSegment& segment,
                                       sketch::Point point) {
          const double dx = segment.b.xMm - segment.a.xMm;
          const double dy = segment.b.yMm - segment.a.yMm;
          const double lengthSquared = dx * dx + dy * dy;
          if (lengthSquared <= 1e-12) return;
          const double parameter =
              ((point.xMm - segment.a.xMm) * dx +
               (point.yMm - segment.a.yMm) * dy) /
              lengthSquared;
          if (parameter < -parameterTolerance ||
              parameter > 1.0 + parameterTolerance)
            return;
          segment.cuts.push_back(std::clamp(parameter, 0.0, 1.0));
        };
        addCutForPoint(source[i], source[j].a);
        addCutForPoint(source[i], source[j].b);
        addCutForPoint(source[j], source[i].a);
        addCutForPoint(source[j], source[i].b);
        continue;
      }

      const double t = sketchCross(qp, s) / denominator;
      const double u = sketchCross(qp, r) / denominator;
      if (t < -parameterTolerance || t > 1.0 + parameterTolerance ||
          u < -parameterTolerance || u > 1.0 + parameterTolerance)
        continue;

      const double tc = std::clamp(t, 0.0, 1.0);
      const double uc = std::clamp(u, 0.0, 1.0);
      source[i].cuts.push_back(tc);
      source[j].cuts.push_back(uc);
    }
  }

  struct SplitSegment {
    sketch::Point a;
    sketch::Point b;
    bool userGeometry{true};
  };
  std::vector<SplitSegment> segments;
  for (auto& item : source) {
    std::sort(item.cuts.begin(), item.cuts.end());
    item.cuts.erase(
        std::unique(item.cuts.begin(), item.cuts.end(),
                    [](double a, double b) {
                      return std::abs(a - b) <= 1e-8;
                    }),
        item.cuts.end());

    const double dx = item.b.xMm - item.a.xMm;
    const double dy = item.b.yMm - item.a.yMm;
    for (std::size_t k = 0; k + 1 < item.cuts.size(); ++k) {
      const double t0 = item.cuts[k];
      const double t1 = item.cuts[k + 1];
      if (t1 - t0 <= 1e-8) continue;
      const sketch::Point a{item.a.xMm + dx * t0,
                            item.a.yMm + dy * t0};
      const sketch::Point b{item.a.xMm + dx * t1,
                            item.a.yMm + dy * t1};
      if (std::hypot(b.xMm - a.xMm, b.yMm - a.yMm) > 1e-8)
        segments.push_back({a, b, item.userGeometry});
    }
  }

  std::vector<sketch::Point> vertices;
  const auto vertexIndex = [&vertices](sketch::Point point) {
    for (std::size_t i = 0; i < vertices.size(); ++i) {
      if (std::hypot(vertices[i].xMm - point.xMm,
                     vertices[i].yMm - point.yMm) <= 1e-6)
        return static_cast<int>(i);
    }
    vertices.push_back(point);
    return static_cast<int>(vertices.size() - 1);
  };

  std::vector<SketchFaceHalfEdge> edges;
  std::vector<std::vector<int>> outgoing;
  for (const auto& segment : segments) {
    const int a = vertexIndex(segment.a);
    const int b = vertexIndex(segment.b);
    if (a == b) continue;
    if (outgoing.size() < vertices.size()) outgoing.resize(vertices.size());

    int duplicate = -1;
    for (int edgeIndex = 0; edgeIndex < static_cast<int>(edges.size());
         edgeIndex += 2) {
      const auto& edge = edges[edgeIndex];
      if ((edge.from == a && edge.to == b) ||
          (edge.from == b && edge.to == a)) {
        duplicate = edgeIndex;
        break;
      }
    }
    if (duplicate >= 0) {
      const bool userGeometry =
          edges[duplicate].userGeometry || segment.userGeometry;
      edges[duplicate].userGeometry = userGeometry;
      edges[edges[duplicate].twin].userGeometry = userGeometry;
      continue;
    }

    const int forward = static_cast<int>(edges.size());
    const int reverse = forward + 1;
    edges.push_back({a, b, reverse, false, segment.userGeometry});
    edges.push_back({b, a, forward, false, segment.userGeometry});
    outgoing[a].push_back(forward);
    outgoing[b].push_back(reverse);
  }
  outgoing.resize(vertices.size());

  const auto edgeAngle = [&edges, &vertices](int edgeIndex) {
    const auto& edge = edges[edgeIndex];
    const auto& a = vertices[edge.from];
    const auto& b = vertices[edge.to];
    return std::atan2(b.yMm - a.yMm, b.xMm - a.xMm);
  };
  for (auto& list : outgoing) {
    std::sort(list.begin(), list.end(),
              [&edgeAngle](int a, int b) {
                return edgeAngle(a) < edgeAngle(b);
              });
  }

  std::vector<std::vector<sketch::Point>> faces;
  for (int startEdge = 0; startEdge < static_cast<int>(edges.size());
       ++startEdge) {
    if (edges[startEdge].used) continue;

    std::vector<sketch::Point> cycle;
    int current = startEdge;
    bool closed = false;
    bool usesUserGeometry = false;
    for (std::size_t guard = 0; guard <= edges.size() + 2; ++guard) {
      if (edges[current].used && current != startEdge) break;
      edges[current].used = true;
      usesUserGeometry = usesUserGeometry || edges[current].userGeometry;
      cycle.push_back(vertices[edges[current].from]);

      const int vertex = edges[current].to;
      const int reverse = edges[current].twin;
      const auto& list = outgoing[vertex];
      const auto found = std::find(list.begin(), list.end(), reverse);
      if (found == list.end() || list.empty()) break;

      const std::size_t reversePosition =
          static_cast<std::size_t>(std::distance(list.begin(), found));
      // Previous CCW edge = clockwise turn from the reverse direction. This
      // keeps the traversed face on the left side of the half-edge.
      const std::size_t nextPosition =
          (reversePosition + list.size() - 1) % list.size();
      current = list[nextPosition];

      if (current == startEdge) {
        closed = true;
        break;
      }
    }

    if (!closed || cycle.size() < 3) continue;
    double area2 = 0.0;
    for (std::size_t i = 0; i < cycle.size(); ++i) {
      const auto& a = cycle[i];
      const auto& b = cycle[(i + 1) % cycle.size()];
      area2 += a.xMm * b.yMm - b.xMm * a.yMm;
    }

    // With the traversal rule above bounded faces are CCW (positive area);
    // the unbounded outside face is clockwise and is discarded.
    // Projected support edges only provide closure. Without at least one
    // ordinary sketch edge this is still the original B-Rep face, not a new
    // selectable sketch region.
    if (area2 > 1e-8 && usesUserGeometry)
      faces.push_back(std::move(cycle));
  }

  return faces;
}

struct AttachedArcProfile {
  sketch::Sketch geometry;
  std::vector<sketch::Point> boundary;
};

bool sameSketchPoint(sketch::Point first, sketch::Point second) {
  return std::hypot(first.xMm - second.xMm,
                    first.yMm - second.yMm) <= 1e-5;
}

// Enumerate bounded planar faces that contain at least one curved edge. Arcs
// are real graph edges, while Circles are split analytically only at their
// line contacts. Curves may close through one chord, a chain of user lines,
// or locked projected support edges. Sampling is used only for face winding
// and hit-testing; the selected B-Rep profile retains exact analytic Arcs.
std::vector<AttachedArcProfile> attachedArcProfiles(
    const sketch::Sketch& source) {
  struct SourceSegment {
    sketch::Point a;
    sketch::Point b;
    std::vector<double> cuts{0.0, 1.0};
    bool userGeometry{true};
  };
  std::vector<SourceSegment> sourceLines;
  for (std::size_t index = 0; index < source.lines().size(); ++index) {
    const auto& line = source.lines()[index];
    const bool lockedProjection =
        line.dashed && source.isGeometryLocked(source.lineId(index));
    if (line.dashed && !lockedProjection) continue;
    if (std::hypot(line.end.xMm - line.start.xMm,
                   line.end.yMm - line.start.yMm) <= 1e-9)
      continue;
    sourceLines.push_back(
        {line.start, line.end, {0.0, 1.0}, !line.dashed});
  }

  struct CurvePiece {
    sketch::Point center;
    double radiusMm{};
    double startAngleRad{};
    double sweepAngleRad{};
  };
  std::vector<CurvePiece> curvePieces;
  std::vector<sketch::Point> curveEndpoints;
  for (std::size_t index = 0; index < source.arcs().size(); ++index) {
    const auto& arc = source.arcs()[index];
    if (arc.dashed || !std::isfinite(arc.radiusMm) ||
        !std::isfinite(arc.sweepAngleRad) || arc.radiusMm <= 1e-9 ||
        arc.sweepAngleRad <= 1e-9)
      continue;
    curvePieces.push_back(
        {arc.center, arc.radiusMm, arc.startAngleRad, arc.sweepAngleRad});
    curveEndpoints.push_back(sketch::arcStartPoint(arc));
    curveEndpoints.push_back(sketch::arcEndPoint(arc));
  }
  if (sourceLines.empty()) return {};

  constexpr double inferenceToleranceMm = 0.25;
  constexpr double parameterTolerance = 1e-8;
  const auto addCutForPoint = [](SourceSegment& segment,
                                 sketch::Point point,
                                 double distanceTolerance) {
    const double dx = segment.b.xMm - segment.a.xMm;
    const double dy = segment.b.yMm - segment.a.yMm;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 1e-12) return;
    const double parameter =
        ((point.xMm - segment.a.xMm) * dx +
         (point.yMm - segment.a.yMm) * dy) /
        lengthSquared;
    if (parameter < -parameterTolerance ||
        parameter > 1.0 + parameterTolerance)
      return;
    const double clamped = std::clamp(parameter, 0.0, 1.0);
    const sketch::Point projection{segment.a.xMm + dx * clamped,
                                   segment.a.yMm + dy * clamped};
    if (std::hypot(point.xMm - projection.xMm,
                   point.yMm - projection.yMm) > distanceTolerance)
      return;
    segment.cuts.push_back(clamped);
  };

  // Retain the same small profile-only healing used by the line face graph.
  // It accommodates old grid-off sketches without mutating the document.
  for (std::size_t index = 0; index < sourceLines.size(); ++index) {
    for (const bool firstEndpoint : {true, false}) {
      auto& endpoint =
          firstEndpoint ? sourceLines[index].a : sourceLines[index].b;
      sketch::Point best = endpoint;
      double bestDistance = inferenceToleranceMm;
      for (std::size_t carrierIndex = 0;
           carrierIndex < sourceLines.size(); ++carrierIndex) {
        if (carrierIndex == index) continue;
        const auto& carrier = sourceLines[carrierIndex];
        const double dx = carrier.b.xMm - carrier.a.xMm;
        const double dy = carrier.b.yMm - carrier.a.yMm;
        const double lengthSquared = dx * dx + dy * dy;
        if (lengthSquared <= 1e-12) continue;
        const double parameter =
            ((endpoint.xMm - carrier.a.xMm) * dx +
             (endpoint.yMm - carrier.a.yMm) * dy) /
            lengthSquared;
        if (parameter < -parameterTolerance ||
            parameter > 1.0 + parameterTolerance)
          continue;
        const sketch::Point projection{
            carrier.a.xMm + dx * std::clamp(parameter, 0.0, 1.0),
            carrier.a.yMm + dy * std::clamp(parameter, 0.0, 1.0)};
        const double distance = std::hypot(endpoint.xMm - projection.xMm,
                                           endpoint.yMm - projection.yMm);
        if (distance >= bestDistance) continue;
        bestDistance = distance;
        best = projection;
      }
      endpoint = best;
    }
  }

  constexpr double twoPi = std::numbers::pi_v<double> * 2.0;
  const auto normalizedAngle = [](double angle) {
    constexpr double period = std::numbers::pi_v<double> * 2.0;
    double normalized = std::fmod(angle, period);
    if (normalized < 0.0) normalized += period;
    return normalized;
  };
  for (const auto& circle : source.circles()) {
    if (circle.dashed || !std::isfinite(circle.radiusMm) ||
        circle.radiusMm <= 1e-9)
      continue;

    std::vector<double> contactAngles;
    for (auto& line : sourceLines) {
      const double dx = line.b.xMm - line.a.xMm;
      const double dy = line.b.yMm - line.a.yMm;
      const double a = dx * dx + dy * dy;
      if (a <= 1e-12) continue;
      const double fx = line.a.xMm - circle.center.xMm;
      const double fy = line.a.yMm - circle.center.yMm;
      const double b = 2.0 * (fx * dx + fy * dy);
      const double c = fx * fx + fy * fy -
                       circle.radiusMm * circle.radiusMm;
      double discriminant = b * b - 4.0 * a * c;
      const double discriminantTolerance =
          1e-8 * a * std::max(1.0, circle.radiusMm * circle.radiusMm);
      if (discriminant < -discriminantTolerance) continue;
      discriminant = std::max(0.0, discriminant);
      const double root = std::sqrt(discriminant);
      const std::array<double, 2> parameters{
          (-b - root) / (2.0 * a), (-b + root) / (2.0 * a)};
      for (const double parameter : parameters) {
        if (parameter < -parameterTolerance ||
            parameter > 1.0 + parameterTolerance)
          continue;
        const double clamped = std::clamp(parameter, 0.0, 1.0);
        const sketch::Point intersection{line.a.xMm + dx * clamped,
                                         line.a.yMm + dy * clamped};
        const double angle = normalizedAngle(std::atan2(
            intersection.yMm - circle.center.yMm,
            intersection.xMm - circle.center.xMm));
        const sketch::Point exact{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        line.cuts.push_back(clamped);
        contactAngles.push_back(angle);
        curveEndpoints.push_back(exact);
      }
    }

    std::sort(contactAngles.begin(), contactAngles.end());
    contactAngles.erase(
        std::unique(contactAngles.begin(), contactAngles.end(),
                    [](double first, double second) {
                      return std::abs(first - second) <= 1e-7;
                    }),
        contactAngles.end());
    if (contactAngles.size() < 2) continue;
    for (std::size_t contact = 0; contact < contactAngles.size(); ++contact) {
      const double startAngle = contactAngles[contact];
      double endAngle = contactAngles[(contact + 1) % contactAngles.size()];
      if (contact + 1 == contactAngles.size()) endAngle += twoPi;
      const double sweep = endAngle - startAngle;
      if (sweep <= 1e-9 || sweep >= twoPi - 1e-9) continue;
      curvePieces.push_back(
          {circle.center, circle.radiusMm, startAngle, sweep});
    }
  }
  if (curvePieces.empty()) return {};

  for (auto& line : sourceLines)
    for (const auto endpoint : curveEndpoints)
      addCutForPoint(line, endpoint, inferenceToleranceMm);

  for (std::size_t i = 0; i < sourceLines.size(); ++i) {
    const sketch::Point p = sourceLines[i].a;
    const sketch::Point r = sketchSubtract(sourceLines[i].b, sourceLines[i].a);
    for (std::size_t j = i + 1; j < sourceLines.size(); ++j) {
      const sketch::Point q = sourceLines[j].a;
      const sketch::Point s =
          sketchSubtract(sourceLines[j].b, sourceLines[j].a);
      const sketch::Point qp = sketchSubtract(q, p);
      const double denominator = sketchCross(r, s);
      if (std::abs(denominator) <= 1e-12) {
        const double rLengthSquared = r.xMm * r.xMm + r.yMm * r.yMm;
        if (rLengthSquared <= 1e-12 ||
            std::abs(sketchCross(qp, r)) / std::sqrt(rLengthSquared) > 1e-7)
          continue;
        addCutForPoint(sourceLines[i], sourceLines[j].a, 1e-7);
        addCutForPoint(sourceLines[i], sourceLines[j].b, 1e-7);
        addCutForPoint(sourceLines[j], sourceLines[i].a, 1e-7);
        addCutForPoint(sourceLines[j], sourceLines[i].b, 1e-7);
        continue;
      }
      const double t = sketchCross(qp, s) / denominator;
      const double u = sketchCross(qp, r) / denominator;
      if (t < -parameterTolerance || t > 1.0 + parameterTolerance ||
          u < -parameterTolerance || u > 1.0 + parameterTolerance)
        continue;
      sourceLines[i].cuts.push_back(std::clamp(t, 0.0, 1.0));
      sourceLines[j].cuts.push_back(std::clamp(u, 0.0, 1.0));
    }
  }

  const auto canonicalArcEndpoint = [&curveEndpoints](sketch::Point point) {
    for (const auto endpoint : curveEndpoints) {
      if (std::hypot(point.xMm - endpoint.xMm,
                     point.yMm - endpoint.yMm) <= inferenceToleranceMm)
        return endpoint;
    }
    return point;
  };
  struct SplitSegment {
    sketch::Point a;
    sketch::Point b;
    bool userGeometry{true};
  };
  std::vector<SplitSegment> segments;
  for (auto& item : sourceLines) {
    std::sort(item.cuts.begin(), item.cuts.end());
    item.cuts.erase(
        std::unique(item.cuts.begin(), item.cuts.end(),
                    [](double first, double second) {
                      return std::abs(first - second) <= 1e-8;
                    }),
        item.cuts.end());
    const double dx = item.b.xMm - item.a.xMm;
    const double dy = item.b.yMm - item.a.yMm;
    for (std::size_t cut = 0; cut + 1 < item.cuts.size(); ++cut) {
      sketch::Point a{item.a.xMm + dx * item.cuts[cut],
                      item.a.yMm + dy * item.cuts[cut]};
      sketch::Point b{item.a.xMm + dx * item.cuts[cut + 1],
                      item.a.yMm + dy * item.cuts[cut + 1]};
      a = canonicalArcEndpoint(a);
      b = canonicalArcEndpoint(b);
      if (std::hypot(b.xMm - a.xMm, b.yMm - a.yMm) > 1e-8)
        segments.push_back({a, b, item.userGeometry});
    }
  }

  struct HalfEdge {
    int from{-1};
    int to{-1};
    int twin{-1};
    bool used{false};
    bool userGeometry{true};
    int curveIndex{-1};
    bool curveForward{false};
    double outgoingAngle{};
  };
  std::vector<sketch::Point> vertices;
  const auto vertexIndex = [&vertices](sketch::Point point) {
    for (std::size_t index = 0; index < vertices.size(); ++index) {
      if (sameSketchPoint(vertices[index], point))
        return static_cast<int>(index);
    }
    vertices.push_back(point);
    return static_cast<int>(vertices.size() - 1);
  };
  std::vector<HalfEdge> edges;
  std::vector<std::vector<int>> outgoing;
  const auto ensureOutgoing = [&outgoing, &vertices]() {
    if (outgoing.size() < vertices.size()) outgoing.resize(vertices.size());
  };
  for (const auto& segment : segments) {
    const int a = vertexIndex(segment.a);
    const int b = vertexIndex(segment.b);
    if (a == b) continue;
    ensureOutgoing();
    int duplicate = -1;
    for (int edgeIndex = 0; edgeIndex < static_cast<int>(edges.size());
         edgeIndex += 2) {
      if (edges[edgeIndex].curveIndex >= 0) continue;
      if ((edges[edgeIndex].from == a && edges[edgeIndex].to == b) ||
          (edges[edgeIndex].from == b && edges[edgeIndex].to == a)) {
        duplicate = edgeIndex;
        break;
      }
    }
    if (duplicate >= 0) {
      const bool userGeometry =
          edges[duplicate].userGeometry || segment.userGeometry;
      edges[duplicate].userGeometry = userGeometry;
      edges[edges[duplicate].twin].userGeometry = userGeometry;
      continue;
    }
    const int forward = static_cast<int>(edges.size());
    const int reverse = forward + 1;
    const double angle = std::atan2(vertices[b].yMm - vertices[a].yMm,
                                    vertices[b].xMm - vertices[a].xMm);
    edges.push_back(
        {a, b, reverse, false, segment.userGeometry, -1, false, angle});
    edges.push_back({b, a, forward, false, segment.userGeometry, -1, false,
                     std::atan2(vertices[a].yMm - vertices[b].yMm,
                                vertices[a].xMm - vertices[b].xMm)});
    outgoing[a].push_back(forward);
    outgoing[b].push_back(reverse);
  }
  for (std::size_t curveIndex = 0; curveIndex < curvePieces.size();
       ++curveIndex) {
    const auto& curve = curvePieces[curveIndex];
    const sketch::Point curveStart{
        curve.center.xMm + curve.radiusMm * std::cos(curve.startAngleRad),
        curve.center.yMm + curve.radiusMm * std::sin(curve.startAngleRad)};
    const double curveEndAngle =
        curve.startAngleRad + curve.sweepAngleRad;
    const sketch::Point curveEnd{
        curve.center.xMm + curve.radiusMm * std::cos(curveEndAngle),
        curve.center.yMm + curve.radiusMm * std::sin(curveEndAngle)};
    const int a = vertexIndex(curveStart);
    const int b = vertexIndex(curveEnd);
    if (a == b) continue;
    ensureOutgoing();
    const int forward = static_cast<int>(edges.size());
    const int reverse = forward + 1;
    // At a tangent contact the straight edge and the exact tangent of the
    // curved edge have the same angle. Probe a tiny distance into the curve
    // so the planar rotation order remains deterministic on either side.
    const double probeStep = curve.sweepAngleRad / 64.0;
    const sketch::Point forwardProbe{
        curve.center.xMm +
            curve.radiusMm * std::cos(curve.startAngleRad + probeStep),
        curve.center.yMm +
            curve.radiusMm * std::sin(curve.startAngleRad + probeStep)};
    const sketch::Point reverseProbe{
        curve.center.xMm +
            curve.radiusMm * std::cos(curveEndAngle - probeStep),
        curve.center.yMm +
            curve.radiusMm * std::sin(curveEndAngle - probeStep)};
    edges.push_back({a, b, reverse, false, true,
                     static_cast<int>(curveIndex), true,
                     std::atan2(forwardProbe.yMm - curveStart.yMm,
                                forwardProbe.xMm - curveStart.xMm)});
    edges.push_back(
        {b, a, forward, false, true, static_cast<int>(curveIndex), false,
         std::atan2(reverseProbe.yMm - curveEnd.yMm,
                    reverseProbe.xMm - curveEnd.xMm)});
    outgoing[a].push_back(forward);
    outgoing[b].push_back(reverse);
  }
  outgoing.resize(vertices.size());
  for (auto& list : outgoing) {
    std::sort(list.begin(), list.end(), [&edges](int first, int second) {
      return edges[first].outgoingAngle < edges[second].outgoingAngle;
    });
  }

  std::vector<AttachedArcProfile> profiles;
  constexpr int arcSteps = 64;
  for (int startEdge = 0; startEdge < static_cast<int>(edges.size());
       ++startEdge) {
    if (edges[startEdge].used) continue;
    std::vector<int> cycleEdges;
    int current = startEdge;
    bool closed = false;
    bool usesArc = false;
    for (std::size_t guard = 0; guard <= edges.size() + 2; ++guard) {
      if (edges[current].used && current != startEdge) break;
      edges[current].used = true;
      cycleEdges.push_back(current);
      usesArc = usesArc || edges[current].curveIndex >= 0;
      const int vertex = edges[current].to;
      const int reverse = edges[current].twin;
      const auto& list = outgoing[vertex];
      const auto found = std::find(list.begin(), list.end(), reverse);
      if (found == list.end() || list.empty()) break;
      const std::size_t reversePosition =
          static_cast<std::size_t>(std::distance(list.begin(), found));
      current = list[(reversePosition + list.size() - 1) % list.size()];
      if (current == startEdge) {
        closed = true;
        break;
      }
    }
    if (!closed || !usesArc || cycleEdges.size() < 2) continue;

    AttachedArcProfile profile;
    std::vector<std::size_t> addedArcs;
    for (const int edgeIndex : cycleEdges) {
      const auto& edge = edges[edgeIndex];
      if (edge.curveIndex < 0) {
        profile.boundary.push_back(vertices[edge.from]);
        profile.geometry.addLine(vertices[edge.from], vertices[edge.to]);
        continue;
      }
      const auto& curve =
          curvePieces[static_cast<std::size_t>(edge.curveIndex)];
      for (int step = 0; step < arcSteps; ++step) {
        const double forwardParameter =
            static_cast<double>(step) / arcSteps;
        const double parameter =
            edge.curveForward ? forwardParameter : 1.0 - forwardParameter;
        const double angle =
            curve.startAngleRad + curve.sweepAngleRad * parameter;
        profile.boundary.push_back(
            {curve.center.xMm + curve.radiusMm * std::cos(angle),
             curve.center.yMm + curve.radiusMm * std::sin(angle)});
      }
      const std::size_t exactCurveIndex =
          static_cast<std::size_t>(edge.curveIndex);
      if (std::find(addedArcs.begin(), addedArcs.end(), exactCurveIndex) ==
          addedArcs.end()) {
        addedArcs.push_back(exactCurveIndex);
        profile.geometry.addArc(curve.center, curve.radiusMm,
                                curve.startAngleRad,
                                curve.sweepAngleRad);
      }
    }
    if (profile.boundary.size() < 3 || !profile.geometry.isClosed()) continue;
    double area2 = 0.0;
    for (std::size_t index = 0; index < profile.boundary.size(); ++index) {
      const auto& a = profile.boundary[index];
      const auto& b =
          profile.boundary[(index + 1) % profile.boundary.size()];
      area2 += a.xMm * b.yMm - b.xMm * a.yMm;
    }
    if (area2 > 1e-8) profiles.push_back(std::move(profile));
  }
  return profiles;
}

template <typename ProjectPoint>
std::vector<QPolygonF> projectedSketchBoundaries(
    const sketch::Sketch& geometry, ProjectPoint projectPoint) {
  struct Primitive {
    sketch::Point start;
    sketch::Point end;
    std::vector<sketch::Point> samples;
    bool used{false};
  };

  std::vector<Primitive> primitives;
  for (const auto& line : geometry.lines()) {
    if (line.dashed || sameSketchPoint(line.start, line.end)) continue;
    primitives.push_back({line.start, line.end, {line.start, line.end}, false});
  }
  constexpr double kTwoPi = 6.28318530717958647692;
  for (const auto& arc : geometry.arcs()) {
    if (arc.dashed || arc.radiusMm <= 1e-9 || arc.sweepAngleRad <= 1e-9)
      continue;
    const int steps = std::max(
        8, static_cast<int>(std::ceil(96.0 * arc.sweepAngleRad / kTwoPi)));
    Primitive primitive;
    primitive.start = sketch::arcStartPoint(arc);
    primitive.end = sketch::arcEndPoint(arc);
    primitive.samples.reserve(static_cast<std::size_t>(steps + 1));
    for (int step = 0; step <= steps; ++step) {
      const double parameter = static_cast<double>(step) / steps;
      const double angle = arc.startAngleRad + arc.sweepAngleRad * parameter;
      primitive.samples.push_back(
          {arc.center.xMm + arc.radiusMm * std::cos(angle),
           arc.center.yMm + arc.radiusMm * std::sin(angle)});
    }
    primitives.push_back(std::move(primitive));
  }
  for (const auto& bezier : geometry.beziers()) {
    if (bezier.dashed) continue;
    Primitive primitive;
    primitive.start = bezier.points[0];
    primitive.end = bezier.points[3];
    constexpr int steps = 64;
    primitive.samples.reserve(steps + 1);
    for (int step = 0; step <= steps; ++step)
      primitive.samples.push_back(sketch::bezierPointAt(
          bezier, static_cast<double>(step) / steps));
    primitives.push_back(std::move(primitive));
  }

  std::vector<QPolygonF> result;
  for (std::size_t seed = 0; seed < primitives.size(); ++seed) {
    if (primitives[seed].used) continue;
    primitives[seed].used = true;
    std::vector<sketch::Point> boundary = primitives[seed].samples;
    const sketch::Point first = primitives[seed].start;
    sketch::Point cursor = primitives[seed].end;
    bool closed = sameSketchPoint(cursor, first);

    for (std::size_t guard = 0;
         !closed && guard < primitives.size(); ++guard) {
      bool found = false;
      for (auto& primitive : primitives) {
        if (primitive.used) continue;
        if (sameSketchPoint(primitive.start, cursor)) {
          boundary.insert(boundary.end(), primitive.samples.begin() + 1,
                          primitive.samples.end());
          cursor = primitive.end;
        } else if (sameSketchPoint(primitive.end, cursor)) {
          for (auto iterator = primitive.samples.rbegin() + 1;
               iterator != primitive.samples.rend(); ++iterator)
            boundary.push_back(*iterator);
          cursor = primitive.start;
        } else {
          continue;
        }
        primitive.used = true;
        found = true;
        break;
      }
      if (!found) break;
      closed = sameSketchPoint(cursor, first);
    }
    if (!closed || boundary.size() < 3) continue;
    if (sameSketchPoint(boundary.front(), boundary.back()))
      boundary.pop_back();
    QPolygonF polygon;
    polygon.reserve(static_cast<qsizetype>(boundary.size()));
    for (const auto point : boundary) polygon << projectPoint(point);
    if (polygon.size() >= 3) result.push_back(std::move(polygon));
  }

  for (const auto& circle : geometry.circles()) {
    if (circle.dashed || circle.radiusMm <= 1e-9) continue;
    QPolygonF polygon;
    for (int step = 0; step < 96; ++step) {
      const double angle = kTwoPi * step / 96.0;
      polygon << projectPoint(
          {circle.center.xMm + circle.radiusMm * std::cos(angle),
           circle.center.yMm + circle.radiusMm * std::sin(angle)});
    }
    result.push_back(std::move(polygon));
  }
  return result;
}


}  // namespace

Viewport::Viewport(QWidget* parent) : QOpenGLWidget(parent) {
  hoverFrameTimer_ = new QTimer(this);
  hoverFrameTimer_->setSingleShot(true);
  hoverFrameTimer_->setInterval(16);
  connect(hoverFrameTimer_, &QTimer::timeout, this,
          [this] { flushPendingHover(); });
  orientationAnimation_ = new QVariantAnimation(this);
  orientationAnimation_->setObjectName("viewOrientationTransition");
  orientationAnimation_->setDuration(200);
  orientationAnimation_->setEasingCurve(QEasingCurve::InOutCubic);
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  // QApplication currently uses Qt's software OpenGL backend on Windows to
  // avoid the project-creation driver crash. That backend exposes OpenGL 3.0
  // / GLSL 1.30 on the affected configuration, which is sufficient for the
  // renderer. Do not require a 3.3 core context here.
  format.setVersion(3, 0);
  format.setProfile(QSurfaceFormat::NoProfile);
  format.setDepthBufferSize(24);
  format.setSamples(4);
  setFormat(format);
  setMinimumSize(480, 320);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  extrusionLengthEditor_ = new QDoubleSpinBox(this);
  extrusionLengthEditor_->setObjectName(QStringLiteral("extrusionLengthHud"));
  extrusionLengthEditor_->setRange(-100000.0, 100000.0);
  extrusionLengthEditor_->setDecimals(2);
  extrusionLengthEditor_->setSuffix(QStringLiteral(" mm"));
  extrusionLengthEditor_->setFixedSize(132, 42);
  // The numeric editor follows the application theme via the global
  // QDoubleSpinBox rule; a local light stylesheet would leave a white field in
  // Dark mode.
  extrusionLengthEditor_->hide();
  extrusionLengthEditor_->installEventFilter(this);
  connect(extrusionLengthEditor_, &QDoubleSpinBox::valueChanged, this,
          &Viewport::setExtrusionPreviewLength);
  toolParameterHud_ = new ToolParameterHud(this);
  toolParameterHud_->hide();
  const auto routeToolHudValue = [this](const QString& id, double value) {
    if (id == QStringLiteral("angle"))
      emit angularToolManipulatorValueChanged(value);
    else if (id == QStringLiteral("distance"))
      emit toolManipulatorValueChanged(value);
    else if (id == QStringLiteral("offset_x"))
      emit translationToolManipulatorValueChanged(0, value);
    else if (id == QStringLiteral("offset_y"))
      emit translationToolManipulatorValueChanged(1, value);
    else if (id == QStringLiteral("offset_z"))
      emit translationToolManipulatorValueChanged(2, value);
    else if (id == QStringLiteral("image_offset_x"))
      emit referenceImageParameterChanged(0, value);
    else if (id == QStringLiteral("image_offset_y"))
      emit referenceImageParameterChanged(1, value);
    else if (id == QStringLiteral("image_offset_z"))
      emit referenceImageParameterChanged(2, value);
    else if (id == QStringLiteral("image_scale"))
      emit referenceImageParameterChanged(3, value);
  };
  connect(toolParameterHud_, &ToolParameterHud::valueChanged, this,
          routeToolHudValue);
  connect(toolParameterHud_, &ToolParameterHud::valueCommitted, this,
          routeToolHudValue);
  // After the committed value has been routed to the session preview, publish a
  // dedicated commit signal so MainWindow can perform the tool's Accept exactly
  // once (the value is already interpreted + preview-synced; do not re-interpret).
  connect(toolParameterHud_, &ToolParameterHud::valueCommitted, this,
          &Viewport::toolParameterCommitted);
  connect(toolParameterHud_, &ToolParameterHud::cancelRequested, this, [this] {
    cancelActiveInteraction();
  });
}

void Viewport::setBox(BoxParameters parameters) {
  box_ = parameters;
  update();
}

SketchPlacement translatedPlacement(const SketchPlacement& placement,
                                     Vector3d direction, double distance) {
  SketchPlacement result = placement;
  result.origin.x += direction.x * distance;
  result.origin.y += direction.y * distance;
  result.origin.z += direction.z * distance;
  return result;
}

LegacySolidFacePick legacyCapPick(const sketch::Sketch& geometry,
                                  const SketchPlacement& basePlacement,
                                  double lengthMm, bool endCap) {
  const Vector3d normal = basePlacement.normal();
  return {endCap ? LegacySolidFace::EndCap : LegacySolidFace::InitialCap,
          endCap ? translatedPlacement(basePlacement, normal, lengthMm)
                 : basePlacement,
          endCap ? normal
                 : Vector3d{-normal.x, -normal.y, -normal.z},
          geometry};
}

LegacySolidFacePick legacyBoxFacePick(int faceIndex,
                                      const BoxParameters& box) {
  const double halfWidth = box.widthMm * 0.5;
  const double halfDepth = box.depthMm * 0.5;
  LegacySolidFacePick result;
  switch (faceIndex) {
    case 0:
      result.face = LegacySolidFace::Bottom;
      result.placement = {{}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
      result.outwardNormal = {0.0, 0.0, -1.0};
      result.geometry.addRectangle({-halfWidth, -halfDepth},
                                   {halfWidth, halfDepth});
      break;
    case 1:
      result.face = LegacySolidFace::Top;
      result.placement = {{0.0, 0.0, box.heightMm}, {1.0, 0.0, 0.0},
                          {0.0, 1.0, 0.0}};
      result.outwardNormal = {0.0, 0.0, 1.0};
      result.geometry.addRectangle({-halfWidth, -halfDepth},
                                   {halfWidth, halfDepth});
      break;
    case 2:
      result.face = LegacySolidFace::Front;
      result.placement = {{0.0, -halfDepth, 0.0}, {1.0, 0.0, 0.0},
                          {0.0, 0.0, 1.0}};
      result.outwardNormal = {0.0, -1.0, 0.0};
      result.geometry.addRectangle({-halfWidth, 0.0},
                                   {halfWidth, box.heightMm});
      break;
    case 3:
      result.face = LegacySolidFace::Right;
      result.placement = {{halfWidth, 0.0, 0.0}, {0.0, 1.0, 0.0},
                          {0.0, 0.0, 1.0}};
      result.outwardNormal = {1.0, 0.0, 0.0};
      result.geometry.addRectangle({-halfDepth, 0.0},
                                   {halfDepth, box.heightMm});
      break;
    case 4:
      result.face = LegacySolidFace::Back;
      result.placement = {{0.0, halfDepth, 0.0}, {-1.0, 0.0, 0.0},
                          {0.0, 0.0, 1.0}};
      result.outwardNormal = {0.0, 1.0, 0.0};
      result.geometry.addRectangle({-halfWidth, 0.0},
                                   {halfWidth, box.heightMm});
      break;
    default:
      result.face = LegacySolidFace::Left;
      result.placement = {{-halfWidth, 0.0, 0.0}, {0.0, -1.0, 0.0},
                          {0.0, 0.0, 1.0}};
      result.outwardNormal = {-1.0, 0.0, 0.0};
      result.geometry.addRectangle({-halfDepth, 0.0},
                                   {halfDepth, box.heightMm});
      break;
  }
  return result;
}

std::vector<QPolygonF> projectedBodyFaces(const TopoDS_Shape& shape,
                                          const QSize& size, float yaw,
                                          float pitch, float zoom,
                                          float offsetX, float offsetY) {
  std::vector<QPolygonF> polygons;
  for (TopExp_Explorer faceExplorer(shape, TopAbs_FACE); faceExplorer.More();
       faceExplorer.Next()) {
    const TopoDS_Face face = TopoDS::Face(faceExplorer.Current());
    const TopoDS_Wire wire = BRepTools::OuterWire(face);
    QPolygonF polygon;
    if (wire.IsNull()) {
      polygons.push_back(std::move(polygon));
      continue;
    }
    for (BRepTools_WireExplorer vertexExplorer(wire, face);
         vertexExplorer.More(); vertexExplorer.Next()) {
      const gp_Pnt point = BRep_Tool::Pnt(vertexExplorer.CurrentVertex());
      polygon << project({static_cast<float>(point.X()) + offsetX,
                          static_cast<float>(point.Y()) + offsetY,
                          static_cast<float>(point.Z())},
                         size, yaw, pitch, zoom);
    }
    polygons.push_back(std::move(polygon));
  }
  return polygons;
}

void Viewport::setBodyShape(ShapeFeature::ShapePtr shape, BodyId bodyId,
                            FeatureId featureId) {
  std::vector<BodyViewShape> shapes;
  if (shape) shapes.push_back({bodyId, featureId, std::move(shape), {}});
  setBodyShapes(std::move(shapes));
}

void Viewport::setBodyShapes(std::vector<BodyViewShape> shapes) {
  invalidatePendingHover();
  (void)rebuildBodyDisplay(shapes, true, meshQuality_);
}

Viewport::~Viewport() {
  if (context()) {
    makeCurrent();
    renderer_.release();
    doneCurrent();
  }
}

void Viewport::initializeGL() {
  renderer_.initialize();
  if (context()) {
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
      makeCurrent();
      renderer_.release();
      doneCurrent();
    }, Qt::DirectConnection);
  }
}

void Viewport::setDisplayMode(ViewportDisplayMode mode) {
  if (displayMode_ == mode) return;
  displayMode_ = mode;
  update();
}

void Viewport::setMeshQuality(ViewportMeshQuality quality) {
  if (meshQuality_ == quality) return;
  if (quality != ViewportMeshQuality::Normal &&
      quality != ViewportMeshQuality::High)
    return;
  GeometryFailure failure;
  std::optional<BodyRenderMesh> pendingToolPreview;
  std::optional<BodyRenderMesh> pendingCutPreview;
  if (toolPreviewShape_ && !toolPreviewShape_->IsNull()) {
    pendingToolPreview.emplace();
    if (!pendingToolPreview->tryRebuild(*toolPreviewShape_, quality, &failure))
      return;
  }
  if (toolCutPreviewShape_ && !toolCutPreviewShape_->IsNull()) {
    pendingCutPreview.emplace();
    if (!pendingCutPreview->tryRebuild(*toolCutPreviewShape_, quality, &failure))
      return;
  }
  if (!rebuildBodyDisplay(bodyViewShapes_, false, quality)) return;
  meshQuality_ = quality;
  if (pendingToolPreview) {
    toolPreviewRenderMesh_ = std::move(*pendingToolPreview);
    ++toolPreviewPresentationRevision_;
  }
  if (pendingCutPreview) {
    toolCutPreviewRenderMesh_ = std::move(*pendingCutPreview);
    ++toolCutPreviewPresentationRevision_;
  }
  update();
}

ViewportDisplayMode Viewport::displayMode() const noexcept { return displayMode_; }
ViewportMeshQuality Viewport::meshQuality() const noexcept { return meshQuality_; }

namespace {

void drawToolArrow(QPainter& painter, QPointF start, QPointF tip,
                   const QColor& color, const QColor& handleFill) {
  const ManipulatorStyle style;
  QLineF direction(start, tip);
  if (direction.length() < 1.0)
    direction.setP2(direction.p1() + QPointF(0.0, -1.0));
  const QPointF unit =
      (direction.p2() - direction.p1()) / direction.length();
  const QPointF perpendicular(-unit.y(), unit.x());

  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(color, style.shaftWidth, Qt::SolidLine, Qt::RoundCap));
  painter.drawLine(start, tip);
  painter.setBrush(color);
  painter.drawPolygon(QPolygonF{tip,
                                tip - unit * style.arrowHeadLength +
                                    perpendicular * style.arrowHeadWidth,
                                tip - unit * style.arrowHeadLength -
                                    perpendicular * style.arrowHeadWidth});
  painter.setBrush(handleFill);
  painter.setPen(QPen(color, 3.0));
  painter.drawEllipse(tip, style.handleRadius, style.handleRadius);
}

void drawTranslationGizmo(
    QPainter& painter,
    const std::array<ManipulatorLayoutResult, 3>& layouts,
    const std::array<QColor, 3>& colors, const QColor& handleFill) {
  const std::array<QString, 3> labels{
      QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
  for (int axis = 0; axis < 3; ++axis) {
    drawToolArrow(painter, layouts[axis].anchor, layouts[axis].handle,
                  colors[axis], handleFill);
    painter.setPen(colors[axis]);
    painter.drawText(layouts[axis].handle + QPointF(9.0, -9.0), labels[axis]);
  }
}

void drawToolArrowHead(QPainter& painter, QPointF preceding, QPointF tip,
                       const QColor& color, const QColor& handleFill) {
  QLineF tangent(preceding, tip);
  if (tangent.length() < 1.0) return;
  const QPointF unit = (tip - preceding) / tangent.length();
  const QPointF perpendicular(-unit.y(), unit.x());
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawPolygon(QPolygonF{tip,
                                tip - unit * 15.0 + perpendicular * 8.0,
                                tip - unit * 15.0 - perpendicular * 8.0});
  painter.setBrush(handleFill);
  painter.setPen(QPen(color, 3.0));
  painter.drawEllipse(tip, 7.0, 7.0);
}

QColor withAlpha(QColor color, int alpha) {
  color.setAlpha(alpha);
  return color;
}

}  // namespace

bool Viewport::rebuildBodyDisplay(const std::vector<BodyViewShape>& shapes,
                                  bool clearSelection,
                                  ViewportMeshQuality quality) {
  constexpr std::size_t kMaxViewportFaces = 1'000'000;
  constexpr std::size_t kMaxViewportEdges = 2'000'000;
  std::vector<BodyViewShape> pendingViewShapes;
  std::vector<BodyTopologyRange> pendingRanges;
  std::vector<BodyDisplayMesh> pendingMeshes;
  ShapeFeature::ShapePtr pendingShape;
  BodyId pendingBodyId = kInvalidBodyId;
  FeatureId pendingFeatureId = kInvalidFeatureId;
  BoxParameters pendingBox = box_;
  bool hasPendingBox = false;
  Point3d pendingCenter{};
  double pendingDiagonal = 0.0;
  GeometryFailure failure;
  const bool prepared = runGeometryOperation(
      [&]() -> bool {
        // In particular for setMeshQuality, copy the current presentation
        // inside the exception firewall and before publishing the new quality.
        pendingViewShapes = shapes;
        Bnd_Box bounds;
        std::size_t firstFace = 0;
        std::size_t firstEdge = 0;
        for (auto& item : pendingViewShapes) {
          if (!item.shape || item.shape->IsNull()) continue;
          const ShapeRevision revision =
              item.shapeRevision != kInvalidShapeRevision
                  ? item.shapeRevision
                  : fallbackBodyShapeRevision_++;
          item.shapeRevision = revision;
          std::string topologyError;
          auto topology = item.topologyIndex;
          if (topology &&
              (!topology->shape() ||
               topology->shape().get() != item.shape.get() ||
               (topology->revision() != kInvalidShapeRevision &&
                topology->revision() != revision)))
            topology.reset();
          if (!topology)
            topology = TopologyIndex::build(item.shape, revision,
                                            &topologyError);
          if (!topology) {
            failure.kind = GeometryFailureKind::InvalidBRep;
            failure.detail = "Viewport topology could not be indexed";
            if (!topologyError.empty()) failure.detail += ": " + topologyError;
            return false;
          }
          item.topologyIndex = topology;
          const std::size_t faceCount = topology->rawFaceCount();
          const std::size_t edgeCount = topology->rawEdgeCount();
          if (faceCount > kMaxViewportFaces - firstFace) {
            failure.kind = GeometryFailureKind::ResourceLimit;
            failure.detail = "Viewport face range limit exceeded";
            return false;
          }
          if (edgeCount > kMaxViewportEdges - firstEdge) {
            failure.kind = GeometryFailureKind::ResourceLimit;
            failure.detail = "Viewport edge range limit exceeded";
            return false;
          }
          pendingRanges.push_back(
              {item.bodyId, item.featureId, item.shape, topology,
               firstFace, faceCount, firstEdge, edgeCount});
          const BodyMeshKey identity{item.bodyId, item.featureId, revision,
                                     item.shape.get(), quality};
          auto mesh = bodyMeshCache_.resolve(identity, *item.shape, &failure);
          if (!mesh) return false;
          pendingMeshes.push_back(
              {std::move(mesh), identity, firstFace, firstEdge});
          firstFace += faceCount;
          firstEdge += edgeCount;
          BRepBndLib::Add(*item.shape, bounds);
          pendingShape = item.shape;
          pendingBodyId = item.bodyId;
          pendingFeatureId = item.featureId;
        }
        if (pendingRanges.empty()) return true;
        if (bounds.IsVoid()) {
          failure.kind = GeometryFailureKind::InvalidBRep;
          failure.detail = "Viewport body bounds are void";
          return false;
        }
        double xMin = 0.0;
        double yMin = 0.0;
        double zMin = 0.0;
        double xMax = 0.0;
        double yMax = 0.0;
        double zMax = 0.0;
        bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        if (!std::isfinite(xMin) || !std::isfinite(yMin) ||
            !std::isfinite(zMin) || !std::isfinite(xMax) ||
            !std::isfinite(yMax) || !std::isfinite(zMax) || xMax < xMin ||
            yMax < yMin || zMax < zMin) {
          failure.kind = GeometryFailureKind::InvalidBRep;
          failure.detail = "Viewport body bounds are invalid";
          return false;
        }
        pendingBox = {xMax - xMin, yMax - yMin, zMax - zMin};
        pendingCenter = {xMin + (xMax - xMin) * 0.5,
                         yMin + (yMax - yMin) * 0.5,
                         zMin + (zMax - zMin) * 0.5};
        pendingDiagonal = std::hypot(
            std::hypot(xMax - xMin, yMax - yMin), zMax - zMin);
        hasPendingBox = true;
        return true;
      },
      &failure);
  if (!prepared) return false;

  const bool cacheRetained = runGeometryOperation(
      [&] {
        std::vector<BodyMeshKey> activeMeshKeys;
        activeMeshKeys.reserve(pendingMeshes.size());
        for (const auto& display : pendingMeshes)
          activeMeshKeys.push_back(display.identity);
        if (activeMeshKeys.empty())
          bodyMeshCache_.clear();
        else
          bodyMeshCache_.retainActive(activeMeshKeys);
      },
      &failure);
  if (!cacheRetained) return false;

  // Publish every body-display component together only after every individual
  // body mesh, topology range and the combined bounds are known to be valid.
  bodyViewShapes_ = std::move(pendingViewShapes);
  bodyShape_ = std::move(pendingShape);
  bodyTopologyRanges_ = std::move(pendingRanges);
  bodyDisplayMeshes_ = std::move(pendingMeshes);
  displayedBodyCenter_ = pendingCenter;
  displayedBodyDiagonal_ = pendingDiagonal;
  pickingScene_.invalidate();
  bodyId_ = pendingBodyId;
  bodyFeatureId_ = pendingFeatureId;
  if (hasPendingBox) box_ = pendingBox;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  if (clearSelection) {
    // Clear the old transient preview before publishing the selection-reset
    // signal. A re-entrant slot may start a new preview, which must not then be
    // erased when setBodyShapes returns.
    toolPreviewShape_.reset();
    toolPreviewRenderMesh_.clear();
    toolCutPreviewShape_.reset();
    toolCutPreviewRenderMesh_.clear();
    toolPreviewBodyId_ = kInvalidBodyId;
    toolPreviewFeatureId_ = kInvalidFeatureId;
    toolPreviewPresentation_ = ToolPreviewPresentation::OverlaySourceSelection;
    selectedFace_ = -1;
    selectedBodyFaceIndices_.clear();
    selectedBodyFaceReferences_.clear();
    selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    selectedBodyEdgeIndices_.clear();
    selectedBodyEdgeReferences_.clear();
    const QPointer<Viewport> lifetimeGuard(this);
    clearWholeBodySelection();
    if (!lifetimeGuard) return true;
  }
  synchronizeRendererResources();
  update();
  return true;
}

Point3d Viewport::displayedBodyCenter() const noexcept {
  return displayedBodyCenter_;
}

double Viewport::displayedBodyDiagonal() const noexcept {
  return displayedBodyDiagonal_;
}

bool Viewport::hasDisplayedBodyTriangles() const noexcept {
  return std::any_of(bodyDisplayMeshes_.begin(), bodyDisplayMeshes_.end(),
                     [](const BodyDisplayMesh& display) {
                       return display.mesh && display.mesh->triangleCount() > 0;
                     });
}

QRectF Viewport::projectedDisplayedBodyBounds() const {
  QRectF result;
  bool first = true;
  const ViewportCameraState camera{yaw_, pitch_, zoom_, {}, size(), 1.0F,
                                   displayedBodyCenter_,
                                   std::max(1.0, displayedBodyDiagonal_ * 3.0)};
  const QMatrix4x4 matrix = camera.worldToClip();
  for (const auto& display : bodyDisplayMeshes_) {
    if (!display.mesh) continue;
    for (const auto& vertex : display.mesh->vertices()) {
      const QVector4D clip = matrix * QVector4D(vertex.position.x,
                                                vertex.position.y,
                                                vertex.position.z, 1.0F);
      const QPointF screen{(clip.x() + 1.0) * size().width() * 0.5,
                           (1.0 - clip.y()) * size().height() * 0.5};
      if (first) {
        result = QRectF(screen, QSizeF());
        first = false;
      } else {
        result |= QRectF(screen, QSizeF());
      }
    }
  }
  return result;
}

std::vector<PickingMeshInput> Viewport::pickingMeshInputs() const {
  std::vector<PickingMeshInput> inputs;
  inputs.reserve(bodyDisplayMeshes_.size());
  for (const auto& display : bodyDisplayMeshes_)
    if (display.mesh)
      inputs.push_back({display.mesh.get(), display.firstFace,
                        display.firstEdge, display.identity});
  return inputs;
}

const ProjectedPickingScene& Viewport::pickingScene() const {
  ViewportCameraState camera{yaw_, pitch_, zoom_, {}, size(), 1.0F,
                             displayedBodyCenter_,
                             std::max(1.0, displayedBodyDiagonal_ * 3.0)};
  static_cast<void>(pickingScene_.ensure(pickingMeshInputs(), camera));
  return pickingScene_;
}

void Viewport::synchronizeRendererResources() {
  QOpenGLContext* glContext = context();
  if (!glContext || !glContext->isValid()) return;
  std::vector<BodyMeshKey> active;
  active.reserve(bodyDisplayMeshes_.size());
  for (const auto& display : bodyDisplayMeshes_)
    if (display.mesh) active.push_back(display.identity);
  makeCurrent();
  renderer_.synchronizeResources(
      active, toolPreviewShape_ && !toolPreviewShape_->IsNull(),
      toolCutPreviewShape_ && !toolCutPreviewShape_->IsNull());
  doneCurrent();
}

void Viewport::setToolPreviewShape(BodyId bodyId, FeatureId featureId,
                                   ShapeFeature::ShapePtr shape) {
  if (shape && !shape->IsNull()) {
    if (toolPreviewShape_.get() == shape.get() &&
        toolPreviewRenderMesh_.quality() == meshQuality_) {
      toolPreviewReplacedBodyIds_.clear();
      toolPreviewBodyId_ = bodyId;
      toolPreviewFeatureId_ = featureId;
      update();
      return;
    }
    GeometryFailure failure;
    BodyRenderMesh pending;
    if (!pending.tryRebuild(*shape, meshQuality_, &failure)) return;
    toolPreviewRenderMesh_ = std::move(pending);
    ++toolPreviewPresentationRevision_;
  } else {
    toolPreviewRenderMesh_.clear();
  }
  toolPreviewReplacedBodyIds_.clear();
  toolPreviewShape_ = std::move(shape);
  toolPreviewBodyId_ = bodyId;
  toolPreviewFeatureId_ = featureId;
  synchronizeRendererResources();
  update();
}
void Viewport::setToolPreviewReplacedBodies(std::vector<BodyId> bodyIds) {
  bodyIds.erase(std::remove(bodyIds.begin(), bodyIds.end(), kInvalidBodyId),
                bodyIds.end());
  std::sort(bodyIds.begin(), bodyIds.end());
  bodyIds.erase(std::unique(bodyIds.begin(), bodyIds.end()), bodyIds.end());
  toolPreviewReplacedBodyIds_ = std::move(bodyIds);
  update();
}
void Viewport::setToolCutPreviewShape(ShapeFeature::ShapePtr shape) {
  if (shape && !shape->IsNull()) {
    if (toolCutPreviewShape_.get() == shape.get() &&
        toolCutPreviewRenderMesh_.quality() == meshQuality_) {
      update();
      return;
    }
    GeometryFailure failure;
    BodyRenderMesh pending;
    if (!pending.tryRebuild(*shape, meshQuality_, &failure)) return;
    toolCutPreviewRenderMesh_ = std::move(pending);
    ++toolCutPreviewPresentationRevision_;
  } else {
    toolCutPreviewRenderMesh_.clear();
  }
  toolCutPreviewShape_ = std::move(shape);
  synchronizeRendererResources();
  update();
}
void Viewport::setToolPreviewPresentation(
    ToolPreviewPresentation presentation) noexcept {
  toolPreviewPresentation_ = presentation;
  update();
}

void Viewport::clearToolPreviewShape() {
  toolPreviewShape_.reset();
  toolPreviewRenderMesh_.clear();
  toolCutPreviewShape_.reset();
  toolCutPreviewRenderMesh_.clear();
  toolPreviewBodyId_ = kInvalidBodyId;
  toolPreviewFeatureId_ = kInvalidFeatureId;
  toolPreviewReplacedBodyIds_.clear();
  toolPreviewPresentation_ = ToolPreviewPresentation::OverlaySourceSelection;
  synchronizeRendererResources();
  update();
}

void Viewport::setSketch(const sketch::Sketch& sketch) {
  setSketch(sketch, SketchPlacement::xy());
}

void Viewport::setSketch(const sketch::Sketch& sketch,
                         const SketchPlacement& placement) {
  sketch_ = sketch;
  sketchPlacement_ = placement;
  update();
}

void Viewport::setSolidSketch(const sketch::Sketch& sketch,
                              const SketchPlacement& placement) {
  solidSketch_ = sketch;
  solidSketchPlacement_ = placement;
  update();
}

void Viewport::setSolidVisible(bool visible) {
  solidVisible_ = visible;
  update();
}

void Viewport::setSolidSupport(const QString& supportName) {
  solidSupportName_ = supportName;
  update();
}

void Viewport::setSketchVisible(bool visible) {
  sketchVisible_ = visible;
  update();
}

void Viewport::addSketch(SketchId sketchId, const sketch::Sketch& sketch,
                         const QString& supportName,
                         const SketchPlacement& placement) {
  if (sketchId == kInvalidSketchId) return;
  sketch_ = sketch;
  sketchPlacement_ = placement;
  displaySketches_.push_back(
      {sketchId, sketch, supportName, placement, true});
  update();
}

void Viewport::updateSketch(std::size_t index, const sketch::Sketch& sketch,
                            const QString& supportName,
                            const SketchPlacement& placement) {
  if (index >= displaySketches_.size()) return;
  displaySketches_[index].geometry = sketch;
  displaySketches_[index].supportName = supportName;
  displaySketches_[index].placement = placement;
  sketch_ = sketch;
  sketchPlacement_ = placement;
  update();
}

void Viewport::removeSketch(std::size_t index) {
  if (index >= displaySketches_.size()) return;
  invalidatePendingHover();
  displaySketches_.erase(displaySketches_.begin() +
                         static_cast<std::ptrdiff_t>(index));
  if (displaySketches_.empty()) {
    sketch_.clear();
    sketchPlacement_ = SketchPlacement::xy();
  } else {
    sketch_ = displaySketches_.back().geometry;
    sketchPlacement_ = displaySketches_.back().placement;
  }
  if (revolveAxisSketchIndex_ == index)
    revolveAxisSketchIndex_ = static_cast<std::size_t>(-1);
  else if (revolveAxisSketchIndex_ != static_cast<std::size_t>(-1) &&
           revolveAxisSketchIndex_ > index)
    --revolveAxisSketchIndex_;
  selectedExtrusionSketch_.clear();
  hoveredExtrusionSketch_.clear();
  selectedExtrusionPlacement_ = SketchPlacement::xy();
  hoveredExtrusionPlacement_ = SketchPlacement::xy();
  selectedExtrusionSupport_.clear();
  hoveredExtrusionSupport_.clear();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  selectedExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  selectedExtrusionBodyFace_ = false;
  selectedExtrusionOnBodyCap_ = false;
  hoveredExtrusionOnBodyCap_ = false;
  selectedExtrusionReverse_ = false;
  hoveredExtrusionReverse_ = false;
  selectedLegacySolidFace_.reset();
  hoveredLegacySolidFace_.reset();
  update();
}

void Viewport::replaceSketchPresentations(
    std::vector<SketchPresentationSnapshot> sketches) {
  invalidatePendingHover();
  displaySketches_.clear();
  displaySketches_.reserve(sketches.size());
  for (auto& item : sketches) {
    if (item.sketchId == kInvalidSketchId) continue;
    displaySketches_.push_back(
        {item.sketchId, std::move(item.geometry),
         std::move(item.presentationLabel), item.placement, true});
  }
  if (displaySketches_.empty()) {
    sketch_.clear();
    sketchPlacement_ = SketchPlacement::xy();
  } else {
    sketch_ = displaySketches_.back().geometry;
    sketchPlacement_ = displaySketches_.back().placement;
  }

  // Sketch picks are positional views into displaySketches_. Replacing that
  // cache invalidates every such pick, while Body presentation and selection
  // remain untouched.
  selectedExtrusionSketch_.clear();
  hoveredExtrusionSketch_.clear();
  selectedExtrusionPlacement_ = SketchPlacement::xy();
  hoveredExtrusionPlacement_ = SketchPlacement::xy();
  selectedExtrusionSupport_.clear();
  hoveredExtrusionSupport_.clear();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  selectedExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  revolveAxisSketchIndex_ = static_cast<std::size_t>(-1);
  selectedExtrusionBodyFace_ = false;
  selectedExtrusionOnBodyCap_ = false;
  selectedExtrusionReverse_ = false;
  hoveredExtrusionReverse_ = false;
  update();
}

void Viewport::setReferenceImages(const std::vector<ReferenceImage>& images) {
  auto previous = std::move(referenceImages_);
  referenceImages_.clear();
  referenceImages_.reserve(images.size());
  for (const auto& image : images) {
    QImage pixels;
    const auto cached = std::find_if(
        previous.begin(), previous.end(), [&image](const auto& displayed) {
          return displayed.model.id == image.id &&
                 displayed.model.sourcePath == image.sourcePath;
        });
    if (cached != previous.end()) {
      pixels = cached->pixels;
    } else {
      QImageReader reader(QString::fromStdString(image.sourcePath));
      const QSize dimensions = reader.size();
      if (dimensions.isValid() && dimensions.width() <= 16384 &&
          dimensions.height() <= 16384 &&
          static_cast<qint64>(dimensions.width()) * dimensions.height() <=
              100000000LL)
        pixels = reader.read();
    }
    referenceImages_.push_back({image, std::move(pixels)});
  }
  update();
}

void Viewport::setSketchVisible(std::size_t index, bool visible) {
  if (index >= displaySketches_.size()) return;
  displaySketches_[index].visible = visible;
  update();
}

void Viewport::setOriginVisible(bool visible) {
  originVisible_ = visible;
  update();
}

void Viewport::setBasePlaneVisible(int plane, bool visible) {
  if (plane < 0 || plane > 2) return;
  basePlanesVisible_[plane] = visible;
  update();
}

bool Viewport::sketchVisible() const noexcept { return sketchVisible_; }

bool Viewport::sketchVisible(std::size_t index) const noexcept {
  return index < displaySketches_.size() && displaySketches_[index].visible;
}

bool Viewport::originVisible() const noexcept { return originVisible_; }

bool Viewport::basePlaneVisible(int plane) const noexcept {
  return plane >= 0 && plane <= 2 && basePlanesVisible_[plane];
}

bool Viewport::sketchPlaneSelectionActive() const noexcept {
  return pickMode_ == PickMode::SketchPlane;
}

bool Viewport::mirrorBodySelectionActive() const noexcept {
  return pickMode_ == PickMode::MirrorBody;
}

bool Viewport::mirrorPlaneSelectionActive() const noexcept {
  return pickMode_ == PickMode::MirrorPlane;
}

bool Viewport::moveBodySelectionActive() const noexcept {
  return pickMode_ == PickMode::MoveBody;
}

bool Viewport::linearPatternBodySelectionActive() const noexcept {
  return pickMode_ == PickMode::LinearPatternBody;
}

bool Viewport::linearPatternAxisSelectionActive() const noexcept {
  return pickMode_ == PickMode::LinearPatternAxis;
}

bool Viewport::circularPatternBodySelectionActive() const noexcept {
  return pickMode_ == PickMode::CircularPatternBody;
}

bool Viewport::circularPatternAxisSelectionActive() const noexcept {
  return pickMode_ == PickMode::CircularPatternAxis;
}

bool Viewport::draftFaceSelectionActive() const noexcept {
  return pickMode_ == PickMode::DraftFace;
}

bool Viewport::draftAxisSelectionActive() const noexcept {
  return pickMode_ == PickMode::DraftAxis;
}

void Viewport::beginRulerMeasurement() {
  resetToolInteraction();
  ruler_.begin();
  pickMode_ = PickMode::Ruler;
  setCursor(Qt::CrossCursor);
  emit rulerActiveChanged(true);
  update();
}

void Viewport::cancelRulerMeasurement() {
  if (!ruler_.active()) return;
  ruler_.cancel();
  if (pickMode_ == PickMode::Ruler) pickMode_ = PickMode::None;
  unsetCursor();
  emit rulerActiveChanged(false);
  update();
}

bool Viewport::rulerMeasurementActive() const noexcept {
  return ruler_.active();
}

std::optional<double> Viewport::rulerDistanceMm() const noexcept {
  return ruler_.measuredDistanceMm();
}

void Viewport::resetScene() {
  invalidatePendingHover();
  const bool rulerWasActive = ruler_.active();
  ruler_.cancel();
  orientationAnimation_->stop();
  clearCubeHover();
  cubePressed_ = {};
  bodyShape_.reset();
  bodyDisplayMeshes_.clear();
  displayedBodyCenter_ = {};
  displayedBodyDiagonal_ = 0.0;
  pickingScene_.invalidate();
  toolPreviewShape_.reset();
  toolPreviewRenderMesh_.clear();
  toolCutPreviewShape_.reset();
  toolCutPreviewRenderMesh_.clear();
  toolPreviewBodyId_ = kInvalidBodyId;
  toolPreviewFeatureId_ = kInvalidFeatureId;
  bodyTopologyRanges_.clear();
  bodyViewShapes_.clear();
  bodyMeshCache_.clear();
  bodyId_ = kInvalidBodyId;
  bodyFeatureId_ = kInvalidFeatureId;
  sketch_.clear();
  solidSketch_.clear();
  solidSketchPlacement_ = SketchPlacement::xy();
  displaySketches_.clear();
  referenceImages_.clear();
  extrusionHoverPolygon_.clear();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  selectedExtrusionSketch_.clear();
  selectedExtrusionPlacement_ = SketchPlacement::xy();
  hoveredExtrusionPlacement_ = SketchPlacement::xy();
  selectedExtrusionSupport_.clear();
  selectedExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  hoveredLegacySolidFace_.reset();
  selectedLegacySolidFace_.reset();
  hoveredExtrusionReverse_ = false;
  selectedExtrusionReverse_ = false;
  solidVisible_ = false;
  sketchVisible_ = true;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  selectedPatternAxis_ = -1;
  clearWholeBodySelection();
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;
  pickMode_ = PickMode::None;
  selectionFilter_ = SelectionFilter::Any;
  edgeMultiSelectionMode_ = false;
  faceMultiSelectionMode_ = false;

  // Tool UI is transient document state. Reset it atomically with the scene so
  // no stale drag snapshot/HUD can emit a value into a ToolSession whose source
  // Body belonged to the previous Document.
  toolManipulator_.reset();
  translationToolManipulator_.reset();
  angularToolManipulator_.reset();
  referenceImageManipulatorActive_ = false;
  linearDragSnapshot_.reset();
  draggingToolManipulator_ = false;
  draggingTranslationToolManipulator_ = false;
  activeTranslationAxis_ = -1;
  draggingAngularToolManipulator_ = false;
  toolHudParameterId_.clear();
  if (toolParameterHud_) toolParameterHud_->hide();
  panningView_ = false;
  draggingBody_ = false;
  marqueeActive_ = false;
  marqueeStart_ = {};
  marqueeCurrent_ = {};
  marqueeAdditive_ = false;
  unsetCursor();

  offsetX_ = 0.0F;
  offsetY_ = 0.0F;
  cameraPan_ = {};
  workGridPlacement_ = SketchPlacement::xy();
  workGridVisible_ = true;
  hideExtrusionManipulator();
  for (bool& visible : basePlanesVisible_) visible = false;
  synchronizeRendererResources();
  if (rulerWasActive) emit rulerActiveChanged(false);
  update();
}

void Viewport::beginSketchPlaneSelection() {
  setSelectionFilter(SelectionFilter::Face);
  pickMode_ = PickMode::SketchPlane;
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionSupport_.clear();
  selectedFace_ = -1;
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;
  for (bool& visible : basePlanesVisible_) visible = true;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginImagePlaneSelection() {
  beginSketchPlaneSelection();
  pickMode_ = PickMode::ImagePlane;
}

void Viewport::beginMirrorBodySelection() {
  pickMode_ = PickMode::MirrorBody;
  selectionFilter_ = SelectionFilter::Face;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  selectedBasePlane_ = -1;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  clearWholeBodySelection();
  for (bool& visible : basePlanesVisible_) visible = false;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginMirrorPlaneSelection() {
  pickMode_ = PickMode::MirrorPlane;
  // Mirror keeps the chosen source Body highlighted while the reference plane
  // is being picked. Assign the filter directly: the public Plane transition
  // intentionally clears whole-body selection for ordinary selection tools.
  selectionFilter_ = SelectionFilter::Plane;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  selectedBasePlane_ = -1;
  for (bool& visible : basePlanesVisible_) visible = true;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::showMirrorPlaneSelection(int planeIndex) {
  if (planeIndex < 0 || planeIndex > 2) return;
  pickMode_ = PickMode::MirrorPreview;
  selectionFilter_ = SelectionFilter::Any;
  selectedBasePlane_ = planeIndex;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  for (bool& visible : basePlanesVisible_) visible = true;
  unsetCursor();
  update();
}

void Viewport::beginMoveBodySelection() {
  pickMode_ = PickMode::MoveBody;
  selectionFilter_ = SelectionFilter::Face;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  clearWholeBodySelection();
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginJoinBodiesSelection() {
  pickMode_ = PickMode::JoinBodies;
  selectionFilter_ = SelectionFilter::Face;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  clearWholeBodySelection();
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::showMovePreview() {
  pickMode_ = PickMode::MovePreview;
  selectionFilter_ = SelectionFilter::Any;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  unsetCursor();
  update();
}

void Viewport::beginLinearPatternBodySelection() {
  pickMode_ = PickMode::LinearPatternBody;
  selectionFilter_ = SelectionFilter::Face;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  selectedPatternAxis_ = -1;
  hoveredRevolveAxisToken_ = 0;
  clearWholeBodySelection();
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginLinearPatternAxisSelection() {
  pickMode_ = PickMode::LinearPatternAxis;
  // Keep the source Body selected while the reference direction is picked.
  selectionFilter_ = SelectionFilter::Any;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  selectedPatternAxis_ = -1;
  hoveredRevolveAxisToken_ = 0;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::showLinearPatternAxisSelection(int axisIndex) {
  if (axisIndex < 0 || axisIndex > 2) return;
  pickMode_ = PickMode::LinearPatternPreview;
  selectionFilter_ = SelectionFilter::Any;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  hoveredRevolveAxisToken_ = 0;
  selectedPatternAxis_ = axisIndex;
  unsetCursor();
  update();
}

void Viewport::beginCircularPatternBodySelection() {
  pickMode_ = PickMode::CircularPatternBody;
  selectionFilter_ = SelectionFilter::Face;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  selectedPatternAxis_ = -1;
  hoveredRevolveAxisToken_ = 0;
  clearWholeBodySelection();
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginCircularPatternAxisSelection() {
  pickMode_ = PickMode::CircularPatternAxis;
  selectionFilter_ = SelectionFilter::Any;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  selectedPatternAxis_ = -1;
  hoveredRevolveAxisToken_ = 0;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::showCircularPatternAxisSelection(int axisIndex) {
  if (axisIndex < 0 || axisIndex > 2) return;
  pickMode_ = PickMode::CircularPatternPreview;
  selectionFilter_ = SelectionFilter::Any;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  hoveredRevolveAxisToken_ = 0;
  selectedPatternAxis_ = axisIndex;
  unsetCursor();
  update();
}

void Viewport::beginDraftFaceSelection() {
  pickMode_ = PickMode::DraftFace;
  selectionFilter_ = SelectionFilter::Face;
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedPatternAxis_ = -1;
  selectedDraftAxisEdge_.reset();
  hoveredRevolveAxisToken_ = 0;
  clearWholeBodySelection();
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginDraftAxisSelection() {
  pickMode_ = PickMode::DraftAxis;
  // Keep the selected face highlighted while choosing the rotation axis.
  selectionFilter_ = SelectionFilter::Edge;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedPatternAxis_ = -1;
  selectedDraftAxisEdge_.reset();
  hoveredRevolveAxisToken_ = 0;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::showDraftAxisSelection(int axisIndex) {
  if (axisIndex < 0 || axisIndex > 2) return;
  pickMode_ = PickMode::DraftPreview;
  selectionFilter_ = SelectionFilter::Any;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredRevolveAxisToken_ = 0;
  selectedPatternAxis_ = axisIndex;
  selectedDraftAxisEdge_.reset();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  unsetCursor();
  update();
}

void Viewport::showDraftEdgeAxisSelection(const EdgeReference& edge) {
  pickMode_ = PickMode::DraftPreview;
  selectionFilter_ = SelectionFilter::Any;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredRevolveAxisToken_ = 0;
  selectedPatternAxis_ = -1;
  selectedDraftAxisEdge_ = edge;
  setSelectedBodyEdges({edge});
  unsetCursor();
  update();
}

void Viewport::beginExtrusionSurfaceSelection() {
  hideExtrusionManipulator();
  pickMode_ = PickMode::ExtrusionSurface;
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  hoveredExtrusionSketch_.clear();
  hoveredExtrusionPlacement_ = SketchPlacement::xy();
  hoveredExtrusionSupport_.clear();
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  hoveredExtrusionOnBodyCap_ = false;
  hoveredExtrusionReverse_ = false;
  hoveredLegacySolidFace_.reset();
  selectedExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionSketch_.clear();
  selectedExtrusionPlacement_ = SketchPlacement::xy();
  selectedExtrusionSupport_.clear();
  selectedExtrusionBodyFace_ = false;
  selectedExtrusionOnBodyCap_ = false;
  selectedExtrusionReverse_ = false;
  selectedLegacySolidFace_.reset();
  selectedFace_ = -1;
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::showExtrusionManipulator(double lengthMm) {
  const bool editorWasVisible = extrusionLengthEditor_->isVisible();
  extrusionManipulatorVisible_ = true;
  setExtrusionPreviewLength(lengthMm);
  extrusionLengthEditor_->show();
  extrusionLengthEditor_->raise();
  if (!editorWasVisible) {
    extrusionLengthEditor_->setFocus(Qt::OtherFocusReason);
    extrusionLengthEditor_->selectAll();
  }
  update();
}

void Viewport::hideExtrusionManipulator() {
  extrusionManipulatorVisible_ = false;
  draggingExtrusionHandle_ = false;
  extrusionLengthEditor_->hide();
  update();
}

void Viewport::clearLegacyExtrusionPreview() {
  hideExtrusionManipulator();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  selectedExtrusionSketch_.clear();
  selectedExtrusionPlacement_ = SketchPlacement::xy();
  selectedExtrusionReverse_ = false;
  selectedLegacySolidFace_.reset();
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  extrusionManipulatorAnchor_ = {};
}

bool Viewport::legacyExtrusionPreviewSuppressed() const noexcept {
  return toolManipulator_ && toolManipulator_->directional;
}

bool Viewport::extrusionManipulatorVisible() const noexcept {
  return extrusionManipulatorVisible_;
}

void Viewport::setExtrusionPreviewLength(double lengthMm) {
  const double clamped = std::clamp(lengthMm, -100000.0, 100000.0);
  const bool semanticChange =
      std::abs(clamped - extrusionPreviewLengthMm_) > 1e-9;
  extrusionPreviewLengthMm_ = clamped;
  if (extrusionLengthEditor_ &&
      !qFuzzyCompare(extrusionLengthEditor_->value(), extrusionPreviewLengthMm_)) {
    const QSignalBlocker blocker(extrusionLengthEditor_);
    extrusionLengthEditor_->setValue(extrusionPreviewLengthMm_);
  }
  if (extrusionManipulatorVisible_) {
    const QPointF handle = extrusionManipulatorAnchor_ +
                           extrusionScreenOffset(extrusionPreviewLengthMm_) + cameraPan_;
    extrusionLengthEditor_->move(
        std::clamp(static_cast<int>(handle.x() + 16), 4,
                   std::max(4, width() - extrusionLengthEditor_->width() - 4)),
        std::clamp(static_cast<int>(handle.y() - 15), 4,
                   std::max(4, height() - extrusionLengthEditor_->height() - 4)));
  }
  if (semanticChange)
    emit extrusionPreviewLengthChanged(extrusionPreviewLengthMm_);
  update();
}

QPointF Viewport::extrusionScreenOffset(double lengthMm) const {
  Point3 normal = placementNormal(selectedExtrusionPlacement_);
  if (selectedLegacySolidFace_) {
    const auto direction = selectedLegacySolidFace_->placement.normal();
    normal = {static_cast<float>(direction.x), static_cast<float>(direction.y),
              static_cast<float>(direction.z)};
  }
  if (selectedExtrusionReverse_)
    normal = {-normal.x, -normal.y, -normal.z};
  if (selectedExtrusionSketchIndex_ < displaySketches_.size()) {
    const auto direction =
        displaySketches_[selectedExtrusionSketchIndex_].placement.normal();
    normal = {static_cast<float>(direction.x), static_cast<float>(direction.y),
              static_cast<float>(direction.z)};
  }
  const QPointF origin = project({0.0F, 0.0F, 0.0F}, size(), yaw_, pitch_, zoom_);
  const Point3 end3 = translated({0.0F, 0.0F, 0.0F}, normal,
                                 static_cast<float>(lengthMm));
  return project(end3, size(), yaw_, pitch_, zoom_) - origin;
}

bool Viewport::hasSelectedFace() const noexcept { return selectedFace_ >= 0; }

QString Viewport::selectedFaceName() const {
  static const std::array<const char*, 6> names{
      "Нижняя", "Верхняя", "Передняя",
      "Правая", "Задняя", "Левая"};
  if (selectedFace_ < 0) return {};
  if (selectedFace_ < static_cast<int>(names.size()))
    return QString::fromUtf8(names[static_cast<std::size_t>(selectedFace_)]);
  return QString::fromUtf8("Грань #%1").arg(selectedFace_ + 1);
}

std::optional<std::size_t> Viewport::selectedBodyFaceIndex() const noexcept {
  return selectedFace_ >= 0
             ? std::optional<std::size_t>(static_cast<std::size_t>(selectedFace_))
             : std::nullopt;
}

std::optional<FaceReference> Viewport::selectedBodyFace() const {
  if (!selectedBodyFaceReferences_.empty())
    return selectedBodyFaceReferences_.front();
  if (selectedFace_ < 0) return std::nullopt;
  return faceReferenceForGlobalIndex(static_cast<std::size_t>(selectedFace_));
}

std::optional<FaceReference> Viewport::faceReferenceForGlobalIndex(
    std::size_t global) const {
  for (const auto& range : bodyTopologyRanges_)
    if (global >= range.firstFace &&
        global < range.firstFace + range.faceCount && range.topologyIndex) {
      const auto created = range.topologyIndex->createFaceReference(
          range.bodyId, range.featureId, global - range.firstFace);
      return created ? std::optional<FaceReference>{created.reference}
                     : std::nullopt;
    }
  return std::nullopt;
}

std::vector<FaceReference> Viewport::selectedBodyFaces() const {
  return selectedBodyFaceReferences_;
}

void Viewport::setSelectedBodyFaces(const std::vector<FaceReference>& faces) {
  clearWholeBodySelection();
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  for (const auto& face : faces)
    for (const auto& range : bodyTopologyRanges_)
      if (range.bodyId == face.bodyId && range.featureId == face.featureId &&
          range.topologyIndex) {
        const auto resolved = range.topologyIndex->resolveFace(face.topology());
        if (resolved) {
          selectedBodyFaceIndices_.push_back(range.firstFace + resolved.index);
          selectedBodyFaceReferences_.push_back(face);
        }
      }
  selectedFace_ = selectedBodyFaceIndices_.empty()
                      ? -1
                      : static_cast<int>(selectedBodyFaceIndices_.front());
  update();
}

void Viewport::setFaceMultiSelectionMode(bool enabled) noexcept {
  faceMultiSelectionMode_ = enabled;
}

bool Viewport::faceMultiSelectionMode() const noexcept {
  return faceMultiSelectionMode_;
}

std::optional<EdgeReference> Viewport::selectedBodyEdge() const noexcept {
  if (selectedBodyEdgeReferences_.empty()) return std::nullopt;
  return selectedBodyEdgeReferences_.front();
}

void Viewport::beginRevolveAxisSelection(std::size_t sketchIndex) {
  pickMode_ = PickMode::RevolveAxis;
  revolveAxisSketchIndex_ = sketchIndex;
  hoveredRevolveAxisToken_ = 0;
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  setCursor(Qt::CrossCursor);
  update();
}

std::optional<EdgeReference> Viewport::edgeReferenceForGlobalIndex(
    std::size_t global) const {
  for (const auto& range : bodyTopologyRanges_)
    if (global >= range.firstEdge &&
        global < range.firstEdge + range.edgeCount && range.topologyIndex) {
      const auto created = range.topologyIndex->createEdgeReference(
          range.bodyId, range.featureId, global - range.firstEdge);
      return created ? std::optional<EdgeReference>{created.reference}
                     : std::nullopt;
    }
  return std::nullopt;
}

std::vector<EdgeReference> Viewport::selectedBodyEdges() const {
  return selectedBodyEdgeReferences_;
}

void Viewport::setSelectedBodyEdges(const std::vector<EdgeReference>& edges) {
  clearWholeBodySelection();
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  for (const auto& edge : edges)
    for (const auto& range : bodyTopologyRanges_)
      if (range.bodyId == edge.bodyId && range.featureId == edge.featureId &&
          range.topologyIndex) {
        const auto resolved = range.topologyIndex->resolveEdge(edge.topology());
        if (resolved) {
          selectedBodyEdgeIndices_.push_back(range.firstEdge + resolved.index);
          selectedBodyEdgeReferences_.push_back(edge);
        }
      }
  selectedBodyEdgeIndex_ = selectedBodyEdgeIndices_.empty()
                               ? static_cast<std::size_t>(-1)
                               : selectedBodyEdgeIndices_.front();
  update();
}

void Viewport::setEdgeMultiSelectionMode(bool enabled) noexcept {
  edgeMultiSelectionMode_ = enabled;
}

bool Viewport::edgeMultiSelectionMode() const noexcept {
  return edgeMultiSelectionMode_;
}

const std::vector<BodyId>& Viewport::selectedBodies() const noexcept {
  return selectedBodyIds_;
}

void Viewport::clearWholeBodySelection() noexcept {
  if (selectedBodyIds_.empty()) return;
  selectedBodyIds_.clear();
  emit bodiesSelected(selectedBodyIds_);
}

void Viewport::setSelectedBodies(std::vector<BodyId> ids) {
  selectedBodyIds_ = std::move(ids);
  // Cross-type: a whole-body selection cannot coexist with face or edge
  // sub-element selection.
  selectedFace_ = -1;
  selectedBodyFaceIndices_.clear();
  selectedBodyFaceReferences_.clear();
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  selectedBodyEdgeReferences_.clear();
  emit selectionChanged(
      selectedBodyIds_.empty()
          ? QString{}
          : QString::fromUtf8("Выбрано тел: %1").arg(selectedBodyIds_.size()));
  emit bodiesSelected(selectedBodyIds_);
  update();
}

std::vector<std::size_t> Viewport::effectiveSelectedFaceIndices() const {
  if (selectedBodyIds_.empty()) return selectedBodyFaceIndices_;
  std::vector<std::size_t> indices;
  for (const auto& range : bodyTopologyRanges_) {
    if (std::find(selectedBodyIds_.begin(), selectedBodyIds_.end(),
                  range.bodyId) == selectedBodyIds_.end())
      continue;
    for (std::size_t face = range.firstFace;
         face < range.firstFace + range.faceCount; ++face)
      indices.push_back(face);
  }
  return indices;
}

std::vector<std::size_t> Viewport::effectiveHoveredFaceIndices() const {
  if ((pickMode_ != PickMode::MirrorBody &&
       pickMode_ != PickMode::MoveBody &&
       pickMode_ != PickMode::JoinBodies &&
       pickMode_ != PickMode::LinearPatternBody &&
       pickMode_ != PickMode::CircularPatternBody) ||
      hoveredToolBodyId_ == kInvalidBodyId) {
    return hoveredBodyFaceIndex_ == static_cast<std::size_t>(-1)
               ? std::vector<std::size_t>{}
               : std::vector<std::size_t>{hoveredBodyFaceIndex_};
  }
  std::vector<std::size_t> indices;
  for (const auto& range : bodyTopologyRanges_) {
    if (range.bodyId != hoveredToolBodyId_) continue;
    for (std::size_t face = range.firstFace;
         face < range.firstFace + range.faceCount; ++face)
      indices.push_back(face);
  }
  return indices;
}

void Viewport::setSelectionFilter(SelectionFilter filter) noexcept {
  selectionFilter_ = filter;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  // Cross-type: switching the selection context to edges or faces clears the
  // now-incompatible selection type so no mixed edge+face selection persists.
  if (filter == SelectionFilter::Edge) {
    selectedFace_ = -1;
    selectedBodyFaceIndices_.clear();
    selectedBodyFaceReferences_.clear();
  } else if (filter == SelectionFilter::Face) {
    selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    selectedBodyEdgeIndices_.clear();
    selectedBodyEdgeReferences_.clear();
  }
  // Whole-body selection only lives in normal (Any) mode; entering a
  // sub-element tool (Edge/Face) or base-plane selection (Plane) drops it so a
  // body selection cannot coexist with face/edge selection.
  if (filter == SelectionFilter::Edge || filter == SelectionFilter::Face ||
      filter == SelectionFilter::Plane) {
    clearWholeBodySelection();
  }
  update();
}

SelectionFilter Viewport::selectionFilter() const noexcept {
  return selectionFilter_;
}

bool Viewport::marqueeActive() const noexcept { return marqueeActive_; }

std::optional<std::size_t> Viewport::hoveredBodyEdgeIndex() const noexcept {
  if (hoveredBodyEdgeIndex_ == static_cast<std::size_t>(-1))
    return std::nullopt;
  return hoveredBodyEdgeIndex_;
}

void Viewport::commitEdgeSelection(std::size_t globalIndex, bool toggle) {
  clearWholeBodySelection();
  const auto clicked = edgeReferenceForGlobalIndex(globalIndex);
  if (toggle && !selectedBodyEdgeIndices_.empty()) {
    const auto first =
        edgeReferenceForGlobalIndex(selectedBodyEdgeIndices_.front());
    if (first && clicked &&
        (first->bodyId != clicked->bodyId ||
         first->featureId != clicked->featureId))
      selectedBodyEdgeIndices_.clear();
  }
  updateEdgeSelection(selectedBodyEdgeIndices_, globalIndex, toggle);
  selectedBodyEdgeIndex_ = selectedBodyEdgeIndices_.empty()
                               ? static_cast<std::size_t>(-1)
                               : selectedBodyEdgeIndices_.front();
  selectedBodyEdgeReferences_.clear();
  for (const auto index : selectedBodyEdgeIndices_)
    if (const auto edge = edgeReferenceForGlobalIndex(index))
      selectedBodyEdgeReferences_.push_back(*edge);
  emit selectionChanged(QString::fromUtf8("Тело 1 • Ребро ") +
                        QString::number(globalIndex + 1));
  emit bodyEdgeSelectionChanged();
  update();
}

void Viewport::commitFaceSelection(std::size_t globalIndex, bool toggle) {
  clearWholeBodySelection();
  const auto clicked = faceReferenceForGlobalIndex(globalIndex);
  if (toggle && !selectedBodyFaceIndices_.empty()) {
    const auto first =
        faceReferenceForGlobalIndex(selectedBodyFaceIndices_.front());
    if (first && clicked &&
        (first->bodyId != clicked->bodyId ||
         first->featureId != clicked->featureId))
      selectedBodyFaceIndices_.clear();
  }
  updateEdgeSelection(selectedBodyFaceIndices_, globalIndex, toggle);
  selectedFace_ = selectedBodyFaceIndices_.empty()
                      ? -1
                      : static_cast<int>(selectedBodyFaceIndices_.front());
  selectedBodyFaceReferences_.clear();
  for (const auto index : selectedBodyFaceIndices_)
    if (const auto face = faceReferenceForGlobalIndex(index))
      selectedBodyFaceReferences_.push_back(*face);
  emit selectionChanged(QString::fromUtf8("Тело 1 • Грань ") +
                        QString::number(globalIndex + 1));
  emit bodyFaceSelectionChanged();
  update();
}

void Viewport::setToolManipulator(const LinearToolManipulator& manipulator) {
  const bool hudWasVisible = toolParameterHud_->isVisible();
  toolManipulator_ = manipulator;
  translationToolManipulator_.reset();
  angularToolManipulator_.reset();
  referenceImageManipulatorActive_ = false;
  // The HUD/panel show the ABSOLUTE length for directional manipulators; the
  // sign only reflects the drag direction, not a separate user-facing control.
  const double hudValue =
      manipulator.directional ? std::abs(manipulator.valueMm) : manipulator.valueMm;
  const double hudMinimum = manipulator.directional ? 0.01 : manipulator.minimumMm;
  const double hudMaximum = manipulator.maximumMm;
  if (toolHudParameterId_ != "distance") {
    toolParameterHud_->setParameters({
        {"distance", "Distance", ToolParameterType::Distance,
         hudValue, hudMinimum, hudMaximum,
         0.1, "mm", true,
         ToolManipulatorType::Linear}});
    toolHudParameterId_ = "distance";
  } else {
    toolParameterHud_->setValue("distance", hudValue);
  }
  const QPointF tip = cameraPan_ + projectBodyPoint(
      {manipulator.origin.x + manipulator.direction.x * manipulator.valueMm,
       manipulator.origin.y + manipulator.direction.y * manipulator.valueMm,
       manipulator.origin.z + manipulator.direction.z * manipulator.valueMm},
      manipulator.origin, size(), yaw_, pitch_, zoom_).screen;
  toolParameterHud_->move(
      std::clamp(static_cast<int>(tip.x() + 12), 4,
                 std::max(4, width() - toolParameterHud_->width() - 4)),
      std::clamp(static_cast<int>(tip.y() - 20), 4,
                 std::max(4, height() - toolParameterHud_->height() - 4)));
  toolParameterHud_->show();
  toolParameterHud_->raise();
  if (!hudWasVisible) toolParameterHud_->focusFirstField();
  update();
}

double Viewport::toolManipulatorHudValue() const noexcept {
  if (!toolManipulator_ || !toolParameterHud_) return 0.0;
  return toolParameterHud_->value("distance");
}

void Viewport::setTranslationToolManipulator(
    const TranslationToolManipulator& manipulator) {
  const bool hudWasVisible = toolParameterHud_->isVisible();
  translationToolManipulator_ = manipulator;
  referenceImageManipulatorActive_ = false;
  toolManipulator_.reset();
  angularToolManipulator_.reset();
  if (toolHudParameterId_ != "translation") {
    toolParameterHud_->setParameters({
        {"offset_x", "X", ToolParameterType::Distance,
         manipulator.offsetMm.x, manipulator.minimumMm, manipulator.maximumMm,
         0.1, "mm", true, ToolManipulatorType::Linear},
        {"offset_y", "Y", ToolParameterType::Distance,
         manipulator.offsetMm.y, manipulator.minimumMm, manipulator.maximumMm,
         0.1, "mm", true, ToolManipulatorType::Linear},
        {"offset_z", "Z", ToolParameterType::Distance,
         manipulator.offsetMm.z, manipulator.minimumMm, manipulator.maximumMm,
         0.1, "mm", true, ToolManipulatorType::Linear}});
    toolHudParameterId_ = "translation";
  } else {
    toolParameterHud_->setValue("offset_x", manipulator.offsetMm.x);
    toolParameterHud_->setValue("offset_y", manipulator.offsetMm.y);
    toolParameterHud_->setValue("offset_z", manipulator.offsetMm.z);
  }

  const QPointF anchor = cameraPan_ + projectBodyPoint(
      manipulator.origin, manipulator.origin, size(), yaw_, pitch_, zoom_).screen;
  toolParameterHud_->move(
      std::clamp(static_cast<int>(anchor.x() + 78), 4,
                 std::max(4, width() - toolParameterHud_->width() - 4)),
      std::clamp(static_cast<int>(anchor.y() - 20), 4,
                 std::max(4, height() - toolParameterHud_->height() - 4)));
  toolParameterHud_->show();
  toolParameterHud_->raise();
  if (!hudWasVisible) toolParameterHud_->focusFirstField();
  update();
}

void Viewport::setReferenceImageManipulator(const ReferenceImage& image) {
  const bool hudWasVisible = toolParameterHud_->isVisible();
  const Vector3d normal = image.placement.normal();
  Point3d origin = image.placement.toWorld(image.offsetXMm, image.offsetYMm);
  origin = offsetPoint(origin, normal, image.offsetZMm);
  translationToolManipulator_ = TranslationToolManipulator{
      origin,
      {image.offsetXMm, image.offsetYMm, image.offsetZMm},
      -1000000.0,
      1000000.0,
      {image.placement.xDirection, image.placement.yDirection, normal}};
  referenceImageManipulatorActive_ = true;
  toolManipulator_.reset();
  angularToolManipulator_.reset();
  if (toolHudParameterId_ != "reference_image") {
    toolParameterHud_->setParameters({
        {"image_offset_x", "X", ToolParameterType::Distance,
         image.offsetXMm, -1000000.0, 1000000.0, 0.1, "mm", true,
         ToolManipulatorType::Linear},
        {"image_offset_y", "Y", ToolParameterType::Distance,
         image.offsetYMm, -1000000.0, 1000000.0, 0.1, "mm", true,
         ToolManipulatorType::Linear},
        {"image_offset_z", "Z", ToolParameterType::Distance,
         image.offsetZMm, -1000000.0, 1000000.0, 0.1, "mm", true,
         ToolManipulatorType::Linear},
        {"image_scale", "Scale", ToolParameterType::Distance,
         image.scale * 100.0, 0.1, 1000000.0, 1.0, "%", true,
         ToolManipulatorType::None}});
    toolHudParameterId_ = "reference_image";
  } else {
    toolParameterHud_->setValue("image_offset_x", image.offsetXMm);
    toolParameterHud_->setValue("image_offset_y", image.offsetYMm);
    toolParameterHud_->setValue("image_offset_z", image.offsetZMm);
    toolParameterHud_->setValue("image_scale", image.scale * 100.0);
  }

  const QPointF anchor = cameraPan_ + projectBodyPoint(
      origin, origin, size(), yaw_, pitch_, zoom_).screen;
  toolParameterHud_->move(
      std::clamp(static_cast<int>(anchor.x() + 78), 4,
                 std::max(4, width() - toolParameterHud_->width() - 4)),
      std::clamp(static_cast<int>(anchor.y() - 20), 4,
                 std::max(4, height() - toolParameterHud_->height() - 4)));
  toolParameterHud_->show();
  toolParameterHud_->raise();
  if (!hudWasVisible) toolParameterHud_->focusFirstField();
  update();
}

void Viewport::setAngularToolManipulator(
    const AngularToolManipulator& manipulator) {
  const bool hudWasVisible = toolParameterHud_->isVisible();
  angularToolManipulator_ = manipulator;
  referenceImageManipulatorActive_ = false;
  toolManipulator_.reset();
  translationToolManipulator_.reset();
  if (toolHudParameterId_ != "angle" ||
      toolHudMinimum_ != manipulator.minimumDeg ||
      toolHudMaximum_ != manipulator.maximumDeg) {
    toolParameterHud_->setParameters({
        {"angle", "Angle", ToolParameterType::Angle, manipulator.angleDeg,
         manipulator.minimumDeg, manipulator.maximumDeg, 1.0, "°", true,
         ToolManipulatorType::Angular}});
    toolHudParameterId_ = "angle";
    toolHudMinimum_ = manipulator.minimumDeg;
    toolHudMaximum_ = manipulator.maximumDeg;
  } else {
    toolParameterHud_->setValue("angle", manipulator.angleDeg);
  }
  const auto [u, v] = angularBasis(manipulator.axis);
  const double angle = manipulator.angleDeg * std::numbers::pi / 180.0;
  const Point3d handleWorld = offsetPoint(
      manipulator.origin, u, manipulator.radiusMm * std::cos(angle), v,
      manipulator.radiusMm * std::sin(angle));
  const QPointF handle = cameraPan_ + projectBodyPoint(
      handleWorld, manipulator.origin, size(), yaw_, pitch_, zoom_).screen;
  toolParameterHud_->move(
      std::clamp(static_cast<int>(handle.x() + 12), 4,
                 std::max(4, width() - toolParameterHud_->width() - 4)),
      std::clamp(static_cast<int>(handle.y() - 20), 4,
                 std::max(4, height() - toolParameterHud_->height() - 4)));
  toolParameterHud_->show();
  toolParameterHud_->raise();
  if (!hudWasVisible) toolParameterHud_->focusFirstField();
  update();
}

void Viewport::clearToolManipulator() {
  toolManipulator_.reset();
  translationToolManipulator_.reset();
  angularToolManipulator_.reset();
  referenceImageManipulatorActive_ = false;
  toolParameterHud_->hide();
  toolHudParameterId_.clear();
  toolHudMinimum_ = 0.0;
  toolHudMaximum_ = 0.0;
  linearDragSnapshot_.reset();
  draggingToolManipulator_ = false;
  draggingTranslationToolManipulator_ = false;
  activeTranslationAxis_ = -1;
  draggingAngularToolManipulator_ = false;
  update();
}

void Viewport::seedToolManipulatorDrag(QPointF scenePosition,
                                       Vector3d worldAxis) {
  if (!toolManipulator_) return;
  const Point3d center = toolManipulator_->origin;
  const QPointF start = projectBodyPoint(
      toolManipulator_->origin, center, size(), yaw_, pitch_, zoom_).screen;
  const Point3d axisEnd{toolManipulator_->origin.x + worldAxis.x,
                        toolManipulator_->origin.y + worldAxis.y,
                        toolManipulator_->origin.z + worldAxis.z};
  QPointF projectedUnitAxis = projectBodyPoint(
      axisEnd, center, size(), yaw_, pitch_, zoom_).screen - start;
  const auto layout = toolManipulatorLayout();
  const QPointF dragAxis = robustLinearDragAxis(
      projectedUnitAxis,
      layout ? layout->direction * layout->visualSign : projectedUnitAxis,
      manipulatorStyle_.nearEndOnThresholdPx);
  linearDragSnapshot_ = {scenePosition, dragAxis, toolManipulator_->valueMm};
  draggingToolManipulator_ = true;
  setCursor(Qt::SizeAllCursor);
}
bool Viewport::focusToolParameterField(bool backward) {
  // Legacy Extrude still owns a dedicated on-canvas spinbox.
  if (extrusionLengthEditor_ && extrusionLengthEditor_->isVisible()) {
    extrusionLengthEditor_->setFocus(
        backward ? Qt::BacktabFocusReason : Qt::TabFocusReason);
    extrusionLengthEditor_->selectAll();
    return true;
  }

  // Modern Part Design tools share ToolParameterHud. Multi-field tools cycle
  // entirely inside that HUD; the right-hand panel never participates.
  if (toolParameterHud_ && toolParameterHud_->isVisible() &&
      toolParameterHud_->hasEditableParameters()) {
    if (backward)
      toolParameterHud_->focusLastField();
    else
      toolParameterHud_->focusFirstField();
    return true;
  }

  return false;
}

std::optional<ManipulatorLayoutResult> Viewport::toolManipulatorLayout() const {
  if (!toolManipulator_) return std::nullopt;
  const Point3d center = toolManipulator_->origin;
  const QPointF start = projectBodyPoint(toolManipulator_->origin, center, size(),
                                         yaw_, pitch_, zoom_).screen;
  // The semantic direction must be value-independent: probe a single world unit
  // (origin + direction), never origin + direction * valueMm. At valueMm == 0
  // (Fillet/Chamfer start) the value-scaled probe degenerates to a zero vector,
  // forcing the screen-up fallback while the drag path still uses the real unit
  // axis — producing a sign mismatch where dragging along the drawn arrow yields
  // no value change. A unit probe keeps draw/hit-test/drag on the same axis.
  const Point3d endWorld = offsetPoint(
      toolManipulator_->origin, toolManipulator_->direction, 1.0);
  const QPointF semanticEnd = projectBodyPoint(endWorld, center, size(), yaw_,
                                               pitch_, zoom_).screen;
  const bool hasToolPreview = toolPreviewShape_ && !toolPreviewShape_->IsNull();
  const QRectF bodyBounds = hasToolPreview
      ? projectedBodyBounds(toolPreviewRenderMesh_, size(), yaw_, pitch_, zoom_)
      : projectedDisplayedBodyBounds();
  ManipulatorStyle style = manipulatorStyle_;
  if (toolManipulator_->directional) style.allowVisualDirectionFlip = false;
  return computeManipulatorLayout(
      {start, semanticEnd - start, QLineF(start, semanticEnd).length(),
       bodyBounds, QRectF(QPointF(-cameraPan_.x(), -cameraPan_.y()), size()),
       toolParameterHud_ ? toolParameterHud_->size() : QSizeF(132, 40),
       {QRectF(width() - 126.0 - cameraPan_.x(), 8.0 - cameraPan_.y(), 116.0,
               116.0)}},
      style);
}

std::array<ManipulatorLayoutResult, 3>
Viewport::translationManipulatorLayouts() const {
  std::array<ManipulatorLayoutResult, 3> result{};
  if (!translationToolManipulator_) return result;
  const auto& manipulator = *translationToolManipulator_;
  const QPointF anchor = projectBodyPoint(manipulator.origin, manipulator.origin,
                                          size(), yaw_, pitch_, zoom_).screen;
  const std::array<QPointF, 3> fallbacks{{{1.0, 0.0},
                                          {-0.7, 0.7},
                                          {0.0, -1.0}}};
  for (int axis = 0; axis < 3; ++axis) {
    const Point3d endpoint =
        offsetPoint(manipulator.origin, manipulator.axes[axis], 1.0);
    const QPointF projected = projectBodyPoint(
        endpoint, manipulator.origin, size(), yaw_, pitch_, zoom_).screen;
    const auto stable = stableProjectedDirection(
        projected - anchor, fallbacks[axis],
        manipulatorStyle_.nearEndOnThresholdPx);
    result[axis] = {anchor,
                    anchor + stable.normalizedDirection * 64.0,
                    {},
                    1.0,
                    64.0,
                    stable.normalizedDirection,
                    stable.usedFallback};
  }
  return result;
}

std::optional<Viewport::AngularVisual> Viewport::angularVisual() const {
  if (!angularToolManipulator_) return std::nullopt;
  const auto& manipulator = *angularToolManipulator_;
  const auto [u, v] = angularBasis(manipulator.axis);
  const Point3d center = manipulator.origin;
  const QPointF origin = projectBodyPoint(manipulator.origin, center, size(),
                                          yaw_, pitch_, zoom_).screen;
  const QPointF requestedRadiusPoint = projectBodyPoint(
      offsetPoint(manipulator.origin, u, manipulator.radiusMm), center, size(),
      yaw_, pitch_, zoom_).screen;
  const QPointF vRadiusPoint = projectBodyPoint(
      offsetPoint(manipulator.origin, v, manipulator.radiusMm), center, size(),
      yaw_, pitch_, zoom_).screen;
  const bool hasToolPreview = toolPreviewShape_ && !toolPreviewShape_->IsNull();
  const QRectF bodyBounds = hasToolPreview
      ? projectedBodyBounds(toolPreviewRenderMesh_, size(), yaw_, pitch_, zoom_)
      : projectedDisplayedBodyBounds();
  const auto visual = computeAngularVisualRadius(
      origin, requestedRadiusPoint, vRadiusPoint, manipulator.radiusMm,
      bodyBounds, manipulatorStyle_);
  return AngularVisual{origin, u, v, normalized(manipulator.axis),
                       visual.visualRadiusMm};
}

void Viewport::fitAll() {
  if (displayedBodyDiagonal() <= 1e-9) return;
  double minX = std::numeric_limits<double>::max();
  double minY = minX;
  double maxX = -minX;
  double maxY = -minX;
  for (const auto& display : bodyDisplayMeshes_) {
    if (!display.mesh) continue;
    for (const auto& vertex : display.mesh->vertices()) {
      const auto projected = projectBodyPoint(vertex.position,
                                              displayedBodyCenter(), size(),
                                              yaw_, pitch_, 1.0F);
      minX = std::min(minX, projected.screen.x());
      minY = std::min(minY, projected.screen.y());
      maxX = std::max(maxX, projected.screen.x());
      maxY = std::max(maxY, projected.screen.y());
    }
  }
  const double projectedWidth = std::max(1.0, maxX - minX);
  const double projectedHeight = std::max(1.0, maxY - minY);
  zoom_ = static_cast<float>(std::clamp(
      0.88 * std::min(width() / projectedWidth, height() / projectedHeight),
      static_cast<double>(kMinimumViewportZoom),
      static_cast<double>(kMaximumViewportZoom)));
  const QPointF viewportCenter(width() * 0.5, height() * 0.52);
  const QPointF boundsCenter((minX + maxX) * 0.5, (minY + maxY) * 0.5);
  cameraPan_ = viewportCenter -
      (viewportCenter + (boundsCenter - viewportCenter) * zoom_);
  update();
}

void Viewport::setStandardView(StandardView view) {
  orientationAnimation_->stop();
  const auto target = orientationFor(view);
  yaw_ = target.yaw; pitch_ = target.pitch;
  fitAll();
}
void Viewport::viewTop() { setStandardView(StandardView::Top); }
void Viewport::viewBottom() { setStandardView(StandardView::Bottom); }
void Viewport::viewFront() { setStandardView(StandardView::Front); }
void Viewport::viewBack() { setStandardView(StandardView::Back); }
void Viewport::viewRight() { setStandardView(StandardView::Right); }
void Viewport::viewLeft() { setStandardView(StandardView::Left); }
void Viewport::viewIsometric() { setStandardView(StandardView::Isometric); }

void Viewport::animateOrientation(CameraOrientation target) {
  orientationAnimation_->stop();
  // Re-target from the currently displayed orientation, including mid-animation.
  disconnect(orientationAnimation_, nullptr, this, nullptr);
  const CameraOrientation start{yaw_,pitch_};
  connect(orientationAnimation_, &QVariantAnimation::valueChanged, this,
          [this,start,target](const QVariant& value) {
    const auto camera = interpolateOrientation(start,target,value.toFloat());
    yaw_ = camera.yaw; pitch_ = camera.pitch;
    update();
  });
  orientationAnimation_->setStartValue(0.0F);
  orientationAnimation_->setEndValue(1.0F);
  orientationAnimation_->start();
}

void Viewport::clearCubeHover() {
  if (cubeHover_) { setCursor(cursorBeforeCube_); QToolTip::hideText(); }
  cubeHover_ = {};
  update();
}

void Viewport::leaveEvent(QEvent* event) {
  clearCubeHover();
  if (pickMode_ == PickMode::MirrorBody ||
      pickMode_ == PickMode::MoveBody ||
      pickMode_ == PickMode::JoinBodies ||
      pickMode_ == PickMode::LinearPatternBody ||
      pickMode_ == PickMode::CircularPatternBody) {
    hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
    hoveredToolBodyId_ = kInvalidBodyId;
    update();
  } else if (pickMode_ == PickMode::MirrorPlane) {
    selectedBasePlane_ = -1;
    update();
  } else if (pickMode_ == PickMode::LinearPatternAxis ||
             pickMode_ == PickMode::CircularPatternAxis ||
             pickMode_ == PickMode::DraftAxis) {
    hoveredRevolveAxisToken_ = 0;
    hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    update();
  } else if (pickMode_ == PickMode::DraftFace) {
    hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
    update();
  } else if (pickMode_ == PickMode::Ruler) {
    ruler_.clearHover();
    setCursor(Qt::CrossCursor);
    update();
  }
  QOpenGLWidget::leaveEvent(event);
}
float Viewport::cameraYawDegrees() const noexcept { return yaw_; }
float Viewport::cameraPitchDegrees() const noexcept { return pitch_; }
Vector3d Viewport::cameraScreenUpDirection() const noexcept {
  const double yaw = yaw_ * std::numbers::pi / 180.0;
  const double pitch = pitch_ * std::numbers::pi / 180.0;
  return {-std::sin(yaw) * std::cos(pitch),
          -std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
}

const sketch::Sketch& Viewport::extrusionCandidateSketch() const noexcept {
  return selectedExtrusionSketch_;
}

std::size_t Viewport::selectedProfileRegionCount() const noexcept {
  return selectedExtrusionRegionSketches_.size();
}

QRectF Viewport::extrusionPreviewBaseBounds() const noexcept {
  QRectF bounds;
  for (const auto& path : selectedExtrusionPaths_)
    bounds = bounds.united(path.boundingRect());
  if (bounds.isEmpty() && !selectedExtrusionPolygon_.isEmpty())
    bounds = selectedExtrusionPolygon_.boundingRect();
  return bounds;
}

QRectF Viewport::extrusionHoverBounds() const noexcept {
  return extrusionHoverPath_.isEmpty() ? extrusionHoverPolygon_.boundingRect()
                                        : extrusionHoverPath_.boundingRect();
}

QString Viewport::extrusionCandidateSupport() const {
  return selectedExtrusionSupport_;
}

std::size_t Viewport::extrusionCandidateSketchIndex() const noexcept {
  return selectedExtrusionSketchIndex_;
}

bool Viewport::extrusionCandidateOnBodyCap() const noexcept {
  return selectedExtrusionOnBodyCap_;
}

const sketch::Sketch& Viewport::solidSketch() const noexcept {
  return solidSketch_;
}

QString Viewport::solidSupport() const { return solidSupportName_; }

QPointF Viewport::bodyPosition() const noexcept { return {offsetX_, offsetY_}; }

void Viewport::setBodyPosition(QPointF position) {
  offsetX_ = static_cast<float>(position.x());
  offsetY_ = static_cast<float>(position.y());
  update();
}

void Viewport::setWorkGridPlacement(const SketchPlacement& placement) {
  workGridPlacement_ = placement;
  update();
}

void Viewport::resetWorkGridPlacement() {
  workGridPlacement_ = SketchPlacement::xy();
  update();
}

void Viewport::setWorkGridVisible(bool visible) {
  workGridVisible_ = visible;
  update();
}

bool Viewport::workGridVisible() const noexcept { return workGridVisible_; }

void Viewport::refreshSelectedExtrusionPolygon() {
  // Screen coordinates become stale whenever a dock is opened, the viewport
  // is resized, or the camera changes.  Reproject the selected sketch from
  // model coordinates instead of moving the previously cached screen polygon.
  if (selectedExtrusionBodyFace_ &&
      (!solidSketch_.lines().empty() || !solidSketch_.circles().empty())) {
    QPolygonF polygon;
    const Point3 normal = placementNormal(solidSketchPlacement_);
    const bool initialFace = selectedExtrusionReverse_;
    const float distance = initialFace ? 0.0F
                                       : static_cast<float>(box_.heightMm);
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point point{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnPlacement(
            point, solidSketchPlacement_, offsetX_, offsetY_);
        polygon << project(translated(base, normal, distance), size(), yaw_,
                           pitch_, zoom_);
      }
    } else {
      for (const auto& line : solidSketch_.lines()) {
        const Point3 base = pointOnPlacement(
            line.start, solidSketchPlacement_, offsetX_, offsetY_);
        polygon << project(translated(base, normal, distance), size(), yaw_,
                           pitch_, zoom_);
      }
    }
    if (initialFace) std::reverse(polygon.begin(), polygon.end());
    if (polygon.size() >= 3) {
      selectedExtrusionPolygon_ = polygon;
      if (selectedExtrusionPolygons_.size() == 1)
        selectedExtrusionPolygons_.front() = polygon;
      extrusionManipulatorAnchor_ = polygon.boundingRect().center();
    }
    return;
  }
  // Selected regions are model data, not screen artefacts. Reproject every
  // region after zoom, orbit, pan or a viewport resize so all Ctrl-selected
  // contours remain coincident with their sketches.
  if (!selectedExtrusionRegionSketches_.empty()) {
    selectedExtrusionPaths_.clear();
    selectedExtrusionPolygons_.clear();
    const auto projectSelectedPoint = [&](sketch::Point point) {
      if (selectedExtrusionSketchIndex_ < displaySketches_.size())
        return project(pointOnPlacement(point,
                           displaySketches_[selectedExtrusionSketchIndex_].placement,
                           offsetX_, offsetY_), size(), yaw_, pitch_, zoom_);
      if (selectedLegacySolidFace_)
        return project(pointOnPlacement(
                           point, selectedLegacySolidFace_->placement,
                           offsetX_, offsetY_),
                       size(), yaw_, pitch_, zoom_);
      Point3 base = pointOnPlacement(
          point, selectedExtrusionPlacement_, offsetX_, offsetY_);
      if (selectedExtrusionOnBodyCap_)
        base = translated(base, placementNormal(solidSketchPlacement_),
                          static_cast<float>(box_.heightMm));
      return project(base, size(), yaw_, pitch_, zoom_);
    };
    for (const auto& regionSketch : selectedExtrusionRegionSketches_) {
      QPainterPath regionPath;
      regionPath.setFillRule(Qt::OddEvenFill);
      QPolygonF firstBoundary;
      for (const auto& boundary :
           projectedSketchBoundaries(regionSketch, projectSelectedPoint)) {
        regionPath.addPolygon(boundary);
        regionPath.closeSubpath();
        if (firstBoundary.isEmpty()) firstBoundary = boundary;
      }
      if (!regionPath.isEmpty()) {
        selectedExtrusionPaths_.push_back(regionPath);
        selectedExtrusionPolygons_.push_back(firstBoundary);
      }
    }
    if (!selectedExtrusionPaths_.empty()) {
      QRectF bounds;
      for (const auto& path : selectedExtrusionPaths_)
        bounds = bounds.united(path.boundingRect());
      selectedExtrusionPolygon_ = selectedExtrusionPolygons_.front();
      extrusionManipulatorAnchor_ = bounds.center();
      return;
    }
  }

  if (selectedExtrusionSketch_.lines().empty() &&
      selectedExtrusionSketch_.circles().empty() &&
      selectedExtrusionSketch_.arcs().empty() &&
      selectedExtrusionSketch_.beziers().empty())
    return;

  const auto projectSelectedPoint = [&](sketch::Point point) {
    if (selectedExtrusionSketchIndex_ < displaySketches_.size())
      return project(pointOnPlacement(point,
                         displaySketches_[selectedExtrusionSketchIndex_].placement,
                         offsetX_, offsetY_), size(), yaw_, pitch_, zoom_);
    if (selectedLegacySolidFace_)
      return project(pointOnPlacement(
                         point, selectedLegacySolidFace_->placement,
                         offsetX_, offsetY_),
                     size(), yaw_, pitch_, zoom_);
    Point3 base = pointOnPlacement(
        point, selectedExtrusionPlacement_, offsetX_, offsetY_);
    if (selectedExtrusionOnBodyCap_)
      base = translated(base, placementNormal(solidSketchPlacement_),
                        static_cast<float>(box_.heightMm));
    return project(base, size(), yaw_, pitch_, zoom_);
  };
  const auto boundaries =
      projectedSketchBoundaries(selectedExtrusionSketch_, projectSelectedPoint);
  if (boundaries.empty()) return;
  const QPolygonF& polygon = boundaries.front();

  if (polygon.size() >= 3) {
    const QPointF previousCenter = selectedExtrusionPolygon_.boundingRect().center();
    const QPointF newCenter = polygon.boundingRect().center();
    if (selectedExtrusionOnBodyCap_ && !selectedExtrusionPaths_.empty() &&
        !selectedExtrusionPolygon_.isEmpty()) {
      QTransform translation;
      translation.translate(newCenter.x() - previousCenter.x(),
                            newCenter.y() - previousCenter.y());
      for (auto& path : selectedExtrusionPaths_) path = translation.map(path);
      for (auto& selectedPolygon : selectedExtrusionPolygons_)
        selectedPolygon.translate(newCenter - previousCenter);
    }
    selectedExtrusionPolygon_ = polygon;
    if (selectedExtrusionPolygons_.size() == 1)
      selectedExtrusionPolygons_.front() = polygon;
    extrusionManipulatorAnchor_ = polygon.boundingRect().center();
  }
}

void Viewport::paintGL() {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const ThemeColors& theme = ThemeManager::instance().colors();
  painter.fillRect(rect(), theme.viewportBackground);

  // World-space work-plane grid. It shares the camera (including pan) with the
  // body, sketches, base planes and ViewCube, so orbit/pan/zoom move them all
  // together instead of leaving a screen-locked checkerboard behind.
  const WorldGridStyle gridStyle{theme.gridMinor, theme.gridMajor, theme.axisX,
                                 theme.axisY, theme.axisZ};
  if (workGridVisible_) {
    const ViewportCameraState gridCamera{yaw_, pitch_, zoom_, cameraPan_,
                                         size(), 1.0F, {}, 1.0};
    paintWorldGrid(painter,
                   buildWorldGrid(workGridPlacement_, gridCamera, size()),
                   gridCamera, gridStyle);
  }

  painter.save();
  painter.translate(cameraPan_);

  refreshSelectedExtrusionPolygon();
  const bool hasParametricBody = bodyShape_ && !bodyShape_->IsNull();
  const bool hasToolPreview = toolPreviewShape_ &&
                              !toolPreviewShape_->IsNull() &&
                              !toolPreviewRenderMesh_.triangles().empty();
  const bool hasToolCutPreview = toolCutPreviewShape_ &&
                                 !toolCutPreviewShape_->IsNull() &&
                                 !toolCutPreviewRenderMesh_.triangles().empty();

  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);
  const std::array<Point3, 8> vertices{{
      {-x + offsetX_, -y + offsetY_, 0}, {x + offsetX_, -y + offsetY_, 0},
      {x + offsetX_, y + offsetY_, 0}, {-x + offsetX_, y + offsetY_, 0},
      {-x + offsetX_, -y + offsetY_, z}, {x + offsetX_, -y + offsetY_, z},
      {x + offsetX_, y + offsetY_, z}, {-x + offsetX_, y + offsetY_, z}}};
  const std::array<std::array<int, 4>, 6> faces{{{{0, 1, 2, 3}}, {{4, 7, 6, 5}},
                                                  {{0, 4, 5, 1}}, {{1, 5, 6, 2}},
                                                  {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
  const std::array<QColor, 6> colors{{
      theme.viewportSurface.darker(115), theme.viewportSurfaceLight,
      theme.viewportSurface, theme.viewportSurface.lighter(120),
      theme.viewportSurfaceDark, theme.viewportSurface.lighter(108)}};

  const float planeSize = std::max(35.0F, std::max(x, y) * 1.35F);
  const std::array<std::array<Point3, 4>, 3> basePlanes{{
      {{{-planeSize, -planeSize, 0}, {planeSize, -planeSize, 0},
         {planeSize, planeSize, 0}, {-planeSize, planeSize, 0}}},
      {{{-planeSize, 0, -planeSize}, {planeSize, 0, -planeSize},
         {planeSize, 0, planeSize}, {-planeSize, 0, planeSize}}},
      {{{0, -planeSize, -planeSize}, {0, planeSize, -planeSize},
         {0, planeSize, planeSize}, {0, -planeSize, planeSize}}}}};
  const std::array<QColor, 3> planeColors{
      withAlpha(theme.axisY, 35), withAlpha(theme.axisX, 35),
      withAlpha(theme.axisZ, 35)};
  for (int plane = 0; plane < 3; ++plane) {
    if (!basePlanesVisible_[plane]) continue;
    QPolygonF polygon;
    for (const auto& point : basePlanes[plane])
      polygon << project(point, size(), yaw_, pitch_, zoom_);
    painter.setBrush(selectedBasePlane_ == plane
                         ? withAlpha(theme.accent, 60) : planeColors[plane]);
    painter.setPen(QPen(selectedBasePlane_ == plane
                            ? theme.accent : planeColors[plane].darker(125),
                        selectedBasePlane_ == plane
                            ? 3.0
                            : (pickMode_ == PickMode::SketchPlane ||
                               pickMode_ == PickMode::ImagePlane ||
                               pickMode_ == PickMode::MirrorPlane)
                                  ? 2.0
                                  : 1.0,
                        selectedBasePlane_ == plane ? Qt::SolidLine : Qt::DashLine));
    painter.drawPolygon(polygon);
    painter.drawText(polygon.boundingRect().center(),
                     plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ");
  }

  for (const auto& displayed : referenceImages_) {
    const auto& image = displayed.model;
    if (!image.visible) continue;
    const double widthMm = image.pixelWidth * 0.1 * image.scale;
    const double heightMm = image.pixelHeight * 0.1 * image.scale;
    const std::array<sketch::Point, 4> local{{
        {image.offsetXMm - widthMm * 0.5,
         image.offsetYMm + heightMm * 0.5},
        {image.offsetXMm + widthMm * 0.5,
         image.offsetYMm + heightMm * 0.5},
        {image.offsetXMm + widthMm * 0.5,
         image.offsetYMm - heightMm * 0.5},
        {image.offsetXMm - widthMm * 0.5,
         image.offsetYMm - heightMm * 0.5}}};
    QPolygonF destination;
    const Vector3d normal = image.placement.normal();
    for (const auto& point : local) {
      Point3d world = image.placement.toWorld(point.xMm, point.yMm);
      world = offsetPoint(world, normal, image.offsetZMm);
      destination << project(
          {static_cast<float>(world.x), static_cast<float>(world.y),
           static_cast<float>(world.z)},
          size(), yaw_, pitch_, zoom_);
    }
    if (displayed.pixels.isNull()) {
      painter.setBrush(withAlpha(theme.viewportSurface, 80));
      painter.setPen(QPen(theme.viewportEdge, 1.0, Qt::DashLine));
      painter.drawPolygon(destination);
      painter.drawText(destination.boundingRect(), Qt::AlignCenter,
                       QString::fromStdString(image.name));
      continue;
    }
    QPolygonF source;
    source << QPointF(0.0, 0.0)
           << QPointF(displayed.pixels.width(), 0.0)
           << QPointF(displayed.pixels.width(), displayed.pixels.height())
           << QPointF(0.0, displayed.pixels.height());
    QTransform transform;
    if (!QTransform::quadToQuad(source, destination, transform)) continue;
    painter.save();
    painter.setOpacity(0.7);
    painter.setWorldTransform(transform, true);
    painter.drawImage(QPointF(0.0, 0.0), displayed.pixels);
    painter.restore();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(withAlpha(theme.viewportEdge, 140), 1.0));
    painter.drawPolygon(destination);
  }

  if ((solidVisible_ && hasParametricBody) || hasToolPreview) {
    painter.beginNativePainting();
    const bool replaceSourcePresentation =
        hasToolPreview &&
        toolPreviewPresentation_ == ToolPreviewPresentation::ReplaceSource &&
        !toolPreviewReplacedBodyIds_.empty();
    const std::vector<std::size_t> selectedFaces =
        effectiveSelectedFaceIndices();
    const std::vector<std::size_t>& selectedEdges = selectedBodyEdgeIndices_;
    const std::vector<std::size_t> hoveredFaces =
        effectiveHoveredFaceIndices();
    const std::size_t hoveredEdge = hoveredBodyEdgeIndex_;
    std::vector<RenderMeshInstance> renderMeshes;
    renderMeshes.reserve(bodyDisplayMeshes_.size());
    for (const auto& display : bodyDisplayMeshes_) {
      if (!display.mesh) continue;
      const bool replaced =
          replaceSourcePresentation &&
          std::binary_search(toolPreviewReplacedBodyIds_.begin(),
                             toolPreviewReplacedBodyIds_.end(),
                             display.identity.bodyId);
      renderMeshes.push_back({display.mesh.get(), display.identity,
                              display.firstFace, display.firstEdge,
                              !replaced});
    }
    renderer_.render(renderMeshes,
                     hasToolPreview ? &toolPreviewRenderMesh_ : nullptr,
                     size(), static_cast<float>(devicePixelRatioF()), yaw_,
                     pitch_, zoom_, cameraPan_, displayMode_, selectedFaces,
                     hoveredFaces, selectedEdges, hoveredEdge,
                     hasToolCutPreview ? &toolCutPreviewRenderMesh_ : nullptr,
                     toolPreviewPresentationRevision_,
                     toolCutPreviewPresentationRevision_, theme);
    painter.endNativePainting();
    if (!renderer_.error().isEmpty()) {
      painter.setPen(theme.danger);
      painter.drawText(rect().adjusted(24, 24, -24, -24),
                       Qt::AlignLeft | Qt::AlignTop,
                       tr("Не удалось инициализировать 3D-ускорение OpenGL.\n%1")
                           .arg(renderer_.error()));
    }
  }

  // Legacy box fallback is only valid when there is no parametric B-Rep body
  // and no native tool preview. The OpenGL renderer above is the only B-Rep
  // body path, so the old painter implementation must not remain in paintGL.
  if (!hasParametricBody && !hasToolPreview && solidVisible_ &&
      solidSketch_.lines().empty() && solidSketch_.circles().empty()) {
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
      QPolygonF polygon;
      for (const int index : faces[faceIndex])
        polygon << project(vertices[index], size(), yaw_, pitch_, zoom_);
      if (!isFrontFacing(polygon)) continue;
      painter.setBrush(colors[faceIndex]);
      painter.setPen(QPen(static_cast<int>(faceIndex) == selectedFace_
                              ? theme.accent
                              : theme.viewportEdge,
                          static_cast<int>(faceIndex) == selectedFace_ ? 3.0
                                                                       : 1.4));
      painter.drawPolygon(polygon);
    }
  }

  // Tool presentation belongs to the active session, not to body rendering.
  // In particular, a New Body Revolve has a valid preview/manipulator before
  // the Document contains any parametric Body.
  if (toolManipulator_) {
    if (const auto layout = toolManipulatorLayout()) {
      drawToolArrow(painter, layout->anchor, layout->handle, theme.accent,
                    theme.viewportHandle);
      if (toolParameterHud_ && toolParameterHud_->isVisible()) {
        toolParameterHud_->move((layout->hudTopLeft + cameraPan_).toPoint());
      }
    }
  }
  if (translationToolManipulator_) {
    const auto layouts = translationManipulatorLayouts();
    drawTranslationGizmo(
        painter, layouts,
        std::array<QColor, 3>{theme.axisX, theme.axisY, theme.axisZ},
        theme.viewportHandle);
    if (toolParameterHud_ && toolParameterHud_->isVisible()) {
      const auto rightmost = std::max_element(
          layouts.begin(), layouts.end(), [](const auto& left, const auto& right) {
            return left.handle.x() < right.handle.x();
          });
      const QPointF hudPosition =
          rightmost->handle + cameraPan_ + QPointF(16.0, -20.0);
      toolParameterHud_->move(
          std::clamp(static_cast<int>(hudPosition.x()), 4,
                     std::max(4, width() - toolParameterHud_->width() - 4)),
          std::clamp(static_cast<int>(hudPosition.y()), 4,
                     std::max(4, height() - toolParameterHud_->height() - 4)));
    }
  }
  if (angularToolManipulator_) {
    const auto& manipulator = *angularToolManipulator_;
    if (const auto visual = angularVisual()) {
      painter.setPen(QPen(theme.viewportAngular, 2.4, Qt::DashLine));
      painter.drawLine(
          projectBodyPoint(offsetPoint(manipulator.origin, visual->axis,
                                       -manipulator.radiusMm * 1.4),
                           manipulator.origin, size(), yaw_, pitch_, zoom_)
              .screen,
          projectBodyPoint(offsetPoint(manipulator.origin, visual->axis,
                                       manipulator.radiusMm * 1.4),
                           manipulator.origin, size(), yaw_, pitch_, zoom_)
              .screen);

      const double endRadians =
          manipulator.angleDeg * std::numbers::pi / 180.0;
      const double minimumSweepRadians =
          manipulatorStyle_.minimumAngularSweepDeg * std::numbers::pi / 180.0;
      const double visualSweepRadians =
          std::max(std::abs(endRadians), minimumSweepRadians);
      const double sweepSign = endRadians < 0.0 ? -1.0 : 1.0;
      const double startRadians =
          endRadians - sweepSign * visualSweepRadians;
      const int segmentCount = std::max(
          18, static_cast<int>(std::ceil(
                  visualSweepRadians * 180.0 / std::numbers::pi / 4.0)));
      QPolygonF arc;
      arc.reserve(segmentCount + 1);
      for (int index = 0; index <= segmentCount; ++index) {
        const double t = startRadians +
                         sweepSign * visualSweepRadians * index / segmentCount;
        arc << projectBodyPoint(
                   offsetPoint(manipulator.origin, visual->u,
                               visual->visualRadiusMm * std::cos(t), visual->v,
                               visual->visualRadiusMm * std::sin(t)),
                   manipulator.origin, size(), yaw_, pitch_, zoom_)
                   .screen;
      }
      painter.setPen(QPen(theme.accent, 3.0));
      painter.drawPolyline(arc);
      if (arc.size() >= 2)
        drawToolArrowHead(painter, arc[arc.size() - 2], arc.back(),
                          theme.accent, theme.viewportHandle);
      painter.setBrush(theme.viewportHandle);
      painter.setPen(QPen(theme.accent, 2.4));
      painter.drawEllipse(arc.back(), 7.0, 7.0);
      if (toolParameterHud_ && toolParameterHud_->isVisible()) {
        const QPointF hud = arc.back() + cameraPan_ + QPointF(12.0, -20.0);
        toolParameterHud_->move(
            std::clamp(static_cast<int>(hud.x()), 4,
                       std::max(4, width() - toolParameterHud_->width() - 4)),
            std::clamp(static_cast<int>(hud.y()), 4,
                       std::max(4, height() - toolParameterHud_->height() - 4)));
      }
    }
  }

  if (solidVisible_ && !hasParametricBody && !solidSketch_.lines().empty()) {
    QPolygonF bottom;
    QPolygonF top;
    const Point3 normal = placementNormal(solidSketchPlacement_);
    for (const auto& line : solidSketch_.lines()) {
      const Point3 baseStart = pointOnPlacement(
          line.start, solidSketchPlacement_, offsetX_, offsetY_);
      const Point3 baseEnd = pointOnPlacement(
          line.end, solidSketchPlacement_, offsetX_, offsetY_);
      const Point3 topStart = translated(baseStart, normal, z);
      const Point3 topEnd = translated(baseEnd, normal, z);
      bottom.prepend(project(baseStart, size(), yaw_, pitch_, zoom_));
      top << project(topStart, size(), yaw_, pitch_, zoom_);
      QPolygonF side;
      side << project(baseStart, size(), yaw_, pitch_, zoom_)
           << project(baseEnd, size(), yaw_, pitch_, zoom_)
           << project(topEnd, size(), yaw_, pitch_, zoom_)
           << project(topStart, size(), yaw_, pitch_, zoom_);
      if (isFrontFacing(side)) {
        painter.setBrush(theme.viewportSurface);
        painter.setPen(QPen(theme.viewportEdge, 1.35));
        painter.drawPolygon(side);
      }
    }
    painter.setPen(QPen(theme.viewportEdge, 1.35));
    if (isFrontFacing(top)) {
      painter.setBrush(theme.viewportSurfaceLight);
      painter.drawPolygon(top);
    }
    if (isFrontFacing(bottom)) {
      painter.setBrush(theme.viewportSurfaceDark);
      painter.drawPolygon(bottom);
    }
  }

  if (solidVisible_ && !hasParametricBody)
    for (const auto& circle : solidSketch_.circles()) {
    QPolygonF top;
    QPolygonF bottom;
    const Point3 normal = placementNormal(solidSketchPlacement_);
    for (int step = 0; step < 64; ++step) {
      const float a = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
      const float b = 2.0F * std::numbers::pi_v<float> * (step + 1) / 64.0F;
      const auto point = [&](float angle, float height) {
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnPlacement(
            profilePoint, solidSketchPlacement_, offsetX_, offsetY_);
        return project(translated(base, normal, height), size(), yaw_, pitch_, zoom_);
      };
      QPolygonF side;
      side << point(a, 0.0F) << point(b, 0.0F) << point(b, z) << point(a, z);
      const int shade = 100 + static_cast<int>(25.0F * std::cos(a));
      if (isFrontFacing(side)) {
        painter.setBrush(theme.viewportSurface.lighter(shade));
        painter.setPen(QPen(theme.viewportEdge, 0.9));
        painter.drawPolygon(side);
      }
      top << point(a, z);
      bottom.prepend(point(a, 0.0F));
    }
    if (isFrontFacing(top)) {
      painter.setBrush(theme.viewportSurfaceLight);
      painter.setPen(QPen(theme.viewportEdge, 1.35));
      painter.drawPolygon(top);
    }
    if (isFrontFacing(bottom)) {
      painter.setBrush(theme.viewportSurfaceDark);
      painter.setPen(QPen(theme.viewportEdge, 1.35));
      painter.drawPolygon(bottom);
    }
  }

  if (solidVisible_ && !hasParametricBody)
    for (const auto& arc : solidSketch_.arcs()) {
      const Point3 normal = placementNormal(solidSketchPlacement_);
      const auto surfacePoint = [&](float angle, float height) {
        const sketch::Point profilePoint{
            arc.center.xMm + arc.radiusMm * std::cos(angle),
            arc.center.yMm + arc.radiusMm * std::sin(angle)};
        const Point3 base = pointOnPlacement(
            profilePoint, solidSketchPlacement_, offsetX_, offsetY_);
        return project(translated(base, normal, height), size(), yaw_, pitch_,
                       zoom_);
      };
      for (int step = 0; step < 64; ++step) {
        const float t0 = static_cast<float>(step) / 64.0F;
        const float t1 = static_cast<float>(step + 1) / 64.0F;
        const float a =
            static_cast<float>(arc.startAngleRad + arc.sweepAngleRad * t0);
        const float b =
            static_cast<float>(arc.startAngleRad + arc.sweepAngleRad * t1);
        QPolygonF side{surfacePoint(a, 0.0F), surfacePoint(b, 0.0F),
                       surfacePoint(b, z), surfacePoint(a, z)};
        const int shade = 100 + static_cast<int>(25.0F * std::cos(a));
        if (isFrontFacing(side)) {
          painter.setBrush(theme.viewportSurface.lighter(shade));
          painter.setPen(QPen(theme.viewportEdge, 0.9));
          painter.drawPolygon(side);
        }
      }
    }

  if (solidVisible_ && !hasParametricBody && selectedFace_ >= 0) {
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      const Point3 normal = placementNormal(solidSketchPlacement_);
      QPolygonF bottomCap;
      QPolygonF topCap;
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnPlacement(
            profilePoint, solidSketchPlacement_, offsetX_, offsetY_);
        bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
        topCap << project(translated(base, normal, z), size(), yaw_, pitch_, zoom_);
      }
      painter.setBrush(withAlpha(theme.accent, 45));
      painter.setPen(QPen(theme.accent, 3.0));
      if (selectedFace_ == 0 && isFrontFacing(bottomCap))
        painter.drawPolygon(bottomCap);
      else if (selectedFace_ == 1 && isFrontFacing(topCap))
        painter.drawPolygon(topCap);
      else if (selectedFace_ == 2) {
        for (int step = 0; step < 64; ++step) {
          const float a = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
          const float b = 2.0F * std::numbers::pi_v<float> * (step + 1) / 64.0F;
          const auto surfacePoint = [&](float angle, float height) {
            const sketch::Point profilePoint{
                circle.center.xMm + circle.radiusMm * std::cos(angle),
                circle.center.yMm + circle.radiusMm * std::sin(angle)};
            const Point3 base = pointOnPlacement(
                profilePoint, solidSketchPlacement_, offsetX_, offsetY_);
            return project(translated(base, normal, height), size(), yaw_, pitch_, zoom_);
          };
          QPolygonF side{surfacePoint(a, 0.0F), surfacePoint(b, 0.0F),
                         surfacePoint(b, z), surfacePoint(a, z)};
          if (isFrontFacing(side)) painter.drawPolygon(side);
        }
      }
    } else {
      QPolygonF selectedPolygon;
      for (int vertex : faces[static_cast<std::size_t>(selectedFace_)])
        selectedPolygon << project(vertices[static_cast<std::size_t>(vertex)],
                                   size(), yaw_, pitch_, zoom_);
      if (isFrontFacing(selectedPolygon)) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme.accent, 3.0));
        painter.drawPolygon(selectedPolygon);
      }
    }
  }

  if (solidVisible_ && selectedVertex_ >= 0 && selectedVertex_ < 8) {
    const QPointF vertex = project(vertices[static_cast<std::size_t>(selectedVertex_)],
                                   size(), yaw_, pitch_, zoom_);
    painter.setBrush(theme.viewportHandle);
    painter.setPen(QPen(theme.accent, 2.5));
    painter.drawEllipse(vertex, 6.0, 6.0);
  }

  if (sketchVisible_ && displaySketches_.empty()) {
    painter.setBrush(Qt::NoBrush);
    for (const auto& line : sketch_.lines()) {
      painter.setPen(QPen(theme.sketchCommitted, 2.2,
                          line.dashed ? Qt::DashLine : Qt::SolidLine));
      painter.drawLine(project(pointOnPlacement(line.start, sketchPlacement_,
                                                offsetX_, offsetY_),
                               size(), yaw_, pitch_, zoom_),
                       project(pointOnPlacement(line.end, sketchPlacement_,
                                                offsetX_, offsetY_),
                               size(), yaw_, pitch_, zoom_));
    }
    for (const auto& circle : sketch_.circles()) {
      painter.setPen(QPen(theme.sketchCommitted, 2.2,
                          circle.dashed ? Qt::DashLine : Qt::SolidLine));
      QPolygonF curve;
      for (int step = 0; step <= 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        curve << project(pointOnPlacement(
                             {circle.center.xMm + circle.radiusMm * std::cos(angle),
                              circle.center.yMm + circle.radiusMm * std::sin(angle)},
                             sketchPlacement_, offsetX_, offsetY_),
                         size(), yaw_, pitch_, zoom_);
      }
      painter.drawPolyline(curve);
    }
    for (const auto& arc : sketch_.arcs()) {
      painter.setPen(QPen(theme.sketchCommitted, 2.2,
                          arc.dashed ? Qt::DashLine : Qt::SolidLine));
      QPolygonF curve;
      for (int step = 0; step <= 64; ++step) {
        const float t = static_cast<float>(step) / 64.0F;
        const float angle =
            static_cast<float>(arc.startAngleRad + arc.sweepAngleRad * t);
        curve << project(pointOnPlacement(
                             {arc.center.xMm + arc.radiusMm * std::cos(angle),
                              arc.center.yMm + arc.radiusMm * std::sin(angle)},
                             sketchPlacement_, offsetX_, offsetY_),
                         size(), yaw_, pitch_, zoom_);
      }
      painter.drawPolyline(curve);
    }
    for (const auto& bezier : sketch_.beziers()) {
      painter.setPen(QPen(theme.sketchCommitted, 2.2,
                          bezier.dashed ? Qt::DashLine : Qt::SolidLine));
      QPolygonF curve;
      for (int step = 0; step <= 64; ++step) {
        const auto point = sketch::bezierPointAt(
            bezier, static_cast<double>(step) / 64.0);
        curve << project(pointOnPlacement(point, sketchPlacement_,
                                          offsetX_, offsetY_),
                         size(), yaw_, pitch_, zoom_);
      }
      painter.drawPolyline(curve);
    }
  }

  if (sketchVisible_ && !displaySketches_.empty()) {
    painter.setBrush(Qt::NoBrush);
    for (const auto& displayed : displaySketches_) {
      if (!displayed.visible) continue;
      for (const auto& line : displayed.geometry.lines()) {
        painter.setPen(QPen(theme.sketchCommitted, 2.2,
                            line.dashed ? Qt::DashLine : Qt::SolidLine));
        painter.drawLine(
            project(pointOnPlacement(line.start, displayed.placement,
                                     offsetX_, offsetY_),
                    size(), yaw_, pitch_, zoom_),
            project(pointOnPlacement(line.end, displayed.placement,
                                     offsetX_, offsetY_),
                    size(), yaw_, pitch_, zoom_));
      }
      painter.setPen(QPen(theme.sketchCommitted, 2.2));
      for (const auto& circle : displayed.geometry.circles()) {
        painter.setPen(QPen(theme.sketchCommitted, 2.2,
                            circle.dashed ? Qt::DashLine : Qt::SolidLine));
        QPolygonF curve;
        for (int step = 0; step <= 64; ++step) {
          const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
          const sketch::Point point{
              circle.center.xMm + circle.radiusMm * std::cos(angle),
              circle.center.yMm + circle.radiusMm * std::sin(angle)};
          curve << project(pointOnPlacement(point, displayed.placement,
                                            offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
        }
        painter.drawPolyline(curve);
      }
      for (const auto& arc : displayed.geometry.arcs()) {
        painter.setPen(QPen(theme.sketchCommitted, 2.2,
                            arc.dashed ? Qt::DashLine : Qt::SolidLine));
        QPolygonF curve;
        for (int step = 0; step <= 64; ++step) {
          const float t = static_cast<float>(step) / 64.0F;
          const float angle =
              static_cast<float>(arc.startAngleRad + arc.sweepAngleRad * t);
          const sketch::Point point{
              arc.center.xMm + arc.radiusMm * std::cos(angle),
              arc.center.yMm + arc.radiusMm * std::sin(angle)};
          curve << project(pointOnPlacement(point, displayed.placement,
                                            offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
        }
        painter.drawPolyline(curve);
      }
      for (const auto& bezier : displayed.geometry.beziers()) {
        painter.setPen(QPen(theme.sketchCommitted, 2.2,
                            bezier.dashed ? Qt::DashLine : Qt::SolidLine));
        QPolygonF curve;
        for (int step = 0; step <= 64; ++step) {
          const auto point = sketch::bezierPointAt(
              bezier, static_cast<double>(step) / 64.0);
          curve << project(pointOnPlacement(point, displayed.placement,
                                            offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
        }
        painter.drawPolyline(curve);
      }
    }
  }

  if (pickMode_ == PickMode::RevolveAxis ||
      pickMode_ == PickMode::LinearPatternAxis ||
      pickMode_ == PickMode::LinearPatternPreview ||
      pickMode_ == PickMode::CircularPatternAxis ||
      pickMode_ == PickMode::CircularPatternPreview ||
      pickMode_ == PickMode::DraftAxis ||
      pickMode_ == PickMode::DraftPreview) {
    const bool hasSketchCandidate =
        pickMode_ == PickMode::RevolveAxis &&
        revolveAxisSketchIndex_ < displaySketches_.size();
    qulonglong emphasizedAxisToken = hoveredRevolveAxisToken_;
    if (pickMode_ == PickMode::LinearPatternPreview ||
        pickMode_ == PickMode::CircularPatternPreview ||
        pickMode_ == PickMode::DraftPreview) {
      emphasizedAxisToken = selectedPatternAxis_ == 0
                                ? kGlobalXAxisToken
                            : selectedPatternAxis_ == 1
                                ? kGlobalYAxisToken
                            : selectedPatternAxis_ == 2
                                ? kGlobalZAxisToken
                                : 0;
    }
    const Point3d center = displayedBodyCenter();
    painter.setBrush(Qt::NoBrush);
    const auto globalScreenPoint = [&](double x, double y, double z) {
      return projectBodyPoint({x, y, z}, center, size(), yaw_, pitch_, zoom_).screen;
    };
    painter.setPen(QPen(theme.axisX, 3.0));
    painter.drawLine(globalScreenPoint(-1000.0, 0.0, 0.0),
                     globalScreenPoint(1000.0, 0.0, 0.0));
    painter.setPen(QPen(theme.axisY, 3.0));
    painter.drawLine(globalScreenPoint(0.0, -1000.0, 0.0),
                     globalScreenPoint(0.0, 1000.0, 0.0));
    painter.setPen(QPen(theme.axisZ, 3.0));
    painter.drawLine(globalScreenPoint(0.0, 0.0, -1000.0),
                     globalScreenPoint(0.0, 0.0, 1000.0));
    if (hasSketchCandidate) {
      const auto& placement =
          displaySketches_[revolveAxisSketchIndex_].placement;
      const auto screenPoint = [&](double x, double y) {
        return projectBodyPoint(placement.toWorld(x, y), center, size(), yaw_,
                                pitch_, zoom_)
            .screen;
      };
      painter.setPen(QPen(theme.viewportAngular, 2.4, Qt::DashLine));
      painter.drawLine(screenPoint(-1000.0, 0.0),
                       screenPoint(1000.0, 0.0));
      painter.setPen(QPen(theme.axisX, 2.4, Qt::DashLine));
      painter.drawLine(screenPoint(0.0, -1000.0),
                       screenPoint(0.0, 1000.0));
    }
    // Axis hover/selection uses the same cyan language as native 3D hover.
    // A candidate is committed only on click; the pattern keeps it highlighted
    // while its parameters are being edited.
    if (emphasizedAxisToken != 0) {
      painter.setPen(QPen(theme.viewportHover, 5.0, Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
      if (emphasizedAxisToken == kGlobalXAxisToken) {
        painter.drawLine(globalScreenPoint(-1000.0, 0.0, 0.0),
                         globalScreenPoint(1000.0, 0.0, 0.0));
      } else if (emphasizedAxisToken == kGlobalYAxisToken) {
        painter.drawLine(globalScreenPoint(0.0, -1000.0, 0.0),
                         globalScreenPoint(0.0, 1000.0, 0.0));
      } else if (emphasizedAxisToken == kGlobalZAxisToken) {
        painter.drawLine(globalScreenPoint(0.0, 0.0, -1000.0),
                         globalScreenPoint(0.0, 0.0, 1000.0));
      } else if (hasSketchCandidate) {
        const auto& hoveredCandidate =
            displaySketches_[revolveAxisSketchIndex_];
        const auto& placement = hoveredCandidate.placement;
        const auto screenPoint = [&](double x, double y) {
          return projectBodyPoint(placement.toWorld(x, y), center, size(), yaw_,
                                  pitch_, zoom_)
              .screen;
        };
        if (emphasizedAxisToken == 1) {
          painter.drawLine(screenPoint(-1000.0, 0.0),
                           screenPoint(1000.0, 0.0));
        } else if (emphasizedAxisToken == 2) {
          painter.drawLine(screenPoint(0.0, -1000.0),
                           screenPoint(0.0, 1000.0));
        } else {
          for (std::size_t i = 0;
               i < hoveredCandidate.geometry.lines().size(); ++i) {
            if (static_cast<qulonglong>(
                    hoveredCandidate.geometry.lineId(i)) + 3 !=
                emphasizedAxisToken)
              continue;
            const auto& line = hoveredCandidate.geometry.lines()[i];
            painter.drawLine(screenPoint(line.start.xMm, line.start.yMm),
                             screenPoint(line.end.xMm, line.end.yMm));
            break;
          }
        }
      }
    }
    if (pickMode_ == PickMode::DraftPreview && selectedDraftAxisEdge_ &&
        selectedDraftAxisEdge_->signature) {
      const auto& signature = *selectedDraftAxisEdge_->signature;
      const double tangentLength = std::sqrt(
          signature.tangent.x * signature.tangent.x +
          signature.tangent.y * signature.tangent.y +
          signature.tangent.z * signature.tangent.z);
      if (tangentLength > 1e-12) {
        const double scale = 1000.0 / tangentLength;
        const Point3d first{
            signature.midpoint.x - signature.tangent.x * scale,
            signature.midpoint.y - signature.tangent.y * scale,
            signature.midpoint.z - signature.tangent.z * scale};
        const Point3d second{
            signature.midpoint.x + signature.tangent.x * scale,
            signature.midpoint.y + signature.tangent.y * scale,
            signature.midpoint.z + signature.tangent.z * scale};
        painter.setPen(QPen(theme.viewportHover, 5.0, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(projectBodyPoint(first, center, size(), yaw_, pitch_,
                                          zoom_)
                             .screen,
                         projectBodyPoint(second, center, size(), yaw_, pitch_,
                                          zoom_)
                             .screen);
      }
    }
  }

  if ((pickMode_ == PickMode::ExtrusionSurface ||
       pickMode_ == PickMode::RevolveAxis) &&
      !selectedExtrusionPaths_.empty()) {
    painter.setBrush(withAlpha(theme.accent, 38));
    painter.setPen(QPen(theme.accent, 2.6));
    for (const auto& selectedPath : selectedExtrusionPaths_)
      painter.drawPath(selectedPath);
  }

  if ((pickMode_ == PickMode::ExtrusionSurface ||
       pickMode_ == PickMode::SketchPlane ||
       pickMode_ == PickMode::ImagePlane ||
       (pickMode_ == PickMode::RevolveAxis &&
        hoveredRevolveAxisToken_ == 0)) &&
      !extrusionHoverPolygon_.isEmpty()) {
    painter.setBrush(withAlpha(theme.viewportHover, 48));
    painter.setPen(QPen(theme.viewportHover, 2.2));
    if (!extrusionHoverPath_.isEmpty())
      painter.drawPath(extrusionHoverPath_);
    else
      painter.drawPolygon(extrusionHoverPolygon_);
  }

  if (extrusionManipulatorVisible_ && !legacyExtrusionPreviewSuppressed()) {
    const QPointF offset = extrusionScreenOffset(extrusionPreviewLengthMm_);
    const QPointF tip = extrusionManipulatorAnchor_ + offset;
    const QColor previewColor = extrusionPreviewLengthMm_ >= 0.0
                                    ? withAlpha(theme.previewPositive, 122)
                                    : withAlpha(theme.previewNegative, 112);
    const QColor previewEdge = extrusionPreviewLengthMm_ >= 0.0
                                   ? theme.previewPositive
                                   : theme.previewNegative;
    const auto previewPolygons = selectedExtrusionPolygons_.empty()
                                     ? std::vector<QPolygonF>{selectedExtrusionPolygon_}
                                     : selectedExtrusionPolygons_;
    // The selected regions are cached in screen coordinates while the
    // manipulator anchor is reprojected from model coordinates on every
    // paint.  Opening the properties dock or changing the camera used to
    // update only the anchor, leaving the preview displaced.  Apply one
    // common correction to all regions so their collective centre remains
    // attached to the manipulator base without collapsing multi-selection.
    QRectF cachedRegionsBounds;
    for (const auto& path : selectedExtrusionPaths_)
      cachedRegionsBounds = cachedRegionsBounds.united(path.boundingRect());
    const QPointF previewAlignment = cachedRegionsBounds.isEmpty()
                                         ? QPointF{}
                                         : extrusionManipulatorAnchor_ -
                                               cachedRegionsBounds.center();
    if (!qFuzzyIsNull(extrusionPreviewLengthMm_)) {
      for (std::size_t regionIndex = 0; regionIndex < previewPolygons.size();
           ++regionIndex) {
        const auto& polygon = previewPolygons[regionIndex];
        if (polygon.size() < 3) continue;
        QPainterPath basePath;
        if (regionIndex < selectedExtrusionPaths_.size())
          basePath = selectedExtrusionPaths_[regionIndex];
        else {
          basePath.addPolygon(polygon);
          basePath.closeSubpath();
        }
        if (!previewAlignment.isNull()) {
          QTransform alignment;
          alignment.translate(previewAlignment.x(), previewAlignment.y());
          basePath = alignment.map(basePath);
        }
        QTransform translation;
        translation.translate(offset.x(), offset.y());
        const QPainterPath capPath = translation.map(basePath);
        std::vector<QPolygonF> sideFaces;
        for (const auto& boundary : basePath.toSubpathPolygons()) {
          const QPolygonF cap = boundary.translated(offset);
          for (qsizetype index = 0; index < boundary.size(); ++index) {
            const qsizetype next = (index + 1) % boundary.size();
            QPolygonF side;
            side << boundary[index] << boundary[next] << cap[next] << cap[index];
            sideFaces.push_back(side);
          }
        }
        QLinearGradient bodyGradient(basePath.boundingRect().center(),
                                     capPath.boundingRect().center());
        const QColor bodyStart = extrusionPreviewLengthMm_ >= 0.0
                                     ? withAlpha(theme.previewPositive.lighter(125), 150)
                                     : withAlpha(theme.previewNegative.lighter(125), 140);
        const QColor bodyEnd = extrusionPreviewLengthMm_ >= 0.0
                                   ? withAlpha(theme.previewPositive.darker(112), 105)
                                   : withAlpha(theme.previewNegative.darker(112), 100);
        bodyGradient.setColorAt(0.0, bodyStart);
        bodyGradient.setColorAt(0.55, previewColor);
        bodyGradient.setColorAt(1.0, bodyEnd);
        const sketch::Sketch& regionGeometry =
            regionIndex < selectedExtrusionRegionSketches_.size()
                ? selectedExtrusionRegionSketches_[regionIndex]
                : selectedExtrusionSketch_;
        const bool curvedBoundary = !regionGeometry.circles().empty() ||
                                    !regionGeometry.arcs().empty() ||
                                    !regionGeometry.beziers().empty();
        QColor sideEdge = previewEdge;
        sideEdge.setAlpha(165);
        painter.setPen(curvedBoundary ? Qt::NoPen
                                     : QPen(sideEdge, 1.35));
        painter.setBrush(bodyGradient);
        // Draw faces independently without a pen. A single winding path can
        // cancel adjacent quads with opposite winding, which made the body
        // disappear and left only two caps connected by thin lines.
        for (const auto& side : sideFaces) painter.drawPolygon(side);

        painter.setPen(QPen(withAlpha(previewEdge, 115), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(basePath);
        painter.setPen(QPen(previewEdge, 2.4));
        painter.setBrush(extrusionPreviewLengthMm_ >= 0.0
                             ? withAlpha(theme.previewPositive.lighter(150), 150)
                             : withAlpha(theme.previewNegative.lighter(150), 145));
        painter.drawPath(capPath);

        // Polygonal profiles expose every longitudinal edge; curved profiles
        // retain two silhouette generators so circles remain visually clean.
        for (const QLineF& generator :
             extrusionPreviewGenerators(basePath, offset, curvedBoundary))
          painter.drawLine(generator);
      }
    }
    drawToolArrow(painter, extrusionManipulatorAnchor_, tip, previewEdge,
                  theme.viewportHandle);
  }

  if (originVisible_) {
    const QPointF origin = project({0, 0, 0}, size(), yaw_, pitch_, zoom_);
    const QPointF xEnd = project({18, 0, 0}, size(), yaw_, pitch_, zoom_);
    const QPointF yEnd = project({0, 18, 0}, size(), yaw_, pitch_, zoom_);
    const QPointF zEnd = project({0, 0, 18}, size(), yaw_, pitch_, zoom_);
    const auto drawAxis = [&](QPointF end, const QColor& color,
                              const QString& label) {
      // Origin trihedron is an interaction-neutral screen overlay. A
      // theme-derived halo keeps it readable over both light and dark solid
      // faces without disabling depth for any selectable model presentation.
      QColor halo = theme.viewportBackground;
      halo.setAlpha(220);
      painter.setPen(QPen(halo, 5.0, Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(origin, end);
      painter.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(origin, end);
      const QPointF labelPosition = end + QPointF(5, 3);
      const QRectF labelBackground(labelPosition + QPointF(-2, -13),
                                   QSizeF(15, 17));
      painter.fillRect(labelBackground, halo);
      painter.setPen(color);
      painter.drawText(labelPosition, label);
    };
    drawAxis(xEnd, theme.axisX, QStringLiteral("X"));
    drawAxis(yEnd, theme.axisY, QStringLiteral("Y"));
    drawAxis(zEnd, theme.axisZ, QStringLiteral("Z"));
    painter.setBrush(selectedOrigin_ ? theme.accentSoft : theme.viewportHandle);
    painter.setPen(QPen(selectedOrigin_ ? theme.accent : theme.axisZ,
                        selectedOrigin_ ? 3.0 : 1.5));
    painter.drawEllipse(origin, 4.0, 4.0);
  }

  painter.restore();

  const ViewportCameraState rulerCamera{
      yaw_, pitch_, zoom_, cameraPan_, size(), 1.0F, displayedBodyCenter(),
      std::max(1.0, displayedBodyDiagonal() * 3.0)};
  ruler_.paint(painter, rulerCamera, theme);

  paintViewCube(painter, viewCubeGeometry(size(), {yaw_,pitch_}),
                {yaw_,pitch_}, cubeHover_, cubePressed_,
                ViewCubeStyle{theme.cubeTop, theme.cubeFront, theme.cubeSide,
                              theme.cubeOutline, theme.cubeText, theme.cubeBevel,
                              theme.cubeHover, theme.cubePressed, theme.cubeActive,
                              theme.cubeAccent, theme.cubeShadow});

  // Rectangle marquee overlay. Drawn after restore + ViewCube so it stays
  // screen-space and topmost. Matches the Sketcher selection-box style.
  if (marqueeActive_) {
    const QRectF marqueeRect(marqueeStart_ + cameraPan_,
                             marqueeCurrent_ + cameraPan_);
    painter.setPen(QPen(theme.sketchSelected, 1.4, Qt::DashLine));
    painter.setBrush(withAlpha(theme.sketchSelected, 32));
    painter.drawRect(marqueeRect.normalized());
  }

  painter.setPen(theme.textSecondary);
  painter.drawText(16, height() - 18, "Drag to orbit  •  Wheel to zoom");
}

qulonglong Viewport::revolveAxisTokenAt(QPointF scenePosition) const {
  if (pickMode_ != PickMode::RevolveAxis) return 0;
  const Point3d center = displayedBodyCenter();
  double bestDistance = 12.0;
  qulonglong bestToken = 0;

  const auto considerSegment = [&](const Point3d& aWorld,
                                   const Point3d& bWorld,
                                   qulonglong token) {
    const QPointF a =
        projectBodyPoint(aWorld, center, size(), yaw_, pitch_, zoom_).screen;
    const QPointF b =
        projectBodyPoint(bWorld, center, size(), yaw_, pitch_, zoom_).screen;
    const QPointF ab = b - a;
    const double length2 = QPointF::dotProduct(ab, ab);
    const double t =
        length2 > 1e-9
            ? std::clamp(QPointF::dotProduct(scenePosition - a, ab) / length2,
                         0.0, 1.0)
            : 0.0;
    const double distance = QLineF(scenePosition, a + ab * t).length();
    if (distance < bestDistance) {
      bestDistance = distance;
      bestToken = token;
    }
  };

  considerSegment({-1000.0, 0.0, 0.0}, {1000.0, 0.0, 0.0},
                  kGlobalXAxisToken);
  considerSegment({0.0, -1000.0, 0.0}, {0.0, 1000.0, 0.0},
                  kGlobalYAxisToken);
  considerSegment({0.0, 0.0, -1000.0}, {0.0, 0.0, 1000.0},
                  kGlobalZAxisToken);

  if (revolveAxisSketchIndex_ >= displaySketches_.size()) return bestToken;
  const auto& candidate = displaySketches_[revolveAxisSketchIndex_];

  considerSegment(candidate.placement.toWorld(-1000.0, 0.0),
                  candidate.placement.toWorld(1000.0, 0.0), 1);
  considerSegment(candidate.placement.toWorld(0.0, -1000.0),
                  candidate.placement.toWorld(0.0, 1000.0), 2);

  for (std::size_t i = 0; i < candidate.geometry.lines().size(); ++i) {
    const auto& line = candidate.geometry.lines()[i];
    considerSegment(
        candidate.placement.toWorld(line.start.xMm, line.start.yMm),
        candidate.placement.toWorld(line.end.xMm, line.end.yMm),
        static_cast<qulonglong>(candidate.geometry.lineId(i)) + 3);
  }

  return bestToken;
}

qulonglong Viewport::principalAxisTokenAt(QPointF scenePosition) const {
  if (pickMode_ != PickMode::LinearPatternAxis &&
      pickMode_ != PickMode::CircularPatternAxis &&
      pickMode_ != PickMode::DraftAxis)
    return 0;
  const Point3d center = displayedBodyCenter();
  double bestDistance = 12.0;
  qulonglong bestToken = 0;
  const auto considerSegment = [&](const Point3d& aWorld,
                                   const Point3d& bWorld,
                                   qulonglong token) {
    const QPointF a =
        projectBodyPoint(aWorld, center, size(), yaw_, pitch_, zoom_).screen;
    const QPointF b =
        projectBodyPoint(bWorld, center, size(), yaw_, pitch_, zoom_).screen;
    const QPointF ab = b - a;
    const double length2 = QPointF::dotProduct(ab, ab);
    const double t =
        length2 > 1e-9
            ? std::clamp(QPointF::dotProduct(scenePosition - a, ab) / length2,
                         0.0, 1.0)
            : 0.0;
    const double distance = QLineF(scenePosition, a + ab * t).length();
    if (distance < bestDistance) {
      bestDistance = distance;
      bestToken = token;
    }
  };
  considerSegment({-1000.0, 0.0, 0.0}, {1000.0, 0.0, 0.0},
                  kGlobalXAxisToken);
  considerSegment({0.0, -1000.0, 0.0}, {0.0, 1000.0, 0.0},
                  kGlobalYAxisToken);
  considerSegment({0.0, 0.0, -1000.0}, {0.0, 0.0, 1000.0},
                  kGlobalZAxisToken);
  return bestToken;
}

void Viewport::mousePressEvent(QMouseEvent* event) {
  setFocus(Qt::MouseFocusReason);
  flushPendingHover(event->position());
  lastMousePosition_ = event->position().toPoint();
  draggingBody_ = false;
  bodyDragStart_ = {offsetX_, offsetY_};
  if (event->button() == Qt::LeftButton) {
    cubePressed_ = viewCubeGeometry(size(), {yaw_,pitch_}).hitTest(event->position());
    if (cubePressed_) {
      orientationAnimation_->stop();
      update();
      event->accept();
      return;
    }
  }
  orientationAnimation_->stop();
  clearCubeHover();
  if (event->button() == Qt::MiddleButton) {
    panningView_ = true;
    setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  if (pickMode_ == PickMode::Ruler) {
    static_cast<void>(ruler_.updateHover(
        pickingScene(), event->position() - cameraPan_, true));
    const RulerClickResult result = ruler_.commitHoveredPoint();
    if (result == RulerClickResult::FirstPoint ||
        result == RulerClickResult::Restarted)
      emit rulerPointPicked(1);
    if (result == RulerClickResult::Completed) {
      emit rulerPointPicked(2);
      if (const auto distance = ruler_.measuredDistanceMm())
        emit rulerMeasurementChanged(*distance);
    }
    event->accept();
    update();
    return;
  }
  const QPointF scenePosition = event->position() - cameraPan_;
  if (translationToolManipulator_) {
    const auto layouts = translationManipulatorLayouts();
    for (int axis = 0; axis < 3; ++axis) {
      if (QLineF(scenePosition, layouts[axis].handle).length() > 18.0)
        continue;
      const auto& manipulator = *translationToolManipulator_;
      const QPointF start = projectBodyPoint(
          manipulator.origin, manipulator.origin, size(), yaw_, pitch_, zoom_)
                                .screen;
      const QPointF unitEnd = projectBodyPoint(
          offsetPoint(manipulator.origin, manipulator.axes[axis], 1.0),
          manipulator.origin, size(), yaw_, pitch_, zoom_)
                                  .screen;
      const QPointF dragAxis = robustLinearDragAxis(
          unitEnd - start, layouts[axis].direction,
          manipulatorStyle_.nearEndOnThresholdPx);
      const std::array<double, 3> values{{manipulator.offsetMm.x,
                                          manipulator.offsetMm.y,
                                          manipulator.offsetMm.z}};
      activeTranslationAxis_ = axis;
      linearDragSnapshot_ = {scenePosition, dragAxis, values[axis]};
      draggingTranslationToolManipulator_ = true;
      setCursor(Qt::SizeAllCursor);
      event->accept();
      return;
    }
  }
  if (toolManipulator_) {
    const Point3d center = toolManipulator_->origin;
    const QPointF start = projectBodyPoint(toolManipulator_->origin, center,
                                           size(), yaw_, pitch_, zoom_).screen;
    const auto layout = toolManipulatorLayout();
    if (layout && QLineF(scenePosition, layout->handle).length() <= 18.0) {
      draggingToolManipulator_ = true;
      const Point3d unitWorld{
          toolManipulator_->origin.x + toolManipulator_->direction.x,
          toolManipulator_->origin.y + toolManipulator_->direction.y,
          toolManipulator_->origin.z + toolManipulator_->direction.z};
      QPointF projectedUnitAxis =
          projectBodyPoint(unitWorld, center, size(), yaw_, pitch_, zoom_).screen -
          start;
      if (layout->visualSign < 0.0 && !toolManipulator_->directional)
        projectedUnitAxis = -projectedUnitAxis;
      // The drag axis must share the drawn arrow's stable direction. When the
      // true 1 mm projection is end-on, fall back to that direction with
      // bounded gain so the value follows the drawn arrow monotonically.
      const QPointF dragAxis = robustLinearDragAxis(
          projectedUnitAxis, layout->direction * layout->visualSign,
          manipulatorStyle_.nearEndOnThresholdPx);
      linearDragSnapshot_ = {scenePosition, dragAxis,
                             toolManipulator_->valueMm};
      setCursor(Qt::SizeAllCursor);
      return;
    }
  }
  if (pickMode_ == PickMode::MirrorBody) {
    updateToolBodyHover(scenePosition);
    if (hoveredToolBodyId_ != kInvalidBodyId) {
      const BodyId pickedBody = hoveredToolBodyId_;
      pickMode_ = PickMode::None;
      selectionFilter_ = SelectionFilter::Any;
      hoveredToolBodyId_ = kInvalidBodyId;
      hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
      setSelectedBodies({pickedBody});
      emit mirrorBodyPicked(pickedBody);
    }
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::MoveBody) {
    updateToolBodyHover(scenePosition);
    if (hoveredToolBodyId_ != kInvalidBodyId) {
      const BodyId pickedBody = hoveredToolBodyId_;
      pickMode_ = PickMode::MovePreview;
      selectionFilter_ = SelectionFilter::Any;
      hoveredToolBodyId_ = kInvalidBodyId;
      hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
      setSelectedBodies({pickedBody});
      emit moveBodyPicked(pickedBody);
    }
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::JoinBodies) {
    updateToolBodyHover(scenePosition);
    if (hoveredToolBodyId_ != kInvalidBodyId) {
      auto selected = selectedBodyIds_;
      const auto found =
          std::find(selected.begin(), selected.end(), hoveredToolBodyId_);
      if (found != selected.end())
        selected.erase(found);
      else if (selected.size() < 2)
        selected.push_back(hoveredToolBodyId_);
      setSelectedBodies(std::move(selected));
    }
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::LinearPatternBody) {
    updateToolBodyHover(scenePosition);
    if (hoveredToolBodyId_ != kInvalidBodyId) {
      const BodyId pickedBody = hoveredToolBodyId_;
      pickMode_ = PickMode::None;
      selectionFilter_ = SelectionFilter::Any;
      hoveredToolBodyId_ = kInvalidBodyId;
      hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
      setSelectedBodies({pickedBody});
      emit linearPatternBodyPicked(pickedBody);
    }
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::CircularPatternBody) {
    updateToolBodyHover(scenePosition);
    if (hoveredToolBodyId_ != kInvalidBodyId) {
      const BodyId pickedBody = hoveredToolBodyId_;
      pickMode_ = PickMode::None;
      selectionFilter_ = SelectionFilter::Any;
      hoveredToolBodyId_ = kInvalidBodyId;
      hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
      setSelectedBodies({pickedBody});
      emit circularPatternBodyPicked(pickedBody);
    }
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::MirrorPlane) {
    updateMirrorPlaneHover(scenePosition);
    if (selectedBasePlane_ >= 0) {
      const int pickedPlane = selectedBasePlane_;
      pickMode_ = PickMode::MirrorPreview;
      selectionFilter_ = SelectionFilter::Any;
      unsetCursor();
      emit mirrorPlanePicked(pickedPlane);
    }
    event->accept();
    update();
    return;
  }
  if (pickMode_ == PickMode::MirrorPreview) {
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::LinearPatternAxis) {
    const qulonglong axisToken = principalAxisTokenAt(scenePosition);
    int axisIndex = -1;
    if (axisToken == kGlobalXAxisToken)
      axisIndex = 0;
    else if (axisToken == kGlobalYAxisToken)
      axisIndex = 1;
    else if (axisToken == kGlobalZAxisToken)
      axisIndex = 2;
    if (axisIndex >= 0) {
      selectedPatternAxis_ = axisIndex;
      hoveredRevolveAxisToken_ = 0;
      pickMode_ = PickMode::LinearPatternPreview;
      selectionFilter_ = SelectionFilter::Any;
      unsetCursor();
      emit linearPatternAxisPicked(axisIndex);
    }
    event->accept();
    update();
    return;
  }
  if (pickMode_ == PickMode::LinearPatternPreview) {
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::CircularPatternAxis) {
    const qulonglong axisToken = principalAxisTokenAt(scenePosition);
    int axisIndex = -1;
    if (axisToken == kGlobalXAxisToken)
      axisIndex = 0;
    else if (axisToken == kGlobalYAxisToken)
      axisIndex = 1;
    else if (axisToken == kGlobalZAxisToken)
      axisIndex = 2;
    if (axisIndex >= 0) {
      selectedPatternAxis_ = axisIndex;
      hoveredRevolveAxisToken_ = 0;
      pickMode_ = PickMode::CircularPatternPreview;
      selectionFilter_ = SelectionFilter::Any;
      unsetCursor();
      emit circularPatternAxisPicked(axisIndex);
    }
    event->accept();
    update();
    return;
  }
  if (pickMode_ == PickMode::DraftAxis) {
    const qulonglong axisToken = principalAxisTokenAt(scenePosition);
    int axisIndex = -1;
    if (axisToken == kGlobalXAxisToken)
      axisIndex = 0;
    else if (axisToken == kGlobalYAxisToken)
      axisIndex = 1;
    else if (axisToken == kGlobalZAxisToken)
      axisIndex = 2;
    if (axisIndex >= 0) {
      selectedPatternAxis_ = axisIndex;
      hoveredRevolveAxisToken_ = 0;
      pickMode_ = PickMode::DraftPreview;
      selectionFilter_ = SelectionFilter::Any;
      unsetCursor();
      emit draftAxisPicked(axisIndex);
    } else {
      updateBodyHover(scenePosition);
      if (hoveredBodyEdgeIndex_ != static_cast<std::size_t>(-1)) {
        if (const auto edge =
                edgeReferenceForGlobalIndex(hoveredBodyEdgeIndex_))
          emit draftEdgeAxisPicked(*edge);
      }
    }
    event->accept();
    update();
    return;
  }
  if (pickMode_ == PickMode::RevolveAxis) {
    const qulonglong axisToken = revolveAxisTokenAt(scenePosition);
    if (axisToken != 0) {
      hoveredRevolveAxisToken_ = 0;
      extrusionHoverPolygon_.clear();
      extrusionHoverPath_ = {};
      pickMode_ = PickMode::None;
      unsetCursor();
      emit revolveAxisPicked(axisToken);
      return;
    }
  }
  if (angularToolManipulator_) {
    const auto& manipulator = *angularToolManipulator_;
    if (const auto visual = angularVisual()) {
      const double angle = manipulator.angleDeg * std::numbers::pi / 180.0;
      const QPointF handle = projectBodyPoint(
          offsetPoint(manipulator.origin, visual->u,
                      visual->visualRadiusMm * std::cos(angle), visual->v,
                      visual->visualRadiusMm * std::sin(angle)),
          manipulator.origin, size(), yaw_, pitch_, zoom_).screen;
      if (QLineF(scenePosition, handle).length() <= 18.0) {
        draggingAngularToolManipulator_ = true;
        setCursor(Qt::SizeAllCursor);
        return;
      }
    }
  }
  if (pickMode_ == PickMode::CircularPatternPreview) {
    event->accept();
    return;
  }
  if (pickMode_ == PickMode::MovePreview) {
    event->accept();
    return;
  }
  if (extrusionManipulatorVisible_) {
    const QPointF handle = extrusionManipulatorAnchor_ +
                           extrusionScreenOffset(extrusionPreviewLengthMm_);
    if (QLineF(scenePosition, handle).length() <= 18.0) {
      draggingExtrusionHandle_ = true;
      setCursor(Qt::SizeVerCursor);
      return;
    }
  }
  if (pickMode_ == PickMode::ExtrusionSurface ||
      pickMode_ == PickMode::RevolveAxis) {
    updateExtrusionHover(scenePosition);
    if (pickMode_ == PickMode::RevolveAxis &&
        hoveredExtrusionSketchIndex_ == static_cast<std::size_t>(-1)) {
      extrusionHoverPolygon_.clear();
      extrusionHoverPath_ = {};
    }
    update();
  }
  const bool revolveProfilePick = pickMode_ == PickMode::RevolveAxis;
  if ((pickMode_ == PickMode::ExtrusionSurface || revolveProfilePick) &&
      !extrusionHoverPolygon_.isEmpty()) {
    const bool append = event->modifiers().testFlag(Qt::ControlModifier);
    if (!append || selectedExtrusionSketchIndex_ != hoveredExtrusionSketchIndex_) {
      selectedExtrusionPolygons_.clear();
      selectedExtrusionPaths_.clear();
      selectedExtrusionRegionSketches_.clear();
    }

    QPainterPath hoveredPath = extrusionHoverPath_;
    if (hoveredPath.isEmpty()) {
      hoveredPath.addPolygon(extrusionHoverPolygon_);
      hoveredPath.closeSubpath();
    }
    const QRectF hoveredBounds = hoveredPath.boundingRect();
    const auto sameRegion = [&hoveredBounds](const QPainterPath& path) {
      const QRectF bounds = path.boundingRect();
      return QLineF(bounds.center(), hoveredBounds.center()).length() < 1.0 &&
             std::abs(bounds.width() - hoveredBounds.width()) < 1.0 &&
             std::abs(bounds.height() - hoveredBounds.height()) < 1.0;
    };
    const auto existing = std::find_if(selectedExtrusionPaths_.begin(),
                                       selectedExtrusionPaths_.end(), sameRegion);
    if (append && existing != selectedExtrusionPaths_.end()) {
      const auto index = static_cast<std::size_t>(
          std::distance(selectedExtrusionPaths_.begin(), existing));
      selectedExtrusionPaths_.erase(existing);
      if (index < selectedExtrusionPolygons_.size())
        selectedExtrusionPolygons_.erase(selectedExtrusionPolygons_.begin() +
                                         static_cast<std::ptrdiff_t>(index));
      if (index < selectedExtrusionRegionSketches_.size())
        selectedExtrusionRegionSketches_.erase(
            selectedExtrusionRegionSketches_.begin() +
            static_cast<std::ptrdiff_t>(index));
    } else {
      selectedExtrusionPaths_.push_back(hoveredPath);
      selectedExtrusionPolygons_.push_back(extrusionHoverPolygon_);
      selectedExtrusionRegionSketches_.push_back(hoveredExtrusionSketch_);
    }
    selectedExtrusionSupport_ = hoveredExtrusionSupport_;
    selectedExtrusionPlacement_ = hoveredExtrusionPlacement_;
    selectedExtrusionSketchIndex_ = hoveredExtrusionSketchIndex_;
    selectedExtrusionOnBodyCap_ = hoveredExtrusionOnBodyCap_;
    selectedExtrusionReverse_ = hoveredExtrusionReverse_;
    selectedLegacySolidFace_ = hoveredLegacySolidFace_;
    const auto pickedFace =
        hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1)
            ? faceReferenceForGlobalIndex(hoveredBodyFaceIndex_)
            : std::nullopt;
    selectedExtrusionBodyFace_ = pickedFace.has_value();
    rebuildSelectedExtrusionSketch();
    const auto sourcePick = [&]() -> std::optional<ExtrusionSourcePick> {
      if (pickedFace)
        return ExtrusionSourcePick{BodyFacePick{*pickedFace},
                                   hoveredExtrusionSurface_};
      if (selectedLegacySolidFace_)
        return ExtrusionSourcePick{*selectedLegacySolidFace_,
                                   hoveredExtrusionSurface_};
      if (selectedExtrusionSketchIndex_ >= displaySketches_.size())
        return std::nullopt;
      const auto& displayed = displaySketches_[selectedExtrusionSketchIndex_];
      if (displayed.sketchId == kInvalidSketchId) return std::nullopt;
      return ExtrusionSourcePick{
          SketchRegionPick{displayed.sketchId, displayed.placement,
                           selectedExtrusionSketch_},
          hoveredExtrusionSurface_};
    }();
    if (selectedExtrusionPaths_.empty()) {
      selectedExtrusionPolygon_.clear();
      hideExtrusionManipulator();
      update();
      if (revolveProfilePick && sourcePick)
        emit revolveProfileSelectionChanged(*sourcePick);
      return;
    }
    QRectF selectedBounds;
    for (const auto& path : selectedExtrusionPaths_)
      selectedBounds = selectedBounds.united(path.boundingRect());
    selectedExtrusionPolygon_ = selectedExtrusionPolygons_.front();
    extrusionManipulatorAnchor_ = selectedBounds.center();
    if (!append && !revolveProfilePick) {
      pickMode_ = PickMode::None;
      unsetCursor();
    }
    extrusionHoverPolygon_.clear();
    update();
    if (sourcePick) {
      if (revolveProfilePick)
        emit revolveProfileSelectionChanged(*sourcePick);
      else
        emit extrusionSourcePicked(*sourcePick);
    }
    return;
  }
  if ((pickMode_ == PickMode::SketchPlane ||
       pickMode_ == PickMode::ImagePlane) &&
      !extrusionHoverPolygon_.isEmpty()) {
    const bool imagePlane = pickMode_ == PickMode::ImagePlane;
    const QString presentationLabel = hoveredExtrusionSurface_;
    std::optional<SketchPlanePick> pickedPlane;
    if (hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1)) {
      selectedFace_ = static_cast<int>(hoveredBodyFaceIndex_);
      if (const auto face = faceReferenceForGlobalIndex(hoveredBodyFaceIndex_))
        pickedPlane = SketchPlanePick{BodyFacePick{*face},
                                      presentationLabel};
    } else if (hoveredLegacySolidFace_) {
      pickedPlane = SketchPlanePick{*hoveredLegacySolidFace_,
                                    presentationLabel};
    } else if (selectedBasePlane_ >= 0 && selectedBasePlane_ < 3) {
      const BasePlane plane = selectedBasePlane_ == 0
                                  ? BasePlane::XY
                                  : selectedBasePlane_ == 1 ? BasePlane::XZ
                                                            : BasePlane::YZ;
      const SketchPlacement placement =
          plane == BasePlane::XY ? SketchPlacement::xy()
          : plane == BasePlane::XZ ? SketchPlacement::xz()
                                   : SketchPlacement::yz();
      pickedPlane = SketchPlanePick{DatumPlanePick{plane, placement},
                                    presentationLabel};
    }
    pickMode_ = PickMode::None;
    unsetCursor();
    extrusionHoverPolygon_.clear();
    extrusionHoverPath_ = {};
    hoveredExtrusionSurface_.clear();
    hoveredExtrusionSupport_.clear();
    update();
    if (pickedPlane) {
      if (imagePlane)
        emit imagePlanePicked(*pickedPlane);
      else
        emit sketchPlanePicked(*pickedPlane);
    }
    return;
  }
  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);
  const std::array<Point3, 8> vertices{{
      {-x + offsetX_, -y + offsetY_, 0}, {x + offsetX_, -y + offsetY_, 0},
      {x + offsetX_, y + offsetY_, 0}, {-x + offsetX_, y + offsetY_, 0},
      {-x + offsetX_, -y + offsetY_, z}, {x + offsetX_, -y + offsetY_, z},
      {x + offsetX_, y + offsetY_, z}, {-x + offsetX_, y + offsetY_, z}}};
  const std::array<std::array<int, 4>, 6> faces{{{{0, 1, 2, 3}}, {{4, 7, 6, 5}},
                                                  {{0, 4, 5, 1}}, {{1, 5, 6, 2}},
                                                  {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};

  if (pickMode_ == PickMode::SketchPlane ||
      pickMode_ == PickMode::ImagePlane) {
    const bool imagePlane = pickMode_ == PickMode::ImagePlane;
    const float planeSize = std::max(35.0F, std::max(x, y) * 1.35F);
    const std::array<std::array<Point3, 4>, 3> planes{{
        {{{-planeSize, -planeSize, 0}, {planeSize, -planeSize, 0},
           {planeSize, planeSize, 0}, {-planeSize, planeSize, 0}}},
        {{{-planeSize, 0, -planeSize}, {planeSize, 0, -planeSize},
           {planeSize, 0, planeSize}, {-planeSize, 0, planeSize}}},
        {{{0, -planeSize, -planeSize}, {0, planeSize, -planeSize},
           {0, planeSize, planeSize}, {0, -planeSize, planeSize}}}}};
    for (int plane = 2; plane >= 0; --plane) {
      if (!basePlanesVisible_[plane]) continue;
      QPolygonF polygon;
      for (const auto& point : planes[plane])
        polygon << project(point, size(), yaw_, pitch_, zoom_);
      if (polygon.containsPoint(scenePosition, Qt::OddEvenFill)) {
        const QString name = plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ";
        pickMode_ = PickMode::None;
        unsetCursor();
        const BasePlane basePlane = plane == 0
                                        ? BasePlane::XY
                                        : plane == 1 ? BasePlane::XZ
                                                     : BasePlane::YZ;
        const SketchPlacement placement =
            basePlane == BasePlane::XY ? SketchPlacement::xy()
            : basePlane == BasePlane::XZ ? SketchPlacement::xz()
                                         : SketchPlacement::yz();
        update();
        const SketchPlanePick pick{
            DatumPlanePick{basePlane, placement},
            QString::fromUtf8("Базовая плоскость ") + name};
        if (imagePlane)
          emit imagePlanePicked(pick);
        else
          emit sketchPlanePicked(pick);
        return;
      }
    }
  }

  const bool draftFacePick = pickMode_ == PickMode::DraftFace;
  if (pickMode_ != PickMode::None && !draftFacePick) return;

  // Direct sketch-profile interaction: in normal mode a visible closed profile
  // starts the direct Extrude instead of selecting body topology. The hover
  // machinery already depth-filters, so an occluded profile is not picked.
  if (!draftFacePick) updateExtrusionHover(scenePosition);
  if (!draftFacePick &&
      hoveredExtrusionSketchIndex_ != static_cast<std::size_t>(-1)) {
    const bool append = event->modifiers().testFlag(Qt::ControlModifier);
    if (!append || selectedExtrusionSketchIndex_ != hoveredExtrusionSketchIndex_) {
      selectedExtrusionPolygons_.clear();
      selectedExtrusionPaths_.clear();
      selectedExtrusionRegionSketches_.clear();
    }

    QPainterPath hoveredPath = extrusionHoverPath_;
    if (hoveredPath.isEmpty()) {
      hoveredPath.addPolygon(extrusionHoverPolygon_);
      hoveredPath.closeSubpath();
    }
    const QRectF hoveredBounds = hoveredPath.boundingRect();
    const auto existing = std::find_if(
        selectedExtrusionPaths_.begin(), selectedExtrusionPaths_.end(),
        [&hoveredBounds](const QPainterPath& path) {
          const QRectF bounds = path.boundingRect();
          return QLineF(bounds.center(), hoveredBounds.center()).length() < 1.0 &&
                 std::abs(bounds.width() - hoveredBounds.width()) < 1.0 &&
                 std::abs(bounds.height() - hoveredBounds.height()) < 1.0;
        });
    if (append && existing != selectedExtrusionPaths_.end()) {
      const auto index = static_cast<std::size_t>(
          std::distance(selectedExtrusionPaths_.begin(), existing));
      selectedExtrusionPaths_.erase(existing);
      if (index < selectedExtrusionPolygons_.size())
        selectedExtrusionPolygons_.erase(selectedExtrusionPolygons_.begin() +
                                         static_cast<std::ptrdiff_t>(index));
      if (index < selectedExtrusionRegionSketches_.size())
        selectedExtrusionRegionSketches_.erase(
            selectedExtrusionRegionSketches_.begin() +
            static_cast<std::ptrdiff_t>(index));
    } else {
      selectedExtrusionPaths_.push_back(hoveredPath);
      selectedExtrusionPolygons_.push_back(extrusionHoverPolygon_);
      selectedExtrusionRegionSketches_.push_back(hoveredExtrusionSketch_);
    }
    selectedExtrusionSketchIndex_ = hoveredExtrusionSketchIndex_;
    selectedExtrusionSupport_ = hoveredExtrusionSupport_;
    selectedExtrusionPlacement_ = hoveredExtrusionPlacement_;
    rebuildSelectedExtrusionSketch();
    if (selectedExtrusionPaths_.empty()) {
      selectedExtrusionPolygon_.clear();
      update();
      event->accept();
      return;
    }
    selectedExtrusionPolygon_ = selectedExtrusionPolygons_.front();
    update();
    event->accept();
    if (selectedExtrusionSketchIndex_ >= displaySketches_.size()) return;
    const auto& displayed = displaySketches_[selectedExtrusionSketchIndex_];
    if (displayed.sketchId == kInvalidSketchId) return;
    const ExtrusionSourcePick pick{
        SketchRegionPick{displayed.sketchId, displayed.placement,
                         selectedExtrusionSketch_},
        hoveredExtrusionSurface_};
    emit directProfilePicked(pick);
    return;
  }

  const bool toggleFace = !draftFacePick &&
                          (faceMultiSelectionMode_ ||
                           event->modifiers().testFlag(Qt::ControlModifier));
  const bool toggleEdge = edgeMultiSelectionMode_ ||
                          event->modifiers().testFlag(Qt::ControlModifier);
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;

  if (bodyShape_ && !bodyShape_->IsNull() && solidVisible_) {
    updateBodyHover(scenePosition);
    if (hoveredBodyEdgeIndex_ != static_cast<std::size_t>(-1)) {
      // Selecting an edge clears any face selection so no mixed edge+face
      // selection persists (the non-additive edge replace itself happens
      // inside commitEdgeSelection).
      if (!toggleFace) {
        selectedFace_ = -1;
        selectedBodyFaceIndices_.clear();
        selectedBodyFaceReferences_.clear();
      }
      commitEdgeSelection(hoveredBodyEdgeIndex_, toggleEdge);
      return;
    }
    if (hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1)) {
      if (!toggleEdge) {
        selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
        selectedBodyEdgeIndices_.clear();
        selectedBodyEdgeReferences_.clear();
      }
      commitFaceSelection(hoveredBodyFaceIndex_, toggleFace);
      return;
    }
    // Empty-area fallthrough: begin a rectangle marquee WITHOUT clearing the
    // current selection, so an aborted marquee (Escape / cancel / release over
    // nothing) preserves the pre-drag selection. The additive flag reflects
    // only the Ctrl modifier; plain drags replace and Ctrl-drags add/toggle.
    marqueeActive_ = true;
    marqueeStart_ = scenePosition;
    marqueeCurrent_ = scenePosition;
    marqueeAdditive_ = event->modifiers().testFlag(Qt::ControlModifier);
    setCursor(Qt::CrossCursor);
    event->accept();
    update();
    return;
  }

  // Points have the highest picking priority because they occupy the
  // smallest area on screen.
  const QPointF originPoint = project({0, 0, 0}, size(), yaw_, pitch_, zoom_);
  if (originVisible_ &&
      QLineF(originPoint, scenePosition).length() <= 9.0) {
    selectedOrigin_ = true;
    emit selectionChanged(QString::fromUtf8("Точка: начало координат"));
    update();
    return;
  }

  if (solidVisible_ && solidSketch_.circles().empty()) {
    for (int vertex = 0; vertex < static_cast<int>(vertices.size()); ++vertex) {
      const QPointF projected = project(vertices[static_cast<std::size_t>(vertex)],
                                        size(), yaw_, pitch_, zoom_);
      if (QLineF(projected, scenePosition).length() > 9.0) continue;
      selectedVertex_ = vertex;
      emit selectionChanged(QString::fromUtf8("Тело 1 • Точка ") +
                            QString::number(vertex + 1));
      update();
      return;
    }
  }

  // Faces are checked before construction planes so that a visible solid
  // remains easy to select where both objects overlap.
  selectedFace_ = -1;
  if (solidVisible_ && !solidSketch_.circles().empty()) {
    const auto& circle = solidSketch_.circles().front();
    const Point3 normal = placementNormal(solidSketchPlacement_);
    QPolygonF bottomCap;
    QPolygonF topCap;
    for (int step = 0; step < 64; ++step) {
      const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
      const sketch::Point profilePoint{
          circle.center.xMm + circle.radiusMm * std::cos(angle),
          circle.center.yMm + circle.radiusMm * std::sin(angle)};
      const Point3 base = pointOnPlacement(
          profilePoint, solidSketchPlacement_, offsetX_, offsetY_);
      bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
      topCap << project(translated(base, normal, z), size(), yaw_, pitch_, zoom_);
    }
    const std::array<QPolygonF, 2> caps{bottomCap, topCap};
    for (int cap = 1; cap >= 0; --cap) {
      if (!isFrontFacing(caps[cap]) ||
          !caps[cap].containsPoint(scenePosition, Qt::OddEvenFill))
        continue;
      selectedFace_ = cap;
      draggingBody_ = true;
      emit selectionChanged(QString::fromUtf8("Тело 1 • Круглый торец"));
      update();
      return;
    }
    for (int step = 63; step >= 0; --step) {
      const float a = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
      const float b = 2.0F * std::numbers::pi_v<float> * (step + 1) / 64.0F;
      const auto surfacePoint = [&](float angle, float height) {
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnPlacement(
            profilePoint, solidSketchPlacement_, offsetX_, offsetY_);
        return project(translated(base, normal, height), size(), yaw_, pitch_, zoom_);
      };
      QPolygonF side{surfacePoint(a, 0.0F), surfacePoint(b, 0.0F),
                     surfacePoint(b, z), surfacePoint(a, z)};
      if (!isFrontFacing(side) ||
          !side.containsPoint(scenePosition, Qt::OddEvenFill))
        continue;
      selectedFace_ = 2;
      draggingBody_ = true;
      emit selectionChanged(QString::fromUtf8("Тело 1 • Боковая грань"));
      update();
      return;
    }
  } else if (solidVisible_) {
    for (int faceIndex = 5; faceIndex >= 0; --faceIndex) {
      QPolygonF polygon;
      for (int vertex : faces[faceIndex])
        polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
      if (!isFrontFacing(polygon)) continue;
      if (polygon.containsPoint(scenePosition, Qt::OddEvenFill)) {
        selectedFace_ = faceIndex;
        draggingBody_ = true;
        emit selectionChanged(QString::fromUtf8("Тело 1 • Грань: ") +
                              selectedFaceName());
        update();
        return;
      }
    }
  }

  const float planeSize = std::max(35.0F, std::max(x, y) * 1.35F);
  const std::array<std::array<Point3, 4>, 3> planes{{
      {{{-planeSize, -planeSize, 0}, {planeSize, -planeSize, 0},
         {planeSize, planeSize, 0}, {-planeSize, planeSize, 0}}},
      {{{-planeSize, 0, -planeSize}, {planeSize, 0, -planeSize},
         {planeSize, 0, planeSize}, {-planeSize, 0, planeSize}}},
      {{{0, -planeSize, -planeSize}, {0, planeSize, -planeSize},
         {0, planeSize, planeSize}, {0, -planeSize, planeSize}}}}};
  for (int plane = 2; plane >= 0; --plane) {
    if (!basePlanesVisible_[plane]) continue;
    QPolygonF polygon;
    for (const auto& point : planes[plane])
      polygon << project(point, size(), yaw_, pitch_, zoom_);
    if (!polygon.containsPoint(scenePosition, Qt::OddEvenFill)) continue;
    selectedBasePlane_ = plane;
    const QString name = plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ";
    emit selectionChanged(QString::fromUtf8("Базовая плоскость ") + name);
    update();
    return;
  }

  emit selectionChanged(QString{});
  update();
}

void Viewport::rebuildSelectedExtrusionSketch() {
  selectedExtrusionSketch_.clear();
  // A real B-Rep face carries no legacy support-name plane. Its selected
  // region is the body's own planar profile, so reuse that cached profile
  // rather than mis-projecting screen polygons onto an unrelated plane.
  if (selectedExtrusionBodyFace_ && bodyShape_ && !bodyShape_->IsNull()) {
    selectedExtrusionSketch_ = solidSketch_;
    return;
  }
  if (!selectedExtrusionRegionSketches_.empty()) {
    std::vector<sketch::Line> boundaryLines;
    std::vector<sketch::Circle> boundaryCircles;
    std::vector<sketch::Arc> boundaryArcs;
    std::vector<sketch::Bezier> boundaryBeziers;
    for (const auto& region : selectedExtrusionRegionSketches_) {
      for (const auto& line : region.lines())
        if (!line.dashed) boundaryLines.push_back(line);
      for (const auto& circle : region.circles())
        if (!circle.dashed) boundaryCircles.push_back(circle);
      for (const auto& arc : region.arcs())
        if (!arc.dashed) boundaryArcs.push_back(arc);
      for (const auto& bezier : region.beziers())
        if (!bezier.dashed) boundaryBeziers.push_back(bezier);
    }

    // Boolean-union the selected planar regions at the boundary level. Shared
    // edges occur twice and cancel. Split collinear partial overlaps first so
    // a short Arc chord can cancel only the covered part of a longer side.
    constexpr double kRegionMergeToleranceMm = 1e-3;
    const auto sameRegionPoint = [](sketch::Point first,
                                    sketch::Point second) {
      return std::hypot(first.xMm - second.xMm,
                        first.yMm - second.yMm) <=
             kRegionMergeToleranceMm;
    };
    const auto regionPointParameterOnLine = [](sketch::Point point,
                                               const sketch::Line& line)
        -> std::optional<double> {
      const double dx = line.end.xMm - line.start.xMm;
      const double dy = line.end.yMm - line.start.yMm;
      const double lengthSquared = dx * dx + dy * dy;
      if (lengthSquared <= 1e-12) return std::nullopt;
      const double parameter =
          ((point.xMm - line.start.xMm) * dx +
           (point.yMm - line.start.yMm) * dy) /
          lengthSquared;
      if (parameter < -1e-7 || parameter > 1.0 + 1e-7)
        return std::nullopt;
      const sketch::Point projection{line.start.xMm + dx * parameter,
                                     line.start.yMm + dy * parameter};
      if (std::hypot(point.xMm - projection.xMm,
                     point.yMm - projection.yMm) >
          kRegionMergeToleranceMm)
        return std::nullopt;
      return std::clamp(parameter, 0.0, 1.0);
    };
    std::vector<sketch::Line> splitLines;
    for (const auto& line : boundaryLines) {
      std::vector<double> cuts{0.0, 1.0};
      for (const auto& other : boundaryLines) {
        for (const auto endpoint : {other.start, other.end}) {
          if (const auto parameter =
                  regionPointParameterOnLine(endpoint, line))
            cuts.push_back(*parameter);
        }
      }
      std::sort(cuts.begin(), cuts.end());
      cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double first,
                                                          double second) {
                   return std::abs(first - second) <= 1e-8;
                 }),
                 cuts.end());
      const double dx = line.end.xMm - line.start.xMm;
      const double dy = line.end.yMm - line.start.yMm;
      for (std::size_t index = 0; index + 1 < cuts.size(); ++index) {
        if (cuts[index + 1] - cuts[index] <= 1e-8) continue;
        splitLines.push_back(
            {{line.start.xMm + dx * cuts[index],
              line.start.yMm + dy * cuts[index]},
             {line.start.xMm + dx * cuts[index + 1],
              line.start.yMm + dy * cuts[index + 1]}});
      }
    }
    std::vector<sketch::Line> survivingLines;
    for (const auto& line : splitLines) {
      const auto duplicate = std::find_if(
          survivingLines.begin(), survivingLines.end(),
          [&line, &sameRegionPoint](const sketch::Line& existing) {
            return (sameRegionPoint(line.start, existing.start) &&
                    sameRegionPoint(line.end, existing.end)) ||
                   (sameRegionPoint(line.start, existing.end) &&
                    sameRegionPoint(line.end, existing.start));
          });
      if (duplicate == survivingLines.end())
        survivingLines.push_back(line);
      else
        survivingLines.erase(duplicate);
    }
    std::erase_if(survivingLines, [](const sketch::Line& line) {
      return std::hypot(line.end.xMm - line.start.xMm,
                        line.end.yMm - line.start.yMm) <=
             kRegionMergeToleranceMm;
    });
    // Region discovery accepts a small geometric tolerance. Canonicalize every
    // surviving boundary vertex with that same tolerance before handing the
    // profile to Sketch/OCCT, whose closed-wire checks intentionally use a much
    // tighter tolerance. Exact Arc endpoints are seeded first so they win over
    // a numerically close line endpoint produced by splitting.
    std::vector<sketch::Point> canonicalEndpoints;
    canonicalEndpoints.reserve(boundaryArcs.size() * 2 +
                               survivingLines.size() * 2);
    for (const auto& arc : boundaryArcs) {
      for (const auto endpoint : {sketch::arcStartPoint(arc),
                                  sketch::arcEndPoint(arc)}) {
        const bool present = std::any_of(
            canonicalEndpoints.begin(), canonicalEndpoints.end(),
            [endpoint](sketch::Point existing) {
              return std::hypot(existing.xMm - endpoint.xMm,
                                existing.yMm - endpoint.yMm) <=
                     kRegionMergeToleranceMm;
            });
        if (!present) canonicalEndpoints.push_back(endpoint);
      }
    }
    const auto canonicalizeEndpoint = [&canonicalEndpoints](
                                          sketch::Point* point) {
      const auto existing = std::find_if(
          canonicalEndpoints.begin(), canonicalEndpoints.end(),
          [point](sketch::Point candidate) {
            return std::hypot(point->xMm - candidate.xMm,
                              point->yMm - candidate.yMm) <=
                   kRegionMergeToleranceMm;
          });
      if (existing != canonicalEndpoints.end()) {
        *point = *existing;
      } else {
        canonicalEndpoints.push_back(*point);
      }
    };
    for (auto& line : survivingLines) {
      canonicalizeEndpoint(&line.start);
      canonicalizeEndpoint(&line.end);
    }
    for (const auto& line : survivingLines)
      selectedExtrusionSketch_.addLine(line.start, line.end);
    for (const auto& circle : boundaryCircles)
      selectedExtrusionSketch_.addCircle(circle.center, circle.radiusMm);
    for (const auto& arc : boundaryArcs)
      selectedExtrusionSketch_.addArc(arc.center, arc.radiusMm,
                                      arc.startAngleRad, arc.sweepAngleRad);
    for (const auto& bezier : boundaryBeziers)
      selectedExtrusionSketch_.addBezier(
          bezier.points[0], bezier.points[1], bezier.points[2],
          bezier.points[3]);
    return;
  }
  Point3 p0 = pointOnPlacement(
      {0, 0}, selectedExtrusionPlacement_, offsetX_, offsetY_);
  Point3 pU = pointOnPlacement(
      {1, 0}, selectedExtrusionPlacement_, offsetX_, offsetY_);
  Point3 pV = pointOnPlacement(
      {0, 1}, selectedExtrusionPlacement_, offsetX_, offsetY_);
  if (selectedExtrusionOnBodyCap_) {
    const Point3 normal = placementNormal(solidSketchPlacement_);
    const float distance = static_cast<float>(box_.heightMm);
    p0 = translated(p0, normal, distance);
    pU = translated(pU, normal, distance);
    pV = translated(pV, normal, distance);
  }
  const QPointF origin = project(p0, size(), yaw_, pitch_, zoom_);
  const QPointF u = project(pU, size(), yaw_, pitch_, zoom_) - origin;
  const QPointF v = project(pV, size(), yaw_, pitch_, zoom_) - origin;
  const double determinant = u.x() * v.y() - u.y() * v.x();
  if (std::abs(determinant) < 1e-9) return;

  for (const auto& path : selectedExtrusionPaths_) {
    for (const auto& boundary : path.toSubpathPolygons()) {
      std::vector<sketch::Point> points;
      points.reserve(static_cast<std::size_t>(boundary.size()));
      for (const QPointF& screenPoint : boundary) {
        const QPointF delta = screenPoint - origin;
        points.push_back({
            (delta.x() * v.y() - delta.y() * v.x()) / determinant,
            (u.x() * delta.y() - u.y() * delta.x()) / determinant});
      }
      if (points.size() > 1 &&
          QLineF(boundary.front(), boundary.back()).length() < 0.01)
        points.pop_back();
      for (std::size_t index = 0; index < points.size(); ++index)
        selectedExtrusionSketch_.addLine(
            points[index], points[(index + 1) % points.size()]);
    }
  }
}

void Viewport::updateBodyHover(QPointF position, bool exact) {
  const auto previousFace = hoveredBodyFaceIndex_;
  const auto previousEdge = hoveredBodyEdgeIndex_;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  if (!solidVisible_ || !hasDisplayedBodyTriangles()) return;
  const auto& scene = pickingScene();
  if (selectionFilter_ != SelectionFilter::Edge &&
      selectionFilter_ != SelectionFilter::Plane)
    if (const auto face = scene.faceAt(position))
      hoveredBodyFaceIndex_ = face->faceIndex;

  if (selectionFilter_ == SelectionFilter::Face ||
      selectionFilter_ == SelectionFilter::Plane)
    return;

  const std::uint64_t uncertaintyBefore = scene.counters().uncertainVisible;
  if (const auto edge = scene.edgeAt(
          position, kEdgeHitRadiusPx,
          exact ? PickingQueryPrecision::Exact
                : PickingQueryPrecision::Interactive))
    hoveredBodyEdgeIndex_ = edge->edgeIndex;
  if (!exact && scene.counters().uncertainVisible != uncertaintyBefore) {
    hoveredBodyFaceIndex_ = previousFace;
    hoveredBodyEdgeIndex_ = previousEdge;
    return;
  }
  if (hoveredBodyEdgeIndex_ != static_cast<std::size_t>(-1))
    hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
}

void Viewport::selectInRect(const QRectF& rect, bool additive, bool singleOnly) {
  // Commit the domain change only when the marquee is accepted, so Escape can
  // still cancel the drag without changing the previous whole-body selection.
  clearWholeBodySelection();

  // Plane filter selects base planes, never body geometry. A marquee in this
  // context must not select body faces/edges; clear any stale body selection.
  if (selectionFilter_ == SelectionFilter::Plane) {
    selectedFace_ = -1;
    selectedBodyFaceIndices_.clear();
    selectedBodyFaceReferences_.clear();
    selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    selectedBodyEdgeIndices_.clear();
    selectedBodyEdgeReferences_.clear();
    return;
  }

  const auto& scene = pickingScene();
  const bool wantsEdges = selectionFilter_ == SelectionFilter::Edge;

  // Single-body consistency, mirroring the click path: selection is restricted
  // to the body/feature of the first collected ordinal.
  const auto keepFirstBody = [this](const std::vector<std::size_t>& ordinals,
                                    bool edge) {
    if (ordinals.empty()) return ordinals;
    std::optional<BodyId> body;
    std::optional<FeatureId> feature;
    if (edge) {
      if (const auto ref = edgeReferenceForGlobalIndex(ordinals.front())) {
        body = ref->bodyId;
        feature = ref->featureId;
      }
    } else if (const auto ref = faceReferenceForGlobalIndex(ordinals.front())) {
      body = ref->bodyId;
      feature = ref->featureId;
    }
    if (!body) return ordinals;
    std::vector<std::size_t> result;
    for (const auto index : ordinals) {
      if (edge) {
        const auto ref = edgeReferenceForGlobalIndex(index);
        if (ref && ref->bodyId == *body && ref->featureId == *feature)
          result.push_back(index);
      } else {
        const auto ref = faceReferenceForGlobalIndex(index);
        if (ref && ref->bodyId == *body && ref->featureId == *feature)
          result.push_back(index);
      }
    }
    return result;
  };

  // Ctrl+A single-select: keep only the frontmost eligible entity (largest
  // camera depth = nearest) so SelectAll cannot widen a single-select tool
  // contract. `ordinals` is already occlusion-filtered and single-body.
  const auto frontmost = [&](const std::vector<std::size_t>& ordinals,
                             bool edge) {
    if (ordinals.size() <= 1) return ordinals;
    const auto best = edge ? scene.frontmostEdge(ordinals)
                           : scene.frontmostFace(ordinals);
    return best ? std::vector<std::size_t>{*best}
                : std::vector<std::size_t>{};
  };

  if (!wantsEdges) {
    auto eligible = keepFirstBody(scene.facesInRect(rect), false);
    if (singleOnly) eligible = frontmost(eligible, false);
    // Cross-type: a face-domain selection must not coexist with edge selection.
    selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    selectedBodyEdgeIndices_.clear();
    selectedBodyEdgeReferences_.clear();
    if (additive) {
      if (!selectedBodyFaceIndices_.empty() && !eligible.empty()) {
        const auto first =
            faceReferenceForGlobalIndex(selectedBodyFaceIndices_.front());
        const auto marqueeFirst =
            faceReferenceForGlobalIndex(eligible.front());
        if (first && marqueeFirst &&
            (first->bodyId != marqueeFirst->bodyId ||
             first->featureId != marqueeFirst->featureId))
          selectedBodyFaceIndices_.clear();
      }
      for (const auto index : eligible)
        if (std::find(selectedBodyFaceIndices_.begin(),
                      selectedBodyFaceIndices_.end(), index) ==
            selectedBodyFaceIndices_.end())
          selectedBodyFaceIndices_.push_back(index);
    } else {
      selectedBodyFaceIndices_ = eligible;
    }
    selectedFace_ = selectedBodyFaceIndices_.empty()
                        ? -1
                        : static_cast<int>(selectedBodyFaceIndices_.front());
    selectedBodyFaceReferences_.clear();
    for (const auto index : selectedBodyFaceIndices_)
      if (const auto face = faceReferenceForGlobalIndex(index))
        selectedBodyFaceReferences_.push_back(*face);
    emit bodyFaceSelectionChanged();
    emit selectionChanged(
        selectedBodyFaceIndices_.empty()
            ? QString{}
            : QString::fromUtf8("Тело 1 • Грань ") +
                  QString::number(selectedFace_ + 1));
  } else {
    auto eligible = keepFirstBody(scene.edgesInRect(rect), true);
    if (singleOnly) eligible = frontmost(eligible, true);
    // Cross-type: an edge-domain selection must not coexist with face selection.
    selectedFace_ = -1;
    selectedBodyFaceIndices_.clear();
    selectedBodyFaceReferences_.clear();
    if (additive) {
      if (!selectedBodyEdgeIndices_.empty() && !eligible.empty()) {
        const auto first =
            edgeReferenceForGlobalIndex(selectedBodyEdgeIndices_.front());
        const auto marqueeFirst =
            edgeReferenceForGlobalIndex(eligible.front());
        if (first && marqueeFirst &&
            (first->bodyId != marqueeFirst->bodyId ||
             first->featureId != marqueeFirst->featureId))
          selectedBodyEdgeIndices_.clear();
      }
      for (const auto index : eligible)
        if (std::find(selectedBodyEdgeIndices_.begin(),
                      selectedBodyEdgeIndices_.end(), index) ==
            selectedBodyEdgeIndices_.end())
          selectedBodyEdgeIndices_.push_back(index);
    } else {
      selectedBodyEdgeIndices_ = eligible;
    }
    selectedBodyEdgeIndex_ = selectedBodyEdgeIndices_.empty()
                                 ? static_cast<std::size_t>(-1)
                                 : selectedBodyEdgeIndices_.front();
    selectedBodyEdgeReferences_.clear();
    for (const auto index : selectedBodyEdgeIndices_)
      if (const auto edge = edgeReferenceForGlobalIndex(index))
        selectedBodyEdgeReferences_.push_back(*edge);
    emit bodyEdgeSelectionChanged();
    emit selectionChanged(
        selectedBodyEdgeIndices_.empty()
            ? QString{}
            : QString::fromUtf8("Тело 1 • Ребро ") +
                  QString::number(selectedBodyEdgeIndex_ + 1));
  }
}

void Viewport::cancelMarquee() {
  marqueeActive_ = false;
  marqueeStart_ = {};
  marqueeCurrent_ = {};
  marqueeAdditive_ = false;
  unsetCursor();
  update();
}

void Viewport::updateToolBodyHover(QPointF position) {
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;
  if (!solidVisible_ || !hasDisplayedBodyTriangles()) return;
  if (const auto face = pickingScene().faceAt(position))
    hoveredBodyFaceIndex_ = face->faceIndex;
  if (hoveredBodyFaceIndex_ == static_cast<std::size_t>(-1)) return;
  if (const auto face = faceReferenceForGlobalIndex(hoveredBodyFaceIndex_))
    hoveredToolBodyId_ = face->bodyId;
}

void Viewport::updateMirrorPlaneHover(QPointF position) {
  selectedBasePlane_ = -1;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredToolBodyId_ = kInvalidBodyId;

  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float planeSize = std::max(35.0F, std::max(x, y) * 1.35F);
  const std::array<std::array<Point3, 4>, 3> planes{{
      {{{-planeSize, -planeSize, 0}, {planeSize, -planeSize, 0},
         {planeSize, planeSize, 0}, {-planeSize, planeSize, 0}}},
      {{{-planeSize, 0, -planeSize}, {planeSize, 0, -planeSize},
         {planeSize, 0, planeSize}, {-planeSize, 0, planeSize}}},
      {{{0, -planeSize, -planeSize}, {0, planeSize, -planeSize},
         {0, planeSize, planeSize}, {0, -planeSize, planeSize}}}}};
  for (int plane = 2; plane >= 0; --plane) {
    if (!basePlanesVisible_[plane]) continue;
    QPolygonF polygon;
    for (const auto& point : planes[plane])
      polygon << project(point, size(), yaw_, pitch_, zoom_);
    if (!polygon.containsPoint(position, Qt::OddEvenFill)) continue;
    selectedBasePlane_ = plane;
    return;
  }
}

void Viewport::updateSketchPlaneHover(QPointF position) {
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionSupport_.clear();
  hoveredExtrusionPlacement_ = SketchPlacement::xy();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredLegacySolidFace_.reset();
  selectedBasePlane_ = -1;

  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);

  // Planar faces of the current solid have priority over construction planes.
  if (solidVisible_ && bodyShape_ && !bodyShape_->IsNull()) {
    const auto& scene = pickingScene();
    if (const auto face = scene.faceAt(position))
      hoveredBodyFaceIndex_ = face->faceIndex;
    if (hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1)) {
      QPainterPath facePath;
      for (const auto& triangle :
           scene.trianglesForFace(hoveredBodyFaceIndex_)) {
        QPolygonF polygon{triangle.a.screen, triangle.b.screen,
                          triangle.c.screen};
        facePath.addPolygon(polygon);
      }
      extrusionHoverPath_ = facePath.simplified();
      const auto polygons = extrusionHoverPath_.toFillPolygons();
      if (!polygons.empty()) extrusionHoverPolygon_ = polygons.front();
      hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела #%1").arg(
          hoveredBodyFaceIndex_ + 1);
      hoveredExtrusionSupport_ = hoveredExtrusionSurface_;
      return;
    }
  } else if (solidVisible_) {
    if (!solidSketch_.circles().empty() || !solidSketch_.lines().empty()) {
      const Vector3d placementNormal = solidSketchPlacement_.normal();
      const Point3 normal{static_cast<float>(placementNormal.x),
                          static_cast<float>(placementNormal.y),
                          static_cast<float>(placementNormal.z)};
      std::array<QPolygonF, 2> caps;
      if (!solidSketch_.circles().empty()) {
        const auto& circle = solidSketch_.circles().front();
        for (int step = 0; step < 96; ++step) {
          const float angle = 2.0F * std::numbers::pi_v<float> * step / 96.0F;
          const sketch::Point profilePoint{
              circle.center.xMm + circle.radiusMm * std::cos(angle),
              circle.center.yMm + circle.radiusMm * std::sin(angle)};
          const Point3 base = pointOnPlacement(
              profilePoint, solidSketchPlacement_, offsetX_, offsetY_);
          caps[0].prepend(project(base, size(), yaw_, pitch_, zoom_));
          caps[1] << project(translated(base, normal, z), size(), yaw_, pitch_,
                             zoom_);
        }
      } else {
        for (const auto& line : solidSketch_.lines()) {
          const Point3 base = pointOnPlacement(
              line.start, solidSketchPlacement_, offsetX_, offsetY_);
          caps[0].prepend(project(base, size(), yaw_, pitch_, zoom_));
          caps[1] << project(translated(base, normal, z), size(), yaw_, pitch_,
                             zoom_);
        }
      }
      const std::array<QString, 2> names{QString::fromUtf8("Начальная"),
                                          QString::fromUtf8("Торцевая")};
      for (int cap = 1; cap >= 0; --cap) {
        if (!isFrontFacing(caps[cap]) ||
            !caps[cap].containsPoint(position, Qt::OddEvenFill))
          continue;
        extrusionHoverPolygon_ = caps[cap];
        hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") + names[cap];
        hoveredExtrusionSupport_ = names[cap];
        hoveredLegacySolidFace_ = legacyCapPick(
            solidSketch_, solidSketchPlacement_, box_.heightMm, cap == 1);
        hoveredExtrusionPlacement_ = hoveredLegacySolidFace_->placement;
        return;
      }
    } else {
      const std::array<Point3, 8> vertices{{
          {-x + offsetX_, -y + offsetY_, 0}, {x + offsetX_, -y + offsetY_, 0},
          {x + offsetX_, y + offsetY_, 0}, {-x + offsetX_, y + offsetY_, 0},
          {-x + offsetX_, -y + offsetY_, z}, {x + offsetX_, -y + offsetY_, z},
          {x + offsetX_, y + offsetY_, z}, {-x + offsetX_, y + offsetY_, z}}};
      const std::array<std::array<int, 4>, 6> faces{{
          {{0, 1, 2, 3}}, {{4, 7, 6, 5}}, {{0, 4, 5, 1}},
          {{1, 5, 6, 2}}, {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
      static const std::array<const char*, 6> names{
          "Нижняя", "Верхняя", "Передняя", "Правая", "Задняя", "Левая"};
      for (int face = 5; face >= 0; --face) {
        QPolygonF polygon;
        for (int vertex : faces[face])
          polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
        if (!isFrontFacing(polygon) ||
            !polygon.containsPoint(position, Qt::OddEvenFill))
          continue;
        extrusionHoverPolygon_ = polygon;
        const QString name = QString::fromUtf8(names[face]);
        hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") + name;
        hoveredExtrusionSupport_ = name;
        hoveredLegacySolidFace_ = legacyBoxFacePick(face, box_);
        hoveredExtrusionPlacement_ = hoveredLegacySolidFace_->placement;
        return;
      }
    }
  }

  // Construction-plane candidates are deliberately handled as a collection;
  // auxiliary user planes can be appended here without changing picking UI.
  const float planeSize = std::max(35.0F, std::max(x, y) * 1.35F);
  const std::array<std::array<Point3, 4>, 3> planes{{
      {{{-planeSize, -planeSize, 0}, {planeSize, -planeSize, 0},
         {planeSize, planeSize, 0}, {-planeSize, planeSize, 0}}},
      {{{-planeSize, 0, -planeSize}, {planeSize, 0, -planeSize},
         {planeSize, 0, planeSize}, {-planeSize, 0, planeSize}}},
      {{{0, -planeSize, -planeSize}, {0, planeSize, -planeSize},
         {0, planeSize, planeSize}, {0, -planeSize, planeSize}}}}};
  for (int plane = 2; plane >= 0; --plane) {
    if (!basePlanesVisible_[plane]) continue;
    QPolygonF polygon;
    for (const auto& point : planes[plane])
      polygon << project(point, size(), yaw_, pitch_, zoom_);
    if (!polygon.containsPoint(position, Qt::OddEvenFill)) continue;
    selectedBasePlane_ = plane;
    const QString name = plane == 0 ? QStringLiteral("XY")
                                    : plane == 1 ? QStringLiteral("XZ")
                                                 : QStringLiteral("YZ");
    extrusionHoverPolygon_ = polygon;
    hoveredExtrusionSurface_ = QString::fromUtf8("Базовая плоскость ") + name;
    hoveredExtrusionSupport_ = name;
    hoveredExtrusionPlacement_ =
        plane == 0 ? SketchPlacement::xy()
                   : plane == 1 ? SketchPlacement::xz()
                                : SketchPlacement::yz();
    return;
  }
}

void Viewport::updateExtrusionHover(QPointF position) {
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  hoveredExtrusionSketch_.clear();
  hoveredExtrusionPlacement_ = SketchPlacement::xy();
  hoveredExtrusionSupport_.clear();
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionOnBodyCap_ = false;
  hoveredExtrusionReverse_ = false;
  hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredLegacySolidFace_.reset();

  // Resolve one real sketch before splitting regions. Screen overlap does not
  // imply coplanarity, and the parametric feature references one DocumentSketch.
  std::vector<QPolygonF> contours;
  std::vector<sketch::Sketch> exactContourProfiles;
  sketch::Sketch exactArcProfile;
  QPolygonF exactArcPolygon;
  std::vector<std::vector<sketch::Point>> exactLineFaces;
  std::vector<QPolygonF> exactLineFacePolygons;
  bool lineFacesAreWholeProfile = false;
  QString regionSupport;
  std::size_t regionSketchIndex = static_cast<std::size_t>(-1);
  const ViewportCameraState camera{yaw_, pitch_, zoom_, {}, size()};
  double bestDepth = -std::numeric_limits<double>::max();
  double surfaceDepth = bestDepth;
  const double depthEpsilon = std::max(1e-5,
      displayedBodyDiagonal() * kDepthEpsilonScale);
  if (solidVisible_) {
    if (const auto hit = pickingScene().faceAt(position))
      surfaceDepth = hit->depth;
  }
  for (std::size_t displayedIndex = 0; displayedIndex < displaySketches_.size();
       ++displayedIndex) {
    const auto& displayed = displaySketches_[displayedIndex];
    if (!displayed.visible) continue;
    std::vector<QPolygonF> sketchContours;
    std::vector<sketch::Sketch> sketchContourProfiles;
    const auto projectContourPoint = [&](sketch::Point point) {
      return project(pointOnPlacement(point, displayed.placement,
                                      offsetX_, offsetY_),
                     size(), yaw_, pitch_, zoom_);
    };
    std::optional<sketch::Sketch> hitArcProfile;
    QPolygonF hitArcPolygon;
    for (auto& profile : attachedArcProfiles(displayed.geometry)) {
      QPolygonF polygon;
      for (const auto point : profile.boundary)
        polygon << projectContourPoint(point);
      if (polygon.size() < 3 || std::abs(signedArea(polygon)) <= 1e-6)
        continue;
      sketchContours.push_back(polygon);
      sketchContourProfiles.push_back(profile.geometry);
      if (!hitArcProfile &&
          polygon.containsPoint(position, Qt::OddEvenFill)) {
        hitArcProfile = std::move(profile.geometry);
        hitArcPolygon = std::move(polygon);
      }
    }
    const bool hasBezier = std::any_of(
        displayed.geometry.beziers().begin(),
        displayed.geometry.beziers().end(),
        [](const sketch::Bezier& bezier) { return !bezier.dashed; });
    if (hasBezier && displayed.geometry.isClosed()) {
      for (const auto& boundary : projectedSketchBoundaries(
               displayed.geometry, projectContourPoint)) {
        if (boundary.size() < 3 ||
            std::abs(signedArea(boundary)) <= 1e-6)
          continue;
        sketchContours.push_back(boundary);
        sketch::Sketch candidate;
        for (const auto& line : displayed.geometry.lines())
          if (!line.dashed) candidate.addLine(line.start, line.end);
        for (const auto& arc : displayed.geometry.arcs())
          if (!arc.dashed)
            candidate.addArc(arc.center, arc.radiusMm, arc.startAngleRad,
                             arc.sweepAngleRad);
        for (const auto& bezier : displayed.geometry.beziers())
          if (!bezier.dashed)
            candidate.addBezier(bezier.points[0], bezier.points[1],
                                bezier.points[2], bezier.points[3]);
        sketchContourProfiles.push_back(std::move(candidate));
      }
    }
    // PLANAR SKETCH REGION GRAPH
    //
    // A rectangle side is one primitive. When a newly drawn line terminates
    // on its middle, elementId-based contour grouping still sees only the
    // original outer rectangle and therefore highlights the whole square.
    // Split T-junctions/crossings into a planar graph and enumerate its bounded
    // faces. The cursor can then select the exact sub-region.
    const auto graphFaces = planarSketchLineFaces(displayed.geometry);
    const bool hasLineGraphFaces = !graphFaces.empty();

    std::vector<std::vector<sketch::Point>> validGraphFaces;
    std::vector<QPolygonF> graphFacePolygons;
    if (hasLineGraphFaces) {
      for (const auto& face : graphFaces) {
        QPolygonF polygon;
        for (const auto point : face)
          polygon << projectContourPoint(point);
        if (polygon.size() >= 3 &&
            std::abs(signedArea(polygon)) > 1e-6) {
          sketch::Sketch candidate;
          for (std::size_t index = 0; index < face.size(); ++index)
            candidate.addLine(face[index], face[(index + 1) % face.size()]);
          validGraphFaces.push_back(face);
          graphFacePolygons.push_back(polygon);
          sketchContours.push_back(std::move(polygon));
          sketchContourProfiles.push_back(std::move(candidate));
        }
      }
    } else {
      std::vector<std::size_t> ids;
      for (const auto& line : displayed.geometry.lines())
        if (!line.dashed &&
            std::find(ids.begin(), ids.end(), line.elementId) == ids.end())
          ids.push_back(line.elementId);
      for (const auto id : ids) {
        QPolygonF polygon;
        sketch::Sketch candidate;
        for (const auto& line : displayed.geometry.lines())
          if (!line.dashed && line.elementId == id) {
            polygon << projectContourPoint(line.start);
            candidate.addLine(line.start, line.end);
          }
        if (polygon.size() >= 3 && candidate.isClosed() &&
            std::abs(signedArea(polygon)) > 1e-6) {
          sketchContours.push_back(polygon);
          sketchContourProfiles.push_back(std::move(candidate));
        }
      }
    }
    for (const auto& circle : displayed.geometry.circles()) {
      if (circle.dashed || !std::isfinite(circle.radiusMm) ||
          circle.radiusMm <= 0) continue;
      QPolygonF polygon;
      for (int step = 0; step < 96; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 96.0F;
        const sketch::Point point{circle.center.xMm + circle.radiusMm * std::cos(angle),
                                  circle.center.yMm + circle.radiusMm * std::sin(angle)};
        polygon << projectContourPoint(point);
      }
      sketchContours.push_back(polygon);
      sketch::Sketch candidate;
      candidate.addCircle(circle.center, circle.radiusMm);
      sketchContourProfiles.push_back(std::move(candidate));
    }
    if (std::none_of(sketchContours.begin(), sketchContours.end(),
        [&](const QPolygonF& contour) {
          return contour.containsPoint(position, Qt::OddEvenFill);
        })) continue;
    const QPointF origin = projectContourPoint({0, 0});
    const QPointF u = projectContourPoint({1, 0}) - origin;
    const QPointF v = projectContourPoint({0, 1}) - origin;
    const double determinant = u.x() * v.y() - u.y() * v.x();
    if (std::abs(determinant) <= 1e-9) continue;
    const QPointF delta = position - origin;
    const Point3 hit = pointOnPlacement(
        {(delta.x() * v.y() - delta.y() * v.x()) / determinant,
         (u.x() * delta.y() - u.y() * delta.x()) / determinant},
        displayed.placement, offsetX_, offsetY_);
    const double depth = camera.cameraDepth({hit.x, hit.y, hit.z});
    if (!std::isfinite(depth) || depth + depthEpsilon < surfaceDepth ||
        depth + 1e-5 < bestDepth) continue;
    // Newest sketch wins only for equal-depth hits.
    bestDepth = depth;
    contours = std::move(sketchContours);
    exactContourProfiles = std::move(sketchContourProfiles);
    if (hitArcProfile) {
      exactArcProfile = std::move(*hitArcProfile);
      exactArcPolygon = std::move(hitArcPolygon);
    } else {
      exactArcProfile.clear();
      exactArcPolygon.clear();
    }
    regionSupport = displayed.supportName;
    regionSketchIndex = displayedIndex;
    hoveredExtrusionPlacement_ = displayed.placement;
    exactLineFaces = std::move(validGraphFaces);
    exactLineFacePolygons = std::move(graphFacePolygons);
    lineFacesAreWholeProfile =
        std::none_of(displayed.geometry.circles().begin(),
                     displayed.geometry.circles().end(),
                     [](const sketch::Circle& circle) { return !circle.dashed; }) &&
        std::none_of(displayed.geometry.arcs().begin(),
                     displayed.geometry.arcs().end(),
                     [](const sketch::Arc& arc) { return !arc.dashed; }) &&
        std::none_of(displayed.geometry.beziers().begin(),
                     displayed.geometry.beziers().end(),
                     [](const sketch::Bezier& bezier) {
                       return !bezier.dashed;
                     });
  }
  // No sketch contour was hit: fall through so a finished body's planar face
  // can still be picked as the extrusion source. Without this the extrusion
  // tool could never re-attach to a surface once the sketch contour was moved
  // off the cursor, and the legacy box/cap heuristics below went dead.

  if (!exactArcPolygon.isEmpty()) {
    extrusionHoverPolygon_ = exactArcPolygon;
    extrusionHoverPath_.addPolygon(exactArcPolygon);
    extrusionHoverPath_.closeSubpath();
    hoveredExtrusionSketch_ = std::move(exactArcProfile);
    hoveredExtrusionSupport_ = regionSupport;
    hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутый контур с дугой");
    hoveredExtrusionSketchIndex_ = regionSketchIndex;
    return;
  }

  // Line-graph faces already carry the authoritative sketch-plane points.
  // Keep those exact coordinates instead of projecting to the screen and
  // numerically inverting the projection. That round trip is visually
  // harmless, but it can move a boundary by a few ulps away from a selected
  // source line used as the revolution axis and make OCCT reject the solid.
  if (lineFacesAreWholeProfile && !exactLineFaces.empty() &&
      exactLineFaces.size() == exactLineFacePolygons.size()) {
    int selectedFace = -1;
    double selectedArea = std::numeric_limits<double>::max();
    for (int index = 0;
         index < static_cast<int>(exactLineFacePolygons.size()); ++index) {
      const auto& polygon = exactLineFacePolygons[index];
      if (!polygon.containsPoint(position, Qt::OddEvenFill)) continue;
      const double area = std::abs(signedArea(polygon));
      if (area < selectedArea) {
        selectedArea = area;
        selectedFace = index;
      }
    }
    if (selectedFace >= 0) {
      QPainterPath selectedRegion;
      selectedRegion.setFillRule(Qt::OddEvenFill);
      QPainterPath outerPath;
      outerPath.addPolygon(exactLineFacePolygons[selectedFace]);
      outerPath.closeSubpath();
      selectedRegion.addPolygon(exactLineFacePolygons[selectedFace]);
      selectedRegion.closeSubpath();

      sketch::Sketch selectedGeometry;
      const auto appendBoundary = [&selectedGeometry](
                                      const std::vector<sketch::Point>& face) {
        for (std::size_t index = 0; index < face.size(); ++index)
          selectedGeometry.addLine(face[index],
                                   face[(index + 1) % face.size()]);
      };
      appendBoundary(exactLineFaces[selectedFace]);

      // A disconnected line loop strictly inside the selected loop is a hole.
      // Preserve its exact boundary too. Clicking inside that inner loop picks
      // it as the smaller face above, so it becomes a standalone region.
      for (int index = 0;
           index < static_cast<int>(exactLineFacePolygons.size()); ++index) {
        if (index == selectedFace) continue;
        QPainterPath candidatePath;
        candidatePath.addPolygon(exactLineFacePolygons[index]);
        candidatePath.closeSubpath();
        if (!outerPath.contains(candidatePath) ||
            candidatePath.contains(position))
          continue;
        selectedRegion.addPolygon(exactLineFacePolygons[index]);
        selectedRegion.closeSubpath();
        appendBoundary(exactLineFaces[index]);
      }

      extrusionHoverPolygon_ = exactLineFacePolygons[selectedFace];
      extrusionHoverPath_ = selectedRegion;
      hoveredExtrusionSketch_ = std::move(selectedGeometry);
      hoveredExtrusionSupport_ = regionSupport;
      hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутая область");
      hoveredExtrusionSketchIndex_ = regionSketchIndex;
      return;
    }
  }

  // Screen polygons are only a hit-test representation. When the source
  // contours are disjoint or strictly nested, retain their exact Sketch
  // primitives for Extrude/Revolve instead of inverting QPainterPath's
  // tessellated polygons back into dozens of short lines. In particular, an
  // annulus must stay two analytic circles so its prism has two cylindrical
  // walls rather than a ring of narrow planar faces and visible hatching.
  if (!contours.empty() && contours.size() == exactContourProfiles.size()) {
    std::vector<QPainterPath> contourPaths;
    contourPaths.reserve(contours.size());
    for (const auto& contour : contours) {
      QPainterPath path;
      path.addPolygon(contour);
      path.closeSubpath();
      contourPaths.push_back(std::move(path));
    }

    bool exactSelectionIsUnambiguous = true;
    for (std::size_t first = 0; first < contourPaths.size(); ++first) {
      for (std::size_t second = first + 1; second < contourPaths.size();
           ++second) {
        if (contourPaths[first].intersects(contourPaths[second]) &&
            !contourPaths[first].contains(contourPaths[second]) &&
            !contourPaths[second].contains(contourPaths[first])) {
          exactSelectionIsUnambiguous = false;
          break;
        }
      }
      if (!exactSelectionIsUnambiguous) break;
    }

    std::optional<std::size_t> outerContour;
    double outerArea = std::numeric_limits<double>::max();
    if (exactSelectionIsUnambiguous) {
      for (std::size_t index = 0; index < contours.size(); ++index) {
        if (!contourPaths[index].contains(position)) continue;
        const double area = std::abs(signedArea(contours[index]));
        if (area < outerArea) {
          outerArea = area;
          outerContour = index;
        }
      }
    }

    if (outerContour) {
      QPainterPath selectedRegion;
      selectedRegion.setFillRule(Qt::OddEvenFill);
      selectedRegion.addPolygon(contours[*outerContour]);
      selectedRegion.closeSubpath();
      sketch::Sketch selectedGeometry;
      const auto appendGeometry = [&selectedGeometry](
                                      const sketch::Sketch& geometry) {
        for (const auto& line : geometry.lines())
          if (!line.dashed)
            selectedGeometry.addLine(line.start, line.end);
        for (const auto& circle : geometry.circles())
          if (!circle.dashed)
            selectedGeometry.addCircle(circle.center, circle.radiusMm);
        for (const auto& arc : geometry.arcs())
          if (!arc.dashed)
            selectedGeometry.addArc(arc.center, arc.radiusMm,
                                    arc.startAngleRad, arc.sweepAngleRad);
        for (const auto& bezier : geometry.beziers())
          if (!bezier.dashed)
            selectedGeometry.addBezier(
                bezier.points[0], bezier.points[1], bezier.points[2],
                bezier.points[3]);
      };
      appendGeometry(exactContourProfiles[*outerContour]);

      for (std::size_t index = 0; index < contours.size(); ++index) {
        if (index == *outerContour ||
            contourPaths[index].contains(position) ||
            !contourPaths[*outerContour].contains(contourPaths[index]))
          continue;
        selectedRegion.addPolygon(contours[index]);
        selectedRegion.closeSubpath();
        appendGeometry(exactContourProfiles[index]);
      }

      extrusionHoverPolygon_ = contours[*outerContour];
      extrusionHoverPath_ = std::move(selectedRegion);
      hoveredExtrusionSketch_ = std::move(selectedGeometry);
      hoveredExtrusionSupport_ = regionSupport;
      hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутая область");
      hoveredExtrusionSketchIndex_ = regionSketchIndex;
      return;
    }
  }

  if (contours.size() >= 2 ||
      (contours.size() == 1 && regionSketchIndex < displaySketches_.size() &&
       !planarSketchLineFaces(
            displaySketches_[regionSketchIndex].geometry).empty())) {
    QPainterPath region;
    bool initialized = false;
    for (const auto& contour : contours) {
      QPainterPath path;
      path.addPolygon(contour);
      path.closeSubpath();
      const bool inside = path.contains(position);
      if (inside) {
        region = initialized ? region.intersected(path) : path;
        initialized = true;
      }
    }
    if (initialized) {
      for (const auto& contour : contours) {
        QPainterPath path;
        path.addPolygon(contour);
        path.closeSubpath();
        if (!path.contains(position)) region = region.subtracted(path);
      }
      const auto pieces = region.toSubpathPolygons();
      int selectedPiece = -1;
      double selectedArea = std::numeric_limits<double>::max();
      for (int index = 0; index < pieces.size(); ++index) {
        const auto& piece = pieces[index];
        if (piece.size() < 3 ||
            !piece.containsPoint(position, Qt::OddEvenFill))
          continue;
        const double area = std::abs(signedArea(piece));
        if (area < selectedArea) {
          selectedArea = area;
          selectedPiece = index;
        }
      }
      if (selectedPiece >= 0) {
        const QPolygonF& piece = pieces[selectedPiece];
        QPainterPath selectedRegion;
        selectedRegion.setFillRule(Qt::OddEvenFill);
        selectedRegion.addPolygon(piece);
        selectedRegion.closeSubpath();
        // Preserve nested boundaries as holes. Thus clicking the outer circle
        // selects an annulus, while clicking inside the inner circle selects
        // only the disk. Ctrl can then combine both explicitly.
        for (int index = 0; index < pieces.size(); ++index) {
          if (index == selectedPiece || pieces[index].size() < 3) continue;
          if (!piece.containsPoint(pieces[index].boundingRect().center(),
                                   Qt::OddEvenFill))
            continue;
          selectedRegion.addPolygon(pieces[index]);
          selectedRegion.closeSubpath();
        }
        extrusionHoverPolygon_ = piece;
        extrusionHoverPath_ = selectedRegion;
        hoveredExtrusionSupport_ = regionSupport;
        hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутая область");
        hoveredExtrusionSketchIndex_ = regionSketchIndex;

        // Convert the selected screen-space boundary back to sketch-plane
        // coordinates so the extrusion preview follows this exact region.
        const auto pointOnRegion = [&](sketch::Point point) {
          return pointOnPlacement(point,
              displaySketches_[regionSketchIndex].placement, offsetX_, offsetY_);
        };
        Point3 modelOrigin = pointOnRegion({0, 0});
        Point3 modelU = pointOnRegion({1, 0});
        Point3 modelV = pointOnRegion({0, 1});
        const QPointF origin = project(modelOrigin, size(), yaw_, pitch_, zoom_);
        const QPointF uPoint = project(modelU, size(), yaw_, pitch_, zoom_);
        const QPointF vPoint = project(modelV, size(), yaw_, pitch_, zoom_);
        const QPointF u = uPoint - origin;
        const QPointF v = vPoint - origin;
        const double determinant = u.x() * v.y() - u.y() * v.x();
        if (std::abs(determinant) > 1e-9) {
          for (const auto& boundary : selectedRegion.toSubpathPolygons()) {
            std::vector<sketch::Point> modelPoints;
            modelPoints.reserve(static_cast<std::size_t>(boundary.size()));
            for (const QPointF& screenPoint : boundary) {
              const QPointF delta = screenPoint - origin;
              modelPoints.push_back({
                  (delta.x() * v.y() - delta.y() * v.x()) / determinant,
                  (u.x() * delta.y() - u.y() * delta.x()) / determinant});
            }
            if (modelPoints.size() > 1 &&
                QLineF(boundary.front(), boundary.back()).length() < 0.01)
              modelPoints.pop_back();
            for (std::size_t index = 0; index < modelPoints.size(); ++index)
              hoveredExtrusionSketch_.addLine(
                  modelPoints[index],
                  modelPoints[(index + 1) % modelPoints.size()]);
          }
        }
        return;
      }
    }
  }

  std::size_t reverseIndex = displaySketches_.size();
  for (auto displayed = displaySketches_.rbegin();
       displayed != displaySketches_.rend() && extrusionHoverPolygon_.isEmpty();
       ++displayed) {
    const std::size_t displayedIndex = --reverseIndex;
    if (displayedIndex != regionSketchIndex) continue;
    std::vector<std::size_t> ids;
    for (const auto& line : displayed->geometry.lines())
      if (!line.dashed &&
          std::find(ids.begin(), ids.end(), line.elementId) == ids.end())
        ids.push_back(line.elementId);
    for (const std::size_t id : ids) {
      QPolygonF polygon;
      sketch::Sketch candidate;
      for (const auto& line : displayed->geometry.lines()) {
        if (line.dashed || line.elementId != id) continue;
        polygon << project(pointOnPlacement(line.start, displayed->placement,
                                            offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
        candidate.addLine(line.start, line.end);
      }
      if (polygon.size() < 3 || !candidate.isClosed() ||
          std::abs(signedArea(polygon)) <= 1e-6 ||
          !polygon.containsPoint(position, Qt::OddEvenFill))
        continue;
      extrusionHoverPolygon_ = polygon;
      hoveredExtrusionSketch_ = candidate;
      hoveredExtrusionPlacement_ = displayed->placement;
      hoveredExtrusionSupport_ = displayed->supportName;
      hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутый контур эскиза");
      hoveredExtrusionSketchIndex_ = displayedIndex;
      break;
    }
    for (const auto& circle : displayed->geometry.circles()) {
      if (!extrusionHoverPolygon_.isEmpty()) break;
      if (circle.dashed || !std::isfinite(circle.radiusMm) ||
          circle.radiusMm <= 0) continue;
      QPolygonF polygon;
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point point{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        polygon << project(pointOnPlacement(point, displayed->placement,
                                            offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
      }
      if (!polygon.containsPoint(position, Qt::OddEvenFill)) continue;
      sketch::Sketch candidate;
      candidate.addCircle(circle.center, circle.radiusMm);
      extrusionHoverPolygon_ = polygon;
      hoveredExtrusionSketch_ = candidate;
      hoveredExtrusionPlacement_ = displayed->placement;
      hoveredExtrusionSupport_ = displayed->supportName;
      hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутый контур эскиза");
      hoveredExtrusionSketchIndex_ = displayedIndex;
      break;
    }
  }

  if (!extrusionHoverPolygon_.isEmpty() || !solidVisible_) return;
  pickFallbackBodyFace(position);
}

void Viewport::pickFallbackBodyFace(QPointF position) {
  // A real parametric body offers its actual B-Rep faces as extrusion sources.
  // Resolve the frontmost mesh face under the cursor (the same depth rule the
  // face hover uses) instead of reconstructing a legacy wireframe box.
  if (bodyShape_ && !bodyShape_->IsNull() && hasDisplayedBodyTriangles()) {
    const auto& scene = pickingScene();
    const auto hit = scene.faceAt(position);
    if (!hit) return;
    const std::size_t faceIndex = hit->faceIndex;
    hoveredBodyFaceIndex_ = faceIndex;
    QPainterPath facePath;
    for (const auto& triangle : scene.trianglesForFace(faceIndex)) {
      QPolygonF polygon{triangle.a.screen, triangle.b.screen,
                        triangle.c.screen};
      facePath.addPolygon(polygon);
    }
    const QPolygonF merged = facePath.simplified().toFillPolygons().isEmpty()
        ? QPolygonF{}
        : facePath.simplified().toFillPolygons().front();
    if (merged.size() < 3) return;
    extrusionHoverPolygon_ = merged;
    extrusionHoverPath_ = facePath.simplified();
    hoveredExtrusionSurface_ =
        QString::fromUtf8("Грань тела #%1").arg(faceIndex + 1);
    hoveredExtrusionSupport_ = hoveredExtrusionSurface_;
    hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
    hoveredExtrusionOnBodyCap_ = false;
    return;
  }

  // Legacy solid (box + cached extrusion profile). A finished body's planar end
  // caps are valid extrusion sources too. Keep the picked polygon in screen
  // space so the manipulator starts exactly on the visible face.
  if (!solidSketch_.lines().empty() || !solidSketch_.circles().empty()) {
    QPolygonF bottomCap;
    QPolygonF topCap;
    const Vector3d placementNormal = solidSketchPlacement_.normal();
    const Point3 normal{static_cast<float>(placementNormal.x),
                        static_cast<float>(placementNormal.y),
                        static_cast<float>(placementNormal.z)};
    const float bodyLength = static_cast<float>(box_.heightMm);
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point point{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnPlacement(
            point, solidSketchPlacement_, offsetX_, offsetY_);
        bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
        topCap << project(translated(base, normal, bodyLength), size(), yaw_,
                          pitch_, zoom_);
      }
    } else {
      for (const auto& line : solidSketch_.lines()) {
        const Point3 base = pointOnPlacement(
            line.start, solidSketchPlacement_, offsetX_, offsetY_);
        bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
        topCap << project(translated(base, normal, bodyLength), size(), yaw_,
                          pitch_, zoom_);
      }
    }
    const std::array<QPolygonF, 2> caps{bottomCap, topCap};
    for (int cap = 1; cap >= 0; --cap) {
      if (caps[cap].size() < 3 || !isFrontFacing(caps[cap]) ||
          !caps[cap].containsPoint(position, Qt::OddEvenFill))
        continue;
      extrusionHoverPolygon_ = caps[cap];
      hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") +
                                 (cap == 1 ? QString::fromUtf8("Торцевая")
                                           : QString::fromUtf8("Начальная"));
      hoveredExtrusionSupport_ = solidSupportName_;
      hoveredExtrusionReverse_ = cap == 0;
      hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
      hoveredExtrusionOnBodyCap_ = (cap == 1);
      hoveredExtrusionSketch_ = solidSketch_;
      hoveredLegacySolidFace_ = legacyCapPick(
          solidSketch_, solidSketchPlacement_, box_.heightMm, cap == 1);
      hoveredExtrusionPlacement_ = hoveredLegacySolidFace_->placement;
      return;
    }
  }

  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);
  const std::array<Point3, 8> vertices{{
      {-x + offsetX_, -y + offsetY_, 0}, {x + offsetX_, -y + offsetY_, 0},
      {x + offsetX_, y + offsetY_, 0}, {-x + offsetX_, y + offsetY_, 0},
      {-x + offsetX_, -y + offsetY_, z}, {x + offsetX_, -y + offsetY_, z},
      {x + offsetX_, y + offsetY_, z}, {-x + offsetX_, y + offsetY_, z}}};
  const std::array<std::array<int, 4>, 6> faces{{
      {{0, 1, 2, 3}}, {{4, 7, 6, 5}}, {{0, 4, 5, 1}},
      {{1, 5, 6, 2}}, {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
  static const std::array<const char*, 6> names{
      "Нижняя", "Верхняя", "Передняя", "Правая", "Задняя", "Левая"};
  for (int face = 5; face >= 0; --face) {
    QPolygonF polygon;
    for (int vertex : faces[face])
      polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
    if (!isFrontFacing(polygon) ||
        !polygon.containsPoint(position, Qt::OddEvenFill))
      continue;
    extrusionHoverPolygon_ = polygon;
    hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") +
                               QString::fromUtf8(names[face]);
    hoveredExtrusionSupport_ = QString::fromUtf8(names[face]);
    hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
    hoveredExtrusionOnBodyCap_ = false;
    hoveredLegacySolidFace_ = legacyBoxFacePick(face, box_);
    hoveredExtrusionSketch_ = hoveredLegacySolidFace_->geometry;
    hoveredExtrusionPlacement_ = hoveredLegacySolidFace_->placement;
    break;
  }
}

void Viewport::scheduleHover(QPointF viewPosition) {
  pendingHoverPosition_ = viewPosition;
  pendingHoverGeneration_ = sceneGeneration_;
  if (hoverFrameTimer_ && !hoverFrameTimer_->isActive())
    hoverFrameTimer_->start();
}

void Viewport::flushPendingHover(std::optional<QPointF> exactPosition) {
  if (hoverFrameTimer_) hoverFrameTimer_->stop();
  if (exactPosition) {
    pendingHoverPosition_ = *exactPosition;
    pendingHoverGeneration_ = sceneGeneration_;
  }
  if (!pendingHoverPosition_) return;
  if (pendingHoverGeneration_ != sceneGeneration_) {
    pendingHoverPosition_.reset();
    return;
  }
  const QPointF position = *pendingHoverPosition_;
  pendingHoverPosition_.reset();
  processHoverAt(position, exactPosition.has_value());
}

void Viewport::invalidatePendingHover() {
  ++sceneGeneration_;
  pendingHoverPosition_.reset();
  if (hoverFrameTimer_) hoverFrameTimer_->stop();
}

void Viewport::processHoverAt(QPointF viewPosition, bool exact) {
  const QPointF scenePosition = viewPosition - cameraPan_;
  if (pickMode_ == PickMode::Ruler) {
    static_cast<void>(ruler_.updateHover(pickingScene(), scenePosition, exact));
    setCursor(ruler_.hoverPoint() ? Qt::PointingHandCursor : Qt::CrossCursor);
    update();
    return;
  }
  if (pickMode_ == PickMode::MirrorBody ||
      pickMode_ == PickMode::MoveBody ||
      pickMode_ == PickMode::JoinBodies ||
      pickMode_ == PickMode::LinearPatternBody ||
      pickMode_ == PickMode::CircularPatternBody) {
    updateToolBodyHover(scenePosition);
    setCursor(hoveredToolBodyId_ != kInvalidBodyId ? Qt::PointingHandCursor
                                                   : Qt::CrossCursor);
    update();
    return;
  }
  if (pickMode_ == PickMode::MirrorPlane) {
    updateMirrorPlaneHover(scenePosition);
    setCursor(selectedBasePlane_ >= 0 ? Qt::PointingHandCursor
                                      : Qt::CrossCursor);
    update();
    return;
  }
  if (pickMode_ == PickMode::MirrorPreview ||
      pickMode_ == PickMode::MovePreview ||
      pickMode_ == PickMode::LinearPatternPreview ||
      pickMode_ == PickMode::CircularPatternPreview ||
      pickMode_ == PickMode::DraftPreview) {
    hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
    hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    hoveredToolBodyId_ = kInvalidBodyId;
    unsetCursor();
    update();
    return;
  }
  if (pickMode_ == PickMode::LinearPatternAxis ||
      pickMode_ == PickMode::CircularPatternAxis) {
    const auto previous = hoveredRevolveAxisToken_;
    hoveredRevolveAxisToken_ = principalAxisTokenAt(scenePosition);
    setCursor(hoveredRevolveAxisToken_ != 0 ? Qt::PointingHandCursor
                                            : Qt::CrossCursor);
    if (previous != hoveredRevolveAxisToken_) update();
    return;
  }
  if (pickMode_ == PickMode::DraftAxis) {
    const auto previous = hoveredRevolveAxisToken_;
    const auto previousEdge = hoveredBodyEdgeIndex_;
    hoveredRevolveAxisToken_ = principalAxisTokenAt(scenePosition);
    if (hoveredRevolveAxisToken_ != 0) {
      hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
      hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
    } else {
      updateBodyHover(scenePosition, exact);
      hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
    }
    const bool hasCandidate =
        hoveredRevolveAxisToken_ != 0 ||
        hoveredBodyEdgeIndex_ != static_cast<std::size_t>(-1);
    setCursor(hasCandidate ? Qt::PointingHandCursor : Qt::CrossCursor);
    if (previous != hoveredRevolveAxisToken_ ||
        previousEdge != hoveredBodyEdgeIndex_)
      update();
    return;
  }
  if (pickMode_ == PickMode::DraftFace) {
    updateBodyHover(scenePosition, exact);
    setCursor(hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1)
                  ? Qt::PointingHandCursor
                  : Qt::CrossCursor);
    update();
    return;
  }
  if (pickMode_ == PickMode::RevolveAxis) {
    const auto previous = hoveredRevolveAxisToken_;
    hoveredRevolveAxisToken_ = revolveAxisTokenAt(scenePosition);
    if (hoveredRevolveAxisToken_ != 0) {
      extrusionHoverPolygon_.clear();
      extrusionHoverPath_ = {};
    } else {
      updateExtrusionHover(scenePosition);
      if (hoveredExtrusionSketchIndex_ == static_cast<std::size_t>(-1)) {
        extrusionHoverPolygon_.clear();
        extrusionHoverPath_ = {};
      }
    }
    setCursor(hoveredRevolveAxisToken_ != 0 ||
                      !extrusionHoverPolygon_.isEmpty()
                  ? Qt::PointingHandCursor
                  : Qt::CrossCursor);
    if (previous != hoveredRevolveAxisToken_ ||
        hoveredRevolveAxisToken_ == 0)
      update();
    return;
  }
  if (pickMode_ == PickMode::ExtrusionSurface) {
    updateExtrusionHover(scenePosition);
    update();
    return;
  }
  if (pickMode_ == PickMode::SketchPlane ||
      pickMode_ == PickMode::ImagePlane) {
    updateSketchPlaneHover(scenePosition);
    update();
    return;
  }
  updateBodyHover(scenePosition, exact);
  update();
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
  if (cubePressed_) { event->accept(); return; }
  if (marqueeActive_ && event->buttons().testFlag(Qt::LeftButton)) {
    marqueeCurrent_ = event->position() - cameraPan_;
    update();
    event->accept();
    return;
  }
  if (event->buttons() == Qt::NoButton) {
    const auto hit = viewCubeGeometry(size(), {yaw_,pitch_}).hitTest(event->position());
    if (hit) {
      if (!cubeHover_) cursorBeforeCube_ = cursor().shape();
      if (!(cubeHover_ == hit)) QToolTip::showText(event->globalPosition().toPoint(), viewCubeToolTip(hit), this);
      cubeHover_ = hit;
      setCursor(Qt::PointingHandCursor);
      hoveredBodyFaceIndex_ = hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
      update(); event->accept(); return;
    }
    clearCubeHover();
  }
  if (event->buttons() == Qt::NoButton) {
    scheduleHover(event->position());
    event->accept();
    return;
  }
  if (draggingAngularToolManipulator_ && angularToolManipulator_ &&
      event->buttons().testFlag(Qt::LeftButton)) {
    if (const auto visual = angularVisual()) {
      const Point3d originWorld = angularToolManipulator_->origin;
      const QPointF uPoint = projectBodyPoint(
          offsetPoint(originWorld, visual->u, visual->visualRadiusMm),
          originWorld, size(), yaw_, pitch_, zoom_).screen;
      const QPointF vPoint = projectBodyPoint(
          offsetPoint(originWorld, visual->v, visual->visualRadiusMm),
          originWorld, size(), yaw_, pitch_, zoom_).screen;
      // A degenerate (edge-on) projected basis reports an indeterminate angle.
      // Skip the update/emit so the currently accepted angle is preserved
      // instead of being overwritten with a fabricated 360°/88.99° jump.
      if (const auto resolved = angularValueFromProjectedBasis(
              event->position() - cameraPan_, visual->origin, uPoint, vPoint,
              angularToolManipulator_->minimumDeg,
              angularToolManipulator_->maximumDeg)) {
        angularToolManipulator_->angleDeg = *resolved;
        toolParameterHud_->setValue("angle", angularToolManipulator_->angleDeg);
        emit angularToolManipulatorValueChanged(
            angularToolManipulator_->angleDeg);
        update();
      }
      return;
    }
  }
  if (draggingTranslationToolManipulator_ && translationToolManipulator_ &&
      activeTranslationAxis_ >= 0 && activeTranslationAxis_ < 3 &&
      event->buttons().testFlag(Qt::LeftButton)) {
    if (linearDragSnapshot_) {
      const double value = linearValueFromDrag(
          *linearDragSnapshot_, event->position() - cameraPan_,
          translationToolManipulator_->minimumMm,
          translationToolManipulator_->maximumMm);
      if (activeTranslationAxis_ == 0)
        translationToolManipulator_->offsetMm.x = value;
      else if (activeTranslationAxis_ == 1)
        translationToolManipulator_->offsetMm.y = value;
      else
        translationToolManipulator_->offsetMm.z = value;
      if (referenceImageManipulatorActive_)
        emit referenceImageParameterChanged(activeTranslationAxis_, value);
      else
        emit translationToolManipulatorValueChanged(activeTranslationAxis_,
                                                    value);
      update();
    }
    return;
  }
  if (draggingToolManipulator_ && toolManipulator_ &&
      event->buttons().testFlag(Qt::LeftButton)) {
    if (linearDragSnapshot_) {
      toolManipulator_->valueMm = linearValueFromDrag(
          *linearDragSnapshot_, event->position() - cameraPan_,
          toolManipulator_->minimumMm, toolManipulator_->maximumMm);
      emit toolManipulatorValueChanged(toolManipulator_->valueMm);
      update();
    }
    return;
  }
  if (panningView_ && event->buttons().testFlag(Qt::MiddleButton)) {
    const QPoint delta = event->position().toPoint() - lastMousePosition_;
    cameraPan_ += QPointF(delta);
    lastMousePosition_ = event->position().toPoint();
    if (extrusionManipulatorVisible_)
      setExtrusionPreviewLength(extrusionPreviewLengthMm_);
    update();
    return;
  }
  if (draggingExtrusionHandle_ && event->buttons().testFlag(Qt::LeftButton)) {
    const QPointF axis = extrusionScreenOffset(1.0);
    const double axisLengthSquared = QPointF::dotProduct(axis, axis);
    if (axisLengthSquared > 0.0001) {
      const QPointF mouseOffset = event->position() - cameraPan_ -
                                  extrusionManipulatorAnchor_;
      setExtrusionPreviewLength(
          QPointF::dotProduct(mouseOffset, axis) / axisLengthSquared);
    }
    return;
  }
  if (draggingBody_ && event->buttons().testFlag(Qt::LeftButton)) {
    const QPoint delta = event->position().toPoint() - lastMousePosition_;
    const float scale = std::max(0.01F, std::min(width(), height()) * 0.008F * zoom_);
    offsetX_ += static_cast<float>(delta.x()) / scale;
    offsetY_ -= static_cast<float>(delta.y()) / scale;
    lastMousePosition_ = event->position().toPoint();
    update();
  } else if (event->buttons().testFlag(Qt::RightButton) ||
             (event->buttons().testFlag(Qt::LeftButton) && !solidVisible_)) {
    const QPoint delta = event->position().toPoint() - lastMousePosition_;
    const Point3d orbitCenter = displayedBodyCenter();
    const QPointF centerBefore = projectBodyPoint(
        orbitCenter, orbitCenter, size(), yaw_, pitch_, zoom_).screen;
    yaw_ += static_cast<float>(delta.x()) * 0.5F;
    pitch_ = std::clamp(pitch_ + static_cast<float>(delta.y()) * 0.5F,
                        -179.5F, 179.5F);
    const QPointF centerAfter = projectBodyPoint(
        orbitCenter, orbitCenter, size(), yaw_, pitch_, zoom_).screen;
    cameraPan_ += centerBefore - centerAfter;
    lastMousePosition_ = event->position().toPoint();
    update();
  }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
  flushPendingHover(event->position());
  if (event->button() == Qt::LeftButton && cubePressed_) {
    const auto hit = viewCubeGeometry(size(), {yaw_,pitch_}).hitTest(event->position());
    const auto pressed = cubePressed_;
    cubePressed_ = {};
    if (hit == pressed) {
      if (hit.zone == ViewCubeZone::Fit) fitAll();
      else if (hit.zone == ViewCubeZone::RotateCounterClockwise)
        animateOrientation({yaw_-45.0F,pitch_});
      else if (hit.zone == ViewCubeZone::RotateClockwise)
        animateOrientation({yaw_+45.0F,pitch_});
      else animateOrientation(hit.zone == ViewCubeZone::Home ?
          orientationFor(StandardView::Isometric) : orientationForDirection(hit.direction));
    }
    clearCubeHover();
    event->accept(); return;
  }
  if (event->button() == Qt::LeftButton && marqueeActive_) {
    marqueeCurrent_ = event->position() - cameraPan_;
    const QRectF marqueeRect(marqueeStart_, marqueeCurrent_);
    const QRectF normalized = marqueeRect.normalized();
    const bool additive = marqueeAdditive_;
    cancelMarquee();
    if (normalized.width() >= kMarqueeMinSizePx ||
        normalized.height() >= kMarqueeMinSizePx) {
      selectInRect(normalized, additive);
    } else if (!additive) {
      // A sub-threshold drag is a click on empty area: preserve the existing
      // empty-click clear semantics (selection was already cleared at press).
      selectedFace_ = -1;
      selectedBodyFaceIndices_.clear();
      selectedBodyFaceReferences_.clear();
      selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
      selectedBodyEdgeIndices_.clear();
      selectedBodyEdgeReferences_.clear();
      clearWholeBodySelection();
      emit selectionChanged({});
    }
    event->accept();
    return;
  }
  if (event->button() == Qt::MiddleButton && panningView_) {
    panningView_ = false;
    unsetCursor();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && draggingExtrusionHandle_) {
    draggingExtrusionHandle_ = false;
    unsetCursor();
    emit extrusionManipulatorDragFinished();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && draggingToolManipulator_) {
    draggingToolManipulator_ = false;
    linearDragSnapshot_.reset();
    unsetCursor();
    emit toolManipulatorDragFinished();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton &&
      draggingTranslationToolManipulator_) {
    draggingTranslationToolManipulator_ = false;
    activeTranslationAxis_ = -1;
    linearDragSnapshot_.reset();
    unsetCursor();
    emit toolManipulatorDragFinished();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && draggingAngularToolManipulator_) {
    draggingAngularToolManipulator_ = false;
    unsetCursor();
    emit toolManipulatorDragFinished();
    event->accept(); return;
  }
  if (event->button() == Qt::LeftButton && draggingBody_) {
    draggingBody_ = false;
    const QPointF current(offsetX_, offsetY_);
    if (QLineF(bodyDragStart_, current).length() > 0.0001)
      emit bodyMoveCommitted(bodyDragStart_, current);
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void Viewport::wheelEvent(QWheelEvent* event) {
  orientationAnimation_->stop();
  const int wheelDelta = event->angleDelta().y();
  if (wheelDelta == 0) {
    event->ignore();
    return;
  }
  zoom_ = steppedViewportZoom(zoom_, wheelDelta);
  lastMousePosition_ = event->position().toPoint();

  // Hover and selected extrusion contours are cached in screen coordinates.
  // Reproject them immediately at the wheel cursor; waiting for MouseMove
  // leaves the blue candidate at its pre-zoom size while the model changes.
  refreshSelectedExtrusionPolygon();
  pendingHoverPosition_.reset();
  if (hoverFrameTimer_) hoverFrameTimer_->stop();
  processHoverAt(event->position());
  event->accept();
  update();
}
bool Viewport::eventFilter(QObject* watched, QEvent* event) {
  if (watched != extrusionLengthEditor_)
    return QOpenGLWidget::eventFilter(watched, event);

  if (event->type() == QEvent::ShortcutOverride) {
    const auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
        key->key() == Qt::Key_Escape || key->key() == Qt::Key_Tab ||
        key->key() == Qt::Key_Backtab) {
      event->accept();
      return true;
    }
    return QOpenGLWidget::eventFilter(watched, event);
  }

  if (event->type() != QEvent::KeyPress)
    return QOpenGLWidget::eventFilter(watched, event);

  const auto* key = static_cast<QKeyEvent*>(event);
  // The legacy extrusion editor cancels the active tool directly through the
  // same MainWindow route used by viewport Escape.
  if (key->key() == Qt::Key_Escape) {
    if (!key->isAutoRepeat()) cancelActiveInteraction();
    return true;
  }
  if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
    // Extrude has one on-canvas numeric field. Keep Tab inside the CAD HUD
    // instead of escaping to the right-hand tool panel.
    extrusionLengthEditor_->setFocus(Qt::TabFocusReason);
    extrusionLengthEditor_->selectAll();
    return true;
  }

  if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
    if (key->isAutoRepeat()) return true;
    extrusionLengthEditor_->interpretText();
    setExtrusionPreviewLength(extrusionLengthEditor_->value());
    emit toolParameterCommitted();
    return true;
  }

  return QOpenGLWidget::eventFilter(watched, event);
}

void Viewport::cancelActiveInteraction() {
  // Escape must cancel the whole transient interaction, regardless of whether
  // keyboard focus currently belongs to the viewport or an on-canvas editor.
  const ViewportCancelReason reason =
      pickMode_ == PickMode::SketchPlane
          ? ViewportCancelReason::SketchPlaneSelection
      : pickMode_ == PickMode::ImagePlane
          ? ViewportCancelReason::ImagePlaneSelection
          : pickMode_ == PickMode::ExtrusionSurface ||
                    pickMode_ == PickMode::RevolveAxis ||
                    pickMode_ == PickMode::MirrorBody ||
                    pickMode_ == PickMode::MirrorPlane ||
                    pickMode_ == PickMode::MoveBody ||
                    pickMode_ == PickMode::JoinBodies ||
                    pickMode_ == PickMode::LinearPatternBody ||
                    pickMode_ == PickMode::LinearPatternAxis ||
                    pickMode_ == PickMode::CircularPatternBody ||
                    pickMode_ == PickMode::CircularPatternAxis ||
                    pickMode_ == PickMode::DraftFace ||
                    pickMode_ == PickMode::DraftAxis
                ? ViewportCancelReason::NestedReselection
                : ViewportCancelReason::ActiveTool;
  resetToolInteraction();
  emit interactionCancelled(reason);
}

void Viewport::resetToolInteraction() {
  invalidatePendingHover();
  const bool rulerWasActive = ruler_.active();
  ruler_.cancel();
  const bool wasConstructionPlane =
      pickMode_ == PickMode::SketchPlane ||
      pickMode_ == PickMode::ImagePlane ||
      pickMode_ == PickMode::MirrorPlane ||
      pickMode_ == PickMode::MirrorPreview;
  pickMode_ = PickMode::None;
  setSelectionFilter(SelectionFilter::Any);
  clearLegacyExtrusionPreview();
  clearToolPreviewShape();
  clearToolManipulator();
  cancelMarquee();
  hoveredToolBodyId_ = kInvalidBodyId;
  selectedBasePlane_ = -1;
  selectedPatternAxis_ = -1;
  selectedDraftAxisEdge_.reset();
  hoveredRevolveAxisToken_ = 0;
  if (wasConstructionPlane)
    for (bool& visible : basePlanesVisible_) visible = false;
  unsetCursor();
  if (rulerWasActive) emit rulerActiveChanged(false);
  update();
}
void Viewport::keyPressEvent(QKeyEvent* event) {
  // CAD Tab workflow is intentionally restricted to numeric fields drawn in
  // the viewport. The right-hand tool panel remains mouse-accessible, but it
  // is not part of this CAD parameter loop.
  if ((event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) &&
      (pickMode_ == PickMode::None ||
       pickMode_ == PickMode::MovePreview ||
       pickMode_ == PickMode::LinearPatternPreview ||
       pickMode_ == PickMode::CircularPatternPreview ||
       pickMode_ == PickMode::DraftPreview)) {
    const bool backward = event->key() == Qt::Key_Backtab ||
                          event->modifiers().testFlag(Qt::ShiftModifier);

    if (focusToolParameterField(backward)) {
      event->accept();
      return;
    }

    // No on-canvas numeric field: consume Tab so focus cannot leak into dock
    // controls that are outside the manipulator workflow.
    event->accept();
    return;
  }  // Ctrl+A selects the eligible visible domain: edges in the Edge tool, faces
  // in Face/normal mode. Multi-selection is only honored when the matching
  // tool's multi-select mode is on; otherwise only the frontmost entity is
  // selected so SelectAll cannot bypass a single-select tool contract.
  // In normal mode (Any filter, no multi-select) the selection domain is whole
  // bodies (model-level entities), not faces.
  if (event->matches(QKeySequence::SelectAll) &&
      pickMode_ == PickMode::None) {
    cancelMarquee();
    // Normal mode: select all distinct visible bodies and publish the body
    // selection. MainWindow consumption of bodiesSelected is deferred.
    if (selectionFilter_ == SelectionFilter::Any && !faceMultiSelectionMode_ &&
        !edgeMultiSelectionMode_) {
      std::vector<BodyId> distinctIds;
      if (solidVisible_) {
        for (const auto& shape : bodyViewShapes_) {
          if (shape.bodyId == kInvalidBodyId) continue;
          if (std::find(distinctIds.begin(), distinctIds.end(), shape.bodyId) ==
              distinctIds.end())
            distinctIds.push_back(shape.bodyId);
        }
      }
      setSelectedBodies(distinctIds);
      event->accept();
      return;
    }
    const bool wantsEdges = selectionFilter_ == SelectionFilter::Edge;
    const bool multiSelect = wantsEdges ? edgeMultiSelectionMode_
                                        : faceMultiSelectionMode_;
    // projectBodyPoint is pan-free (camera pan = {}), so the rect is offset by
    // -cameraPan_ to match the projected geometry at any pan/zoom.
    const QRectF fullRect(QPointF(0, 0) - cameraPan_, QSizeF(size()));
    selectInRect(fullRect, /*additive=*/false, /*singleOnly=*/!multiSelect);
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Control && extrusionManipulatorVisible_) {
    hideExtrusionManipulator();
    pickMode_ = PickMode::ExtrusionSurface;
    extrusionHoverPolygon_.clear();
    extrusionHoverPath_ = {};
    setCursor(Qt::CrossCursor);
    update();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    cancelActiveInteraction();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

}  // namespace solidar

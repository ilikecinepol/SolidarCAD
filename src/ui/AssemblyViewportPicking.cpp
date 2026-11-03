#include "ui/AssemblyViewportPicking.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <numbers>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ui/RigidTransform3dOps.h"

namespace solidar {
namespace {

struct Vec3 final {
  double x{};
  double y{};
  double z{};
};

[[nodiscard]] bool finite(Vec3 value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] Vec3 vector(Point3d value) noexcept {
  return {value.x, value.y, value.z};
}

[[nodiscard]] Point3d point(Vec3 value) noexcept {
  return {value.x, value.y, value.z};
}

[[nodiscard]] Vec3 operator+(Vec3 left, Vec3 right) noexcept {
  return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] Vec3 operator-(Vec3 left, Vec3 right) noexcept {
  return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] Vec3 operator*(Vec3 value, double scalar) noexcept {
  return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] double dot(Vec3 left, Vec3 right) noexcept {
  return std::fma(left.x, right.x,
                  std::fma(left.y, right.y, left.z * right.z));
}

[[nodiscard]] Vec3 cross(Vec3 left, Vec3 right) noexcept {
  return {left.y * right.z - left.z * right.y,
          left.z * right.x - left.x * right.z,
          left.x * right.y - left.y * right.x};
}

[[nodiscard]] double squaredLength(Vec3 value) noexcept {
  return dot(value, value);
}

struct PickingRay final {
  Vec3 origin;
  Vec3 direction;
  Vec3 view;
  double length{};
  double nearDepth{};
  double scale{};
};

[[nodiscard]] std::optional<PickingRay> makeRay(
    const ViewportCameraState& camera, QPointF pixel) noexcept {
  if (!std::isfinite(camera.yawDeg) || !std::isfinite(camera.pitchDeg) ||
      !std::isfinite(camera.zoom) || camera.zoom <= 0.0F ||
      !std::isfinite(camera.pan.x()) || !std::isfinite(camera.pan.y()) ||
      camera.logicalSize.width() <= 0 || camera.logicalSize.height() <= 0 ||
      !std::isfinite(camera.devicePixelRatio) ||
      camera.devicePixelRatio <= 0.0F || !finite(vector(camera.center)) ||
      !std::isfinite(camera.depthExtent) || camera.depthExtent <= 0.0 ||
      !std::isfinite(pixel.x()) || !std::isfinite(pixel.y()))
    return std::nullopt;

  const double width = camera.logicalSize.width();
  const double height = camera.logicalSize.height();
  const double scale = std::min(width, height) * 0.008 * camera.zoom;
  const double depthExtent = std::max(1.0, camera.depthExtent);
  if (!std::isfinite(scale) || scale <= 0.0 ||
      !std::isfinite(depthExtent * 2.0))
    return std::nullopt;

  const double yaw = camera.yawDeg * std::numbers::pi / 180.0;
  const double pitch = camera.pitchDeg * std::numbers::pi / 180.0;
  const double cy = std::cos(yaw);
  const double sy = std::sin(yaw);
  const double cp = std::cos(pitch);
  const double sp = std::sin(pitch);
  const Vec3 right{cy, -sy, 0.0};
  const Vec3 up{-sy * cp, -cy * cp, sp};
  const Vec3 view{sy * sp, cy * sp, cp};
  const double rightCoordinate =
      (pixel.x() - width * 0.5 - camera.pan.x()) / scale;
  const double upCoordinate =
      (height * 0.52 + camera.pan.y() - pixel.y()) / scale;
  const double centerDepth = dot(vector(camera.center), view);
  const double nearDepth = centerDepth + depthExtent;
  const Vec3 origin = right * rightCoordinate + up * upCoordinate +
                      view * nearDepth;
  const PickingRay ray{origin, view * -1.0, view, depthExtent * 2.0,
                       nearDepth, scale};
  if (!finite(ray.origin) || !finite(ray.direction) || !finite(ray.view) ||
      !std::isfinite(ray.nearDepth))
    return std::nullopt;
  return ray;
}

[[nodiscard]] bool rayIntersectsBounds(const PickingRay& ray,
                                       const Bounds3d& bounds,
                                       double expansion = 0.0) noexcept {
  if (!std::isfinite(expansion) || expansion < 0.0) return false;
  const Point3d minimum = bounds.minimum();
  const Point3d maximum = bounds.maximum();
  const std::array<double, 3> origin{ray.origin.x, ray.origin.y, ray.origin.z};
  const std::array<double, 3> direction{ray.direction.x, ray.direction.y,
                                        ray.direction.z};
  const std::array<double, 3> low{minimum.x - expansion,
                                  minimum.y - expansion,
                                  minimum.z - expansion};
  const std::array<double, 3> high{maximum.x + expansion,
                                   maximum.y + expansion,
                                   maximum.z + expansion};
  double entry = 0.0;
  double exit = ray.length;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(low[axis]) || !std::isfinite(high[axis])) return false;
    if (std::abs(direction[axis]) <= 1e-15) {
      if (origin[axis] < low[axis] || origin[axis] > high[axis]) return false;
      continue;
    }
    double a = (low[axis] - origin[axis]) / direction[axis];
    double b = (high[axis] - origin[axis]) / direction[axis];
    if (a > b) std::swap(a, b);
    entry = std::max(entry, a);
    exit = std::min(exit, b);
    if (entry > exit) return false;
  }
  return exit >= 0.0 && entry <= ray.length;
}

[[nodiscard]] Vec3 inversePoint(const std::array<double, 16>& matrix,
                                Vec3 world) noexcept {
  const Vec3 shifted{world.x - matrix[3], world.y - matrix[7],
                     world.z - matrix[11]};
  return {std::fma(matrix[0], shifted.x,
                   std::fma(matrix[4], shifted.y, matrix[8] * shifted.z)),
          std::fma(matrix[1], shifted.x,
                   std::fma(matrix[5], shifted.y, matrix[9] * shifted.z)),
          std::fma(matrix[2], shifted.x,
                   std::fma(matrix[6], shifted.y, matrix[10] * shifted.z))};
}

[[nodiscard]] Vec3 inverseDirection(const std::array<double, 16>& matrix,
                                    Vec3 world) noexcept {
  return {std::fma(matrix[0], world.x,
                   std::fma(matrix[4], world.y, matrix[8] * world.z)),
          std::fma(matrix[1], world.x,
                   std::fma(matrix[5], world.y, matrix[9] * world.z)),
          std::fma(matrix[2], world.x,
                   std::fma(matrix[6], world.y, matrix[10] * world.z))};
}

[[nodiscard]] std::optional<double> triangleIntersection(
    Vec3 origin, Vec3 direction, double maximumParameter,
    const RenderTriangle& triangle, double scale) noexcept {
  const Vec3 a = vector(triangle.a);
  const Vec3 b = vector(triangle.b);
  const Vec3 c = vector(triangle.c);
  const Vec3 edge1 = b - a;
  const Vec3 edge2 = c - a;
  const Vec3 crossDirection = cross(direction, edge2);
  const double determinant = dot(edge1, crossDirection);
  const double determinantTolerance =
      std::max(1.0, scale * scale) * 1e-14;
  if (!std::isfinite(determinant) ||
      std::abs(determinant) <= determinantTolerance)
    return std::nullopt;
  const double inverseDeterminant = 1.0 / determinant;
  const Vec3 fromA = origin - a;
  const double u = dot(fromA, crossDirection) * inverseDeterminant;
  const double barycentricTolerance = 1e-12;
  if (u < -barycentricTolerance || u > 1.0 + barycentricTolerance)
    return std::nullopt;
  const Vec3 q = cross(fromA, edge1);
  const double v = dot(direction, q) * inverseDeterminant;
  if (v < -barycentricTolerance || u + v > 1.0 + barycentricTolerance)
    return std::nullopt;
  const double parameter = dot(edge2, q) * inverseDeterminant;
  const double parameterTolerance = std::max(1.0, scale) * 1e-11;
  if (!std::isfinite(parameter) || parameter < -parameterTolerance ||
      parameter > maximumParameter + parameterTolerance)
    return std::nullopt;
  return std::clamp(parameter, 0.0, maximumParameter);
}

struct SegmentPair final {
  double firstParameter{};
  double secondParameter{};
  double squaredDistance{};
};

[[nodiscard]] SegmentPair closestSegments(Vec3 p0, Vec3 p1, Vec3 q0,
                                          Vec3 q1) noexcept {
  const Vec3 d1 = p1 - p0;
  const Vec3 d2 = q1 - q0;
  const Vec3 r = p0 - q0;
  const double a = squaredLength(d1);
  const double e = squaredLength(d2);
  const double f = dot(d2, r);
  constexpr double epsilon = 1e-24;
  double s = 0.0;
  double t = 0.0;
  if (a <= epsilon && e <= epsilon) {
    return {0.0, 0.0, squaredLength(p0 - q0)};
  }
  if (a <= epsilon) {
    t = std::clamp(f / e, 0.0, 1.0);
  } else {
    const double c = dot(d1, r);
    if (e <= epsilon) {
      s = std::clamp(-c / a, 0.0, 1.0);
    } else {
      const double b = dot(d1, d2);
      const double denominator = a * e - b * b;
      if (std::abs(denominator) > epsilon)
        s = std::clamp((b * f - c * e) / denominator, 0.0, 1.0);
      t = (b * s + f) / e;
      if (t < 0.0) {
        t = 0.0;
        s = std::clamp(-c / a, 0.0, 1.0);
      } else if (t > 1.0) {
        t = 1.0;
        s = std::clamp((b - c) / a, 0.0, 1.0);
      }
    }
  }
  const Vec3 first = p0 + d1 * s;
  const Vec3 second = q0 + d2 * t;
  return {s, t, squaredLength(first - second)};
}

[[nodiscard]] bool pathLess(const OccurrencePath& left,
                            const OccurrencePath& right) noexcept {
  return std::lexicographical_compare(
      left.entries().begin(), left.entries().end(), right.entries().begin(),
      right.entries().end(), [](OccurrenceId a, OccurrenceId b) {
        return a.value() < b.value();
      });
}

struct StableHitIdentity final {
  const AssemblyOccurrenceIdentity* occurrence{};
  const BodyMeshKey* source{};
  std::size_t rawOrdinal{};
  std::size_t primitiveOrdinal{};
};

struct ProjectedPoint final {
  QPointF screen;
  Point3d world{};
  double depth{};
};

struct LogicalRectangle final {
  double left{};
  double top{};
  double right{};
  double bottom{};
};

[[nodiscard]] std::optional<LogicalRectangle> logicalRectangle(
    QPointF first, QPointF second) noexcept {
  if (!std::isfinite(first.x()) || !std::isfinite(first.y()) ||
      !std::isfinite(second.x()) || !std::isfinite(second.y()))
    return std::nullopt;
  LogicalRectangle value{std::min(first.x(), second.x()),
                         std::min(first.y(), second.y()),
                         std::max(first.x(), second.x()),
                         std::max(first.y(), second.y())};
  if (value.left == value.right || value.top == value.bottom)
    return std::nullopt;
  return value;
}

[[nodiscard]] bool validCamera(const ViewportCameraState& camera) noexcept {
  return std::isfinite(camera.yawDeg) && std::isfinite(camera.pitchDeg) &&
         std::isfinite(camera.zoom) && camera.zoom > 0.0F &&
         std::isfinite(camera.pan.x()) && std::isfinite(camera.pan.y()) &&
         camera.logicalSize.width() > 0 && camera.logicalSize.height() > 0 &&
         std::isfinite(camera.devicePixelRatio) &&
         camera.devicePixelRatio > 0.0F && finite(vector(camera.center)) &&
         std::isfinite(camera.depthExtent) && camera.depthExtent > 0.0;
}

[[nodiscard]] std::optional<ProjectedPoint> projectPointDouble(
    const ViewportCameraState& camera, Point3d world) noexcept {
  if (!validCamera(camera) || !finite(vector(world))) return std::nullopt;
  const double width = camera.logicalSize.width();
  const double height = camera.logicalSize.height();
  const double scale = std::min(width, height) * 0.008 * camera.zoom;
  const double yaw = camera.yawDeg * std::numbers::pi / 180.0;
  const double pitch = camera.pitchDeg * std::numbers::pi / 180.0;
  const double cy = std::cos(yaw);
  const double sy = std::sin(yaw);
  const double cp = std::cos(pitch);
  const double sp = std::sin(pitch);
  const double right = world.x * cy - world.y * sy;
  const double up = -world.x * sy * cp - world.y * cy * cp + world.z * sp;
  const double depth = (world.x * sy + world.y * cy) * sp + world.z * cp;
  ProjectedPoint result{{width * 0.5 + camera.pan.x() + scale * right,
                         height * 0.52 + camera.pan.y() - scale * up},
                        world, depth};
  if (!std::isfinite(result.screen.x()) ||
      !std::isfinite(result.screen.y()) || !std::isfinite(result.depth))
    return std::nullopt;
  return result;
}

[[nodiscard]] ProjectedPoint interpolate(const ProjectedPoint& first,
                                         const ProjectedPoint& second,
                                         double t) noexcept {
  return {{first.screen.x() + (second.screen.x() - first.screen.x()) * t,
           first.screen.y() + (second.screen.y() - first.screen.y()) * t},
          {first.world.x + (second.world.x - first.world.x) * t,
           first.world.y + (second.world.y - first.world.y) * t,
           first.world.z + (second.world.z - first.world.z) * t},
          first.depth + (second.depth - first.depth) * t};
}

enum class ClipSide { Left, Right, Top, Bottom };

[[nodiscard]] double sideValue(const ProjectedPoint& point,
                               const LogicalRectangle& rectangle,
                               ClipSide side) noexcept {
  switch (side) {
    case ClipSide::Left:
      return point.screen.x() - rectangle.left;
    case ClipSide::Right:
      return rectangle.right - point.screen.x();
    case ClipSide::Top:
      return point.screen.y() - rectangle.top;
    case ClipSide::Bottom:
      return rectangle.bottom - point.screen.y();
  }
  return -1.0;
}

[[nodiscard]] std::vector<ProjectedPoint> clipPolygon(
    std::vector<ProjectedPoint> polygon,
    const LogicalRectangle& rectangle) {
  constexpr std::array sides{ClipSide::Left, ClipSide::Right, ClipSide::Top,
                             ClipSide::Bottom};
  for (const ClipSide side : sides) {
    if (polygon.empty()) break;
    std::vector<ProjectedPoint> output;
    output.reserve(polygon.size() + 2);
    ProjectedPoint previous = polygon.back();
    double previousValue = sideValue(previous, rectangle, side);
    for (const ProjectedPoint& current : polygon) {
      const double currentValue = sideValue(current, rectangle, side);
      const bool previousInside = previousValue >= -1e-10;
      const bool currentInside = currentValue >= -1e-10;
      if (previousInside != currentInside) {
        const double denominator = previousValue - currentValue;
        if (std::isfinite(denominator) && std::abs(denominator) > 1e-20)
          output.push_back(interpolate(previous, current,
                                       previousValue / denominator));
      }
      if (currentInside) output.push_back(current);
      previous = current;
      previousValue = currentValue;
    }
    polygon = std::move(output);
  }
  return polygon;
}

[[nodiscard]] std::optional<std::pair<ProjectedPoint, ProjectedPoint>>
clipSegment(ProjectedPoint first, ProjectedPoint second,
            const LogicalRectangle& rectangle) noexcept {
  const double dx = second.screen.x() - first.screen.x();
  const double dy = second.screen.y() - first.screen.y();
  const std::array<double, 4> p{-dx, dx, -dy, dy};
  const std::array<double, 4> q{first.screen.x() - rectangle.left,
                                rectangle.right - first.screen.x(),
                                first.screen.y() - rectangle.top,
                                rectangle.bottom - first.screen.y()};
  double enter = 0.0;
  double leave = 1.0;
  for (std::size_t index = 0; index < p.size(); ++index) {
    if (std::abs(p[index]) <= 1e-20) {
      if (q[index] < 0.0) return std::nullopt;
      continue;
    }
    const double ratio = q[index] / p[index];
    if (p[index] < 0.0)
      enter = std::max(enter, ratio);
    else
      leave = std::min(leave, ratio);
    if (enter > leave) return std::nullopt;
  }
  return std::pair{interpolate(first, second, enter),
                   interpolate(first, second, leave)};
}

[[nodiscard]] bool withinBudget(std::size_t& used, std::size_t limit) noexcept {
  if (limit != 0 && used >= limit) return false;
  ++used;
  return true;
}

[[nodiscard]] bool stableIdentityLess(const StableHitIdentity& left,
                                      const StableHitIdentity& right) noexcept {
  if (pathLess(left.occurrence->path, right.occurrence->path)) return true;
  if (pathLess(right.occurrence->path, left.occurrence->path)) return false;
  return std::tuple{left.source->definitionId, left.source->bodyId,
                    left.source->featureId, left.rawOrdinal,
                    left.primitiveOrdinal} <
         std::tuple{right.source->definitionId, right.source->bodyId,
                    right.source->featureId, right.rawOrdinal,
                    right.primitiveOrdinal};
}

[[nodiscard]] bool validOccurrence(const AssemblyOccurrenceIdentity& identity,
                                   const BodyMeshKey& source) noexcept {
  return identity.epoch.isValid() &&
         identity.epoch.value() == source.productEpoch &&
         identity.rootAssemblyDefinitionId.isValid() &&
         identity.path.depth() > 0 && source.definitionId != 0 &&
         source.bodyId != kInvalidBodyId &&
         source.featureId != kInvalidFeatureId;
}

}  // namespace

struct AssemblyViewportPicking::PublishedScene final {
  struct InstanceRecord final {
    std::shared_ptr<const SourceTopology> source;
    RigidTransform3d localToWorld{RigidTransform3d::identity()};
    std::array<double, 16> matrix{};
    AssemblyOccurrenceIdentity occurrence;
    Bounds3d worldBounds;
  };

  std::shared_ptr<const AssemblyViewportScene> scene;
  AssemblyScenePublicationHandle publication;
  std::vector<InstanceRecord> instances;
};

struct AssemblyViewportPicking::ProjectedScene final {
  struct CameraKey final {
    float yaw{};
    float pitch{};
    float zoom{};
    QPointF pan;
    QSize logicalSize;
    Point3d center{};
    double depthExtent{};

    friend bool operator==(const CameraKey& left,
                           const CameraKey& right) noexcept {
      return left.yaw == right.yaw && left.pitch == right.pitch &&
             left.zoom == right.zoom && left.pan == right.pan &&
             left.logicalSize == right.logicalSize &&
             left.center.x == right.center.x &&
             left.center.y == right.center.y &&
             left.center.z == right.center.z &&
             left.depthExtent == right.depthExtent;
    }
  };

  struct FacePrimitive final {
    const PublishedScene::InstanceRecord* instance{};
    std::size_t rawFace{};
    std::size_t primitive{};
    std::array<ProjectedPoint, 3> points;
  };

  struct EdgePrimitive final {
    const PublishedScene::InstanceRecord* instance{};
    std::size_t rawEdge{};
    std::size_t primitive{};
    ProjectedPoint first;
    ProjectedPoint second;
  };

  std::shared_ptr<const PublishedScene> published;
  AssemblyScenePublicationHandle publication;
  CameraKey camera;
  std::vector<FacePrimitive> faces;
  std::vector<EdgePrimitive> edges;
};

namespace {

enum class SpatialVisibility {
  Visible,
  Occluded,
  OutsideClip,
  Uncertain,
  Stale,
  Invalid
};

enum class ClipParameterStatus { Inside, Outside, Invalid };

struct BoundedRayParameter final {
  ClipParameterStatus status{ClipParameterStatus::Invalid};
  double value{};
};

[[nodiscard]] BoundedRayParameter boundedRayParameter(
    const PickingRay& ray, double cameraDepth, double sceneScale) noexcept {
  if (!std::isfinite(cameraDepth) || !std::isfinite(sceneScale) ||
      sceneScale < 0.0 || !std::isfinite(ray.nearDepth) ||
      !std::isfinite(ray.length) || ray.length <= 0.0)
    return {};
  const double parameter = ray.nearDepth - cameraDepth;
  const double tolerance = std::max(1.0, sceneScale) * 1e-11;
  if (!std::isfinite(parameter) || !std::isfinite(tolerance)) return {};
  if (parameter < -tolerance || parameter > ray.length + tolerance)
    return {ClipParameterStatus::Outside, {}};
  return {ClipParameterStatus::Inside,
          std::clamp(parameter, 0.0, ray.length)};
}

[[nodiscard]] std::optional<std::array<double, 3>> barycentricAt(
    QPointF point, const std::array<ProjectedPoint, 3>& triangle) noexcept {
  const QPointF v0 = triangle[1].screen - triangle[0].screen;
  const QPointF v1 = triangle[2].screen - triangle[0].screen;
  const QPointF v2 = point - triangle[0].screen;
  const double denominator = v0.x() * v1.y() - v1.x() * v0.y();
  if (!std::isfinite(denominator) || std::abs(denominator) < 1e-12)
    return std::nullopt;
  const double u = (v2.x() * v1.y() - v1.x() * v2.y()) / denominator;
  const double v = (v0.x() * v2.y() - v2.x() * v0.y()) / denominator;
  const double w = 1.0 - u - v;
  if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w))
    return std::nullopt;
  return std::array<double, 3>{w, u, v};
}

[[nodiscard]] double planeDepth(
    const std::array<ProjectedPoint, 3>& triangle,
    const std::array<double, 3>& weights) noexcept {
  return triangle[0].depth * weights[0] +
         triangle[1].depth * weights[1] +
         triangle[2].depth * weights[2];
}

void normalizePolygon(std::vector<ProjectedPoint>& polygon) {
  const auto same = [](const ProjectedPoint& left,
                       const ProjectedPoint& right) {
    return std::abs(left.screen.x() - right.screen.x()) <= 1e-10 &&
           std::abs(left.screen.y() - right.screen.y()) <= 1e-10 &&
           std::abs(left.depth - right.depth) <= 1e-12;
  };
  std::vector<ProjectedPoint> normalized;
  normalized.reserve(polygon.size());
  for (const auto& point : polygon)
    if (normalized.empty() || !same(normalized.back(), point))
      normalized.push_back(point);
  if (normalized.size() > 1 && same(normalized.front(), normalized.back()))
    normalized.pop_back();
  polygon = std::move(normalized);
}

[[nodiscard]] bool hasPositiveProjectedArea(
    const std::vector<ProjectedPoint>& polygon) noexcept {
  if (polygon.size() < 3) return false;
  double twiceArea = 0.0;
  double minimumX = polygon.front().screen.x();
  double maximumX = minimumX;
  double minimumY = polygon.front().screen.y();
  double maximumY = minimumY;
  for (std::size_t index = 0; index < polygon.size(); ++index) {
    const QPointF current = polygon[index].screen;
    const QPointF next = polygon[(index + 1) % polygon.size()].screen;
    if (!std::isfinite(current.x()) || !std::isfinite(current.y()))
      return false;
    twiceArea = std::fma(current.x(), next.y(), twiceArea) -
                current.y() * next.x();
    minimumX = std::min(minimumX, current.x());
    maximumX = std::max(maximumX, current.x());
    minimumY = std::min(minimumY, current.y());
    maximumY = std::max(maximumY, current.y());
  }
  const double scale =
      std::max({1.0, maximumX - minimumX, maximumY - minimumY});
  const double tolerance = scale * scale * 1e-12;
  return std::isfinite(twiceArea) && std::abs(twiceArea) > tolerance;
}

template <typename Evaluate>
[[nodiscard]] std::pair<std::vector<ProjectedPoint>,
                        std::vector<ProjectedPoint>>
splitConvexPolygon(const std::vector<ProjectedPoint>& polygon,
                   Evaluate&& evaluate, bool boundaryInside) {
  std::vector<ProjectedPoint> inside;
  std::vector<ProjectedPoint> outside;
  if (polygon.empty()) return {inside, outside};
  inside.reserve(polygon.size() + 1);
  outside.reserve(polygon.size() + 1);
  ProjectedPoint previous = polygon.back();
  double previousValue = evaluate(previous);
  const auto classifiedInside = [boundaryInside](double value) {
    return value > 0.0 || (boundaryInside && value == 0.0);
  };
  bool previousInside = classifiedInside(previousValue);
  for (const auto& current : polygon) {
    const double currentValue = evaluate(current);
    const bool currentInside = classifiedInside(currentValue);
    if (currentInside != previousInside) {
      const double denominator = previousValue - currentValue;
      const double parameter = std::abs(denominator) > 1e-20
                                   ? std::clamp(previousValue / denominator,
                                                0.0, 1.0)
                                   : 0.5;
      const ProjectedPoint crossing =
          interpolate(previous, current, parameter);
      inside.push_back(crossing);
      outside.push_back(crossing);
    }
    (currentInside ? inside : outside).push_back(current);
    previous = current;
    previousValue = currentValue;
    previousInside = currentInside;
  }
  normalizePolygon(inside);
  normalizePolygon(outside);
  return {std::move(inside), std::move(outside)};
}

[[nodiscard]] std::vector<std::vector<ProjectedPoint>> subtractOccluder(
    const std::vector<ProjectedPoint>& polygon,
    const std::array<ProjectedPoint, 3>& occluder,
    double depthEpsilon) {
  std::vector<std::vector<ProjectedPoint>> visible;
  std::vector<ProjectedPoint> hiddenCandidate = polygon;
  const QPointF v0 = occluder[1].screen - occluder[0].screen;
  const QPointF v1 = occluder[2].screen - occluder[0].screen;
  const double denominator = v0.x() * v1.y() - v1.x() * v0.y();
  if (!std::isfinite(denominator) || std::abs(denominator) < 1e-12) {
    visible.push_back(polygon);
    return visible;
  }
  constexpr double insideTolerance = -1e-10;
  const auto weight = [&](const ProjectedPoint& point,
                          std::size_t coordinate) {
    const QPointF v2 = point.screen - occluder[0].screen;
    const double u = (v2.x() * v1.y() - v1.x() * v2.y()) / denominator;
    const double v = (v0.x() * v2.y() - v2.x() * v0.y()) / denominator;
    const std::array weights{1.0 - u - v, u, v};
    return weights[coordinate] - insideTolerance;
  };
  for (std::size_t coordinate = 0; coordinate < 3; ++coordinate) {
    auto [inside, outside] = splitConvexPolygon(
        hiddenCandidate,
        [&](const ProjectedPoint& point) { return weight(point, coordinate); },
        true);
    if (hasPositiveProjectedArea(outside))
      visible.push_back(std::move(outside));
    hiddenCandidate = std::move(inside);
    if (!hasPositiveProjectedArea(hiddenCandidate)) return visible;
  }
  const double required = depthEpsilon +
                          std::max(1e-12, depthEpsilon * 1e-6);
  auto [hidden, depthVisible] = splitConvexPolygon(
      hiddenCandidate,
      [&](const ProjectedPoint& point) {
        const auto weights = barycentricAt(point.screen, occluder);
        return weights ? planeDepth(occluder, *weights) - point.depth - required
                       : -std::numeric_limits<double>::infinity();
      },
      false);
  if (hasPositiveProjectedArea(depthVisible))
    visible.push_back(std::move(depthVisible));
  static_cast<void>(hidden);
  return visible;
}

template <typename Projected>
[[nodiscard]] SpatialVisibility polygonVisibilityExact(
    const Projected& projection, std::vector<ProjectedPoint> polygon,
    double depthEpsilon, std::size_t& occluderWork,
    std::size_t maximumOccluderWork,
    AssemblyViewportPickingDiagnostics& diagnostics) {
  std::vector<std::vector<ProjectedPoint>> visible;
  if (!hasPositiveProjectedArea(polygon)) return SpatialVisibility::Occluded;
  visible.push_back(std::move(polygon));
  for (const auto& occluder : projection.faces) {
    if (!projection.published->scene->isCurrent())
      return SpatialVisibility::Stale;
    std::vector<std::vector<ProjectedPoint>> next;
    for (const auto& piece : visible) {
      if (!withinBudget(occluderWork, maximumOccluderWork))
        return SpatialVisibility::Uncertain;
      ++diagnostics.occluderTests;
      auto difference =
          subtractOccluder(piece, occluder.points, depthEpsilon);
      for (auto& candidate : difference)
        if (hasPositiveProjectedArea(candidate))
          next.push_back(std::move(candidate));
    }
    visible = std::move(next);
    if (visible.empty()) return SpatialVisibility::Occluded;
  }
  return SpatialVisibility::Visible;
}

struct ParamInterval final {
  double begin{};
  double end{1.0};
};

[[nodiscard]] bool intersectClosed(ParamInterval& interval, double atBegin,
                                   double atEnd, double lowerBound) noexcept {
  const double delta = atEnd - atBegin;
  if (!std::isfinite(atBegin) || !std::isfinite(atEnd)) return false;
  if (std::abs(delta) < 1e-15) return atBegin >= lowerBound;
  const double crossing = (lowerBound - atBegin) / delta;
  if (delta > 0.0) {
    if (crossing > interval.end) return false;
    interval.begin = std::max(interval.begin, crossing);
  } else {
    if (crossing < interval.begin) return false;
    interval.end = std::min(interval.end, crossing);
  }
  return interval.begin <= interval.end;
}

[[nodiscard]] bool intersectStrict(ParamInterval& interval, double atBegin,
                                   double atEnd, double lowerBound) noexcept {
  const double delta = atEnd - atBegin;
  if (!std::isfinite(atBegin) || !std::isfinite(atEnd)) return false;
  if (std::abs(delta) < 1e-15) return atBegin > lowerBound;
  const double crossing = (lowerBound - atBegin) / delta;
  if (delta > 0.0) {
    if (crossing >= interval.end) return false;
    interval.begin = std::max(interval.begin, crossing);
  } else {
    if (crossing <= interval.begin) return false;
    interval.end = std::min(interval.end, crossing);
  }
  return interval.begin < interval.end;
}

[[nodiscard]] std::optional<ParamInterval> strictOcclusionInterval(
    const ProjectedPoint& first, const ProjectedPoint& second,
    const std::array<ProjectedPoint, 3>& occluder,
    double depthEpsilon) noexcept {
  const auto firstWeights = barycentricAt(first.screen, occluder);
  const auto secondWeights = barycentricAt(second.screen, occluder);
  if (!firstWeights || !secondWeights) return std::nullopt;
  ParamInterval interval;
  constexpr double insideTolerance = -1e-10;
  for (std::size_t coordinate = 0; coordinate < 3; ++coordinate)
    if (!intersectClosed(interval, (*firstWeights)[coordinate],
                         (*secondWeights)[coordinate], insideTolerance))
      return std::nullopt;
  const double firstDifference =
      planeDepth(occluder, *firstWeights) - first.depth;
  const double secondDifference =
      planeDepth(occluder, *secondWeights) - second.depth;
  const double required = depthEpsilon +
                          std::max(1e-12, depthEpsilon * 1e-6);
  if (!intersectStrict(interval, firstDifference, secondDifference, required))
    return std::nullopt;
  interval.begin = std::clamp(interval.begin, 0.0, 1.0);
  interval.end = std::clamp(interval.end, 0.0, 1.0);
  return interval;
}

void insertMerged(std::vector<ParamInterval>& intervals,
                  ParamInterval value) {
  auto position = intervals.begin();
  while (position != intervals.end() && position->end < value.begin)
    ++position;
  while (position != intervals.end() && position->begin <= value.end) {
    value.begin = std::min(value.begin, position->begin);
    value.end = std::max(value.end, position->end);
    position = intervals.erase(position);
  }
  intervals.insert(position, value);
}

template <typename Projected>
[[nodiscard]] SpatialVisibility segmentVisibilityExact(
    const Projected& projection, const ProjectedPoint& first,
    const ProjectedPoint& second, double depthEpsilon,
    std::size_t& occluderWork, std::size_t maximumOccluderWork,
    AssemblyViewportPickingDiagnostics& diagnostics) {
  std::vector<ParamInterval> hidden;
  for (const auto& occluder : projection.faces) {
    if (!projection.published->scene->isCurrent())
      return SpatialVisibility::Stale;
    if (!withinBudget(occluderWork, maximumOccluderWork))
      return SpatialVisibility::Uncertain;
    ++diagnostics.occluderTests;
    if (const auto interval = strictOcclusionInterval(
            first, second, occluder.points, depthEpsilon)) {
      insertMerged(hidden, *interval);
      if (hidden.size() == 1 && hidden.front().begin <= 0.0 &&
          hidden.front().end >= 1.0)
        return SpatialVisibility::Occluded;
    }
  }
  return SpatialVisibility::Visible;
}

template <typename Published>
[[nodiscard]] SpatialVisibility pointVisibility(
    const Published& published, const PickingRay& ray, double sceneScale,
    const ProjectedPoint& candidate, std::size_t& occluderWork,
    std::size_t maximumOccluderWork,
    AssemblyViewportPickingDiagnostics& diagnostics) noexcept {
  if (!finite(vector(candidate.world)) || !std::isfinite(candidate.depth))
    return SpatialVisibility::Invalid;
  const BoundedRayParameter parameter =
      boundedRayParameter(ray, candidate.depth, sceneScale);
  if (parameter.status == ClipParameterStatus::Invalid)
    return SpatialVisibility::Invalid;
  if (parameter.status == ClipParameterStatus::Outside)
    return SpatialVisibility::OutsideClip;
  const double tolerance =
      std::max(1.0, sceneScale) * 1e-8;
  PickingRay occlusionRay{
      vector(candidate.world) + ray.view * parameter.value,
      ray.direction,
      ray.view,
      parameter.value,
      ray.nearDepth,
      ray.scale};
  if (!finite(occlusionRay.origin)) return SpatialVisibility::Invalid;
  for (const auto& instance : published.instances) {
    if (!published.scene->isCurrent()) return SpatialVisibility::Stale;
    if (!rayIntersectsBounds(occlusionRay, instance.worldBounds)) continue;
    const Vec3 localOrigin = inversePoint(instance.matrix, occlusionRay.origin);
    const Vec3 localDirection =
        inverseDirection(instance.matrix, occlusionRay.direction);
    for (const RenderTriangle triangle : instance.source->mesh->triangles()) {
      if (!withinBudget(occluderWork, maximumOccluderWork))
        return SpatialVisibility::Uncertain;
      ++diagnostics.occluderTests;
      const auto intersection = triangleIntersection(
          localOrigin, localDirection, occlusionRay.length, triangle,
          sceneScale);
      if (intersection && *intersection < occlusionRay.length - tolerance)
        return SpatialVisibility::Occluded;
    }
  }
  return SpatialVisibility::Visible;
}

template <typename Primitive>
[[nodiscard]] bool identityLess(const Primitive& left,
                                const Primitive& right) {
  return stableIdentityLess(
      {&left.instance->occurrence, &left.instance->source->key,
       [&left]() {
         if constexpr (requires { left.rawFace; }) return left.rawFace;
         else return left.rawEdge;
       }(),
       left.primitive},
      {&right.instance->occurrence, &right.instance->source->key,
       [&right]() {
         if constexpr (requires { right.rawFace; }) return right.rawFace;
         else return right.rawEdge;
       }(),
       right.primitive});
}

}  // namespace

std::shared_ptr<const AssemblyViewportPicking::ProjectedScene>
AssemblyViewportPicking::projectedFor(
    const ViewportCameraState& camera,
    AssemblyViewportPickingQueryStatus& failureStatus) const noexcept {
  failureStatus = AssemblyViewportPickingQueryStatus::InvalidInput;
  try {
    const auto published = active_;
    if (!published) {
      failureStatus = AssemblyViewportPickingQueryStatus::NoScene;
      return {};
    }
    if (!validCamera(camera)) return {};
    if (!published->scene || !published->scene->isCurrent() ||
        !published->publication.isSameCurrentPublication(
            published->scene->publicationHandle())) {
      failureStatus = AssemblyViewportPickingQueryStatus::StalePublication;
      return {};
    }
    const ProjectedScene::CameraKey key{
        camera.yawDeg, camera.pitchDeg, camera.zoom, camera.pan,
        camera.logicalSize, camera.center, camera.depthExtent};
    if (projected_ && projected_->published == published &&
        projected_->camera == key &&
        projected_->publication.isSameCurrentPublication(
            published->publication)) {
      ++diagnostics_.projectedReuses;
      failureStatus = AssemblyViewportPickingQueryStatus::Hit;
      return projected_;
    }

    auto pending = std::make_shared<ProjectedScene>();
    pending->published = published;
    pending->publication = published->publication;
    pending->camera = key;
    std::size_t faceCount = 0;
    std::size_t edgeCount = 0;
    for (const auto& instance : published->instances) {
      faceCount += instance.source->mesh->triangleCount();
      for (const RenderEdge& edge : instance.source->mesh->edges())
        if (edge.points.size() > 1) edgeCount += edge.points.size() - 1;
    }
    pending->faces.reserve(faceCount);
    pending->edges.reserve(edgeCount);
    for (const auto& instance : published->instances) {
      if (failProjectionBuildCountdown_ > 0 &&
          --failProjectionBuildCountdown_ == 0)
        return {};
      if (!published->scene->isCurrent()) {
        failureStatus = AssemblyViewportPickingQueryStatus::StalePublication;
        return {};
      }
      const auto triangles = instance.source->mesh->triangles();
      for (std::size_t primitive = 0; primitive < triangles.size();
           ++primitive) {
        const RenderTriangle triangle = triangles[primitive];
        if (triangle.faceIndex >= instance.source->faces.size()) return {};
        const auto a = transformPoint(instance.matrix, triangle.a);
        const auto b = transformPoint(instance.matrix, triangle.b);
        const auto c = transformPoint(instance.matrix, triangle.c);
        if (!a || !b || !c) return {};
        const auto pa = projectPointDouble(camera, *a);
        const auto pb = projectPointDouble(camera, *b);
        const auto pc = projectPointDouble(camera, *c);
        if (!pa || !pb || !pc) return {};
        pending->faces.push_back(
            {&instance, triangle.faceIndex, primitive, {*pa, *pb, *pc}});
      }
      for (const RenderEdge& edge : instance.source->mesh->edges()) {
        if (edge.edgeIndex >= instance.source->edges.size()) return {};
        for (std::size_t primitive = 1; primitive < edge.points.size();
             ++primitive) {
          const auto first = transformPoint(instance.matrix,
                                            edge.points[primitive - 1]);
          const auto second =
              transformPoint(instance.matrix, edge.points[primitive]);
          if (!first || !second) return {};
          const auto projectedFirst = projectPointDouble(camera, *first);
          const auto projectedSecond = projectPointDouble(camera, *second);
          if (!projectedFirst || !projectedSecond) return {};
          pending->edges.push_back({&instance, edge.edgeIndex, primitive - 1,
                                    *projectedFirst, *projectedSecond});
        }
      }
    }
    if (!published->scene->isCurrent() ||
        !pending->publication.isSameCurrentPublication(
            published->scene->publicationHandle())) {
      failureStatus = AssemblyViewportPickingQueryStatus::StalePublication;
      return {};
    }
    projected_ = std::move(pending);
    ++diagnostics_.projectedBuilds;
    failureStatus = AssemblyViewportPickingQueryStatus::Hit;
    return projected_;
  } catch (...) {
    return {};
  }
}

std::optional<AssemblyOccurrenceHighlightTarget>
AssemblyOccurrenceHighlightTarget::create(
    const AssemblyScenePublicationHandle& publication,
    const AssemblyOccurrenceIdentity& occurrence) noexcept {
  try {
    const auto snapshot = publication.snapshotIfCurrent();
    if (!snapshot || !publication.isCurrent() || !occurrence.epoch.isValid() ||
        occurrence.epoch != snapshot->epoch() ||
        !occurrence.rootAssemblyDefinitionId.isValid() ||
        occurrence.path.depth() == 0)
      return std::nullopt;
    const bool exists = std::any_of(
        snapshot->instances().begin(), snapshot->instances().end(),
        [&occurrence](const AssemblyPresentationInstance& instance) {
          return instance.occurrenceIdentity() == occurrence &&
                 instance.effectivelyVisible();
        });
    if (!exists || !publication.isCurrent()) return std::nullopt;
    return AssemblyOccurrenceHighlightTarget(publication, occurrence);
  } catch (...) {
    return std::nullopt;
  }
}

bool AssemblyOccurrenceHighlightTarget::isCurrentFor(
    const AssemblyViewportScene& scene) const noexcept {
  return occurrence_.epoch.isValid() &&
         occurrence_.epoch.value() != 0 && scene.isCurrent() &&
         publication_.isSameCurrentPublication(scene.publicationHandle());
}

std::optional<AssemblyOccurrenceHighlightTarget>
AssemblyFaceRayHit::highlightTarget() const noexcept {
  return AssemblyOccurrenceHighlightTarget::create(publication,
                                                    address.occurrence);
}

std::optional<AssemblyOccurrenceHighlightTarget>
AssemblyEdgeRayHit::highlightTarget() const noexcept {
  return AssemblyOccurrenceHighlightTarget::create(publication,
                                                    address.occurrence);
}

std::optional<AssemblyOccurrenceHighlightTarget>
AssemblyViewportSnapHit::highlightTarget() const noexcept {
  return AssemblyOccurrenceHighlightTarget::create(publication, occurrence);
}

AssemblyViewportPickingPublishResult AssemblyViewportPicking::publish(
    const std::shared_ptr<const AssemblyViewportScene>& scene) noexcept {
  ++diagnostics_.publicationAttempts;
  const auto fail = [this](AssemblyViewportPickingPublishStatus status) {
    ++diagnostics_.publicationFailures;
    if (status == AssemblyViewportPickingPublishStatus::StalePublication)
      ++diagnostics_.stalePublicationRejections;
    return AssemblyViewportPickingPublishResult{status, {}};
  };

  try {
    if (!scene) return fail(AssemblyViewportPickingPublishStatus::InvalidScene);
    if (!scene->isCurrent())
      return fail(AssemblyViewportPickingPublishStatus::StalePublication);
    if (!scene->snapshot() ||
        !scene->publicationHandle().isSameCurrentPublication(
            scene->publicationHandle()))
      return fail(AssemblyViewportPickingPublishStatus::InvalidScene);

    std::vector<detail::ViewportPickingTopologyInput> topologyInputs;
    topologyInputs.reserve(scene->sources().size());
    for (const auto& source : scene->sources())
      topologyInputs.push_back({source.key, source.shape,
                                source.topologyLineage, source.mesh});
    const auto mapTopologyStatus = [](detail::ViewportPickingTopologyStatus s) {
      using Shared = detail::ViewportPickingTopologyStatus;
      switch (s) {
        case Shared::Prepared:
          return AssemblyViewportPickingPublishStatus::Published;
        case Shared::StalePublication:
          return AssemblyViewportPickingPublishStatus::StalePublication;
        case Shared::InvalidSource:
          return AssemblyViewportPickingPublishStatus::InvalidSource;
        case Shared::DuplicateSource:
          return AssemblyViewportPickingPublishStatus::DuplicateSource;
        case Shared::TopologyBuildFailed:
          return AssemblyViewportPickingPublishStatus::TopologyBuildFailed;
        case Shared::InvalidTopologyMapping:
          return AssemblyViewportPickingPublishStatus::InvalidTopologyMapping;
        case Shared::ResourceFailure:
          return AssemblyViewportPickingPublishStatus::PreparationFailed;
      }
      return AssemblyViewportPickingPublishStatus::PreparationFailed;
    };
    detail::ViewportPickingTopologyCounters counters{
        &diagnostics_.topologySourceBuildAttempts,
        &diagnostics_.topologySourceBuildSuccesses,
        &diagnostics_.topologySourceBuildFailures,
        &diagnostics_.topologySourceReuses,
        &diagnostics_.v2FaceReferences,
        &diagnostics_.v1FaceFallbacks,
        &diagnostics_.v2EdgeReferences,
        &diagnostics_.v1EdgeFallbacks};
    auto topology = detail::prepareViewportPickingTopology(
        topologyInputs, cache_, [&scene] { return scene->isCurrent(); },
        failTopologyBuildCountdown_, failMappingCountdown_, counters);
    if (!topology) return fail(mapTopologyStatus(topology.status));
    const auto& activeSources = topology.activeSources;

    ++diagnostics_.spatialBuilds;
    auto pending = std::make_shared<PublishedScene>();
    pending->scene = scene;
    pending->publication = scene->publicationHandle();
    pending->instances.reserve(scene->instances().size());
    for (const auto& instance : scene->instances()) {
      if (!scene->isCurrent())
        return fail(AssemblyViewportPickingPublishStatus::StalePublication);
      const auto source = activeSources.find(instance.sourceKey);
      const auto matrix = rigidTransformMatrix(instance.localToWorld);
      if (source == activeSources.end() || !source->second ||
          source->second->mesh != instance.mesh || !matrix ||
          !validOccurrence(instance.occurrenceIdentity, instance.sourceKey))
        return fail(AssemblyViewportPickingPublishStatus::InvalidScene);
      pending->instances.push_back(
          {source->second, instance.localToWorld, *matrix,
           instance.occurrenceIdentity, instance.worldBounds});
    }

    if (!scene->isCurrent() ||
        !pending->publication.isSameCurrentPublication(
            scene->publicationHandle()))
      return fail(AssemblyViewportPickingPublishStatus::StalePublication);

    cache_.swap(topology.cache);
    projected_.reset();
    active_ = std::move(pending);
    ++diagnostics_.publicationSuccesses;
    diagnostics_.residentTopologySources = cache_.size();
    diagnostics_.activeSources = activeSources.size();
    diagnostics_.activeInstances = active_->instances.size();
    diagnostics_.activeGeneration = scene->generation();
    return {AssemblyViewportPickingPublishStatus::Published,
            scene->generation()};
  } catch (const std::exception&) {
    return fail(AssemblyViewportPickingPublishStatus::PreparationFailed);
  } catch (...) {
    return fail(AssemblyViewportPickingPublishStatus::PreparationFailed);
  }
}

bool AssemblyViewportPicking::seedReusableSourcesFrom(
    const AssemblyViewportPicking& source) noexcept {
  if (this == &source) return true;
  try {
    cache_ = source.cache_;
    active_.reset();
    projected_.reset();
    diagnostics_.residentTopologySources = cache_.size();
    diagnostics_.activeSources = 0;
    diagnostics_.activeInstances = 0;
    diagnostics_.activeGeneration = {};
    return true;
  } catch (...) {
    clear();
    return false;
  }
}

void AssemblyViewportPicking::swapPrepared(
    AssemblyViewportPicking& prepared) noexcept {
  cache_.swap(prepared.cache_);
  active_.swap(prepared.active_);
  projected_.swap(prepared.projected_);
  std::swap(diagnostics_, prepared.diagnostics_);
  std::swap(failTopologyBuildCountdown_,
            prepared.failTopologyBuildCountdown_);
  std::swap(failMappingCountdown_, prepared.failMappingCountdown_);
  std::swap(failProjectionBuildCountdown_,
            prepared.failProjectionBuildCountdown_);
}

AssemblyFaceRayQueryResult AssemblyViewportPicking::faceAt(
    const ViewportCameraState& camera, QPointF logicalPixel) const noexcept {
  ++diagnostics_.faceQueries;
  const auto invalid = [this](AssemblyViewportPickingQueryStatus status) {
    ++diagnostics_.queryFailures;
    if (status == AssemblyViewportPickingQueryStatus::StalePublication)
      ++diagnostics_.staleQueryRejections;
    return AssemblyFaceRayQueryResult{status, {}};
  };
  try {
    const auto published = active_;
    if (!published) return invalid(AssemblyViewportPickingQueryStatus::NoScene);
    if (!published->scene || !published->scene->isCurrent() ||
        !published->publication.isSameCurrentPublication(
            published->scene->publicationHandle()))
      return invalid(
          AssemblyViewportPickingQueryStatus::StalePublication);
    const auto ray = makeRay(camera, logicalPixel);
    if (!ray)
      return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);

    struct Candidate final {
      double parameter{};
      double cameraDepth{};
      Point3d worldPoint{};
      const PublishedScene::InstanceRecord* instance{};
      std::size_t rawFace{};
      std::size_t triangle{};
    };
    std::optional<Candidate> best;
    const double sceneScale =
        published->scene->worldBounds()
            ? std::max(1.0, published->scene->worldBounds()->diagonal())
            : 1.0;
    const double tieTolerance =
        std::max({sceneScale, camera.depthExtent, 1.0}) * 1e-10;

    for (const auto& instance : published->instances) {
      if (!published->scene->isCurrent())
        return invalid(
            AssemblyViewportPickingQueryStatus::StalePublication);
      if (!rayIntersectsBounds(*ray, instance.worldBounds)) continue;
      ++diagnostics_.broadPhaseCandidates;
      const Vec3 localOrigin = inversePoint(instance.matrix, ray->origin);
      const Vec3 localDirection =
          inverseDirection(instance.matrix, ray->direction);
      if (!finite(localOrigin) || !finite(localDirection))
        return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);
      const auto triangles = instance.source->mesh->triangles();
      for (std::size_t triangleOrdinal = 0;
           triangleOrdinal < triangles.size(); ++triangleOrdinal) {
        ++diagnostics_.triangleTests;
        const RenderTriangle triangle = triangles[triangleOrdinal];
        if (triangle.faceIndex >= instance.source->faces.size())
          return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);
        const auto parameter = triangleIntersection(
            localOrigin, localDirection, ray->length, triangle, sceneScale);
        if (!parameter) continue;
        const Vec3 world = ray->origin + ray->direction * *parameter;
        const double cameraDepth = dot(world, ray->view);
        if (!finite(world) || !std::isfinite(cameraDepth))
          return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);
        Candidate candidate{*parameter, cameraDepth, point(world), &instance,
                            triangle.faceIndex, triangleOrdinal};
        bool replace = !best || candidate.parameter <
                                    best->parameter - tieTolerance;
        if (best &&
            std::abs(candidate.parameter - best->parameter) <= tieTolerance) {
          replace = stableIdentityLess(
              {&candidate.instance->occurrence, &candidate.instance->source->key,
               candidate.rawFace, candidate.triangle},
              {&best->instance->occurrence, &best->instance->source->key,
               best->rawFace, best->triangle});
        }
        if (replace) best = candidate;
      }
    }

    if (!published->scene->isCurrent() ||
        !published->publication.isSameCurrentPublication(
            published->scene->publicationHandle()))
      return invalid(
          AssemblyViewportPickingQueryStatus::StalePublication);
    if (!best) return {AssemblyViewportPickingQueryStatus::Miss, {}};
    const auto& source = *best->instance->source;
    AssemblyFaceRayHit hit{
        {best->instance->occurrence, DefinitionId{source.key.definitionId},
         source.faces[best->rawFace]},
        best->worldPoint,
        best->parameter,
        best->cameraDepth,
        published->publication};
    return {AssemblyViewportPickingQueryStatus::Hit, std::move(hit)};
  } catch (...) {
    return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);
  }
}

AssemblyEdgeRayQueryResult AssemblyViewportPicking::edgeAt(
    const ViewportCameraState& camera, QPointF logicalPixel,
    double radiusLogicalPixels) const noexcept {
  ++diagnostics_.edgeQueries;
  const auto invalid = [this](AssemblyViewportPickingQueryStatus status) {
    ++diagnostics_.queryFailures;
    if (status == AssemblyViewportPickingQueryStatus::StalePublication)
      ++diagnostics_.staleQueryRejections;
    return AssemblyEdgeRayQueryResult{status, {}};
  };
  try {
    const auto published = active_;
    if (!published) return invalid(AssemblyViewportPickingQueryStatus::NoScene);
    if (!published->scene || !published->scene->isCurrent() ||
        !published->publication.isSameCurrentPublication(
            published->scene->publicationHandle()))
      return invalid(
          AssemblyViewportPickingQueryStatus::StalePublication);
    const auto ray = makeRay(camera, logicalPixel);
    if (!ray || !std::isfinite(radiusLogicalPixels) ||
        radiusLogicalPixels <= 0.0)
      return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);
    const double worldRadius = radiusLogicalPixels / ray->scale;
    if (!std::isfinite(worldRadius) || worldRadius <= 0.0)
      return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);

    struct Candidate final {
      double rayParameter{};
      double cameraDepth{};
      double distance{};
      Point3d worldPoint{};
      const PublishedScene::InstanceRecord* instance{};
      std::size_t rawEdge{};
      std::size_t segment{};
    };
    std::optional<Candidate> best;
    const double sceneScale =
        published->scene->worldBounds()
            ? std::max(1.0, published->scene->worldBounds()->diagonal())
            : 1.0;
    const double depthTolerance =
        std::max({sceneScale, camera.depthExtent, 1.0}) * 1e-8;
    const double distanceTolerance = std::max(1.0, worldRadius) * 1e-10;

    const auto occluded = [&](Point3d candidatePoint,
                              double candidateDepth) -> std::optional<bool> {
      const double candidateParameter = ray->nearDepth - candidateDepth;
      if (!std::isfinite(candidateParameter) || candidateParameter < 0.0 ||
          candidateParameter > ray->length + depthTolerance)
        return true;
      PickingRay occlusionRay{vector(candidatePoint) +
                                  ray->view * candidateParameter,
                              ray->direction,
                              ray->view,
                              std::clamp(candidateParameter, 0.0, ray->length),
                              ray->nearDepth,
                              ray->scale};
      for (const auto& occluder : published->instances) {
        if (!published->scene->isCurrent()) return std::nullopt;
        if (!rayIntersectsBounds(occlusionRay, occluder.worldBounds)) continue;
        const Vec3 localOrigin =
            inversePoint(occluder.matrix, occlusionRay.origin);
        const Vec3 localDirection =
            inverseDirection(occluder.matrix, occlusionRay.direction);
        for (const RenderTriangle triangle :
             occluder.source->mesh->triangles()) {
          ++diagnostics_.occluderTests;
          const auto parameter = triangleIntersection(
              localOrigin, localDirection, occlusionRay.length, triangle,
              sceneScale);
          if (parameter &&
              *parameter < occlusionRay.length - depthTolerance)
            return true;
        }
      }
      return false;
    };

    for (const auto& instance : published->instances) {
      if (!published->scene->isCurrent())
        return invalid(
            AssemblyViewportPickingQueryStatus::StalePublication);
      if (!rayIntersectsBounds(*ray, instance.worldBounds, worldRadius))
        continue;
      ++diagnostics_.broadPhaseCandidates;
      const Vec3 localOrigin = inversePoint(instance.matrix, ray->origin);
      const Vec3 localDirection =
          inverseDirection(instance.matrix, ray->direction);
      const Vec3 localEnd = localOrigin + localDirection * ray->length;
      for (const RenderEdge& edge : instance.source->mesh->edges()) {
        if (edge.edgeIndex >= instance.source->edges.size())
          return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);
        for (std::size_t segment = 1; segment < edge.points.size(); ++segment) {
          ++diagnostics_.edgeSegmentTests;
          const Vec3 edgeStart = vector(edge.points[segment - 1]);
          const Vec3 edgeEnd = vector(edge.points[segment]);
          const SegmentPair closest = closestSegments(
              localOrigin, localEnd, edgeStart, edgeEnd);
          if (!std::isfinite(closest.squaredDistance) ||
              closest.squaredDistance > worldRadius * worldRadius)
            continue;
          const Vec3 localPoint =
              edgeStart + (edgeEnd - edgeStart) * closest.secondParameter;
          const auto transformed = transformPoint(instance.matrix,
                                                  point(localPoint));
          if (!transformed) continue;
          const double cameraDepth = dot(vector(*transformed), ray->view);
          const double rayParameter = ray->nearDepth - cameraDepth;
          const double distance = std::sqrt(
              std::max(0.0, closest.squaredDistance));
          const auto hidden = occluded(*transformed, cameraDepth);
          if (!hidden)
            return invalid(
                AssemblyViewportPickingQueryStatus::StalePublication);
          if (*hidden) continue;
          Candidate candidate{rayParameter, cameraDepth, distance,
                              *transformed, &instance, edge.edgeIndex,
                              segment - 1};
          bool replace = !best || candidate.rayParameter <
                                      best->rayParameter - depthTolerance;
          if (best && std::abs(candidate.rayParameter - best->rayParameter) <=
                          depthTolerance) {
            if (candidate.distance < best->distance - distanceTolerance) {
              replace = true;
            } else if (std::abs(candidate.distance - best->distance) <=
                       distanceTolerance) {
              replace = stableIdentityLess(
                  {&candidate.instance->occurrence,
                   &candidate.instance->source->key, candidate.rawEdge,
                   candidate.segment},
                  {&best->instance->occurrence, &best->instance->source->key,
                   best->rawEdge, best->segment});
            }
          }
          if (replace) best = candidate;
        }
      }
    }

    if (!published->scene->isCurrent() ||
        !published->publication.isSameCurrentPublication(
            published->scene->publicationHandle()))
      return invalid(
          AssemblyViewportPickingQueryStatus::StalePublication);
    if (!best) return {AssemblyViewportPickingQueryStatus::Miss, {}};
    const auto& source = *best->instance->source;
    AssemblyEdgeRayHit hit{
        {best->instance->occurrence, DefinitionId{source.key.definitionId},
         source.edges[best->rawEdge]},
        best->worldPoint,
        best->rayParameter,
        best->cameraDepth,
        best->distance,
        published->publication};
    return {AssemblyViewportPickingQueryStatus::Hit, std::move(hit)};
  } catch (...) {
    return invalid(AssemblyViewportPickingQueryStatus::InvalidInput);
  }
}

AssemblyFaceRectangleQueryResult AssemblyViewportPicking::facesInRectangle(
    const ViewportCameraState& camera, QPointF firstLogicalCorner,
    QPointF secondLogicalCorner,
    AssemblyViewportSpatialBudget budget) const noexcept {
  ++diagnostics_.rectangleQueries;
  const auto finishFailure = [this](AssemblyViewportPickingQueryStatus status) {
    ++diagnostics_.queryFailures;
    if (status == AssemblyViewportPickingQueryStatus::StalePublication)
      ++diagnostics_.staleQueryRejections;
    if (status == AssemblyViewportPickingQueryStatus::Uncertain)
      ++diagnostics_.uncertainQueries;
    return AssemblyFaceRectangleQueryResult{status, {}};
  };
  try {
    const auto rectangle =
        logicalRectangle(firstLogicalCorner, secondLogicalCorner);
    if (!rectangle)
      return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
    AssemblyViewportPickingQueryStatus projectionStatus;
    const auto projection = projectedFor(camera, projectionStatus);
    if (!projection) return finishFailure(projectionStatus);

    std::size_t primitiveWork = 0;
    std::size_t occluderWork = 0;
    std::vector<const ProjectedScene::FacePrimitive*> selected;
    selected.reserve(projection->faces.size());
    for (const auto& face : projection->faces) {
      if (!withinBudget(primitiveWork, budget.maximumPrimitiveTests))
        return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
      std::vector<ProjectedPoint> polygon{face.points.begin(),
                                          face.points.end()};
      polygon = clipPolygon(std::move(polygon), *rectangle);
      normalizePolygon(polygon);
      if (!hasPositiveProjectedArea(polygon)) continue;

      const double depthEpsilon =
          std::max({1.0, camera.depthExtent,
                    projection->published->scene->worldBounds()
                        ? projection->published->scene->worldBounds()->diagonal()
                        : 1.0}) *
          1e-8;
      const SpatialVisibility visibility = polygonVisibilityExact(
          *projection, std::move(polygon), depthEpsilon, occluderWork,
          budget.maximumOccluderTests, diagnostics_);
      if (visibility == SpatialVisibility::Uncertain)
        return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
      if (visibility == SpatialVisibility::Stale)
        return finishFailure(
            AssemblyViewportPickingQueryStatus::StalePublication);
      if (visibility == SpatialVisibility::Invalid)
        return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
      if (visibility == SpatialVisibility::Visible) selected.push_back(&face);
    }
    if (!projection->published->scene->isCurrent() ||
        !projection->publication.isSameCurrentPublication(
            projection->published->scene->publicationHandle()))
      return finishFailure(
          AssemblyViewportPickingQueryStatus::StalePublication);
    std::sort(selected.begin(), selected.end(), [](const auto* left,
                                                   const auto* right) {
      return identityLess(*left, *right);
    });
    selected.erase(
        std::unique(selected.begin(), selected.end(), [](const auto* left,
                                                         const auto* right) {
          return left->instance == right->instance &&
                 left->rawFace == right->rawFace;
        }),
        selected.end());
    std::vector<AssemblyFaceRectangleHit> hits;
    hits.reserve(selected.size());
    for (const auto* face : selected) {
      const auto& source = *face->instance->source;
      hits.push_back({{face->instance->occurrence,
                       DefinitionId{source.key.definitionId},
                       source.faces[face->rawFace]},
                      projection->publication});
    }
    return {hits.empty() ? AssemblyViewportPickingQueryStatus::Miss
                         : AssemblyViewportPickingQueryStatus::Hit,
            std::move(hits)};
  } catch (...) {
    return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
  }
}

AssemblyEdgeRectangleQueryResult AssemblyViewportPicking::edgesInRectangle(
    const ViewportCameraState& camera, QPointF firstLogicalCorner,
    QPointF secondLogicalCorner,
    AssemblyViewportSpatialBudget budget) const noexcept {
  ++diagnostics_.rectangleQueries;
  const auto finishFailure = [this](AssemblyViewportPickingQueryStatus status) {
    ++diagnostics_.queryFailures;
    if (status == AssemblyViewportPickingQueryStatus::StalePublication)
      ++diagnostics_.staleQueryRejections;
    if (status == AssemblyViewportPickingQueryStatus::Uncertain)
      ++diagnostics_.uncertainQueries;
    return AssemblyEdgeRectangleQueryResult{status, {}};
  };
  try {
    const auto rectangle =
        logicalRectangle(firstLogicalCorner, secondLogicalCorner);
    if (!rectangle)
      return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
    AssemblyViewportPickingQueryStatus projectionStatus;
    const auto projection = projectedFor(camera, projectionStatus);
    if (!projection) return finishFailure(projectionStatus);

    std::size_t primitiveWork = 0;
    std::size_t occluderWork = 0;
    std::vector<const ProjectedScene::EdgePrimitive*> selected;
    selected.reserve(projection->edges.size());
    for (const auto& edge : projection->edges) {
      if (!withinBudget(primitiveWork, budget.maximumPrimitiveTests))
        return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
      const auto clipped = clipSegment(edge.first, edge.second, *rectangle);
      if (!clipped) continue;
      const double depthEpsilon =
          std::max({1.0, camera.depthExtent,
                    projection->published->scene->worldBounds()
                        ? projection->published->scene->worldBounds()->diagonal()
                        : 1.0}) *
          1e-8;
      const SpatialVisibility visibility = segmentVisibilityExact(
          *projection, clipped->first, clipped->second, depthEpsilon,
          occluderWork, budget.maximumOccluderTests, diagnostics_);
      if (visibility == SpatialVisibility::Uncertain)
        return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
      if (visibility == SpatialVisibility::Stale)
        return finishFailure(
            AssemblyViewportPickingQueryStatus::StalePublication);
      if (visibility == SpatialVisibility::Invalid)
        return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
      if (visibility == SpatialVisibility::Visible) selected.push_back(&edge);
    }
    if (!projection->published->scene->isCurrent() ||
        !projection->publication.isSameCurrentPublication(
            projection->published->scene->publicationHandle()))
      return finishFailure(
          AssemblyViewportPickingQueryStatus::StalePublication);
    std::sort(selected.begin(), selected.end(), [](const auto* left,
                                                   const auto* right) {
      return identityLess(*left, *right);
    });
    selected.erase(
        std::unique(selected.begin(), selected.end(), [](const auto* left,
                                                         const auto* right) {
          return left->instance == right->instance &&
                 left->rawEdge == right->rawEdge;
        }),
        selected.end());
    std::vector<AssemblyEdgeRectangleHit> hits;
    hits.reserve(selected.size());
    for (const auto* edge : selected) {
      const auto& source = *edge->instance->source;
      hits.push_back({{edge->instance->occurrence,
                       DefinitionId{source.key.definitionId},
                       source.edges[edge->rawEdge]},
                      projection->publication});
    }
    return {hits.empty() ? AssemblyViewportPickingQueryStatus::Miss
                         : AssemblyViewportPickingQueryStatus::Hit,
            std::move(hits)};
  } catch (...) {
    return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
  }
}

AssemblyViewportSnapQueryResult AssemblyViewportPicking::snapAt(
    const ViewportCameraState& camera, QPointF logicalPixel,
    double radiusLogicalPixels,
    AssemblyViewportSpatialBudget budget) const noexcept {
  ++diagnostics_.snapQueries;
  const auto finishFailure = [this](AssemblyViewportPickingQueryStatus status) {
    ++diagnostics_.queryFailures;
    if (status == AssemblyViewportPickingQueryStatus::StalePublication)
      ++diagnostics_.staleQueryRejections;
    if (status == AssemblyViewportPickingQueryStatus::Uncertain)
      ++diagnostics_.uncertainQueries;
    return AssemblyViewportSnapQueryResult{status, {}};
  };
  try {
    if (!std::isfinite(logicalPixel.x()) ||
        !std::isfinite(logicalPixel.y()) ||
        !std::isfinite(radiusLogicalPixels) || radiusLogicalPixels <= 0.0)
      return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
    AssemblyViewportPickingQueryStatus projectionStatus;
    const auto projection = projectedFor(camera, projectionStatus);
    if (!projection) return finishFailure(projectionStatus);
    const auto ray = makeRay(camera, logicalPixel);
    if (!ray)
      return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);

    struct VertexCandidate final {
      const ProjectedScene::EdgePrimitive* edge{};
      ProjectedPoint point;
      double distance{};
      double rayParameter{};
    };
    std::optional<VertexCandidate> bestVertex;
    std::size_t primitiveWork = 0;
    std::size_t occluderWork = 0;
    const double radiusSquared = radiusLogicalPixels * radiusLogicalPixels;
    const double sceneScale =
        projection->published->scene->worldBounds()
            ? std::max(
                  1.0,
                  projection->published->scene->worldBounds()->diagonal())
            : 1.0;
    const double depthTolerance =
        std::max({sceneScale, camera.depthExtent, 1.0}) * 1e-10;
    for (const auto& edge : projection->edges) {
      for (const ProjectedPoint& endpoint : {edge.first, edge.second}) {
        if (!withinBudget(primitiveWork, budget.maximumPrimitiveTests))
          return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
        const double dx = endpoint.screen.x() - logicalPixel.x();
        const double dy = endpoint.screen.y() - logicalPixel.y();
        const double squaredDistance = dx * dx + dy * dy;
        if (!std::isfinite(squaredDistance) ||
            squaredDistance > radiusSquared)
          continue;
        const SpatialVisibility visibility = pointVisibility(
            *projection->published, *ray, sceneScale, endpoint, occluderWork,
            budget.maximumOccluderTests, diagnostics_);
        if (visibility == SpatialVisibility::Uncertain)
          return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
        if (visibility == SpatialVisibility::Stale)
          return finishFailure(
              AssemblyViewportPickingQueryStatus::StalePublication);
        if (visibility == SpatialVisibility::Invalid)
          return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
        if (visibility != SpatialVisibility::Visible) continue;
        const double rayParameter = std::clamp(
            ray->nearDepth - endpoint.depth, 0.0, ray->length);
        VertexCandidate candidate{&edge, endpoint,
                                  std::sqrt(squaredDistance),
                                  rayParameter};
        bool replace = !bestVertex ||
                       candidate.rayParameter <
                           bestVertex->rayParameter - depthTolerance;
        if (bestVertex &&
            std::abs(candidate.rayParameter - bestVertex->rayParameter) <=
                depthTolerance) {
          if (candidate.distance < bestVertex->distance - 1e-10)
            replace = true;
          else if (std::abs(candidate.distance - bestVertex->distance) <=
                   1e-10)
            replace = identityLess(*candidate.edge, *bestVertex->edge);
        }
        if (replace) bestVertex = candidate;
      }
    }
    if (bestVertex) {
      if (!projection->published->scene->isCurrent() ||
          !projection->publication.isSameCurrentPublication(
              projection->published->scene->publicationHandle()))
        return finishFailure(
            AssemblyViewportPickingQueryStatus::StalePublication);
      const auto& instance = *bestVertex->edge->instance;
      const auto& source = *instance.source;
      AssemblyEdgeAddress support{instance.occurrence,
                                  DefinitionId{source.key.definitionId},
                                  source.edges[bestVertex->edge->rawEdge]};
      AssemblyViewportSnapHit hit{
          AssemblySnapKind::Vertex,
          instance.occurrence,
          DefinitionId{source.key.definitionId},
          bestVertex->point.world,
          bestVertex->point.screen,
          bestVertex->point.depth,
          std::nullopt,
          std::move(support),
          projection->publication};
      return {AssemblyViewportPickingQueryStatus::Hit, std::move(hit)};
    }

    struct EdgeCandidate final {
      const ProjectedScene::EdgePrimitive* edge{};
      ProjectedPoint point;
      double distance{};
      double rayParameter{};
    };
    std::optional<EdgeCandidate> bestEdge;
    for (const auto& edge : projection->edges) {
      if (!withinBudget(primitiveWork, budget.maximumPrimitiveTests))
        return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
      const QPointF delta = edge.second.screen - edge.first.screen;
      const double lengthSquared = QPointF::dotProduct(delta, delta);
      const double parameter =
          lengthSquared <= 1e-18
              ? 0.0
              : std::clamp(QPointF::dotProduct(
                               logicalPixel - edge.first.screen, delta) /
                               lengthSquared,
                           0.0, 1.0);
      const ProjectedPoint candidatePoint =
          interpolate(edge.first, edge.second, parameter);
      const double dx = candidatePoint.screen.x() - logicalPixel.x();
      const double dy = candidatePoint.screen.y() - logicalPixel.y();
      const double squaredDistance = dx * dx + dy * dy;
      if (!std::isfinite(squaredDistance) ||
          squaredDistance > radiusSquared)
        continue;
      const SpatialVisibility visibility = pointVisibility(
          *projection->published, *ray, sceneScale, candidatePoint,
          occluderWork,
          budget.maximumOccluderTests, diagnostics_);
      if (visibility == SpatialVisibility::Uncertain)
        return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
      if (visibility == SpatialVisibility::Stale)
        return finishFailure(
            AssemblyViewportPickingQueryStatus::StalePublication);
      if (visibility == SpatialVisibility::Invalid)
        return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
      if (visibility != SpatialVisibility::Visible) continue;
      const double rayParameter = std::clamp(
          ray->nearDepth - candidatePoint.depth, 0.0, ray->length);
      EdgeCandidate candidate{&edge, candidatePoint,
                              std::sqrt(squaredDistance),
                              rayParameter};
      bool replace = !bestEdge ||
                     candidate.rayParameter <
                         bestEdge->rayParameter - depthTolerance;
      if (bestEdge &&
          std::abs(candidate.rayParameter - bestEdge->rayParameter) <=
              depthTolerance) {
        if (candidate.distance < bestEdge->distance - 1e-10)
          replace = true;
        else if (std::abs(candidate.distance - bestEdge->distance) <= 1e-10)
          replace = identityLess(*candidate.edge, *bestEdge->edge);
      }
      if (replace) bestEdge = candidate;
    }
    if (bestEdge) {
      if (!projection->publication.isSameCurrentPublication(
              projection->published->scene->publicationHandle()))
        return finishFailure(
            AssemblyViewportPickingQueryStatus::StalePublication);
      const auto& instance = *bestEdge->edge->instance;
      const auto& source = *instance.source;
      AssemblyEdgeAddress address{instance.occurrence,
                                  DefinitionId{source.key.definitionId},
                                  source.edges[bestEdge->edge->rawEdge]};
      AssemblyViewportSnapHit hit{
          AssemblySnapKind::Edge,
          instance.occurrence,
          DefinitionId{source.key.definitionId},
          bestEdge->point.world,
          bestEdge->point.screen,
          bestEdge->point.depth,
          std::nullopt,
          std::move(address),
          projection->publication};
      return {AssemblyViewportPickingQueryStatus::Hit, std::move(hit)};
    }

    struct FaceCandidate final {
      const ProjectedScene::FacePrimitive* face{};
      ProjectedPoint point;
      double rayParameter{};
    };
    std::optional<FaceCandidate> bestFace;
    for (const auto& face : projection->faces) {
      if (!withinBudget(primitiveWork, budget.maximumPrimitiveTests))
        return finishFailure(AssemblyViewportPickingQueryStatus::Uncertain);
      const auto weights = barycentricAt(logicalPixel, face.points);
      constexpr double insideTolerance = -1e-10;
      if (!weights || (*weights)[0] < insideTolerance ||
          (*weights)[1] < insideTolerance ||
          (*weights)[2] < insideTolerance)
        continue;
      ProjectedPoint point;
      point.screen = logicalPixel;
      point.depth = planeDepth(face.points, *weights);
      point.world = {
          face.points[0].world.x * (*weights)[0] +
              face.points[1].world.x * (*weights)[1] +
              face.points[2].world.x * (*weights)[2],
          face.points[0].world.y * (*weights)[0] +
              face.points[1].world.y * (*weights)[1] +
              face.points[2].world.y * (*weights)[2],
          face.points[0].world.z * (*weights)[0] +
              face.points[1].world.z * (*weights)[1] +
              face.points[2].world.z * (*weights)[2]};
      if (!finite(vector(point.world)) || !std::isfinite(point.depth))
        return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
      const BoundedRayParameter parameter =
          boundedRayParameter(*ray, point.depth, sceneScale);
      if (parameter.status == ClipParameterStatus::Invalid)
        return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
      if (parameter.status == ClipParameterStatus::Outside) continue;
      FaceCandidate candidate{&face, point, parameter.value};
      bool replace = !bestFace ||
                     candidate.rayParameter <
                         bestFace->rayParameter - depthTolerance;
      if (bestFace &&
          std::abs(candidate.rayParameter - bestFace->rayParameter) <=
              depthTolerance)
        replace = identityLess(*candidate.face, *bestFace->face);
      if (replace) bestFace = candidate;
    }
    if (bestFace) {
      if (!projection->publication.isSameCurrentPublication(
              projection->published->scene->publicationHandle()))
        return finishFailure(
            AssemblyViewportPickingQueryStatus::StalePublication);
      const auto& instance = *bestFace->face->instance;
      const auto& source = *instance.source;
      AssemblyFaceAddress address{instance.occurrence,
                                  DefinitionId{source.key.definitionId},
                                  source.faces[bestFace->face->rawFace]};
      AssemblyViewportSnapHit hit{
          AssemblySnapKind::Surface,
          instance.occurrence,
          DefinitionId{source.key.definitionId},
          bestFace->point.world,
          bestFace->point.screen,
          bestFace->point.depth,
          std::move(address),
          std::nullopt,
          projection->publication};
      return {AssemblyViewportPickingQueryStatus::Hit, std::move(hit)};
    }
    if (!projection->published->scene->isCurrent() ||
        !projection->publication.isSameCurrentPublication(
            projection->published->scene->publicationHandle()))
      return finishFailure(
          AssemblyViewportPickingQueryStatus::StalePublication);
    return {AssemblyViewportPickingQueryStatus::Miss, {}};
  } catch (...) {
    return finishFailure(AssemblyViewportPickingQueryStatus::InvalidInput);
  }
}

void AssemblyViewportPicking::clear() noexcept {
  active_.reset();
  projected_.reset();
  cache_.clear();
  diagnostics_.residentTopologySources = 0;
  diagnostics_.activeSources = 0;
  diagnostics_.activeInstances = 0;
  diagnostics_.activeGeneration = {};
}

}  // namespace solidar

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "sketch/Sketch.h"

namespace solidar {

struct SketchHitPoint {
  double x{};
  double y{};
};

struct SketchScreenSegment {
  SketchHitPoint first;
  SketchHitPoint second;
};

struct SketchScreenBox {
  SketchHitPoint minimum;
  SketchHitPoint maximum;
};

struct SketchScreenOrientedBox {
  SketchHitPoint center;
  double halfWidth{};
  double halfHeight{};
  double angleRad{};
};

enum class SketchBoxHitPolicy {
  SegmentIntersection,
  CurveBoundsOrCenter
};

enum class SketchPickEntityKind { Line, Circle, Arc };

struct SketchPickEntityRef {
  SketchPickEntityKind kind{SketchPickEntityKind::Line};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  std::size_t elementId{};
  bool dashed{};
  bool construction{};
  bool projected{};

  bool operator==(const SketchPickEntityRef&) const noexcept = default;
};

enum class SketchPickPointKind {
  LineEndpoint,
  CircleCenter,
  ArcEndpoint,
  ElementCenter,
  LineMidpoint
};

struct SketchPickPointRef {
  SketchPickPointKind kind{SketchPickPointKind::LineEndpoint};
  sketch::PointReference reference;
  sketch::Point point;
  sketch::GeometryId carrierId{sketch::kInvalidGeometryId};
  std::size_t elementId{};

  bool operator==(const SketchPickPointRef&) const noexcept = default;
};

enum class SketchPickDatumKind { Origin, XAxis, YAxis };

struct SketchPickDatumRef {
  SketchPickDatumKind kind{SketchPickDatumKind::Origin};
  sketch::Point point;

  bool operator==(const SketchPickDatumRef&) const noexcept = default;
};

enum class SketchProjectionSource {
  ReferenceBody,
  ReferenceFace,
  SceneBody
};

// The edge slot is deliberately ephemeral. Canvas must revalidate source,
// meshRevision and slot against the current mesh immediately before use.
struct SketchProjectionEdgeToken {
  SketchProjectionSource source{SketchProjectionSource::ReferenceBody};
  std::uint64_t meshRevision{};
  std::size_t edgeSlot{};
  std::size_t sceneBodySlot{};

  bool operator==(const SketchProjectionEdgeToken&) const noexcept = default;
};

// The slot is deliberately transient; persistent identity is DimensionId.
// Canvas revalidates both immediately before applying a hit.
enum class SketchDimensionHitKind { Geometry, Label };

struct SketchDimensionToken {
  std::size_t transientSlot{};
  sketch::DimensionId dimensionId{sketch::kInvalidDimensionId};
  SketchDimensionHitKind hitKind{SketchDimensionHitKind::Geometry};
  sketch::DimensionKind kind{sketch::DimensionKind::LineLength};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  sketch::PointReference firstPoint;
  sketch::PointReference secondPoint;

  bool operator==(const SketchDimensionToken&) const noexcept = default;
};

using SketchPickTarget =
    std::variant<SketchPickEntityRef, SketchPickPointRef, SketchPickDatumRef,
                 SketchProjectionEdgeToken, SketchDimensionToken>;

struct SketchPickCandidate {
  SketchPickTarget target;
  std::vector<SketchScreenSegment> segments;
  std::optional<SketchHitPoint> point;
  double tolerancePx{};
  int priority{};
  std::size_t stableOrder{};
  bool visible{true};
  bool enabled{true};
  std::vector<SketchScreenBox> boxes;
  std::optional<SketchScreenOrientedBox> orientedBox;
  SketchBoxHitPolicy boxHitPolicy{SketchBoxHitPolicy::SegmentIntersection};
  std::optional<SketchScreenBox> selectionBounds;
  std::optional<SketchHitPoint> selectionCenter;
};

struct SketchHitScene {
  std::vector<SketchPickCandidate> candidates;
};

struct SketchHitTolerancePolicy {
  double entityPx{9.0};
  double pointPx{8.0};
  double endpointPx{9.0};
  double midpointPx{14.0};
  double constructionBodyPx{10.0};
  double datumAxisPx{7.0};
  double dimensionPx{8.0};
  double projectionPx{9.0};
  double trimPx{9.0};

  [[nodiscard]] bool valid() const noexcept;
};

struct SketchPickFilter {
  bool entities{true};
  bool points{true};
  bool datums{true};
  bool projections{true};
  bool dimensions{true};
  bool dimensionGeometry{true};
  bool dimensionLabels{true};
  bool lines{true};
  bool circles{true};
  bool arcs{true};
  bool lineEndpoints{true};
  bool circleCenters{true};
  bool arcEndpoints{true};
  bool elementCenters{true};
  bool lineMidpoints{true};
  bool includeConstruction{true};
  bool includeProjected{true};
};

struct SketchPickResult {
  SketchPickTarget target;
  double distancePx{};
  int priority{};

  [[nodiscard]] const SketchPickEntityRef* entity() const noexcept;
  [[nodiscard]] const SketchPickPointRef* point() const noexcept;
  [[nodiscard]] const SketchPickDatumRef* datum() const noexcept;
  [[nodiscard]] const SketchProjectionEdgeToken* projection() const noexcept;
  [[nodiscard]] const SketchDimensionToken* dimension() const noexcept;
};

class SketchHitTester final {
 public:
  [[nodiscard]] static std::optional<SketchPickResult> pick(
      const SketchHitScene& scene, SketchHitPoint cursor,
      const SketchPickFilter& filter = {}) noexcept;

  [[nodiscard]] static std::vector<SketchPickTarget> pickInBox(
      const SketchHitScene& scene, SketchHitPoint first,
      SketchHitPoint second,
      const SketchPickFilter& filter = {});

  [[nodiscard]] static double distanceToSegment(
      SketchHitPoint point, SketchScreenSegment segment) noexcept;
  [[nodiscard]] static double distanceToSegments(
      SketchHitPoint point,
      const std::vector<SketchScreenSegment>& segments) noexcept;
  [[nodiscard]] static bool finite(SketchHitPoint point) noexcept;

 private:
  [[nodiscard]] static bool accepts(const SketchPickTarget& target,
                                    const SketchPickFilter& filter) noexcept;
  [[nodiscard]] static std::optional<double> candidateDistance(
      const SketchPickCandidate& candidate,
      SketchHitPoint cursor) noexcept;
};

}  // namespace solidar

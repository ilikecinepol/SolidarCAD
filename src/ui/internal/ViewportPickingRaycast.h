#pragma once

#include <QPointF>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "ui/ViewportCamera.h"
#include "ui/internal/ViewportPickingTopology.h"

namespace solidar::detail {

struct ViewportPickingRayInstance final {
  std::shared_ptr<const ViewportPickingTopologySource> source;
  std::array<double, 16> localToWorld{};
  Bounds3d worldBounds;
  std::size_t ordinal{};
};

enum class ViewportPickingRayStatus : std::uint8_t {
  Hit,
  Miss,
  StalePublication,
  InvalidInput,
  ResourceFailure,
};

struct ViewportPickingRayCounters final {
  std::uint64_t* broadPhaseCandidates{};
  std::uint64_t* triangleTests{};
  std::uint64_t* edgeSegmentTests{};
  std::uint64_t* occluderTests{};
};

struct ViewportPickingFaceRayHit final {
  std::size_t instanceOrdinal{};
  FaceReference face;
  Point3d worldPoint{};
  double rayParameter{};
  double cameraDepth{};
};

struct ViewportPickingEdgeRayHit final {
  std::size_t instanceOrdinal{};
  EdgeReference edge;
  Point3d worldPoint{};
  double rayParameter{};
  double cameraDepth{};
  double distance{};
};

struct ViewportPickingFaceRayResult final {
  ViewportPickingRayStatus status{ViewportPickingRayStatus::InvalidInput};
  std::optional<ViewportPickingFaceRayHit> hit;
};

struct ViewportPickingEdgeRayResult final {
  ViewportPickingRayStatus status{ViewportPickingRayStatus::InvalidInput};
  std::optional<ViewportPickingEdgeRayHit> hit;
};

[[nodiscard]] ViewportPickingFaceRayResult viewportFaceAt(
    const std::vector<ViewportPickingRayInstance>& instances,
    const std::optional<Bounds3d>& sceneBounds,
    const ViewportCameraState& camera, QPointF logicalPixel,
    const std::function<bool()>& current,
    ViewportPickingRayCounters counters) noexcept;

[[nodiscard]] ViewportPickingEdgeRayResult viewportEdgeAt(
    const std::vector<ViewportPickingRayInstance>& instances,
    const std::optional<Bounds3d>& sceneBounds,
    const ViewportCameraState& camera, QPointF logicalPixel,
    double radiusLogicalPixels, const std::function<bool()>& current,
    ViewportPickingRayCounters counters) noexcept;

}  // namespace solidar::detail

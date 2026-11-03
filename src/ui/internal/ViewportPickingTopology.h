#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"
#include "ui/BodyRenderMesh.h"

class TopoDS_Shape;

namespace solidar {

class TopologyIndex;
class TopologyLineage;

namespace detail {

struct ViewportPickingTopologySource final {
  BodyMeshKey key;
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const BodyRenderMesh> mesh;
  std::shared_ptr<const TopologyIndex> topology;
  std::vector<FaceReference> faces;
  std::vector<EdgeReference> edges;
};

using ViewportPickingTopologyCache =
    std::unordered_map<BodyMeshKey,
                       std::shared_ptr<const ViewportPickingTopologySource>,
                       BodyMeshKeyHash>;

struct ViewportPickingTopologyInput final {
  BodyMeshKey key;
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const TopologyLineage> topologyLineage;
  std::shared_ptr<const BodyRenderMesh> mesh;
};

enum class ViewportPickingTopologyStatus : std::uint8_t {
  Prepared,
  StalePublication,
  InvalidSource,
  DuplicateSource,
  TopologyBuildFailed,
  InvalidTopologyMapping,
  ResourceFailure,
};

struct ViewportPickingTopologyCounters final {
  std::uint64_t* buildAttempts{};
  std::uint64_t* buildSuccesses{};
  std::uint64_t* buildFailures{};
  std::uint64_t* reuses{};
  std::uint64_t* v2Faces{};
  std::uint64_t* v1Faces{};
  std::uint64_t* v2Edges{};
  std::uint64_t* v1Edges{};
};

struct PreparedViewportPickingTopology final {
  ViewportPickingTopologyStatus status{
      ViewportPickingTopologyStatus::ResourceFailure};
  ViewportPickingTopologyCache cache;
  ViewportPickingTopologyCache activeSources;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ViewportPickingTopologyStatus::Prepared;
  }
};

[[nodiscard]] PreparedViewportPickingTopology prepareViewportPickingTopology(
    const std::vector<ViewportPickingTopologyInput>& inputs,
    const ViewportPickingTopologyCache& incumbent,
    const std::function<bool()>& current,
    std::size_t& failBuildCountdown, std::size_t& failMappingCountdown,
    ViewportPickingTopologyCounters counters) noexcept;

}  // namespace detail
}  // namespace solidar

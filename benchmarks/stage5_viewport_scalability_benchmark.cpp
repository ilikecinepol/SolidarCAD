#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QLineF>
#include <QRectF>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model/ExtrudeToolSession.h"
#include "ui/BodyRenderMesh.h"
#include "ui/PreviewUpdateCoordinator.h"
#include "ui/ViewportPicking.h"

namespace solidar {

class BodyRenderMeshBenchmarkAdapter final {
 public:
  static BodyRenderMesh create(std::vector<RenderVertex> vertices,
                               std::vector<std::uint32_t> indices,
                               std::vector<RenderEdge> edges, Point3d center,
                               double diagonal, std::size_t faceCount) {
    BodyRenderMesh mesh;
    mesh.vertices_ = std::move(vertices);
    mesh.triangleIndices_ = std::move(indices);
    mesh.edges_ = std::move(edges);
    mesh.center_ = center;
    mesh.diagonal_ = diagonal;
    mesh.faceCount_ = faceCount;
    for (const auto& edge : mesh.edges_) mesh.edgeSampleCount_ += edge.points.size();
    mesh.revision_ = 1;
    mesh.quality_ = ViewportMeshQuality::Normal;
    return mesh;
  }
};

}  // namespace solidar

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kHashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

struct Options {
  std::string implementation;
  std::string fixture{"grid"};
  std::string measurement{"actual"};
  std::string scenario;
  std::size_t count{};
  std::size_t warmups{5};
  std::size_t samples{101};
};

struct Work {
  std::uint64_t primitiveTests{};
  std::uint64_t occlusionActual{};
  std::uint64_t occlusionEstimated{};
  std::uint64_t nodeVisits{};
  std::uint64_t projectedPoints{};
  std::uint64_t previewRequests{};
  std::uint64_t previewExecutions{};
  std::uint64_t occtBuilds{};
  std::uint64_t meshBuildAttempts{};
  Work& operator+=(const Work& value) {
    primitiveTests += value.primitiveTests;
    occlusionActual += value.occlusionActual;
    occlusionEstimated += value.occlusionEstimated;
    nodeVisits += value.nodeVisits;
    projectedPoints += value.projectedPoints;
    previewRequests += value.previewRequests;
    previewExecutions += value.previewExecutions;
    occtBuilds += value.occtBuilds;
    meshBuildAttempts += value.meshBuildAttempts;
    return *this;
  }
};

struct OperationResult {
  std::uint64_t hash{kHashOffset};
  Work work;
  std::uint64_t maxRequestNs{};
  std::uint64_t flushNs{};
  std::uint64_t maxSyncNs{};
};

struct Result {
  std::uint64_t p50{};
  std::uint64_t p95{};
  std::uint64_t p99{};
  std::uint64_t setupNs{};
  std::uint64_t hash{kHashOffset};
  Work work;
  std::uint64_t inputMeshBytes{};
  std::uint64_t derivedPeakBytes{};
  std::uint64_t rssBeforeBytes{};
  std::uint64_t peakRssBytes{};
  std::uint64_t maxRequestNs{};
  std::uint64_t flushNs{};
  std::uint64_t maxSyncNs{};
};

void hashValue(std::uint64_t& hash, std::uint64_t value) {
  hash ^= value;
  hash *= kHashPrime;
}

template <typename Range>
std::uint64_t hashIndices(const Range& values) {
  std::uint64_t hash = kHashOffset;
  hashValue(hash, values.size());
  for (const auto value : values) hashValue(hash, value);
  return hash;
}

std::uint64_t capacityBytes(std::size_t capacity, std::size_t elementSize) {
  if (capacity && elementSize > std::numeric_limits<std::uint64_t>::max() / capacity)
    return std::numeric_limits<std::uint64_t>::max();
  return static_cast<std::uint64_t>(capacity) * elementSize;
}

std::uint64_t currentRssBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX value{};
  value.cb = sizeof(value);
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value),
                            sizeof(value))) return 0;
  return static_cast<std::uint64_t>(value.WorkingSetSize);
#else
  return 0;
#endif
}

std::uint64_t peakRssBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX value{};
  value.cb = sizeof(value);
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value),
                            sizeof(value))) return 0;
  return static_cast<std::uint64_t>(value.PeakWorkingSetSize);
#elif defined(__unix__) || defined(__APPLE__)
  rusage value{};
  if (getrusage(RUSAGE_SELF, &value)) return 0;
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(value.ru_maxrss);
#else
  return static_cast<std::uint64_t>(value.ru_maxrss) * 1024ULL;
#endif
#else
  return 0;
#endif
}

solidar::BodyRenderMesh makeMesh(std::size_t count, bool dense) {
  if (!count || count > std::numeric_limits<std::uint32_t>::max() / 3ULL)
    throw std::invalid_argument("count exceeds benchmark fixture limits");
  std::vector<solidar::RenderVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<solidar::RenderEdge> edges;
  vertices.reserve(count * 3);
  indices.reserve(count * 3);
  const std::size_t columns = dense ? 1 : std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count)))));
  for (std::size_t index = 0; index < count; ++index) {
    const double x = dense ? 0.0 : static_cast<double>(index % columns) * 2.0;
    const double y = dense ? 0.0 : static_cast<double>(index / columns) * 2.0;
    const double z = dense ? static_cast<double>(index) * 1e-4 : 0.0;
    const auto base = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({{x, y, z}, {0.0, 0.0, 1.0}, static_cast<std::uint32_t>(index)});
    vertices.push_back({{x + 1.25, y, z}, {0.0, 0.0, 1.0}, static_cast<std::uint32_t>(index)});
    vertices.push_back({{x, y + 1.25, z}, {0.0, 0.0, 1.0}, static_cast<std::uint32_t>(index)});
    indices.insert(indices.end(), {base, base + 1, base + 2});
  }
  // One polyline per grid row avoids a million per-edge heap allocations.
  // Dense fixtures intentionally use one edge per depth layer for the actual
  // quadratic calibration, which is capped by the runner at 4096.
  if (dense) {
    edges.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const double z = static_cast<double>(index) * 1e-4;
      edges.push_back({{{0.15, 0.15, z}, {1.0, 0.15, z}}, index});
    }
  } else {
    const std::size_t rows = (count + columns - 1) / columns;
    edges.reserve(rows);
    for (std::size_t row = 0; row < rows; ++row) {
      solidar::RenderEdge edge;
      edge.edgeIndex = row;
      const std::size_t rowCount = std::min(columns, count - row * columns);
      edge.points.reserve(rowCount + 1);
      for (std::size_t column = 0; column <= rowCount; ++column)
        edge.points.push_back({static_cast<double>(column) * 2.0,
                               static_cast<double>(row) * 2.0 + 0.15, 0.0});
      edges.push_back(std::move(edge));
    }
  }
  const double width = dense ? 1.25 : static_cast<double>(columns) * 2.0;
  const double height = dense ? 1.25
      : static_cast<double>((count + columns - 1) / columns) * 2.0;
  const double depth = dense ? static_cast<double>(count) * 1e-4 : 0.0;
  return solidar::BodyRenderMeshBenchmarkAdapter::create(
      std::move(vertices), std::move(indices), std::move(edges),
      {width * 0.5, height * 0.5, depth * 0.5},
      std::sqrt(width * width + height * height + depth * depth), count);
}

solidar::ViewportCameraState cameraFor(const solidar::BodyRenderMesh& mesh,
                                       std::size_t step = 0) {
  return {static_cast<float>(step) * 0.01F, 0.0F, 1.0F, {}, QSize(4096, 4096),
          1.0F, mesh.center(), std::max(1.0, mesh.diagonal() * 3.0)};
}

struct HistoricalScene {
  std::vector<solidar::ProjectedTriangle> triangles;
  std::vector<solidar::ProjectedEdge> edges;
  double depthEpsilon{};

  void rebuild(const solidar::BodyRenderMesh& mesh,
               const solidar::ViewportCameraState& camera, Work& work) {
    triangles.clear();
    edges.clear();
    triangles.reserve(mesh.triangleCount());
    edges.reserve(mesh.edges().size());
    depthEpsilon = mesh.diagonal() * solidar::kDepthEpsilonScale;
    for (std::size_t index = 0; index < mesh.triangleCount(); ++index) {
      const auto triangle = mesh.triangle(index);
      const auto project = [&](solidar::Point3d point) {
        ++work.projectedPoints;
        return solidar::ProjectedPoint{camera.worldToScreen(point),
                                       camera.cameraDepth(point)};
      };
      triangles.push_back({project(triangle.a), project(triangle.b),
                           project(triangle.c), triangle.faceIndex,
                           depthEpsilon});
    }
    for (const auto& source : mesh.edges()) {
      solidar::ProjectedEdge edge;
      edge.edgeIndex = source.edgeIndex;
      edge.points.reserve(source.points.size());
      for (const auto point : source.points) {
        ++work.projectedPoints;
        edge.points.push_back(
            {camera.worldToScreen(point), camera.cameraDepth(point)});
      }
      edges.push_back(std::move(edge));
    }
  }

  std::optional<std::size_t> faceAt(QPointF point, Work& work) const {
    std::optional<std::size_t> face;
    double best = -std::numeric_limits<double>::max();
    for (const auto& triangle : triangles) {
      ++work.primitiveTests;
      const auto depth = solidar::triangleDepthAt(
          point, triangle.a, triangle.b, triangle.c);
      if (!depth) continue;
      if (!face || *depth > best + triangle.depthEpsilon ||
          (std::abs(*depth - best) <= triangle.depthEpsilon &&
           triangle.faceIndex < *face)) {
        face = triangle.faceIndex;
        best = *depth;
      }
    }
    return face;
  }

  std::optional<std::size_t> edgeAt(QPointF point, double radius,
                                    Work& work) const {
    std::optional<std::size_t> result;
    double bestDistance = radius;
    double bestDepth = -std::numeric_limits<double>::max();
    for (const auto& edge : edges) {
      for (std::size_t index = 1; index < edge.points.size(); ++index) {
        ++work.primitiveTests;
        const auto hit = solidar::closestSegmentHit(
            point, edge.points[index - 1], edge.points[index]);
        if (hit.distance > radius) continue;
        bool hidden = false;
        for (const auto& triangle : triangles) {
          ++work.occlusionActual;
          const auto depth = solidar::triangleDepthAt(
              hit.parameter == 0.0 ? edge.points[index - 1].screen
                                   : QPointF(edge.points[index - 1].screen +
                                      (edge.points[index].screen - edge.points[index - 1].screen) *
                                          hit.parameter),
              triangle.a, triangle.b, triangle.c);
          if (depth && *depth > hit.depth + triangle.depthEpsilon) {
            hidden = true;
          }
        }
        if (hidden) continue;
        if (!result || hit.distance < bestDistance ||
            (hit.distance == bestDistance &&
             (hit.depth > bestDepth ||
              (hit.depth == bestDepth && edge.edgeIndex < *result)))) {
          result = edge.edgeIndex;
          bestDistance = hit.distance;
          bestDepth = hit.depth;
        }
      }
    }
    return result;
  }

  std::vector<std::size_t> facesInRect(const QRectF& rect, Work& work) const {
    // Frozen pre-index implementation copied from the removed ViewportPicking
    // collector. It intentionally retains the nested full scan and original
    // clipped-centroid/control-flow semantics.
    std::vector<std::size_t> result;
    for (const auto& triangle : triangles) {
      ++work.primitiveTests;
      if (!solidar::triangleIntersectsRect(triangle.a, triangle.b,
                                           triangle.c, rect))
        continue;
      const auto clipped = solidar::clipTriangleToRect(
          triangle.a, triangle.b, triangle.c, rect);
      if (clipped.size() < 3) continue;
      QPointF sample;
      double sampleDepth = 0.0;
      for (const auto& vertex : clipped) {
        sample += vertex.screen;
        sampleDepth += vertex.depth;
      }
      sample /= static_cast<double>(clipped.size());
      sampleDepth /= static_cast<double>(clipped.size());
      bool frontmost = true;
      for (const auto& other : triangles) {
        if (other.faceIndex == triangle.faceIndex) continue;
        ++work.occlusionActual;
        const auto depth = solidar::triangleDepthAt(
            sample, other.a, other.b, other.c);
        if (depth && *depth > sampleDepth + depthEpsilon) {
          frontmost = false;
          break;
        }
      }
      if (!frontmost) continue;
      if (std::find(result.begin(), result.end(), triangle.faceIndex) ==
          result.end())
        result.push_back(triangle.faceIndex);
    }
    return result;
  }

  std::vector<std::size_t> edgesInRect(const QRectF& rect, Work& work) const {
    // Frozen pre-index implementation copied from the removed collector,
    // including clipped midpoint sampling and the accepted-edge break.
    std::vector<std::size_t> result;
    for (const auto& edge : edges) {
      for (std::size_t index = 1; index < edge.points.size(); ++index) {
        ++work.primitiveTests;
        const auto& a = edge.points[index - 1];
        const auto& b = edge.points[index];
        if (!solidar::segmentIntersectsRect(a, b, rect)) continue;
        const auto clipped = solidar::clipSegmentToRect(a, b, rect);
        if (!clipped) continue;
        const QPointF midpoint =
            (clipped->first.screen + clipped->second.screen) * 0.5;
        const double edgeDepth =
            (clipped->first.depth + clipped->second.depth) * 0.5;
        double surfaceDepth = -std::numeric_limits<double>::max();
        for (const auto& triangle : triangles) {
          ++work.occlusionActual;
          if (const auto depth = solidar::triangleDepthAt(
                  midpoint, triangle.a, triangle.b, triangle.c))
            surfaceDepth = std::max(surfaceDepth, *depth);
        }
        if (edgeDepth + depthEpsilon < surfaceDepth) continue;
        if (std::find(result.begin(), result.end(), edge.edgeIndex) ==
            result.end())
          result.push_back(edge.edgeIndex);
        break;
      }
    }
    return result;
  }

  std::uint64_t ownedBytes() const {
    std::uint64_t bytes = capacityBytes(triangles.capacity(), sizeof(triangles[0]));
    bytes += capacityBytes(edges.capacity(), sizeof(edges[0]));
    for (const auto& edge : edges)
      bytes += capacityBytes(edge.points.capacity(), sizeof(edge.points[0]));
    return bytes;
  }
};

void validateHistoricalMarqueeFixture() {
  constexpr double epsilon = 1e-9;
  const auto point = [](double x, double y, double depth) {
    return solidar::ProjectedPoint{{x, y}, depth};
  };
  const QRectF cornerRect(-0.1, -0.1, 1.2, 1.2);

  HistoricalScene clippedFace;
  clippedFace.depthEpsilon = epsilon;
  clippedFace.triangles = {
      {point(0.0, 0.0, 0.0), point(10.0, 0.0, 0.0),
       point(0.0, 10.0, 0.0), 7, epsilon},
      // Same-face overlap must not occlude face 7.
      {point(0.0, 0.0, 10.0), point(1.0, 0.0, 10.0),
       point(0.0, 1.0, 10.0), 7, epsilon},
      // Covers the raw triangle centroid but not the clipped centroid.
      {point(3.0, 3.0, 1.0), point(4.0, 3.0, 1.0),
       point(3.0, 4.0, 1.0), 8, epsilon}};
  Work faceWork;
  if (clippedFace.facesInRect(cornerRect, faceWork) !=
      std::vector<std::size_t>{7})
    throw std::runtime_error("historical clipped-face fixture failed");

  HistoricalScene clippedEdge;
  clippedEdge.depthEpsilon = epsilon;
  clippedEdge.triangles = {
      {point(0.0, 0.0, 0.0), point(10.0, 0.0, 0.0),
       point(0.0, 10.0, 0.0), 1, epsilon},
      // Covers the raw segment midpoint, outside the accepted rectangle.
      {point(4.5, -0.5, 1.0), point(5.5, -0.5, 1.0),
       point(5.0, 0.5, 1.0), 2, epsilon}};
  clippedEdge.edges = {{{point(0.0, 0.0, 0.0),
                          point(10.0, 0.0, 0.0),
                          point(20.0, 0.0, 0.0)},
                         11}};
  Work edgeWork;
  if (clippedEdge.edgesInRect(cornerRect, edgeWork) !=
          std::vector<std::size_t>{11} ||
      edgeWork.primitiveTests != 1)
    throw std::runtime_error("historical clipped-edge fixture failed");

  HistoricalScene earlyExit;
  earlyExit.depthEpsilon = epsilon;
  earlyExit.triangles = {
      {point(0.0, 0.0, 0.0), point(1.0, 0.0, 0.0),
       point(0.0, 1.0, 0.0), 1, epsilon},
      {point(0.0, 0.0, 1.0), point(1.0, 0.0, 1.0),
       point(0.0, 1.0, 1.0), 2, epsilon},
      {point(0.0, 0.0, 2.0), point(1.0, 0.0, 2.0),
       point(0.0, 1.0, 2.0), 3, epsilon}};
  Work exitWork;
  if (earlyExit.facesInRect(cornerRect, exitWork) !=
          std::vector<std::size_t>{3} ||
      exitWork.occlusionActual != 5)
    throw std::runtime_error("historical face control-flow fixture failed");

  // The frozen tolerance is mesh-based, never camera-depth based. A depth
  // difference between D*1e-4 and 3D*1e-4 must remain distinguishable.
  constexpr double boundaryEpsilon = 1e-11;
  HistoricalScene nearDepth;
  nearDepth.depthEpsilon = boundaryEpsilon;
  nearDepth.triangles = {
      {point(0.0, 0.0, 0.0), point(1.0, 0.0, 0.0),
       point(0.0, 1.0, 0.0), 1, boundaryEpsilon},
      {point(0.0, 0.0, 2e-11), point(1.0, 0.0, 2e-11),
       point(0.0, 1.0, 2e-11), 2, boundaryEpsilon}};
  nearDepth.edges = {{{point(0.0, 0.25, 0.0),
                        point(1.0, 0.25, 0.0)},
                       4}};
  Work boundaryFaceWork;
  Work boundaryEdgeWork;
  if (nearDepth.facesInRect(cornerRect, boundaryFaceWork) !=
          std::vector<std::size_t>{2} ||
      !nearDepth.edgesInRect(cornerRect, boundaryEdgeWork).empty())
    throw std::runtime_error("historical depth-epsilon fixture failed");

  std::vector<solidar::RenderVertex> epsilonVertices{
      {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 0},
      {{1e-7, 0.0, 0.0}, {0.0, 0.0, 1.0}, 0},
      {{0.0, 1e-7, 0.0}, {0.0, 0.0, 1.0}, 0}};
  auto epsilonMesh = solidar::BodyRenderMeshBenchmarkAdapter::create(
      std::move(epsilonVertices), {0, 1, 2}, {}, {5e-8, 5e-8, 0.0},
      std::sqrt(2.0) * 1e-7, 1);
  HistoricalScene rebuilt;
  Work rebuildWork;
  rebuilt.rebuild(epsilonMesh, cameraFor(epsilonMesh), rebuildWork);
  const double expectedEpsilon =
      epsilonMesh.diagonal() * solidar::kDepthEpsilonScale;
  if (expectedEpsilon >= 1e-9 || rebuilt.triangles.empty() ||
      std::abs(rebuilt.triangles.front().depthEpsilon - expectedEpsilon) >
          expectedEpsilon * 1e-12)
    throw std::runtime_error("historical mesh epsilon fixture failed");
}

Work counterDelta(const solidar::PickingQueryCounters& before,
                  const solidar::PickingQueryCounters& after) {
  Work result;
  result.primitiveTests = after.triangleCandidates - before.triangleCandidates +
                          after.segmentCandidates - before.segmentCandidates;
  result.occlusionActual = after.occluderTests - before.occluderTests;
  result.nodeVisits = after.nodeVisits - before.nodeVisits;
  return result;
}

template <typename Callback>
Result sample(std::size_t warmups, std::size_t samples, Callback&& callback) {
  for (std::size_t index = 0; index < warmups; ++index)
    static_cast<void>(callback(index));
  Result result;
  std::vector<std::uint64_t> timings;
  timings.reserve(samples);
  for (std::size_t index = 0; index < samples; ++index) {
    const auto started = Clock::now();
    const auto operation = callback(index);
    timings.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count()));
    hashValue(result.hash, operation.hash);
    result.work += operation.work;
    result.maxRequestNs = std::max(result.maxRequestNs, operation.maxRequestNs);
    result.flushNs = std::max(result.flushNs, operation.flushNs);
    result.maxSyncNs = std::max(result.maxSyncNs, operation.maxSyncNs);
  }
  std::sort(timings.begin(), timings.end());
  const auto percentile = [&](double fraction) {
    const auto ordinal = std::max<std::size_t>(1, static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(timings.size()))));
    return timings[std::min(timings.size() - 1, ordinal - 1)];
  };
  result.p50 = percentile(0.50);
  result.p95 = percentile(0.95);
  result.p99 = percentile(0.99);
  return result;
}

QPointF faceQuery(const solidar::BodyRenderMesh& mesh,
                  const solidar::ViewportCameraState& camera,
                  std::size_t sample) {
  const auto triangle = mesh.triangle((mesh.triangleCount() / 2 + sample * 7919) %
                                      mesh.triangleCount());
  return camera.worldToScreen({(triangle.a.x + triangle.b.x + triangle.c.x) / 3.0,
                               (triangle.a.y + triangle.b.y + triangle.c.y) / 3.0,
                               (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0});
}

QPointF edgeQuery(const solidar::BodyRenderMesh& mesh,
                  const solidar::ViewportCameraState& camera,
                  std::size_t sample) {
  const auto& edge = mesh.edges()[sample % mesh.edges().size()];
  return camera.worldToScreen(edge.points[edge.points.size() / 2]);
}

Result runPicking(const Options& options) {
  const bool dense = options.fixture == "dense";
  Result result;
  result.rssBeforeBytes = currentRssBytes();
  auto mesh = makeMesh(options.count, dense);
  auto camera = cameraFor(mesh);
  const auto setupStarted = Clock::now();
  HistoricalScene historical;
  solidar::ProjectedPickingScene production;
  const solidar::BodyMeshKey key{1, 1, mesh.revision(), &mesh, mesh.quality()};
  const std::vector<solidar::PickingMeshInput> inputs{{&mesh, 0, 0, key}};
  Work setupWork;
  if (options.implementation == "historical")
    historical.rebuild(mesh, camera, setupWork);
  else if (!production.ensure(inputs, camera))
    throw std::runtime_error("production scene build failed");
  result.setupNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - setupStarted).count());

  if (options.measurement == "estimate") {
    result.work.occlusionEstimated = static_cast<std::uint64_t>(options.count) *
                                     static_cast<std::uint64_t>(options.count);
    result.hash = kHashOffset;
  } else {
    std::size_t cameraRevision = 0;
    auto operation = [&](std::size_t sampleIndex) {
      OperationResult operation;
      if (options.scenario == "camera-reproject") {
        camera = cameraFor(mesh, ++cameraRevision);
        const auto before = production.counters();
        if (options.implementation == "historical") {
          historical.rebuild(mesh, camera, operation.work);
        } else {
          const std::uint64_t failuresBefore = before.buildFailures;
          const bool rebuilt = production.ensure(inputs, camera);
          if (!rebuilt &&
              production.counters().buildFailures != failuresBefore)
            throw std::runtime_error("camera reprojection failed");
          if (rebuilt)
            operation.work.projectedPoints =
                mesh.vertices().size() + mesh.edgeSampleCount();
          // A second ensure is an intentional cache hit. `false` means
          // unchanged here and must not be confused with a failed rebuild.
          const std::uint64_t failuresAfterBuild =
              production.counters().buildFailures;
          if (production.ensure(inputs, camera) ||
              production.counters().buildFailures != failuresAfterBuild)
            throw std::runtime_error("camera cache-hit contract failed");
        }
        // Hash real post-reprojection semantics, not the sample ordinal. These
        // probes are derived from stable world primitives and therefore catch
        // stale projected caches while remaining comparable across versions.
        const QPointF facePoint = faceQuery(mesh, camera, sampleIndex);
        const QPointF edgePoint = edgeQuery(mesh, camera, sampleIndex);
        const auto face = options.implementation == "historical"
            ? historical.faceAt(facePoint, operation.work)
            : [&] {
                const auto value = production.faceAt(facePoint);
                return value ? std::optional<std::size_t>(value->faceIndex)
                             : std::nullopt;
              }();
        const auto edge = options.implementation == "historical"
            ? historical.edgeAt(edgePoint, 3.0, operation.work)
            : [&] {
                const auto value = production.edgeAt(
                    edgePoint, 3.0, solidar::PickingQueryPrecision::Exact);
                return value ? std::optional<std::size_t>(value->edgeIndex)
                             : std::nullopt;
              }();
        const QRectF faceRect(facePoint - QPointF(1.0, 1.0),
                              QSizeF(2.0, 2.0));
        const QRectF edgeRect(edgePoint - QPointF(1.0, 1.0),
                              QSizeF(2.0, 2.0));
        const auto faces = options.implementation == "historical"
            ? historical.facesInRect(faceRect, operation.work)
            : production.facesInRect(faceRect);
        const auto edges = options.implementation == "historical"
            ? historical.edgesInRect(edgeRect, operation.work)
            : production.edgesInRect(edgeRect);
        hashValue(operation.hash, face ? *face + 1 : 0);
        hashValue(operation.hash, edge ? *edge + 1 : 0);
        hashValue(operation.hash, hashIndices(faces));
        hashValue(operation.hash, hashIndices(edges));
        if (options.implementation == "production")
          operation.work += counterDelta(before, production.counters());
        return operation;
      }
      const QPointF facePoint = faceQuery(mesh, camera, sampleIndex);
      const QPointF edgePoint = edgeQuery(mesh, camera, sampleIndex);
      const auto before = production.counters();
      if (options.scenario == "hover-face") {
        const auto hit = options.implementation == "historical"
            ? historical.faceAt(facePoint, operation.work)
            : [&] { const auto value = production.faceAt(facePoint);
                    return value ? std::optional<std::size_t>(value->faceIndex) : std::nullopt; }();
        hashValue(operation.hash, hit ? *hit + 1 : 0);
      } else if (options.scenario == "hover-edge" ||
                 options.scenario == "dense-occlusion") {
        const auto hit = options.implementation == "historical"
            ? historical.edgeAt(edgePoint, 3.0, operation.work)
            : [&] { const auto value = production.edgeAt(
                        edgePoint, 3.0, solidar::PickingQueryPrecision::Exact);
                    return value ? std::optional<std::size_t>(value->edgeIndex) : std::nullopt; }();
        if (options.scenario == "dense-occlusion") {
          const double epsilon =
              mesh.diagonal() * solidar::kDepthEpsilonScale;
          const bool inFrontDepthBand =
              hit && static_cast<double>((options.count - 1) - *hit) * 1e-4 <=
                         epsilon;
          hashValue(operation.hash, inFrontDepthBand ? 1 : 0);
        } else {
          hashValue(operation.hash, hit ? *hit + 1 : 0);
        }
      } else {
        const QPointF center = options.scenario == "marquee-face" ? facePoint : edgePoint;
        const QRectF rect(center - QPointF(1.0, 1.0), QSizeF(2.0, 2.0));
        const auto hits = options.scenario == "marquee-face"
            ? (options.implementation == "historical"
                   ? historical.facesInRect(rect, operation.work)
                   : production.facesInRect(rect))
            : (options.implementation == "historical"
                   ? historical.edgesInRect(rect, operation.work)
                   : production.edgesInRect(rect));
        operation.hash = hashIndices(hits);
      }
      if (options.implementation == "production")
        operation.work += counterDelta(before, production.counters());
      return operation;
    };
    result = [&] {
      auto measured = sample(options.warmups, options.samples, operation);
      measured.setupNs = result.setupNs;
      measured.rssBeforeBytes = result.rssBeforeBytes;
      return measured;
    }();
  }
  result.inputMeshBytes = mesh.ownedBytes();
  result.derivedPeakBytes = options.implementation == "historical"
      ? historical.ownedBytes() + capacityBytes(mesh.triangleCount(), sizeof(solidar::RenderTriangle))
      : production.ownedBytes();
  result.work += setupWork;
  result.peakRssBytes = peakRssBytes();
  return result;
}

solidar::DocumentSketch squareProfile() {
  solidar::DocumentSketch profile;
  profile.id = 1;
  profile.name = "Stage 5 drag profile";
  profile.geometry.addRectangle({-10.0, -10.0}, {10.0, 10.0});
  return profile;
}

std::uint64_t previewHash(double value, const solidar::BodyRenderMesh& mesh) {
  std::uint64_t hash = kHashOffset;
  hashValue(hash, static_cast<std::uint64_t>(std::llround(value * 1000.0)));
  hashValue(hash, mesh.triangleCount());
  hashValue(hash, mesh.edges().size());
  hashValue(hash, static_cast<std::uint64_t>(std::llround(mesh.diagonal() * 1000.0)));
  return hash;
}

Result runDrag(const Options& options) {
  Result result;
  result.rssBeforeBytes = currentRssBytes();
  const auto profile = squareProfile();
  QObject owner;
  solidar::PreviewUpdateCoordinator coordinator(&owner, 60'000);
  const auto setupStarted = Clock::now();
  result.setupNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - setupStarted).count());
  auto operation = [&](std::size_t sampleIndex) {
    OperationResult operationResult;
    solidar::ExtrudeToolSession session;
    session.beginSketch(profile, profile.id, {}, 1.0,
                        solidar::ExtrudeOperation::NewBody, false);
    ++operationResult.work.occtBuilds;
    std::uint64_t published = kHashOffset;
    solidar::BodyRenderMesh finalMesh;
    const double base = 1.0 + static_cast<double>(sampleIndex) * 0.001;
    const auto execute = [&](double value) {
      session.setSignedLength(value);
      ++operationResult.work.occtBuilds;
      const auto shape = session.previewShape();
      if (!shape || shape->IsNull()) throw std::runtime_error("extrude preview failed");
      ++operationResult.work.meshBuildAttempts;
      if (!finalMesh.tryRebuild(*shape, solidar::ViewportMeshQuality::Normal))
        throw std::runtime_error("preview mesh failed");
      published = previewHash(value, finalMesh);
      ++operationResult.work.previewExecutions;
    };
    for (std::size_t request = 0; request < options.count; ++request) {
      const double value = base + static_cast<double>(request) * 0.05;
      const auto started = Clock::now();
      if (options.implementation == "historical") {
        execute(value);
      } else {
        static_cast<void>(coordinator.request(
            [&, value](solidar::PreviewUpdateCoordinator::Token token) {
              execute(value);
              if (!coordinator.claimPublication(token))
                throw std::runtime_error("fresh preview publication rejected");
            }));
      }
      ++operationResult.work.previewRequests;
      const auto elapsed = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
      operationResult.maxRequestNs = std::max(operationResult.maxRequestNs, elapsed);
      operationResult.maxSyncNs = std::max(operationResult.maxSyncNs, elapsed);
    }
    if (options.implementation == "production") {
      const auto started = Clock::now();
      coordinator.flush();
      operationResult.flushNs = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
      operationResult.maxSyncNs = std::max(operationResult.maxSyncNs,
                                           operationResult.flushNs);
    }
    operationResult.hash = published;
    result.inputMeshBytes = finalMesh.ownedBytes();
    return operationResult;
  };
  auto measured = sample(options.warmups, options.samples, operation);
  measured.setupNs = result.setupNs;
  measured.rssBeforeBytes = result.rssBeforeBytes;
  measured.inputMeshBytes = result.inputMeshBytes;
  measured.derivedPeakBytes = result.inputMeshBytes;
  measured.peakRssBytes = peakRssBytes();
  return measured;
}

std::size_t parseSize(std::string_view value, bool allowZero = false) {
  std::size_t consumed{};
  const auto parsed = std::stoull(std::string(value), &consumed);
  if (consumed != value.size() || (!allowZero && parsed == 0) ||
      parsed > std::numeric_limits<std::size_t>::max())
    throw std::invalid_argument("invalid numeric argument");
  return static_cast<std::size_t>(parsed);
}

void usage(std::ostream& stream) {
  stream << "usage: stage5_viewport_scalability_benchmark "
            "--implementation historical|production --fixture grid|dense|drag "
            "--measurement actual|estimate --scenario hover-face|hover-edge|"
            "marquee-face|marquee-edge|camera-reproject|dense-occlusion|drag-preview "
            "--count N [--warmups N] [--samples N]\n";
}

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") { usage(std::cout); std::exit(EXIT_SUCCESS); }
    if (++index >= argc) throw std::invalid_argument("missing option value");
    const std::string value = argv[index];
    if (argument == "--implementation") options.implementation = value;
    else if (argument == "--fixture") options.fixture = value;
    else if (argument == "--measurement") options.measurement = value;
    else if (argument == "--scenario") options.scenario = value;
    else if (argument == "--count") options.count = parseSize(value);
    else if (argument == "--warmups") options.warmups = parseSize(value, true);
    else if (argument == "--samples") options.samples = parseSize(value);
    else throw std::invalid_argument("unknown option: " + argument);
  }
  if (options.implementation != "historical" && options.implementation != "production")
    throw std::invalid_argument("invalid implementation");
  if (options.fixture != "grid" && options.fixture != "dense" && options.fixture != "drag")
    throw std::invalid_argument("invalid fixture");
  if (options.measurement != "actual" && options.measurement != "estimate")
    throw std::invalid_argument("invalid measurement");
  if (options.fixture == "drag" && options.scenario != "drag-preview")
    throw std::invalid_argument("drag fixture requires drag-preview");
  if (options.fixture != "drag" && options.scenario == "drag-preview")
    throw std::invalid_argument("drag-preview requires drag fixture");
  if (options.measurement == "estimate" && options.implementation != "historical")
    throw std::invalid_argument("only historical quadratic work is estimated");
  return options;
}

void print(const Options& options, const Result& result) {
  const auto total = result.inputMeshBytes + result.derivedPeakBytes;
  const auto rssDelta = result.peakRssBytes > result.rssBeforeBytes
      ? result.peakRssBytes - result.rssBeforeBytes : 0;
  const auto coalesced = result.work.previewRequests > result.work.previewExecutions
      ? result.work.previewRequests - result.work.previewExecutions : 0;
  std::cout << "implementation,fixture,measurement,count,scenario,warmups,samples,"
               "p50_ns,p95_ns,p99_ns,setup_ns,result_hash,primitive_tests,"
               "occlusion_tests_actual,occlusion_tests_estimated,node_visits,"
               "projected_points,preview_requests,preview_executions,coalesced_requests,"
               "occt_builds,mesh_build_attempts,input_mesh_bytes,derived_peak_bytes,"
               "total_peak_bytes,rss_before_bytes,peak_rss_bytes,peak_rss_delta_bytes,"
               "max_request_ns,flush_ns,max_sync_ns\n";
  std::cout << options.implementation << ',' << options.fixture << ','
            << options.measurement << ',' << options.count << ',' << options.scenario
            << ',' << options.warmups << ',' << options.samples << ','
            << result.p50 << ',' << result.p95 << ',' << result.p99 << ','
            << result.setupNs << ",0x" << std::hex << std::setw(16)
            << std::setfill('0') << result.hash << std::dec << ','
            << result.work.primitiveTests << ',' << result.work.occlusionActual << ','
            << result.work.occlusionEstimated << ',' << result.work.nodeVisits << ','
            << result.work.projectedPoints << ',' << result.work.previewRequests << ','
            << result.work.previewExecutions << ',' << coalesced << ','
            << result.work.occtBuilds << ',' << result.work.meshBuildAttempts << ','
            << result.inputMeshBytes << ',' << result.derivedPeakBytes << ',' << total
            << ',' << result.rssBeforeBytes << ',' << result.peakRssBytes << ','
            << rssDelta << ',' << result.maxRequestNs << ',' << result.flushNs << ','
            << result.maxSyncNs << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    QCoreApplication application(argc, argv);
    validateHistoricalMarqueeFixture();
    const auto options = parseOptions(argc, argv);
    const auto result = options.fixture == "drag" ? runDrag(options)
                                                    : runPicking(options);
    print(options, result);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "stage5 benchmark failure: " << error.what() << '\n';
    usage(std::cerr);
    return EXIT_FAILURE;
  }
}

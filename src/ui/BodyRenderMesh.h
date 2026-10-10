#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <functional>
#include <iterator>
#include <memory>
#include <vector>

#include "model/GeometryOperation.h"
#include "model/SketchPlacement.h"

class TopoDS_Shape;

namespace solidar {

enum class ViewportMeshQuality { Normal, High };

// Collision-safe immutable identity shared by the CPU cache, projected
// picking scene and GPU cache. Hashes are used only for bucket selection;
// every cache compares all model/source fields before reuse.
struct BodyMeshKey {
  std::uint64_t bodyId{};
  std::uint64_t featureId{};
  std::uint64_t shapeRevision{};
  const void* shapeIdentity{};
  ViewportMeshQuality quality{ViewportMeshQuality::Normal};
  bool operator==(const BodyMeshKey&) const noexcept = default;
};

struct BodyMeshKeyHash {
  [[nodiscard]] std::size_t operator()(const BodyMeshKey& key) const noexcept;
};

struct RenderVertex {
  Point3d position;
  Vector3d normal;
  std::uint32_t faceIndex{};
};

struct RenderTriangle {
  Point3d a;
  Point3d b;
  Point3d c;
  Vector3d normal;
  Vector3d normalA;
  Vector3d normalB;
  Vector3d normalC;
  std::size_t faceIndex{};
};

struct RenderEdge {
  std::vector<Point3d> points;
  std::size_t edgeIndex{};

  // Analytic curve classification so Sketcher projection can preserve
  // circular edges as native circles/arcs instead of tessellated segments.
  enum class Kind { Tessellated, Circle, Arc };
  Kind kind{Kind::Tessellated};
  Point3d center{};    // Circle/Arc: center in world space.
  double radius{0.0};  // Circle/Arc: radius in world units.
  Point3d arcStart{};  // Arc: start point in world space.
  Point3d arcEnd{};    // Arc: end point in world space.
};

// Immutable world-space tessellation. Camera projection is deliberately kept
// out of this cache, so mouse movement never invokes OCCT meshing.
class BodyRenderMesh final {
 public:
  // Lightweight indexed view. RenderTriangle values are materialized on
  // demand from the canonical vertex/index buffers; the mesh never keeps a
  // second expanded triangle array in memory.
  class TriangleView final {
   public:
    class Iterator final {
     public:
      using iterator_category = std::random_access_iterator_tag;
      using value_type = RenderTriangle;
      using difference_type = std::ptrdiff_t;
      using reference = RenderTriangle;

      [[nodiscard]] RenderTriangle operator*() const noexcept;
      Iterator& operator++() noexcept;
      Iterator operator++(int) noexcept;
      Iterator& operator--() noexcept;
      Iterator& operator+=(difference_type offset) noexcept;
      Iterator& operator-=(difference_type offset) noexcept;
      [[nodiscard]] Iterator operator+(difference_type offset) const noexcept;
      [[nodiscard]] Iterator operator-(difference_type offset) const noexcept;
      [[nodiscard]] difference_type operator-(const Iterator& other) const noexcept;
      [[nodiscard]] bool operator==(const Iterator& other) const noexcept = default;
      [[nodiscard]] auto operator<=>(const Iterator& other) const noexcept = default;

     private:
      friend class TriangleView;
      Iterator(const BodyRenderMesh* mesh, std::size_t index) noexcept
          : mesh_(mesh), index_(index) {}
      const BodyRenderMesh* mesh_{};
      std::size_t index_{};
    };

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] RenderTriangle operator[](std::size_t index) const noexcept;
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;

   private:
    friend class BodyRenderMesh;
    explicit TriangleView(const BodyRenderMesh* mesh) noexcept : mesh_(mesh) {}
    const BodyRenderMesh* mesh_{};
  };

  // Preserve the original ABI for incrementally linked UI targets while the
  // explicit overload carries the selectable viewport quality.
  void rebuild(const TopoDS_Shape& shape) noexcept;
  void rebuild(const TopoDS_Shape& shape,
               ViewportMeshQuality quality) noexcept;
  [[nodiscard]] bool tryRebuild(
      const TopoDS_Shape& shape, GeometryFailure* failure = nullptr) noexcept;
  [[nodiscard]] bool tryRebuild(
      const TopoDS_Shape& shape, ViewportMeshQuality quality,
      GeometryFailure* failure = nullptr) noexcept;
  void clear() noexcept;

  [[nodiscard]] TriangleView triangles() const noexcept;
  [[nodiscard]] std::size_t triangleCount() const noexcept;
  [[nodiscard]] RenderTriangle triangle(std::size_t index) const noexcept;
  [[nodiscard]] const std::vector<RenderVertex>& vertices() const noexcept;
  [[nodiscard]] const std::vector<std::uint32_t>& triangleIndices() const noexcept;
  [[nodiscard]] const std::vector<RenderEdge>& edges() const noexcept;
  [[nodiscard]] Point3d center() const noexcept;
  [[nodiscard]] double diagonal() const noexcept;
  [[nodiscard]] std::size_t faceCount() const noexcept;
  [[nodiscard]] std::size_t edgeSampleCount() const noexcept;
  [[nodiscard]] std::uint64_t revision() const noexcept;
  [[nodiscard]] double rebuildMilliseconds() const noexcept;
  [[nodiscard]] ViewportMeshQuality quality() const noexcept;
  [[nodiscard]] double linearDeflection() const noexcept;
  [[nodiscard]] double angularDeflection() const noexcept;
  [[nodiscard]] std::uint64_t buildAttemptCount() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

 private:
  // The scalability benchmark needs a large deterministic indexed fixture
  // without spending the measurement in OCCT shape construction/meshing.
  // Keeping the adapter as a friend avoids a production mutator on this
  // otherwise immutable-after-build value type.
  friend class BodyRenderMeshBenchmarkAdapter;
  std::vector<RenderVertex> vertices_;
  std::vector<std::uint32_t> triangleIndices_;
  std::vector<RenderEdge> edges_;
  Point3d center_{};
  double diagonal_{};
  std::size_t faceCount_{};
  std::size_t edgeSampleCount_{};
  std::uint64_t revision_{};
  double rebuildMilliseconds_{};
  ViewportMeshQuality quality_{ViewportMeshQuality::Normal};
  double linearDeflection_{};
  double angularDeflection_{};
  std::uint64_t buildAttemptCount_{};
};

struct BodyMeshCacheStats {
  std::uint64_t hits{};
  std::uint64_t misses{};
  std::uint64_t buildAttempts{};
};

// Small immutable mesh cache keyed by the model-owned shape identity and its
// explicit revision. Failed builds are never inserted, so callers can keep
// publishing their previous last-valid display transactionally.
class BodyRenderMeshCache final {
 public:
  [[nodiscard]] std::shared_ptr<const BodyRenderMesh> resolve(
      const BodyMeshKey& key, const TopoDS_Shape& shape,
      GeometryFailure* failure = nullptr);
  // Called only after a complete viewport transaction succeeds. All meshes
  // in the published scene remain resident, regardless of body count; only a
  // small bounded tail of obsolete revisions is retained for undo/redo.
  void retainActive(const std::vector<BodyMeshKey>& activeKeys,
                    std::size_t maximumObsoleteEntries = 8);
  void clear() noexcept;
  [[nodiscard]] const BodyMeshCacheStats& stats() const noexcept;
  [[nodiscard]] std::size_t entryCount() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

 private:
  struct Entry {
    BodyMeshKey key;
    std::shared_ptr<const BodyRenderMesh> mesh;
    std::uint64_t lastUse{};
  };
  std::vector<Entry> entries_;
  BodyMeshCacheStats stats_;
  std::uint64_t useClock_{};
};

}  // namespace solidar

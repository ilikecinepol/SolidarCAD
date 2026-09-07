#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "model/SketchPlacement.h"

class TopoDS_Shape;

namespace solidar {

enum class ViewportMeshQuality { Normal, High };

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
};

// Immutable world-space tessellation. Camera projection is deliberately kept
// out of this cache, so mouse movement never invokes OCCT meshing.
class BodyRenderMesh final {
 public:
  // Preserve the original ABI for incrementally linked UI targets while the
  // explicit overload carries the selectable viewport quality.
  void rebuild(const TopoDS_Shape& shape);
  void rebuild(const TopoDS_Shape& shape, ViewportMeshQuality quality);
  void clear() noexcept;

  [[nodiscard]] const std::vector<RenderTriangle>& triangles() const noexcept;
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

 private:
  std::vector<RenderTriangle> triangles_;
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
};

}  // namespace solidar

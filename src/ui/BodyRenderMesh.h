#pragma once

#include <cstddef>
#include <vector>

#include "model/SketchPlacement.h"

class TopoDS_Shape;

namespace solidar {

struct RenderTriangle {
  Point3d a;
  Point3d b;
  Point3d c;
  Vector3d normal;
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
  void rebuild(const TopoDS_Shape& shape);
  void clear() noexcept;

  [[nodiscard]] const std::vector<RenderTriangle>& triangles() const noexcept;
  [[nodiscard]] const std::vector<RenderEdge>& edges() const noexcept;
  [[nodiscard]] Point3d center() const noexcept;
  [[nodiscard]] double diagonal() const noexcept;
  [[nodiscard]] std::size_t faceCount() const noexcept;

 private:
  std::vector<RenderTriangle> triangles_;
  std::vector<RenderEdge> edges_;
  Point3d center_{};
  double diagonal_{};
  std::size_t faceCount_{};
};

}  // namespace solidar

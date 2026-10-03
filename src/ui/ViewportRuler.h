#pragma once

#include <QPointF>

#include <optional>
#include <vector>

#include "model/SketchPlacement.h"
#include "ui/BodyRenderMesh.h"
#include "ui/ViewportCamera.h"

class QPainter;

namespace solidar {

struct ThemeColors;

enum class RulerSnapKind { Surface, Edge, Vertex };

struct RulerHit {
  Point3d world;
  QPointF screen;
  double depth{};
  RulerSnapKind snap{RulerSnapKind::Surface};
};

enum class RulerClickResult { Ignored, FirstPoint, Completed, Restarted };

// Transient, document-independent measurement state for the 3D viewport.
// It owns no model data: all points are world-space snapshots and disappear
// when the viewport interaction state is reset.
class ViewportRuler final {
 public:
  void begin() noexcept;
  void cancel() noexcept;
  [[nodiscard]] bool active() const noexcept;

  [[nodiscard]] bool updateHover(const BodyRenderMesh& mesh,
                                 const ViewportCameraState& camera,
                                 QPointF cursor);
  void clearHover() noexcept;
  [[nodiscard]] RulerClickResult commitHoveredPoint() noexcept;
  [[nodiscard]] RulerClickResult commitPoint(const RulerHit& hit) noexcept;

  [[nodiscard]] const std::optional<RulerHit>& firstPoint() const noexcept;
  [[nodiscard]] const std::optional<RulerHit>& secondPoint() const noexcept;
  [[nodiscard]] const std::optional<RulerHit>& hoverPoint() const noexcept;
  [[nodiscard]] std::optional<double> measuredDistanceMm() const noexcept;
  [[nodiscard]] std::optional<double> previewDistanceMm() const noexcept;

  void paint(QPainter& painter, const ViewportCameraState& camera,
             const ThemeColors& theme) const;

  // Public pure picker for deterministic regression tests. Vertex and edge
  // snaps take precedence over the frontmost surface point.
  [[nodiscard]] static std::optional<RulerHit> pickPoint(
      const std::vector<RenderTriangle>& triangles,
      const std::vector<RenderEdge>& edges,
      const ViewportCameraState& camera, QPointF cursor);

 private:
  bool active_{false};
  std::optional<RulerHit> first_;
  std::optional<RulerHit> second_;
  std::optional<RulerHit> hover_;
};

}  // namespace solidar

#include "ui/AssemblyPlacementPreview.h"

#include <QPainter>
#include <QPolygonF>

#include <TopoDS_Shape.hxx>

#include <limits>
#include <utility>

#include "ui/RigidTransform3dOps.h"

namespace solidar {

bool AssemblyPlacementPreview::prepare(const Document& source,
                                       ViewportMeshQuality quality) noexcept {
  try {
    std::vector<BodyRenderMesh> candidate;
    candidate.reserve(source.bodies().size());
    for (const Body& body : source.bodies()) {
      if (!body.visible()) continue;
      const auto shape = body.lastValidResultShape();
      if (!shape || shape->IsNull()) continue;
      BodyRenderMesh mesh;
      GeometryFailure failure;
      if (!mesh.tryRebuild(*shape, quality, &failure)) return false;
      candidate.push_back(std::move(mesh));
    }
    if (candidate.empty()) return false;
    meshes_ = std::move(candidate);
    placement_ = RigidTransform3d::identity();
    if (tokenClock_ != std::numeric_limits<std::uint64_t>::max())
      ++tokenClock_;
    activeToken_ = tokenClock_;
    ++meshBuildCount_;
    return true;
  } catch (...) {
    return false;
  }
}

bool AssemblyPlacementPreview::setPlacement(
    RigidTransform3d placement) noexcept {
  if (!active()) return false;
  placement_ = std::move(placement);
  ++placementUpdateCount_;
  return true;
}

void AssemblyPlacementPreview::clear() noexcept {
  meshes_.clear();
  placement_ = RigidTransform3d::identity();
  activeToken_ = 0;
}

void AssemblyPlacementPreview::paint(QPainter& painter,
                                     const ViewportCameraState& camera,
                                     QColor surface, QColor edge) const {
  if (!active()) return;
  surface.setAlpha(92);
  edge.setAlpha(210);
  painter.save();
  painter.setBrush(surface);
  painter.setPen(QPen(edge, 1.35));
  for (const BodyRenderMesh& mesh : meshes_) {
    for (const RenderTriangle triangle : mesh.triangles()) {
      const auto a = transformPoint(placement_, triangle.a);
      const auto b = transformPoint(placement_, triangle.b);
      const auto c = transformPoint(placement_, triangle.c);
      if (!a || !b || !c) continue;
      QPolygonF polygon;
      polygon << camera.worldToScreen(*a) << camera.worldToScreen(*b)
              << camera.worldToScreen(*c);
      painter.drawPolygon(polygon);
    }
  }
  painter.restore();
}

}  // namespace solidar

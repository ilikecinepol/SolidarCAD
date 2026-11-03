#pragma once

#include <QColor>

#include <cstdint>
#include <vector>

#include "model/Document.h"
#include "model/RigidTransform3d.h"
#include "ui/BodyRenderMesh.h"
#include "ui/ViewportCamera.h"

class QPainter;

namespace solidar {

// Separate preview-only CPU scene. It has no occurrence identity and is never
// published to committed picking/tree consumers. Meshes are built once when a
// source is accepted; placement changes update only the rigid instance value.
class AssemblyPlacementPreview final {
 public:
  [[nodiscard]] bool prepare(const Document& source,
                             ViewportMeshQuality quality) noexcept;
  [[nodiscard]] bool setPlacement(RigidTransform3d placement) noexcept;
  void clear() noexcept;
  void paint(QPainter& painter, const ViewportCameraState& camera,
             QColor surface, QColor edge) const;

  [[nodiscard]] bool active() const noexcept { return !meshes_.empty(); }
  [[nodiscard]] std::uint64_t token() const noexcept { return activeToken_; }
  [[nodiscard]] std::uint64_t meshBuildCount() const noexcept {
    return meshBuildCount_;
  }
  [[nodiscard]] std::uint64_t placementUpdateCount() const noexcept {
    return placementUpdateCount_;
  }

 private:
  std::vector<BodyRenderMesh> meshes_;
  RigidTransform3d placement_{RigidTransform3d::identity()};
  std::uint64_t tokenClock_{};
  std::uint64_t activeToken_{};
  std::uint64_t meshBuildCount_{};
  std::uint64_t placementUpdateCount_{};
};

}  // namespace solidar

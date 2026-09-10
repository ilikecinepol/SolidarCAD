#pragma once

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSize>

#include <cstdint>
#include <memory>
#include <vector>

#include "ui/BodyRenderMesh.h"

namespace solidar {

enum class ViewportDisplayMode { Shaded, ShadedWithEdges, Wireframe };

struct ViewportSurfacePassPolicy {
  bool ordinarySurfaces{};
  bool highlightOnlySourceFaces{};

  bool operator==(const ViewportSurfacePassPolicy&) const = default;
};

[[nodiscard]] constexpr ViewportSurfacePassPolicy viewportSurfacePassPolicy(
    ViewportDisplayMode mode, bool hasPreview, bool hasFaceHighlights) noexcept {
  return {mode != ViewportDisplayMode::Wireframe,
          hasFaceHighlights &&
              (hasPreview || mode == ViewportDisplayMode::Wireframe)};
}

class ViewportRenderer final {
 public:
  ViewportRenderer();
  ~ViewportRenderer();

  bool initialize();
  void release();
  void render(const BodyRenderMesh& source, const BodyRenderMesh* preview,
              const QSize& logicalSize, float devicePixelRatio, float yawDeg,
              float pitchDeg, float zoom, QPointF pan,
              ViewportDisplayMode mode,
              const std::vector<std::size_t>& selectedFaces,
              std::size_t hoveredFace, const std::vector<std::size_t>& selectedEdges,
              std::size_t hoveredEdge);

  [[nodiscard]] QString error() const { return error_; }
  [[nodiscard]] double lastUploadMilliseconds() const noexcept {
    return lastUploadMilliseconds_;
  }

  static QMatrix4x4 projectionMatrix(const QSize& logicalSize, float yawDeg,
                                     float pitchDeg, float zoom, QPointF pan,
                                     double depthExtent, Point3d center = {});

 private:
  struct GpuMesh;
  bool upload(GpuMesh& gpu, const BodyRenderMesh& mesh);
  void drawSurfaces(GpuMesh& gpu, const QMatrix4x4& matrix,
                    float yawDeg, float pitchDeg,
                    const std::vector<std::size_t>& selectedFaces,
                    std::size_t hoveredFace, bool preview,
                    bool highlightOnly);
  void drawEdges(GpuMesh& gpu, const QMatrix4x4& matrix,
                 const std::vector<std::size_t>& selectedEdges,
                 std::size_t hoveredEdge, bool ordinaryEdges,
                 float devicePixelRatio);

  std::unique_ptr<GpuMesh> source_;
  std::unique_ptr<GpuMesh> preview_;
  QOpenGLShaderProgram surfaceProgram_;
  QOpenGLShaderProgram edgeProgram_;
  QString error_;
  double lastUploadMilliseconds_{};
  bool initialized_{};
};

}  // namespace solidar

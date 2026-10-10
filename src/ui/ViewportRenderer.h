#pragma once

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPointer>
#include <QSize>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ui/BodyRenderMesh.h"
#include "ui/ThemeColors.h"

namespace solidar {

enum class ViewportDisplayMode { Shaded, ShadedWithEdges, Wireframe };

struct RenderMeshInstance {
  const BodyRenderMesh* mesh{};
  BodyMeshKey identity;
  std::size_t faceOffset{};
  std::size_t edgeOffset{};
  // ReplaceSource keeps the canonical source resident for topology-based
  // hover/selection overlays while suppressing its ordinary surface/edge
  // passes. Unrelated bodies remain ordinary-visible.
  bool drawOrdinary{true};
};

struct RendererHighlightRepresentation {
  std::vector<std::size_t> selectedFaces;
  std::vector<std::size_t> hoveredFaces;
  std::vector<std::size_t> selectedEdges;
  std::optional<std::size_t> hoveredEdge;
};

struct RendererResourceDiagnostics {
  bool initialized{};
  std::size_t sourceMeshes{};
  bool previewResident{};
  bool cutPreviewResident{};
  std::size_t selectedFaces{};
  std::size_t hoveredFaces{};
  std::size_t selectedEdges{};
  std::size_t hoveredEdges{};
  // Monotonic upload identities and deterministic content fingerprints make
  // preview publication observable without exposing OpenGL object names.
  std::uint64_t previewUploadGeneration{};
  std::uint64_t cutPreviewUploadGeneration{};
  std::uint64_t previewContentHash{};
  std::uint64_t cutPreviewContentHash{};
  std::size_t previewIndexCount{};
  std::size_t cutPreviewIndexCount{};
  // Work performed while gathering changed highlight overlays. These counts
  // are primitive-output proportional; an unchanged overlay reports zero.
  std::size_t highlightFaceIndexVisits{};
  std::size_t highlightEdgeVertexVisits{};
  std::size_t highlightLookupBytes{};
  std::size_t ordinarySourceMeshes{};
  std::size_t highlightOnlySourceMeshes{};
  std::size_t highlightOverlayMeshes{};
};

class ViewportRenderer final {
 public:
  ViewportRenderer();
  ~ViewportRenderer();

  bool initialize();
  void release();
  // Prunes resources while the owning/share-group context is current. This is
  // also called by Viewport when paintGL is intentionally skipped for an empty
  // scene, so nonempty -> empty transitions cannot retain GPU buffers.
  void synchronizeResources(const std::vector<BodyMeshKey>& activeSources,
                            bool keepPreview, bool keepCutPreview);
  [[nodiscard]] RendererResourceDiagnostics resourceDiagnostics() const noexcept;
  void render(const BodyRenderMesh& source, const BodyRenderMesh* preview,
              const QSize& logicalSize, float devicePixelRatio, float yawDeg,
              float pitchDeg, float zoom, QPointF pan,
              ViewportDisplayMode mode,
              const std::vector<std::size_t>& selectedFaces,
              const std::vector<std::size_t>& hoveredFaces,
              const std::vector<std::size_t>& selectedEdges,
              std::size_t hoveredEdge,
              const BodyRenderMesh* cutPreview = nullptr,
              std::uint64_t previewPresentationRevision = 0,
              std::uint64_t cutPreviewPresentationRevision = 0,
              const ThemeColors& theme = lightThemeColors());
  void render(const std::vector<RenderMeshInstance>& sources,
              const BodyRenderMesh* preview, const QSize& logicalSize,
              float devicePixelRatio, float yawDeg, float pitchDeg,
              float zoom, QPointF pan, ViewportDisplayMode mode,
              const std::vector<std::size_t>& selectedFaces,
              const std::vector<std::size_t>& hoveredFaces,
              const std::vector<std::size_t>& selectedEdges,
              std::size_t hoveredEdge,
              const BodyRenderMesh* cutPreview = nullptr,
              std::uint64_t previewPresentationRevision = 0,
              std::uint64_t cutPreviewPresentationRevision = 0,
              const ThemeColors& theme = lightThemeColors());

  [[nodiscard]] static RendererHighlightRepresentation buildHighlightRepresentation(
      std::size_t faceOffset, std::size_t faceCount,
      std::size_t edgeOffset, std::size_t edgeCount,
      const std::vector<std::size_t>& selectedFaces,
      const std::vector<std::size_t>& hoveredFaces,
      const std::vector<std::size_t>& selectedEdges,
      std::size_t hoveredEdge);

  [[nodiscard]] QString error() const { return error_; }
  [[nodiscard]] double lastUploadMilliseconds() const noexcept {
    return lastUploadMilliseconds_;
  }

  static QMatrix4x4 projectionMatrix(const QSize& logicalSize, float yawDeg,
                                     float pitchDeg, float zoom, QPointF pan,
                                     double depthExtent, Point3d center = {});

 private:
  friend class ViewportRendererTestAdapter;
  enum class FailureInjection {
    None,
    SourceCacheAllocation,
  };
  struct GpuMesh;
  [[nodiscard]] bool currentContextIsCompatible() const noexcept;
  [[nodiscard]] bool bindVertexArrayChecked(
      QOpenGLVertexArrayObject& vao) noexcept;
  void destroyGpuMesh(GpuMesh& gpu, bool destroyGlObjects) noexcept;
  bool upload(GpuMesh& gpu, const BodyRenderMesh& mesh,
              const BodyMeshKey& identity);
  void prepareHighlights(GpuMesh& gpu, const BodyRenderMesh& mesh,
                         std::size_t faceOffset, std::size_t edgeOffset,
                         const std::vector<std::size_t>& selectedFaces,
                         const std::vector<std::size_t>& hoveredFaces,
                         const std::vector<std::size_t>& selectedEdges,
                         std::size_t hoveredEdge);
  void renderImpl(const std::vector<RenderMeshInstance>& sources,
                  const BodyRenderMesh* preview, const QSize& logicalSize,
                  float devicePixelRatio, float yawDeg, float pitchDeg,
                  float zoom, QPointF pan, ViewportDisplayMode mode,
                  const std::vector<std::size_t>& selectedFaces,
                  const std::vector<std::size_t>& hoveredFaces,
                  const std::vector<std::size_t>& selectedEdges,
                  std::size_t hoveredEdge, const BodyRenderMesh* cutPreview,
                  std::uint64_t previewPresentationRevision,
                  std::uint64_t cutPreviewPresentationRevision,
                  const ThemeColors& theme);
  void drawSurfaces(GpuMesh& gpu, const QMatrix4x4& matrix,
                    float yawDeg, float pitchDeg,
                    const ThemeColors& theme, bool preview,
                    bool cutPreview = false,
                    int highlightMode = 0);
  void drawEdges(GpuMesh& gpu, const QMatrix4x4& matrix,
                 const ThemeColors& theme, bool ordinaryEdges,
                 float devicePixelRatio,
                 int highlightMode = 0);

  std::unordered_map<BodyMeshKey, std::unique_ptr<GpuMesh>, BodyMeshKeyHash>
      sources_;
  std::unique_ptr<GpuMesh> preview_;
  std::unique_ptr<GpuMesh> cutPreview_;
  QOpenGLShaderProgram surfaceProgram_;
  QOpenGLShaderProgram edgeProgram_;
  QString error_;
  double lastUploadMilliseconds_{};
  bool initialized_{};
  QPointer<QOpenGLContext> context_;
  std::uint64_t uploadGenerationClock_{};
  std::size_t lastOrdinarySourceMeshes_{};
  std::size_t lastHighlightOnlySourceMeshes_{};
  std::size_t lastHighlightOverlayMeshes_{};
  FailureInjection failureInjection_{FailureInjection::None};
  std::size_t uploadFailureCountdown_{};
  std::size_t vaoBindFailureCountdown_{};
};

}  // namespace solidar

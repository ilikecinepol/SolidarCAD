#pragma once

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QMatrix3x3>
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
#include "ui/AssemblyViewportPicking.h"
#include "ui/AssemblyViewportScene.h"
#include "ui/ComponentViewportPicking.h"
#include "ui/ComponentPlacementDrag.h"
#include "ui/ComponentViewportScene.h"
#include "ui/ThemeColors.h"

namespace solidar {

enum class ViewportDisplayMode { Shaded, ShadedWithEdges, Wireframe };
enum class RenderInstanceVisualState : std::uint8_t {
  None,
  Hovered,
  Selected,
  Ghosted,
};

enum class RendererInstancedSceneKind : std::uint8_t {
  None,
  LegacyAssembly,
  Component,
};

struct RenderMeshInstance {
  const BodyRenderMesh* mesh{};
  BodyMeshKey identity;
  std::size_t faceOffset{};
  std::size_t edgeOffset{};
  // ReplaceSource keeps the canonical source resident for topology-based
  // hover/selection overlays while suppressing its ordinary surface/edge
  // passes. Unrelated bodies remain ordinary-visible.
  bool drawOrdinary{true};
  // Definition-local CPU/GPU data is shared. Only this per-instance matrix
  // places it into product world space; occurrence placement is never baked
  // into B-Rep or shared buffers.
  QMatrix4x4 localToWorld;
  std::optional<AssemblyOccurrenceIdentity> occurrenceIdentity;
  RenderInstanceVisualState visualState{RenderInstanceVisualState::None};
};

struct OrdinaryRenderComposition final {
  std::optional<Point3d> cameraCenter;
  std::optional<double> cameraDepthExtent;
  bool preserveDepthBuffer{};
};

struct RendererSourceDiagnostics final {
  BodyMeshKey key;
  std::uint64_t uploadGeneration{};
  std::uint64_t contentFingerprint{};
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
  Point3d ordinaryCameraCenter{};
  double ordinaryCameraDepthExtent{1.0};
  bool ordinaryDepthBufferPreserved{};
  std::uint64_t sourceUploadAttempts{};
  std::uint64_t sourceUploadSuccesses{};
  std::uint64_t sourceUploadFailures{};
  std::uint64_t sourceDestroyCount{};
  std::uint64_t sourcePruneCount{};
  // Staged GL wrappers rolled back while the owner context is unavailable are
  // retained here until a later exact-owner-context entry can destroy them.
  std::size_t deferredSourceDestructions{};
  std::uint64_t deferredSourceCleanupCount{};
  std::uint64_t staleAssemblyRejections{};
  std::size_t activeAssemblyUsers{};
  std::uint64_t activePublicationGeneration{};
  std::size_t submittedAssemblyInstances{};
  std::size_t drawnAssemblyInstances{};
  std::size_t highlightedAssemblyInstances{};
  std::size_t drawnAssemblySurfacePasses{};
  std::size_t drawnAssemblyEdgePasses{};
  bool assemblyWorldBoundsValid{};
  Point3d assemblyCameraCenter{};
  double assemblyCameraDepthExtent{1.0};
  RendererInstancedSceneKind activeInstancedSceneKind{
      RendererInstancedSceneKind::None};
  std::uint64_t staleComponentRejections{};
  std::size_t activeComponentUsers{};
  std::uint64_t activeComponentPublicationGeneration{};
  std::size_t submittedComponentInstances{};
  std::size_t drawnComponentInstances{};
  std::size_t highlightedComponentInstances{};
  std::size_t drawnComponentSurfacePasses{};
  std::size_t drawnComponentEdgePasses{};
  bool componentWorldBoundsValid{};
  Point3d componentCameraCenter{};
  double componentCameraDepthExtent{1.0};
  std::vector<RendererSourceDiagnostics> sources;
};

class ViewportRenderer final {
 public:
  ViewportRenderer();
  ~ViewportRenderer();

  bool initialize();
  void release();
  // Prunes resources while the exact owning context is current. This is
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
              const ThemeColors& theme = lightThemeColors(),
              OrdinaryRenderComposition composition = {});

  // Transactional assembly path. Missing GPU sources are uploaded privately,
  // staged in preallocated cache slots under a rollback guard, and committed
  // with the draw list only after the full generation draws successfully.
  [[nodiscard]] bool renderAssembly(
      const std::shared_ptr<const AssemblyViewportScene>& scene,
      const QSize& logicalSize, float devicePixelRatio, float yawDeg,
      float pitchDeg, float zoom, QPointF pan, ViewportDisplayMode mode,
      const std::optional<AssemblyOccurrenceHighlightTarget>& selectedOccurrence =
          std::nullopt,
      const std::optional<AssemblyOccurrenceHighlightTarget>& hoveredOccurrence =
          std::nullopt,
      const ThemeColors& theme = lightThemeColors());

  [[nodiscard]] bool renderComponent(
      const std::shared_ptr<const ComponentViewportScene>& scene,
      const QSize& logicalSize, float devicePixelRatio, float yawDeg,
      float pitchDeg, float zoom, QPointF pan, ViewportDisplayMode mode,
      const std::optional<ComponentInstanceHighlightTarget>& selectedComponent =
          std::nullopt,
      const std::optional<ComponentInstanceHighlightTarget>& hoveredComponent =
          std::nullopt,
      const ThemeColors& theme = lightThemeColors(),
      const ComponentPlacementPreview* placementPreview = nullptr,
      std::uint64_t componentOwnershipRevision = 0);

  // In-place component authoring keeps the product visible as non-interactive
  // context.  The active occurrence itself is omitted: its current detached
  // Document is rendered by the ordinary Part path. Child authoring ghosts
  // the surrounding product; root authoring keeps the complete product opaque.
  [[nodiscard]] bool renderComponentEditContext(
      const std::shared_ptr<const ComponentViewportScene>& scene,
      const ComponentInstanceAddress& activeComponent,
      const QSize& logicalSize, float devicePixelRatio, float yawDeg,
      float pitchDeg, float zoom, QPointF pan, ViewportDisplayMode mode,
      Point3d activeDocumentCenter, double activeDocumentDepthExtent,
      bool ghostInactiveComponents,
      const ThemeColors& theme = lightThemeColors(),
      const std::optional<ComponentInstanceHighlightTarget>& selectedComponent =
          std::nullopt,
      const std::optional<ComponentInstanceHighlightTarget>& hoveredComponent =
          std::nullopt,
      const ComponentPlacementPreview* placementPreview = nullptr,
      std::uint64_t componentOwnershipRevision = 0);
  void clearComponentEditContextResources() noexcept;

  // Logical detach is safe without a current context. It drops only retained
  // publication/draw state; source wrappers remain owned until the next exact
  // owner-context synchronize/release operation can prune them safely.
  void detachAssemblyScene() noexcept;
  void detachComponentScene() noexcept;
  // Definition editing temporarily replaces the assembly presentation with a
  // Part presentation.  Preserve only the already-resident assembly source
  // keys across that suspension; instances/publication remain detached and
  // therefore cannot be drawn or picked.  The retention is committed away by
  // the first successful replacement assembly frame, or cancelled by an
  // ordinary detach/project teardown.
  [[nodiscard]] bool suspendAssemblySceneResources() noexcept;

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
  [[nodiscard]] static std::optional<QMatrix4x4> modelMatrix(
      const RigidTransform3d& transform) noexcept;

 private:
  friend class ViewportRendererTestAdapter;
  enum class FailureInjection {
    None,
    SourceCacheAllocation,
  };
  enum class ContextMismatchCheckpoint {
    None,
    AfterAssemblyPreflight,
    AfterAssemblyDraw,
  };
  struct GpuMesh;
  struct InstancedFrame;
  struct InstancedDrawRecord final {
    std::shared_ptr<const BodyRenderMesh> mesh;
    BodyMeshKey key;
    QMatrix4x4 localToWorld;
    RenderInstanceVisualState visualState{RenderInstanceVisualState::None};
  };
  [[nodiscard]] bool renderInstancedFrame(
      const std::shared_ptr<const InstancedFrame>& scene,
      const QSize& logicalSize, float devicePixelRatio, float yawDeg,
      float pitchDeg, float zoom, QPointF pan, ViewportDisplayMode mode,
      const ThemeColors& theme);
  [[nodiscard]] bool currentContextIsCompatible() const noexcept;
  void drainDeferredDestruction() noexcept;
  [[nodiscard]] bool bindVertexArrayChecked(
      QOpenGLVertexArrayObject& vao,
      bool consumeInjectedFailure = true) noexcept;
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
                  const ThemeColors& theme,
                  OrdinaryRenderComposition composition);
  void drawSurfaces(GpuMesh& gpu, const QMatrix4x4& matrix,
                    float yawDeg, float pitchDeg,
                    const ThemeColors& theme, bool preview,
                    bool cutPreview = false,
                    int highlightMode = 0,
                    const QMatrix3x3* normalMatrixOverride = nullptr,
                    int instanceVisualState = 0);
  void drawEdges(GpuMesh& gpu, const QMatrix4x4& matrix,
                 const ThemeColors& theme, bool ordinaryEdges,
                 float devicePixelRatio,
                 int highlightMode = 0,
                 int instanceVisualState = 0);

  std::unordered_map<BodyMeshKey, std::unique_ptr<GpuMesh>, BodyMeshKeyHash>
      sources_;
  std::vector<std::unique_ptr<GpuMesh>> deferredDestruction_;
  std::unique_ptr<GpuMesh> preview_;
  std::unique_ptr<GpuMesh> cutPreview_;
  std::unique_ptr<QOpenGLShaderProgram> surfaceProgram_;
  std::unique_ptr<QOpenGLShaderProgram> edgeProgram_;
  QString error_;
  double lastUploadMilliseconds_{};
  bool initialized_{};
  bool released_{true};
  QPointer<QOpenGLContext> context_;
  std::uint64_t uploadGenerationClock_{};
  std::vector<InstancedDrawRecord> activeInstancedDraws_;
  std::shared_ptr<const InstancedFrame> activeInstancedFrame_;
  RendererInstancedSceneKind activeInstancedSceneKind_{
      RendererInstancedSceneKind::None};
  std::uint64_t activeInstancedGeneration_{};
  std::vector<BodyMeshKey> suspendedAssemblySourceKeys_;
  bool assemblyResourcesSuspended_{};
  std::vector<BodyMeshKey> componentEditContextSourceKeys_;
  std::vector<BodyMeshKey> activeOrdinarySourceKeys_;
  bool componentEditContextResourcesActive_{};
  std::uint64_t sourceUploadAttempts_{};
  std::uint64_t sourceUploadSuccesses_{};
  std::uint64_t sourceUploadFailures_{};
  std::uint64_t sourceDestroyCount_{};
  std::uint64_t sourcePruneCount_{};
  std::uint64_t deferredSourceCleanupCount_{};
  std::uint64_t staleAssemblyRejections_{};
  std::uint64_t staleComponentRejections_{};
  std::size_t submittedInstancedInstances_{};
  std::size_t drawnInstancedInstances_{};
  std::size_t highlightedInstancedInstances_{};
  std::size_t drawnInstancedSurfacePasses_{};
  std::size_t drawnInstancedEdgePasses_{};
  bool instancedWorldBoundsValid_{};
  Point3d instancedCameraCenter_{};
  double instancedCameraDepthExtent_{1.0};
  std::size_t lastOrdinarySourceMeshes_{};
  std::size_t lastHighlightOnlySourceMeshes_{};
  std::size_t lastHighlightOverlayMeshes_{};
  Point3d ordinaryCameraCenter_{};
  double ordinaryCameraDepthExtent_{1.0};
  bool ordinaryDepthBufferPreserved_{};
  FailureInjection failureInjection_{FailureInjection::None};
  std::size_t uploadFailureCountdown_{};
  std::size_t vaoBindFailureCountdown_{};
  bool injectPreDrawGlError_{};
  ContextMismatchCheckpoint contextMismatchCheckpoint_{
      ContextMismatchCheckpoint::None};
  bool injectedContextMismatchActive_{};
};

}  // namespace solidar

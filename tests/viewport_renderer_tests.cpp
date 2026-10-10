#include "TestAssertions.h"

#include "ui/ViewportRenderer.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QVector4D>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <vector>

namespace solidar {

class BodyRenderMeshBenchmarkAdapter final {
 public:
  static BodyRenderMesh onePrimitivePerTopology(std::size_t count) {
    BodyRenderMesh mesh;
    mesh.vertices_.reserve(count * 3);
    mesh.triangleIndices_.reserve(count * 3);
    mesh.edges_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const double x = static_cast<double>(index % 1000) * 0.01;
      const double y = static_cast<double>(index / 1000) * 0.01;
      const auto first = static_cast<std::uint32_t>(mesh.vertices_.size());
      const auto face = static_cast<std::uint32_t>(index);
      mesh.vertices_.push_back({{x, y, 0.0}, {0.0, 0.0, 1.0}, face});
      mesh.vertices_.push_back({{x + 0.004, y, 0.0}, {0.0, 0.0, 1.0}, face});
      mesh.vertices_.push_back(
          {{x, y + 0.004, 0.0}, {0.0, 0.0, 1.0}, face});
      mesh.triangleIndices_.insert(mesh.triangleIndices_.end(),
                                   {first, first + 1, first + 2});
      mesh.edges_.push_back(
          {{{x, y, 0.0}, {x + 0.004, y, 0.0}}, index});
    }
    mesh.center_ = {5.0, 0.5, 0.0};
    mesh.diagonal_ = 11.0;
    mesh.faceCount_ = count;
    mesh.edgeSampleCount_ = count * 2;
    mesh.revision_ = 1;
    return mesh;
  }
};

class ViewportRendererTestAdapter final {
 public:
  static void failNextSourceCacheAllocation(ViewportRenderer& renderer) {
    renderer.failureInjection_ =
        ViewportRenderer::FailureInjection::SourceCacheAllocation;
  }
  static void failNthUploadAfterVertexBuffer(ViewportRenderer& renderer,
                                             std::size_t ordinal) {
    renderer.uploadFailureCountdown_ = ordinal;
  }
  static void failNthVaoBind(ViewportRenderer& renderer,
                             std::size_t ordinal) {
    renderer.vaoBindFailureCountdown_ = ordinal;
  }
};

}  // namespace solidar

namespace {

bool imageContainsColorNear(const QImage& image, const QColor& expected,
                            int tolerance = 12) {
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor actual = image.pixelColor(x, y);
      if (std::abs(actual.red() - expected.red()) <= tolerance &&
          std::abs(actual.green() - expected.green()) <= tolerance &&
          std::abs(actual.blue() - expected.blue()) <= tolerance)
        return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
  QGuiApplication application(argc, argv);
  const QSize size(1000, 800);
  const QPointF pan(17.0, -11.0);
  const auto matrix = solidar::ViewportRenderer::projectionMatrix(
      size, -35.0F, 25.0F, 1.4F, pan, 500.0);
  const QVector4D world(13.0F, -7.0F, 22.0F, 1.0F);
  const QVector4D clip = matrix * world;

  const double yaw = -35.0 * std::numbers::pi / 180.0;
  const double pitch = 25.0 * std::numbers::pi / 180.0;
  const double x1 = world.x() * std::cos(yaw) - world.y() * std::sin(yaw);
  const double y1 = world.x() * std::sin(yaw) + world.y() * std::cos(yaw);
  const double y2 = y1 * std::cos(pitch) - world.z() * std::sin(pitch);
  const double scale = 800.0 * 0.008 * 1.4;
  const double expectedX = 500.0 + x1 * scale + pan.x();
  const double expectedY = 800.0 * 0.52 + y2 * scale + pan.y();
  const double actualX = (clip.x() + 1.0) * 500.0;
  const double actualY = (1.0 - clip.y()) * 400.0;
  CHECK(std::abs(actualX - expectedX) < 1e-3);
  CHECK(std::abs(actualY - expectedY) < 1e-3);

  // The renderer-owned overlay buffers are built from the complete selection,
  // not the historical first-32 uniform array. Exercise counts on both sides
  // of that old boundary and across non-zero per-body offsets.
  std::vector<std::size_t> selectedFaces;
  for (std::size_t face = 100; face < 165; ++face)
    selectedFaces.push_back(face);
  std::vector<std::size_t> selectedEdges;
  for (std::size_t edge = 700; edge < 740; ++edge)
    selectedEdges.push_back(edge);
  const auto highlights =
      solidar::ViewportRenderer::buildHighlightRepresentation(
          100, 80, 700, 60, selectedFaces, {179}, selectedEdges, 759);
  CHECK(highlights.selectedFaces.size() == 65);
  CHECK(highlights.selectedFaces.front() == 0);
  CHECK(highlights.selectedFaces.back() == 64);
  CHECK(highlights.hoveredFaces == std::vector<std::size_t>{79});
  CHECK(highlights.selectedEdges.size() == 40);
  CHECK(highlights.selectedEdges.front() == 0);
  CHECK(highlights.selectedEdges.back() == 39);
  CHECK(highlights.hoveredEdge == 59);

  const auto filtered =
      solidar::ViewportRenderer::buildHighlightRepresentation(
          100, 2, 700, 2, {99, 100, 101, 102}, {},
          {699, 700, 701, 702}, 699);
  CHECK((filtered.selectedFaces == std::vector<std::size_t>{0, 1}));
  CHECK((filtered.selectedEdges == std::vector<std::size_t>{0, 1}));
  CHECK(!filtered.hoveredEdge);

  // Exercise the real upload/prepare/draw path with multiple structural body
  // keys. This verifies that the old 32-element shader-uniform limit is not
  // merely absent from the pure localization helper, but also absent from the
  // GPU overlay buffers actually prepared for rendering.
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(3, 0);
  format.setProfile(QSurfaceFormat::NoProfile);
  format.setDepthBufferSize(24);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  CHECK(surface.isValid());
  QOpenGLContext context;
  context.setFormat(surface.format());
  CHECK(context.create());
  CHECK(context.makeCurrent(&surface));
  QOpenGLFramebufferObject framebuffer(QSize(320, 240));
  CHECK(framebuffer.isValid());
  CHECK(framebuffer.bind());

  solidar::BodyRenderMesh mesh;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
  CHECK(mesh.tryRebuild(box));
  CHECK(mesh.faceCount() >= 6);
  CHECK(mesh.edges().size() >= 12);
  std::vector<solidar::RenderMeshInstance> instances;
  constexpr std::size_t bodyCount = 40;
  for (std::size_t body = 0; body < bodyCount; ++body) {
    const auto identityValue = static_cast<std::uintptr_t>(body + 1);
    instances.push_back(
        {&mesh,
         {body + 1, 100 + body, 200 + body,
          reinterpret_cast<const void*>(identityValue), mesh.quality()},
         body * mesh.faceCount(), body * mesh.edges().size()});
  }
  std::vector<std::size_t> gpuSelectedFaces;
  for (std::size_t face = 0; face < 65; ++face)
    gpuSelectedFaces.push_back(face);
  std::vector<std::size_t> gpuSelectedEdges;
  for (std::size_t edge = 0; edge < 40; ++edge)
    gpuSelectedEdges.push_back(edge);
  solidar::ViewportRenderer renderer;
  renderer.render(instances, &mesh, QSize(320, 240), 1.0F, -35.0F,
                  25.0F, 1.0F, {},
                  solidar::ViewportDisplayMode::ShadedWithEdges,
                  gpuSelectedFaces, {65}, gpuSelectedEdges, 40, &mesh);
  CHECK(renderer.error().isEmpty());
  auto resources = renderer.resourceDiagnostics();
  CHECK(resources.initialized);
  CHECK(resources.sourceMeshes == bodyCount);
  CHECK(resources.previewResident);
  CHECK(resources.cutPreviewResident);
  CHECK(resources.selectedFaces == 65);
  CHECK(resources.hoveredFaces == 1);
  CHECK(resources.selectedEdges == 40);
  CHECK(resources.hoveredEdges == 1);

  // The native B-Rep pass must consume the resolved ThemeColors rather than
  // retaining shader literals. Clear the framebuffer to each theme's
  // background and verify that the ordinary edge uniform reaches real pixels.
  const auto renderTheme = [&](const solidar::ThemeColors& theme) {
    auto* gl = context.functions();
    gl->glClearColor(theme.viewportBackground.redF(),
                     theme.viewportBackground.greenF(),
                     theme.viewportBackground.blueF(), 1.0F);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.render({instances.front()}, nullptr, QSize(320, 240), 1.0F,
                    -35.0F, 25.0F, 1.0F, {},
                    solidar::ViewportDisplayMode::ShadedWithEdges, {}, {}, {},
                    std::size_t(-1), nullptr, 0, 0, theme);
    gl->glFinish();
    return framebuffer.toImage();
  };
  const solidar::ThemeColors lightTheme = solidar::lightThemeColors();
  const solidar::ThemeColors darkTheme = solidar::darkThemeColors();
  const QImage lightFrame = renderTheme(lightTheme);
  const QImage darkFrame = renderTheme(darkTheme);
  CHECK(imageContainsColorNear(lightFrame, lightTheme.viewportEdge));
  CHECK(imageContainsColorNear(darkFrame, darkTheme.viewportEdge));
  CHECK(lightFrame != darkFrame);

  // ReplaceSource keeps the canonical source available only for interaction
  // overlays. Its ordinary geometry must stay suppressed while an unrelated
  // body remains in the regular surface/edge passes.
  auto highlightOnly = instances[0];
  highlightOnly.drawOrdinary = false;
  const std::vector<solidar::RenderMeshInstance> mixedPresentation{
      highlightOnly, instances[1]};
  renderer.render(mixedPresentation, &mesh, QSize(320, 240), 1.0F, -35.0F,
                  25.0F, 1.0F, {},
                  solidar::ViewportDisplayMode::ShadedWithEdges, {0}, {}, {},
                  0);
  const auto overlayOnly = renderer.resourceDiagnostics();
  CHECK(overlayOnly.ordinarySourceMeshes == 1);
  CHECK(overlayOnly.highlightOnlySourceMeshes == 1);
  CHECK(overlayOnly.highlightOverlayMeshes == 1);
  CHECK(overlayOnly.selectedFaces == 1);
  CHECK(overlayOnly.hoveredEdges == 1);

  // A fresh BodyRenderMesh starts its local revision at one. The stable
  // preview/cut slots therefore need the Viewport-owned presentation revision
  // to distinguish A from B while memoizing the exact same publication.
  solidar::BodyRenderMesh previewSlot;
  CHECK(previewSlot.tryRebuild(BRepPrimAPI_MakeBox(7.0, 8.0, 9.0).Shape()));
  const std::vector<solidar::RenderMeshInstance> twoBodies{
      instances.begin(), instances.begin() + 2};
  renderer.render(twoBodies, &previewSlot, QSize(320, 240), 1.0F, -35.0F,
                  25.0F, 1.0F, {},
                  solidar::ViewportDisplayMode::ShadedWithEdges, {}, {}, {},
                  std::size_t(-1), &previewSlot, 1001, 2001);
  const auto previewA = renderer.resourceDiagnostics();
  CHECK(previewA.sourceMeshes == 2);
  CHECK(previewA.previewUploadGeneration != 0);
  CHECK(previewA.cutPreviewUploadGeneration != 0);
  renderer.render(twoBodies, &previewSlot, QSize(320, 240), 1.0F, -35.0F,
                  25.0F, 1.0F, {},
                  solidar::ViewportDisplayMode::ShadedWithEdges, {}, {}, {},
                  std::size_t(-1), &previewSlot, 1001, 2001);
  const auto memoized = renderer.resourceDiagnostics();
  CHECK(memoized.previewUploadGeneration == previewA.previewUploadGeneration);
  CHECK(memoized.cutPreviewUploadGeneration ==
        previewA.cutPreviewUploadGeneration);

  solidar::BodyRenderMesh pendingPreview;
  CHECK(pendingPreview.tryRebuild(
      BRepPrimAPI_MakeCylinder(4.0, 13.0).Shape()));
  previewSlot = std::move(pendingPreview);  // stable address, local revision 1
  renderer.render(twoBodies, &previewSlot, QSize(320, 240), 1.0F, -35.0F,
                  25.0F, 1.0F, {},
                  solidar::ViewportDisplayMode::ShadedWithEdges, {}, {}, {},
                  std::size_t(-1), &previewSlot, 1002, 2002);
  const auto previewB = renderer.resourceDiagnostics();
  CHECK(previewB.previewUploadGeneration > previewA.previewUploadGeneration);
  CHECK(previewB.cutPreviewUploadGeneration >
        previewA.cutPreviewUploadGeneration);
  CHECK(previewB.previewContentHash != previewA.previewContentHash);
  CHECK(previewB.cutPreviewContentHash != previewA.cutPreviewContentHash);
  CHECK(previewB.previewIndexCount != previewA.previewIndexCount);
  CHECK(previewB.cutPreviewIndexCount != previewA.cutPreviewIndexCount);

  // A single hovered primitive must gather only its own pre-indexed span,
  // independent of the 100k unrelated triangles/edges in the mesh.
  auto largeMesh = solidar::BodyRenderMeshBenchmarkAdapter::
      onePrimitivePerTopology(100000);
  const solidar::BodyMeshKey largeIdentity{
      900, 901, 1, &largeMesh, largeMesh.quality()};
  renderer.render({{&largeMesh, largeIdentity, 0, 0}}, nullptr,
                  QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F, {},
                  solidar::ViewportDisplayMode::ShadedWithEdges, {}, {99999},
                  {}, 99999);
  const auto localWork = renderer.resourceDiagnostics();
  CHECK(localWork.sourceMeshes == 1);
  CHECK(localWork.highlightFaceIndexVisits == 3);
  CHECK(localWork.highlightEdgeVertexVisits == 2);
  const std::size_t duplicatedPrimitiveBytes =
      largeMesh.triangleIndices().size() * sizeof(std::uint32_t) +
      largeMesh.edgeSampleCount() * 4 * sizeof(float);
  CHECK(localWork.highlightLookupBytes < duplicatedPrimitiveBytes / 2);

  // Allocation and upload failures are injected before publication. They
  // must be contained by the renderer boundary, preserve the previous
  // preview/cut slots, and leave no null source cache entry behind.
  solidar::ViewportRenderer failureRenderer;
  solidar::ViewportRendererTestAdapter::failNextSourceCacheAllocation(
      failureRenderer);
  failureRenderer.render({instances.front()}, nullptr, QSize(320, 240), 1.0F,
                         -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {}, std::size_t(-1));
  CHECK(!failureRenderer.error().isEmpty());
  CHECK(failureRenderer.resourceDiagnostics().sourceMeshes == 0);
  failureRenderer.release();

  failureRenderer.render({instances.front()}, &mesh, QSize(320, 240), 1.0F,
                         -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {}, std::size_t(-1), &mesh, 8001, 9001);
  CHECK(failureRenderer.error().isEmpty());
  const auto stableSlots = failureRenderer.resourceDiagnostics();
  CHECK(stableSlots.sourceMeshes == 1);
  CHECK(stableSlots.previewUploadGeneration != 0);
  CHECK(stableSlots.cutPreviewUploadGeneration != 0);

  // VAO binding is verified separately from object creation. Fail the surface
  // and edge binds inside a pending preview upload; neither attempt may
  // publish a new preview generation.
  for (const std::size_t ordinal : {std::size_t{1}, std::size_t{2}}) {
    solidar::ViewportRendererTestAdapter::failNthVaoBind(failureRenderer,
                                                         ordinal);
    failureRenderer.render(
        {instances.front()}, &previewSlot, QSize(320, 240), 1.0F, -35.0F,
        25.0F, 1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges, {},
        {}, {}, std::size_t(-1), &mesh, 8002, 9001);
    CHECK(!failureRenderer.error().isEmpty());
    const auto failedVao = failureRenderer.resourceDiagnostics();
    CHECK(failedVao.previewUploadGeneration ==
          stableSlots.previewUploadGeneration);
    CHECK(failedVao.previewContentHash == stableSlots.previewContentHash);
  }

  // The same checked binding applies to dynamically materialized highlight
  // VAOs. A failed bind leaves the key unpublished so the next frame retries.
  solidar::ViewportRendererTestAdapter::failNthVaoBind(failureRenderer, 1);
  failureRenderer.render({instances.front()}, &mesh, QSize(320, 240), 1.0F,
                         -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {0}, std::size_t(-1), &mesh, 8001, 9001);
  CHECK(!failureRenderer.error().isEmpty());
  CHECK(failureRenderer.resourceDiagnostics().selectedEdges == 0);
  failureRenderer.render({instances.front()}, &mesh, QSize(320, 240), 1.0F,
                         -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {0}, std::size_t(-1), &mesh, 8001, 9001);
  CHECK(failureRenderer.error().isEmpty());
  CHECK(failureRenderer.resourceDiagnostics().selectedEdges == 1);

  solidar::ViewportRendererTestAdapter::failNthUploadAfterVertexBuffer(
      failureRenderer, 1);
  failureRenderer.render({instances.front()}, &previewSlot, QSize(320, 240),
                         1.0F, -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {}, std::size_t(-1), &mesh, 8002, 9001);
  CHECK(!failureRenderer.error().isEmpty());
  auto failedSlots = failureRenderer.resourceDiagnostics();
  CHECK(failedSlots.previewUploadGeneration ==
        stableSlots.previewUploadGeneration);
  CHECK(failedSlots.previewContentHash == stableSlots.previewContentHash);
  CHECK(failedSlots.cutPreviewUploadGeneration ==
        stableSlots.cutPreviewUploadGeneration);

  solidar::ViewportRendererTestAdapter::failNthUploadAfterVertexBuffer(
      failureRenderer, 2);
  failureRenderer.render({instances.front()}, &previewSlot, QSize(320, 240),
                         1.0F, -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {}, std::size_t(-1), &previewSlot, 8002, 9002);
  CHECK(!failureRenderer.error().isEmpty());
  failedSlots = failureRenderer.resourceDiagnostics();
  CHECK(failedSlots.previewUploadGeneration ==
        stableSlots.previewUploadGeneration);
  CHECK(failedSlots.cutPreviewUploadGeneration ==
        stableSlots.cutPreviewUploadGeneration);
  CHECK(failedSlots.cutPreviewContentHash == stableSlots.cutPreviewContentHash);

  failureRenderer.render({instances.front()}, &previewSlot, QSize(320, 240),
                         1.0F, -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {}, std::size_t(-1), &previewSlot, 8002, 9002);
  CHECK(failureRenderer.error().isEmpty());
  const auto recoveredSlots = failureRenderer.resourceDiagnostics();
  CHECK(recoveredSlots.previewUploadGeneration >
        stableSlots.previewUploadGeneration);
  CHECK(recoveredSlots.cutPreviewUploadGeneration >
        stableSlots.cutPreviewUploadGeneration);
  failureRenderer.release();

  // Nonempty -> empty and preview removal release every resident GPU slot even
  // when the widget's paintGL path would otherwise be skipped.
  renderer.synchronizeResources({}, false, false);
  resources = renderer.resourceDiagnostics();
  CHECK(resources.sourceMeshes == 0);
  CHECK(!resources.previewResident);
  CHECK(!resources.cutPreviewResident);
  renderer.release();
  CHECK(!renderer.resourceDiagnostics().initialized);
  CHECK(renderer.initialize());
  CHECK(renderer.resourceDiagnostics().initialized);
  renderer.release();
  framebuffer.release();
  context.doneCurrent();

  return EXIT_SUCCESS;
}

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
#include <limits>
#include <numbers>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/AssemblyScenePublicationCoordinator.h"
#include "presentation/ComponentScenePublicationCoordinator.h"
#include "ui/AssemblyViewportScene.h"
#include "ui/ComponentViewportScene.h"

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
  static void injectGlErrorAfterAssemblyPreflight(
      ViewportRenderer& renderer) {
    renderer.injectPreDrawGlError_ = true;
  }
  static void injectContextMismatchAfterAssemblyPreflight(
      ViewportRenderer& renderer) {
    renderer.contextMismatchCheckpoint_ =
        ViewportRenderer::ContextMismatchCheckpoint::AfterAssemblyPreflight;
  }
  static void injectContextMismatchAfterAssemblyDraw(
      ViewportRenderer& renderer) {
    renderer.contextMismatchCheckpoint_ =
        ViewportRenderer::ContextMismatchCheckpoint::AfterAssemblyDraw;
  }
};

}  // namespace solidar

namespace {

struct AssemblyGpuFixture final {
  std::unique_ptr<solidar::ProductDocument> product;
  std::vector<solidar::OccurrenceId> occurrences;
};

AssemblyGpuFixture assemblyGpuFixture() {
  using namespace solidar;
  auto document = std::make_unique<Document>();
  Body& first = document->addBody(1, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      1, std::make_shared<const TopoDS_Shape>(
             BRepPrimAPI_MakeBox(10.0, 12.0, 14.0).Shape()),
      "first feature"));
  Body& second = document->addBody(2, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      2, std::make_shared<const TopoDS_Shape>(
             BRepPrimAPI_MakeCylinder(4.0, 13.0).Shape()),
      "second feature"));
  CHECK(document->recompute());
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{10}, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  std::vector<OccurrenceId> ids;
  for (std::uint64_t ordinal = 0; ordinal < 3; ++ordinal) {
    const OccurrenceId id{1001 + ordinal};
    ids.push_back(id);
    auto transform = RigidTransform3d::create(
        {static_cast<double>(ordinal) * 25.0, 0.0, 0.0},
        {1.0, 0.0, 0.0, 0.0});
    CHECK(transform);
    CHECK(root.insert(ComponentOccurrence(id, DefinitionId{10},
                                          *transform.transform)) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  }
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{100});
  CHECK(made);
  return {std::move(made.product), std::move(ids)};
}

std::unique_ptr<solidar::ProductDocument> emptyAssemblyGpuProduct() {
  using namespace solidar;
  DefinitionRegistry definitions;
  CHECK(definitions.insert(AssemblyDefinition(DefinitionId{900})) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions),
                                      DefinitionId{900});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<solidar::Document> componentGpuDocument() {
  using namespace solidar;
  auto document = std::make_unique<Document>();
  Body& first = document->addBody(1, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      1, std::make_shared<const TopoDS_Shape>(
             BRepPrimAPI_MakeBox(10.0, 12.0, 14.0).Shape()),
      "first feature"));
  Body& second = document->addBody(2, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      2, std::make_shared<const TopoDS_Shape>(
             BRepPrimAPI_MakeCylinder(4.0, 13.0).Shape()),
      "second feature"));
  CHECK(document->recompute());
  return document;
}

struct ComponentGpuFixture final {
  std::unique_ptr<solidar::ProductDocument> product;
  std::vector<solidar::OccurrenceId> occurrences;
};

ComponentGpuFixture componentGpuFixture() {
  using namespace solidar;
  ComponentDefinition root(DefinitionId{300}, componentGpuDocument());
  ComponentDefinition child(DefinitionId{200}, componentGpuDocument());
  std::vector<OccurrenceId> ids;
  for (std::uint64_t ordinal = 0; ordinal < 2; ++ordinal) {
    const OccurrenceId id{10 + ordinal};
    ids.push_back(id);
    const auto placement = RigidTransform3d::create(
        {30.0 + static_cast<double>(ordinal) * 30.0, 0.0, 0.0},
        {1.0, 0.0, 0.0, 0.0});
    CHECK(placement);
    CHECK(root.insert(ComponentOccurrence(
              id, DefinitionId{200}, *placement.transform)) ==
          ComponentOccurrenceInsertStatus::Inserted);
  }
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(child));
  definitions.push_back(std::move(root));
  auto made = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{300});
  CHECK(made);
  return {std::move(made.product), std::move(ids)};
}

std::unique_ptr<solidar::ProductDocument> emptyRootComponentGpuProduct() {
  using namespace solidar;
  ComponentDefinition root(DefinitionId{300}, std::make_unique<Document>());
  ComponentDefinition child(DefinitionId{200}, componentGpuDocument());
  const auto placement = RigidTransform3d::create(
      {30.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(placement);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{200}, *placement.transform)) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(child));
  definitions.push_back(std::move(root));
  auto made = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{300});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<solidar::ProductDocument> nestedComponentGpuProduct() {
  using namespace solidar;
  ComponentDefinition root(DefinitionId{300}, componentGpuDocument());
  ComponentDefinition middle(DefinitionId{200}, componentGpuDocument());
  ComponentDefinition leaf(DefinitionId{100}, componentGpuDocument());
  const auto direct = RigidTransform3d::create(
      {30.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  const auto nested = RigidTransform3d::create(
      {0.0, 30.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  const auto sibling = RigidTransform3d::create(
      {-30.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(direct && nested && sibling);
  CHECK(middle.insert(ComponentOccurrence(
            OccurrenceId{20}, DefinitionId{100}, *nested.transform)) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{200}, *direct.transform)) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{11}, DefinitionId{100}, *sibling.transform)) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(leaf));
  definitions.push_back(std::move(middle));
  definitions.push_back(std::move(root));
  auto made = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{300});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<solidar::ProductDocument> emptyComponentGpuProduct() {
  using namespace solidar;
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{901}, std::make_unique<Document>());
  auto made = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{901});
  CHECK(made);
  return std::move(made.product);
}

void moveComponentOccurrence(solidar::ProductDocument& product,
                             solidar::OccurrenceId id, double x) {
  using namespace solidar;
  auto* root = const_cast<ComponentDefinition*>(
      product.rootComponentDefinition());
  CHECK(root);
  const auto* before = root->find(id);
  CHECK(before);
  const auto flags = before->flags();
  const auto target = before->targetDefinitionId();
  CHECK(root->erase(id) == ComponentOccurrenceEraseStatus::Erased);
  const auto placement =
      RigidTransform3d::create({x, 7.0, 2.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(placement);
  CHECK(root->insert(ComponentOccurrence(id, target, *placement.transform,
                                         flags)) ==
        ComponentOccurrenceInsertStatus::Inserted);
}

solidar::OccurrencePath occurrencePath(solidar::OccurrenceId id) {
  auto made = solidar::OccurrencePath::create({id});
  CHECK(made);
  return std::move(*made.path);
}

std::optional<solidar::Bounds3d> previewBounds(
    const solidar::ComponentViewportScene& scene,
    const solidar::ComponentPlacementPreview& preview) {
  std::optional<solidar::Bounds3d> result;
  for (const auto& instance : scene.instances()) {
    solidar::Bounds3d bounds = instance.worldBounds;
    if (preview.moves(instance.bodyIdentity.component)) {
      const auto minimum = bounds.minimum();
      const auto maximum = bounds.maximum();
      const auto& delta = preview.worldDeltaMm();
      auto shifted = solidar::Bounds3d::create(
          {minimum.x + delta[0], minimum.y + delta[1], minimum.z + delta[2]},
          {maximum.x + delta[0], maximum.y + delta[1], maximum.z + delta[2]});
      CHECK(shifted);
      bounds = *shifted;
    }
    if (!result)
      result = bounds;
    else {
      auto united = result->united(bounds);
      CHECK(united);
      result = *united;
    }
  }
  return result;
}

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

bool sameRendererDiagnostics(
    const solidar::RendererResourceDiagnostics& left,
    const solidar::RendererResourceDiagnostics& right,
    bool ignoreStaleRejections = false) {
  if (left.initialized != right.initialized ||
      left.sourceMeshes != right.sourceMeshes ||
      left.previewResident != right.previewResident ||
      left.cutPreviewResident != right.cutPreviewResident ||
      left.selectedFaces != right.selectedFaces ||
      left.hoveredFaces != right.hoveredFaces ||
      left.selectedEdges != right.selectedEdges ||
      left.hoveredEdges != right.hoveredEdges ||
      left.previewUploadGeneration != right.previewUploadGeneration ||
      left.cutPreviewUploadGeneration != right.cutPreviewUploadGeneration ||
      left.previewContentHash != right.previewContentHash ||
      left.cutPreviewContentHash != right.cutPreviewContentHash ||
      left.previewIndexCount != right.previewIndexCount ||
      left.cutPreviewIndexCount != right.cutPreviewIndexCount ||
      left.highlightFaceIndexVisits != right.highlightFaceIndexVisits ||
      left.highlightEdgeVertexVisits != right.highlightEdgeVertexVisits ||
      left.highlightLookupBytes != right.highlightLookupBytes ||
      left.ordinarySourceMeshes != right.ordinarySourceMeshes ||
      left.highlightOnlySourceMeshes != right.highlightOnlySourceMeshes ||
      left.highlightOverlayMeshes != right.highlightOverlayMeshes ||
      left.ordinaryCameraCenter.x != right.ordinaryCameraCenter.x ||
      left.ordinaryCameraCenter.y != right.ordinaryCameraCenter.y ||
      left.ordinaryCameraCenter.z != right.ordinaryCameraCenter.z ||
      left.ordinaryCameraDepthExtent != right.ordinaryCameraDepthExtent ||
      left.ordinaryDepthBufferPreserved !=
          right.ordinaryDepthBufferPreserved ||
      left.sourceUploadAttempts != right.sourceUploadAttempts ||
      left.sourceUploadSuccesses != right.sourceUploadSuccesses ||
      left.sourceUploadFailures != right.sourceUploadFailures ||
      left.sourceDestroyCount != right.sourceDestroyCount ||
      left.sourcePruneCount != right.sourcePruneCount ||
      left.deferredSourceDestructions !=
          right.deferredSourceDestructions ||
      left.deferredSourceCleanupCount !=
          right.deferredSourceCleanupCount ||
      (!ignoreStaleRejections &&
       (left.staleAssemblyRejections != right.staleAssemblyRejections ||
        left.staleComponentRejections != right.staleComponentRejections)) ||
      left.activeInstancedSceneKind != right.activeInstancedSceneKind ||
      left.activeAssemblyUsers != right.activeAssemblyUsers ||
      left.activePublicationGeneration != right.activePublicationGeneration ||
      left.submittedAssemblyInstances != right.submittedAssemblyInstances ||
      left.drawnAssemblyInstances != right.drawnAssemblyInstances ||
      left.highlightedAssemblyInstances !=
          right.highlightedAssemblyInstances ||
      left.drawnAssemblySurfacePasses != right.drawnAssemblySurfacePasses ||
      left.drawnAssemblyEdgePasses != right.drawnAssemblyEdgePasses ||
      left.assemblyWorldBoundsValid != right.assemblyWorldBoundsValid ||
      left.assemblyCameraCenter.x != right.assemblyCameraCenter.x ||
      left.assemblyCameraCenter.y != right.assemblyCameraCenter.y ||
      left.assemblyCameraCenter.z != right.assemblyCameraCenter.z ||
      left.assemblyCameraDepthExtent != right.assemblyCameraDepthExtent ||
      left.activeComponentUsers != right.activeComponentUsers ||
      left.activeComponentPublicationGeneration !=
          right.activeComponentPublicationGeneration ||
      left.submittedComponentInstances != right.submittedComponentInstances ||
      left.drawnComponentInstances != right.drawnComponentInstances ||
      left.highlightedComponentInstances !=
          right.highlightedComponentInstances ||
      left.drawnComponentSurfacePasses != right.drawnComponentSurfacePasses ||
      left.drawnComponentEdgePasses != right.drawnComponentEdgePasses ||
      left.componentWorldBoundsValid != right.componentWorldBoundsValid ||
      left.componentCameraCenter.x != right.componentCameraCenter.x ||
      left.componentCameraCenter.y != right.componentCameraCenter.y ||
      left.componentCameraCenter.z != right.componentCameraCenter.z ||
      left.componentCameraDepthExtent != right.componentCameraDepthExtent ||
      left.sources.size() != right.sources.size())
    return false;
  for (std::size_t index = 0; index < left.sources.size(); ++index) {
    if (!(left.sources[index].key == right.sources[index].key) ||
        left.sources[index].uploadGeneration !=
            right.sources[index].uploadGeneration ||
        left.sources[index].contentFingerprint !=
            right.sources[index].contentFingerprint)
      return false;
  }
  return true;
}

bool sameAuthoritativeAssemblyDiagnostics(
    const solidar::RendererResourceDiagnostics& left,
    const solidar::RendererResourceDiagnostics& right) {
  if (left.sourceMeshes != right.sourceMeshes ||
      left.sourceDestroyCount != right.sourceDestroyCount ||
      left.sourcePruneCount != right.sourcePruneCount ||
      left.activeAssemblyUsers != right.activeAssemblyUsers ||
      left.activePublicationGeneration != right.activePublicationGeneration ||
      left.submittedAssemblyInstances != right.submittedAssemblyInstances ||
      left.drawnAssemblyInstances != right.drawnAssemblyInstances ||
      left.highlightedAssemblyInstances !=
          right.highlightedAssemblyInstances ||
      left.drawnAssemblySurfacePasses != right.drawnAssemblySurfacePasses ||
      left.drawnAssemblyEdgePasses != right.drawnAssemblyEdgePasses ||
      left.assemblyWorldBoundsValid != right.assemblyWorldBoundsValid ||
      left.assemblyCameraCenter.x != right.assemblyCameraCenter.x ||
      left.assemblyCameraCenter.y != right.assemblyCameraCenter.y ||
      left.assemblyCameraCenter.z != right.assemblyCameraCenter.z ||
      left.assemblyCameraDepthExtent != right.assemblyCameraDepthExtent ||
      left.sources.size() != right.sources.size())
    return false;
  for (std::size_t index = 0; index < left.sources.size(); ++index) {
    if (!(left.sources[index].key == right.sources[index].key) ||
        left.sources[index].uploadGeneration !=
            right.sources[index].uploadGeneration ||
        left.sources[index].contentFingerprint !=
            right.sources[index].contentFingerprint)
      return false;
  }
  return true;
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

  const double halfSqrt = std::sqrt(0.5);
  const auto rigid = solidar::RigidTransform3d::create(
      {10.0, 20.0, 30.0}, {halfSqrt, 0.0, 0.0, halfSqrt});
  CHECK(rigid);
  const auto model = solidar::ViewportRenderer::modelMatrix(*rigid.transform);
  CHECK(model);
  const QVector4D transformed = *model * QVector4D(2.0F, 3.0F, 4.0F, 1.0F);
  CHECK(std::abs(transformed.x() - 7.0F) < 1e-5F);
  CHECK(std::abs(transformed.y() - 22.0F) < 1e-5F);
  CHECK(std::abs(transformed.z() - 34.0F) < 1e-5F);
  const auto overflow = solidar::RigidTransform3d::create(
      {std::numeric_limits<double>::max(), 0.0, 0.0},
      {1.0, 0.0, 0.0, 0.0});
  CHECK(overflow);
  CHECK(!solidar::ViewportRenderer::modelMatrix(*overflow.transform));

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

  // All renderer-owned Qt GL wrappers are destroyed while the owner context
  // is current, matching QOpenGLWidget's teardown contract.
  {

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

  // Component authoring draws the surrounding product first and the active
  // definition second. The second pass must reuse the exact camera and keep
  // the first pass depth buffer so orbiting cannot change their apparent
  // relative placement or surface ordering.
  const solidar::Point3d sharedCenter{17.0, -9.0, 23.0};
  context.functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  renderer.render(
      {instances.front()}, nullptr, QSize(320, 240), 1.0F, 42.0F, -18.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges, {}, {}, {},
      std::size_t(-1), nullptr, 0, 0, solidar::lightThemeColors(),
      solidar::OrdinaryRenderComposition{sharedCenter, 900.0, true});
  CHECK(renderer.error().isEmpty());
  const auto composed = renderer.resourceDiagnostics();
  CHECK(composed.ordinaryCameraCenter.x == sharedCenter.x);
  CHECK(composed.ordinaryCameraCenter.y == sharedCenter.y);
  CHECK(composed.ordinaryCameraCenter.z == sharedCenter.z);
  CHECK(composed.ordinaryCameraDepthExtent == 900.0);
  CHECK(composed.ordinaryDepthBufferPreserved);

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
  {
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

  auto failedSlots = stableSlots;
  solidar::ViewportRendererTestAdapter::failNthUploadAfterVertexBuffer(
      failureRenderer, 1);
  failureRenderer.render({instances.front()}, &previewSlot, QSize(320, 240),
                         1.0F, -35.0F, 25.0F, 1.0F, {},
                         solidar::ViewportDisplayMode::ShadedWithEdges, {},
                         {}, {}, std::size_t(-1), &mesh, 8002, 9001);
  CHECK(!failureRenderer.error().isEmpty());
  failedSlots = failureRenderer.resourceDiagnostics();
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
  }

  // Assembly instancing owns two definition-local GPU sources for six draws.
  // Whole-occurrence tint is a draw uniform, so selecting one occurrence
  // highlights both body instances without modifying either shared source.
  {
  auto assemblyFixture = assemblyGpuFixture();
  solidar::AssemblyScenePublicationCoordinator assemblyCoordinator;
  constexpr solidar::AssemblyMeshProfile normalProfile{
      solidar::AssemblyMeshQuality::Normal};
  CHECK(assemblyCoordinator.rebuildAndPublish(
      *assemblyFixture.product, solidar::ProductPresentationEpoch{501},
      normalProfile));
  solidar::AssemblyViewportSceneAdapter sceneAdapter;
  CHECK(sceneAdapter.publish(assemblyCoordinator.active()));
  solidar::AssemblyViewportPicking assemblyPicking;
  CHECK(assemblyPicking.publish(sceneAdapter.active()));
  solidar::ViewportRenderer assemblyRenderer;
  const auto pickingCamera = solidar::ViewportCameraState{
      0.0F, 0.0F, 1.0F, {}, QSize{320, 240}, 1.0F,
      sceneAdapter.active()->worldBounds()->center(),
      sceneAdapter.active()->worldBounds()->diagonal() * 3.0};
  const auto selectedHit = assemblyPicking.faceAt(
      pickingCamera, pickingCamera.worldToScreen({5.0, 6.0, 14.0}));
  CHECK(selectedHit);
  const auto selectedTarget = selectedHit.hit->highlightTarget();
  CHECK(selectedTarget);
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges, selectedTarget));
  const auto assemblyInitial = assemblyRenderer.resourceDiagnostics();
  CHECK(assemblyInitial.sourceMeshes == 2);
  CHECK(assemblyInitial.sourceUploadAttempts == 2);
  CHECK(assemblyInitial.sourceUploadSuccesses == 2);
  CHECK(assemblyInitial.activeAssemblyUsers == 6);
  CHECK(assemblyInitial.submittedAssemblyInstances == 6);
  CHECK(assemblyInitial.drawnAssemblyInstances == 6);
  CHECK(assemblyInitial.highlightedAssemblyInstances == 2);
  CHECK(assemblyInitial.drawnAssemblySurfacePasses == 6);
  CHECK(assemblyInitial.drawnAssemblyEdgePasses == 6);
  CHECK(assemblyInitial.activePublicationGeneration != 0);
  CHECK(assemblyInitial.sources.size() == 2);
  CHECK(assemblyInitial.assemblyWorldBoundsValid);
  CHECK(sceneAdapter.active()->worldBounds());
  CHECK(std::abs(assemblyInitial.assemblyCameraCenter.x -
                 sceneAdapter.active()->worldBounds()->center().x) < 1e-9);
  CHECK(std::abs(assemblyInitial.assemblyCameraCenter.y -
                 sceneAdapter.active()->worldBounds()->center().y) < 1e-9);
  CHECK(std::abs(assemblyInitial.assemblyCameraCenter.z -
                 sceneAdapter.active()->worldBounds()->center().z) < 1e-9);
  CHECK(std::abs(assemblyInitial.assemblyCameraDepthExtent -
                 sceneAdapter.active()->worldBounds()->diagonal() * 3.0) <
        1e-9);
  const auto initialSourceDiagnostics = assemblyInitial.sources;
  // Embedded-definition editing detaches every assembly instance while its
  // Part presentation is active, but the already-uploaded definition sources
  // remain resident.  A generic Part/empty synchronization must not prune
  // them, and resuming the same publication must not upload them again.
  CHECK(assemblyRenderer.suspendAssemblySceneResources());
  const auto suspendedAssembly = assemblyRenderer.resourceDiagnostics();
  CHECK(suspendedAssembly.sourceMeshes == assemblyInitial.sourceMeshes);
  CHECK(suspendedAssembly.activeAssemblyUsers == 0);
  CHECK(suspendedAssembly.activePublicationGeneration == 0);
  assemblyRenderer.synchronizeResources({}, false, false);
  const auto synchronizedWhileSuspended =
      assemblyRenderer.resourceDiagnostics();
  CHECK(synchronizedWhileSuspended.sourceMeshes ==
        assemblyInitial.sourceMeshes);
  CHECK(synchronizedWhileSuspended.sourceDestroyCount ==
        assemblyInitial.sourceDestroyCount);
  CHECK(synchronizedWhileSuspended.sourcePruneCount ==
        assemblyInitial.sourcePruneCount);
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto resumedAssembly = assemblyRenderer.resourceDiagnostics();
  CHECK(resumedAssembly.sourceUploadAttempts ==
        assemblyInitial.sourceUploadAttempts);
  CHECK(resumedAssembly.sources.size() == initialSourceDiagnostics.size());
  for (std::size_t index = 0; index < resumedAssembly.sources.size(); ++index) {
    CHECK(resumedAssembly.sources[index].key ==
          initialSourceDiagnostics[index].key);
    CHECK(resumedAssembly.sources[index].uploadGeneration ==
          initialSourceDiagnostics[index].uploadGeneration);
    CHECK(resumedAssembly.sources[index].contentFingerprint ==
          initialSourceDiagnostics[index].contentFingerprint);
  }
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  CHECK(assemblyRenderer.resourceDiagnostics().highlightedAssemblyInstances ==
        0);
  CHECK(assemblyRenderer.resourceDiagnostics().sourceUploadAttempts ==
        assemblyInitial.sourceUploadAttempts);
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges, selectedTarget));
  CHECK(assemblyRenderer.resourceDiagnostics().highlightedAssemblyInstances ==
        2);
  CHECK(assemblyRenderer.resourceDiagnostics().sourceUploadAttempts ==
        assemblyInitial.sourceUploadAttempts);

  auto moved = solidar::RigidTransform3d::create(
      {-31.0, 9.0, 2.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(moved);
  CHECK(assemblyFixture.product->setRootOccurrencePlacement(
            occurrencePath(assemblyFixture.occurrences[1]),
            *moved.transform) ==
        solidar::ProductOccurrenceMutationStatus::Committed);
  CHECK(assemblyCoordinator.rebuildAndPublish(
      *assemblyFixture.product, solidar::ProductPresentationEpoch{501},
      normalProfile));
  CHECK(sceneAdapter.publish(assemblyCoordinator.active()));
  CHECK(assemblyPicking.publish(sceneAdapter.active()));
  CHECK(!selectedTarget->isCurrentFor(*sceneAdapter.active()));
  // A foreign caller's pre-existing error is drained at the transaction
  // boundary and cannot be misattributed to this otherwise reusable scene.
  context.functions()->glEnable(static_cast<GLenum>(0xFFFFFFFFU));
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges, selectedTarget));
  const auto placementOnly = assemblyRenderer.resourceDiagnostics();
  CHECK(placementOnly.sourceMeshes == 2);
  CHECK(placementOnly.sourceUploadAttempts ==
        assemblyInitial.sourceUploadAttempts);
  CHECK(placementOnly.highlightedAssemblyInstances == 0);
  CHECK(placementOnly.sources.size() == initialSourceDiagnostics.size());
  CHECK(placementOnly.assemblyWorldBoundsValid);
  CHECK(sceneAdapter.active()->worldBounds());
  CHECK(std::abs(placementOnly.assemblyCameraDepthExtent -
                 sceneAdapter.active()->worldBounds()->diagonal() * 3.0) <
        1e-9);
  CHECK(placementOnly.assemblyCameraCenter.x !=
            assemblyInitial.assemblyCameraCenter.x ||
        placementOnly.assemblyCameraCenter.y !=
            assemblyInitial.assemblyCameraCenter.y ||
        placementOnly.assemblyCameraCenter.z !=
            assemblyInitial.assemblyCameraCenter.z);
  CHECK(placementOnly.assemblyCameraDepthExtent !=
        assemblyInitial.assemblyCameraDepthExtent);
  for (std::size_t index = 0; index < placementOnly.sources.size(); ++index) {
    CHECK(placementOnly.sources[index].key ==
          initialSourceDiagnostics[index].key);
    CHECK(placementOnly.sources[index].uploadGeneration ==
          initialSourceDiagnostics[index].uploadGeneration);
    CHECK(placementOnly.sources[index].contentFingerprint ==
          initialSourceDiagnostics[index].contentFingerprint);
  }

  // Equal numeric generations from another coordinator cannot authorize a
  // highlight in this scene. The guarded target is ignored and source uploads
  // remain untouched.
  auto foreignFixture = assemblyGpuFixture();
  solidar::AssemblyScenePublicationCoordinator foreignCoordinator;
  solidar::AssemblyViewportSceneAdapter foreignAdapter;
  solidar::AssemblyViewportPicking foreignPicking;
  CHECK(foreignCoordinator.rebuildAndPublish(
      *foreignFixture.product, solidar::ProductPresentationEpoch{501},
      normalProfile));
  CHECK(foreignAdapter.publish(foreignCoordinator.active()));
  CHECK(foreignPicking.publish(foreignAdapter.active()));
  CHECK(foreignCoordinator.rebuildAndPublish(
      *foreignFixture.product, solidar::ProductPresentationEpoch{501},
      normalProfile));
  CHECK(foreignAdapter.publish(foreignCoordinator.active()));
  CHECK(foreignPicking.publish(foreignAdapter.active()));
  CHECK(foreignAdapter.active()->generation() ==
        sceneAdapter.active()->generation());
  const auto foreignCamera = solidar::ViewportCameraState{
      0.0F, 0.0F, 1.0F, {}, QSize{320, 240}, 1.0F,
      foreignAdapter.active()->worldBounds()->center(),
      foreignAdapter.active()->worldBounds()->diagonal() * 3.0};
  const auto foreignHit = foreignPicking.faceAt(
      foreignCamera, foreignCamera.worldToScreen({5.0, 6.0, 14.0}));
  CHECK(foreignHit);
  const auto foreignTarget = foreignHit.hit->highlightTarget();
  CHECK(foreignTarget);
  CHECK(!foreignTarget->isCurrentFor(*sceneAdapter.active()));
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges, foreignTarget));
  const auto foreignIgnored = assemblyRenderer.resourceDiagnostics();
  CHECK(foreignIgnored.highlightedAssemblyInstances == 0);
  CHECK(foreignIgnored.sourceUploadAttempts ==
        placementOnly.sourceUploadAttempts);

  // A changed epoch creates two new source identities. Fail the second GPU
  // upload and prove that neither the first pending source nor a partial draw
  // generation is published.
  CHECK(assemblyCoordinator.rebuildAndPublish(
      *assemblyFixture.product, solidar::ProductPresentationEpoch{502},
      normalProfile));
  CHECK(sceneAdapter.publish(assemblyCoordinator.active()));
  solidar::ViewportRendererTestAdapter::failNthUploadAfterVertexBuffer(
      assemblyRenderer, 2);
  CHECK(!assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto failedAssembly = assemblyRenderer.resourceDiagnostics();
  CHECK(failedAssembly.sourceMeshes == 2);
  CHECK(failedAssembly.activePublicationGeneration ==
        placementOnly.activePublicationGeneration);
  CHECK(failedAssembly.sourceUploadFailures == 1);
  CHECK(failedAssembly.sourceDestroyCount ==
        placementOnly.sourceDestroyCount);
  CHECK(failedAssembly.sourcePruneCount == placementOnly.sourcePruneCount);
  CHECK(failedAssembly.activeAssemblyUsers ==
        placementOnly.activeAssemblyUsers);
  CHECK(failedAssembly.drawnAssemblyInstances ==
        placementOnly.drawnAssemblyInstances);
  CHECK(failedAssembly.drawnAssemblySurfacePasses ==
        placementOnly.drawnAssemblySurfacePasses);
  CHECK(failedAssembly.drawnAssemblyEdgePasses ==
        placementOnly.drawnAssemblyEdgePasses);
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto recoveredAssembly = assemblyRenderer.resourceDiagnostics();
  CHECK(recoveredAssembly.sourceMeshes == 2);
  CHECK(recoveredAssembly.activePublicationGeneration ==
        sceneAdapter.active()->generation().value());
  CHECK(recoveredAssembly.sourcePruneCount >= 2);
  CHECK(recoveredAssembly.sources.size() == 2);
  for (const auto& source : recoveredAssembly.sources) {
    CHECK(source.key.productEpoch == 502);
    CHECK(source.uploadGeneration >
          initialSourceDiagnostics.back().uploadGeneration);
    CHECK(source.contentFingerprint != 0);
  }

  const auto staleScene = sceneAdapter.active();

  // Four upload binds prepare the two new epoch sources. The fifth injected
  // bind is the first draw preflight and must fail before any surface/edge
  // pass or authoritative generation/resource mutation.
  CHECK(assemblyCoordinator.rebuildAndPublish(
      *assemblyFixture.product, solidar::ProductPresentationEpoch{503},
      normalProfile));
  CHECK(sceneAdapter.publish(assemblyCoordinator.active()));
  const auto beforeBindFailure = assemblyRenderer.resourceDiagnostics();
  solidar::ViewportRendererTestAdapter::failNthVaoBind(assemblyRenderer, 5);
  CHECK(!assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto afterBindFailure = assemblyRenderer.resourceDiagnostics();
  CHECK(afterBindFailure.sourceMeshes == beforeBindFailure.sourceMeshes);
  CHECK(afterBindFailure.activePublicationGeneration ==
        beforeBindFailure.activePublicationGeneration);
  CHECK(afterBindFailure.sourceDestroyCount ==
        beforeBindFailure.sourceDestroyCount);
  CHECK(afterBindFailure.sourcePruneCount == beforeBindFailure.sourcePruneCount);
  CHECK(afterBindFailure.activeAssemblyUsers ==
        beforeBindFailure.activeAssemblyUsers);
  CHECK(afterBindFailure.submittedAssemblyInstances ==
        beforeBindFailure.submittedAssemblyInstances);
  CHECK(afterBindFailure.drawnAssemblyInstances ==
        beforeBindFailure.drawnAssemblyInstances);
  CHECK(afterBindFailure.highlightedAssemblyInstances ==
        beforeBindFailure.highlightedAssemblyInstances);
  CHECK(afterBindFailure.drawnAssemblySurfacePasses ==
        beforeBindFailure.drawnAssemblySurfacePasses);
  CHECK(afterBindFailure.drawnAssemblyEdgePasses ==
        beforeBindFailure.drawnAssemblyEdgePasses);
  CHECK(afterBindFailure.sources.size() == beforeBindFailure.sources.size());
  for (std::size_t index = 0; index < afterBindFailure.sources.size(); ++index) {
    CHECK(afterBindFailure.sources[index].key ==
          beforeBindFailure.sources[index].key);
    CHECK(afterBindFailure.sources[index].uploadGeneration ==
          beforeBindFailure.sources[index].uploadGeneration);
    CHECK(afterBindFailure.sources[index].contentFingerprint ==
          beforeBindFailure.sources[index].contentFingerprint);
  }

  // A genuine GL error created after all uploads and preflight must be caught
  // by the distinct pre-draw gate. Pending slots are destroyed without being
  // counted as resident destruction and no pass/publication becomes visible.
  solidar::ViewportRendererTestAdapter::injectGlErrorAfterAssemblyPreflight(
      assemblyRenderer);
  CHECK(!assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  CHECK(assemblyRenderer.error() ==
        QStringLiteral("Assembly upload/preflight produced an OpenGL error"));
  const auto afterPreDrawGlError = assemblyRenderer.resourceDiagnostics();
  CHECK(afterPreDrawGlError.sourceMeshes == beforeBindFailure.sourceMeshes);
  CHECK(afterPreDrawGlError.activePublicationGeneration ==
        beforeBindFailure.activePublicationGeneration);
  CHECK(afterPreDrawGlError.sourceUploadAttempts ==
        afterBindFailure.sourceUploadAttempts + 2);
  CHECK(afterPreDrawGlError.sourceUploadSuccesses ==
        afterBindFailure.sourceUploadSuccesses + 2);
  CHECK(afterPreDrawGlError.sourceUploadFailures ==
        afterBindFailure.sourceUploadFailures);
  CHECK(afterPreDrawGlError.sourceDestroyCount ==
        beforeBindFailure.sourceDestroyCount);
  CHECK(afterPreDrawGlError.sourcePruneCount ==
        beforeBindFailure.sourcePruneCount);
  CHECK(afterPreDrawGlError.activeAssemblyUsers ==
        beforeBindFailure.activeAssemblyUsers);
  CHECK(afterPreDrawGlError.submittedAssemblyInstances ==
        beforeBindFailure.submittedAssemblyInstances);
  CHECK(afterPreDrawGlError.drawnAssemblyInstances ==
        beforeBindFailure.drawnAssemblyInstances);
  CHECK(afterPreDrawGlError.highlightedAssemblyInstances ==
        beforeBindFailure.highlightedAssemblyInstances);
  CHECK(afterPreDrawGlError.drawnAssemblySurfacePasses ==
        beforeBindFailure.drawnAssemblySurfacePasses);
  CHECK(afterPreDrawGlError.drawnAssemblyEdgePasses ==
        beforeBindFailure.drawnAssemblyEdgePasses);
  CHECK(afterPreDrawGlError.sources.size() ==
        beforeBindFailure.sources.size());
  for (std::size_t index = 0; index < afterPreDrawGlError.sources.size();
       ++index) {
    CHECK(afterPreDrawGlError.sources[index].key ==
          beforeBindFailure.sources[index].key);
    CHECK(afterPreDrawGlError.sources[index].uploadGeneration ==
          beforeBindFailure.sources[index].uploadGeneration);
    CHECK(afterPreDrawGlError.sources[index].contentFingerprint ==
          beforeBindFailure.sources[index].contentFingerprint);
  }

  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto activeB = assemblyRenderer.resourceDiagnostics();
  CHECK(activeB.sourceUploadAttempts ==
        afterPreDrawGlError.sourceUploadAttempts + 2);
  CHECK(activeB.sourceUploadSuccesses ==
        afterPreDrawGlError.sourceUploadSuccesses + 2);
  CHECK(activeB.activePublicationGeneration ==
        sceneAdapter.active()->generation().value());
  CHECK(activeB.activePublicationGeneration !=
        recoveredAssembly.activePublicationGeneration);

  // Context loss after staging/preflight cannot destroy VAO-owning wrappers
  // under the wrong context. The failed generation is absent from the
  // resident cache and retained only in renderer-owned deferred storage.
  CHECK(assemblyCoordinator.rebuildAndPublish(
      *assemblyFixture.product, solidar::ProductPresentationEpoch{504},
      normalProfile));
  CHECK(sceneAdapter.publish(assemblyCoordinator.active()));
  solidar::ViewportRendererTestAdapter::
      injectContextMismatchAfterAssemblyPreflight(assemblyRenderer);
  CHECK(!assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  CHECK(assemblyRenderer.error() ==
        QStringLiteral("OpenGL context changed during assembly preflight"));
  const auto afterPreflightContextMismatch =
      assemblyRenderer.resourceDiagnostics();
  CHECK(sameAuthoritativeAssemblyDiagnostics(
      activeB, afterPreflightContextMismatch));
  CHECK(afterPreflightContextMismatch.deferredSourceDestructions == 2);
  CHECK(afterPreflightContextMismatch.deferredSourceCleanupCount ==
        activeB.deferredSourceCleanupCount);
  for (const auto& source : afterPreflightContextMismatch.sources)
    CHECK(source.key.productEpoch == 503);

  // initialize() is a safe owner-context entry. It drains deferred wrappers
  // exactly once without changing the old authoritative assembly generation.
  CHECK(assemblyRenderer.initialize());
  const auto afterPreflightDeferredDrain =
      assemblyRenderer.resourceDiagnostics();
  CHECK(sameAuthoritativeAssemblyDiagnostics(
      activeB, afterPreflightDeferredDrain));
  CHECK(afterPreflightDeferredDrain.deferredSourceDestructions == 0);
  CHECK(afterPreflightDeferredDrain.deferredSourceCleanupCount ==
        activeB.deferredSourceCleanupCount + 2);
  CHECK(assemblyRenderer.initialize());
  CHECK(assemblyRenderer.resourceDiagnostics().deferredSourceCleanupCount ==
        afterPreflightDeferredDrain.deferredSourceCleanupCount);
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto activeAfterPreflightMismatch =
      assemblyRenderer.resourceDiagnostics();
  CHECK(activeAfterPreflightMismatch.activePublicationGeneration ==
        sceneAdapter.active()->generation().value());
  CHECK(activeAfterPreflightMismatch.deferredSourceDestructions == 0);
  CHECK(activeAfterPreflightMismatch.deferredSourceCleanupCount ==
        activeB.deferredSourceCleanupCount + 2);

  // Repeat at the stricter checkpoint after draw but before any post-draw GL
  // call or logical commit. The old generation/camera/draw diagnostics remain
  // authoritative and cleanup is again deferred to an exact-owner entry.
  CHECK(assemblyCoordinator.rebuildAndPublish(
      *assemblyFixture.product, solidar::ProductPresentationEpoch{505},
      normalProfile));
  CHECK(sceneAdapter.publish(assemblyCoordinator.active()));
  solidar::ViewportRendererTestAdapter::injectContextMismatchAfterAssemblyDraw(
      assemblyRenderer);
  CHECK(!assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  CHECK(assemblyRenderer.error() ==
        QStringLiteral("OpenGL context changed after assembly draw"));
  const auto afterDrawContextMismatch =
      assemblyRenderer.resourceDiagnostics();
  CHECK(sameAuthoritativeAssemblyDiagnostics(
      activeAfterPreflightMismatch, afterDrawContextMismatch));
  CHECK(afterDrawContextMismatch.deferredSourceDestructions == 2);
  CHECK(afterDrawContextMismatch.deferredSourceCleanupCount ==
        activeAfterPreflightMismatch.deferredSourceCleanupCount);
  for (const auto& source : afterDrawContextMismatch.sources)
    CHECK(source.key.productEpoch == 504);

  CHECK(assemblyRenderer.initialize());
  const auto afterDrawDeferredDrain = assemblyRenderer.resourceDiagnostics();
  CHECK(sameAuthoritativeAssemblyDiagnostics(
      activeAfterPreflightMismatch, afterDrawDeferredDrain));
  CHECK(afterDrawDeferredDrain.deferredSourceDestructions == 0);
  CHECK(afterDrawDeferredDrain.deferredSourceCleanupCount ==
        activeAfterPreflightMismatch.deferredSourceCleanupCount + 2);
  CHECK(assemblyRenderer.renderAssembly(
      sceneAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto activeAfterDrawMismatch = assemblyRenderer.resourceDiagnostics();
  CHECK(activeAfterDrawMismatch.activePublicationGeneration ==
        sceneAdapter.active()->generation().value());
  CHECK(activeAfterDrawMismatch.deferredSourceDestructions == 0);
  CHECK(activeAfterDrawMismatch.deferredSourceCleanupCount ==
        activeB.deferredSourceCleanupCount + 4);

  const QString errorBeforeStale = assemblyRenderer.error();
  CHECK(!assemblyRenderer.renderAssembly(
      staleScene, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F, {},
      solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto afterDelayedA = assemblyRenderer.resourceDiagnostics();
  CHECK(afterDelayedA.staleAssemblyRejections ==
        activeAfterDrawMismatch.staleAssemblyRejections + 1);
  CHECK(sameRendererDiagnostics(activeAfterDrawMismatch, afterDelayedA, true));
  CHECK(assemblyRenderer.error() == errorBeforeStale);

  // A valid empty publication is a committed empty generation and prunes the
  // retained GPU sources after the last visible user disappears.
  auto emptyProduct = emptyAssemblyGpuProduct();
  solidar::AssemblyScenePublicationCoordinator emptyCoordinator;
  solidar::AssemblyViewportSceneAdapter emptyAdapter;
  CHECK(emptyCoordinator.rebuildAndPublish(
      *emptyProduct, solidar::ProductPresentationEpoch{504}, normalProfile));
  CHECK(emptyAdapter.publish(emptyCoordinator.active()));
  CHECK(emptyAdapter.active()->sources().empty());
  CHECK(emptyAdapter.active()->instances().empty());
  CHECK(assemblyRenderer.renderAssembly(
      emptyAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto emptyAssembly = assemblyRenderer.resourceDiagnostics();
  CHECK(emptyAssembly.sourceMeshes == 0);
  CHECK(emptyAssembly.activeAssemblyUsers == 0);
  CHECK(emptyAssembly.drawnAssemblyInstances == 0);
  CHECK(!emptyAssembly.assemblyWorldBoundsValid);
  CHECK(emptyAssembly.assemblyCameraDepthExtent == 1.0);
  CHECK(emptyAssembly.activePublicationGeneration ==
        emptyAdapter.active()->generation().value());

  // The component path uses the same transactional instanced-frame core.
  // Root and two child instances publish six draws from four definition-local
  // sources; equal local body/feature IDs never alias across definitions.
  auto componentFixture = componentGpuFixture();
  solidar::ComponentScenePublicationCoordinator componentCoordinator;
  solidar::ComponentViewportSceneAdapter componentAdapter;
  CHECK(componentCoordinator.rebuildAndPublish(
      *componentFixture.product, solidar::ProductPresentationEpoch{601},
      normalProfile));
  CHECK(componentAdapter.publish(componentCoordinator.active()));
  const auto componentSceneA = componentAdapter.active();
  CHECK(componentSceneA->sources().size() == 4);
  CHECK(componentSceneA->instances().size() == 6);
  const auto firstChild = std::find_if(
      componentSceneA->instances().begin(), componentSceneA->instances().end(),
      [](const auto& instance) {
        const auto& path = instance.bodyIdentity.component.occurrencePath;
        return path && path->entries().back() == solidar::OccurrenceId{10};
      });
  const auto secondChild = std::find_if(
      componentSceneA->instances().begin(), componentSceneA->instances().end(),
      [](const auto& instance) {
        const auto& path = instance.bodyIdentity.component.occurrencePath;
        return path && path->entries().back() == solidar::OccurrenceId{11};
      });
  CHECK(firstChild != componentSceneA->instances().end());
  CHECK(secondChild != componentSceneA->instances().end());
  const auto selectedComponent =
      solidar::ComponentInstanceHighlightTarget::create(
          componentSceneA->publicationHandle(),
          firstChild->bodyIdentity.component);
  const auto hoveredComponent =
      solidar::ComponentInstanceHighlightTarget::create(
          componentSceneA->publicationHandle(),
          secondChild->bodyIdentity.component);
  CHECK(selectedComponent);
  CHECK(hoveredComponent);
  const auto attemptsBeforeComponent =
      assemblyRenderer.resourceDiagnostics().sourceUploadAttempts;
  CHECK(assemblyRenderer.renderComponent(
      componentSceneA, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges,
      selectedComponent, hoveredComponent));
  const auto componentInitial = assemblyRenderer.resourceDiagnostics();
  CHECK(componentInitial.activeInstancedSceneKind ==
        solidar::RendererInstancedSceneKind::Component);
  CHECK(componentInitial.activeAssemblyUsers == 0);
  CHECK(componentInitial.activeComponentUsers == 6);
  CHECK(componentInitial.submittedComponentInstances == 6);
  CHECK(componentInitial.drawnComponentInstances == 6);
  CHECK(componentInitial.highlightedComponentInstances == 4);
  CHECK(componentInitial.drawnComponentSurfacePasses == 6);
  CHECK(componentInitial.drawnComponentEdgePasses == 6);
  CHECK(componentInitial.sourceMeshes == 4);
  CHECK(componentInitial.sourceUploadAttempts == attemptsBeforeComponent + 4);
  CHECK(componentInitial.componentWorldBoundsValid);
  bool sawRootLocalBody = false;
  bool sawChildLocalBody = false;
  for (const auto& source : componentInitial.sources) {
    if (source.key.bodyId != 1) continue;
    sawRootLocalBody = sawRootLocalBody || source.key.definitionId == 300;
    sawChildLocalBody = sawChildLocalBody || source.key.definitionId == 200;
  }
  CHECK(sawRootLocalBody);
  CHECK(sawChildLocalBody);

  // Placement preview changes only per-instance transforms and aggregate
  // bounds. Both bodies owned by occurrence 10 move together, while the root
  // and shared sibling occurrence 11 remain in place; no definition-local GPU
  // source is uploaded again.
  auto placementPreview = solidar::ComponentPlacementPreview::create(
      componentSceneA->publicationHandle(),
      firstChild->bodyIdentity.component, {100.0, 20.0, -10.0}, 41);
  CHECK(placementPreview);
  CHECK(placementPreview->moves(firstChild->bodyIdentity.component));
  CHECK(!placementPreview->moves(secondChild->bodyIdentity.component));
  const auto expectedMovedBounds =
      previewBounds(*componentSceneA, *placementPreview);
  CHECK(expectedMovedBounds);
  const auto uploadsBeforePreview = componentInitial.sourceUploadSuccesses;
  CHECK(assemblyRenderer.renderComponent(
      componentSceneA, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges,
      selectedComponent, hoveredComponent, solidar::lightThemeColors(),
      &*placementPreview, 41));
  const auto previewFrame = assemblyRenderer.resourceDiagnostics();
  CHECK(previewFrame.sourceUploadSuccesses == uploadsBeforePreview);
  CHECK(previewFrame.sources.size() == componentInitial.sources.size());
  for (std::size_t index = 0; index < previewFrame.sources.size(); ++index) {
    CHECK(previewFrame.sources[index].key ==
          componentInitial.sources[index].key);
    CHECK(previewFrame.sources[index].uploadGeneration ==
          componentInitial.sources[index].uploadGeneration);
  }
  CHECK(previewFrame.componentCameraCenter.x ==
        expectedMovedBounds->center().x);
  CHECK(previewFrame.componentCameraCenter.y ==
        expectedMovedBounds->center().y);
  CHECK(previewFrame.componentCameraCenter.z ==
        expectedMovedBounds->center().z);
  CHECK(componentSceneA->worldBounds());
  CHECK(componentSceneA->worldBounds()->center().x ==
        componentInitial.componentCameraCenter.x);

  // A wrong ownership revision is an exact stale input: the successfully
  // drawn preview remains incumbent and no cache/bounds state is rewritten.
  const auto staleBeforePreview = previewFrame.staleComponentRejections;
  CHECK(!assemblyRenderer.renderComponent(
      componentSceneA, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges,
      selectedComponent, hoveredComponent, solidar::lightThemeColors(),
      &*placementPreview, 42));
  const auto rejectedPreview = assemblyRenderer.resourceDiagnostics();
  CHECK(rejectedPreview.staleComponentRejections ==
        staleBeforePreview + 1);
  CHECK(rejectedPreview.activeComponentPublicationGeneration ==
        previewFrame.activeComponentPublicationGeneration);
  CHECK(rejectedPreview.sourceUploadSuccesses ==
        previewFrame.sourceUploadSuccesses);
  CHECK(rejectedPreview.componentCameraCenter.x ==
        previewFrame.componentCameraCenter.x);
  CHECK(rejectedPreview.componentCameraCenter.y ==
        previewFrame.componentCameraCenter.y);
  CHECK(rejectedPreview.componentCameraCenter.z ==
        previewFrame.componentCameraCenter.z);

  // Removing preview returns to the immutable scene bounds without remeshing.
  CHECK(assemblyRenderer.renderComponent(
      componentSceneA, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges,
      selectedComponent, hoveredComponent));
  const auto previewCleared = assemblyRenderer.resourceDiagnostics();
  CHECK(previewCleared.sourceUploadSuccesses == uploadsBeforePreview);
  CHECK(previewCleared.componentCameraCenter.x ==
        componentSceneA->worldBounds()->center().x);
  CHECK(previewCleared.componentCameraCenter.y ==
        componentSceneA->worldBounds()->center().y);
  CHECK(previewCleared.componentCameraCenter.z ==
        componentSceneA->worldBounds()->center().z);

  // Placement-only publication keeps all four GPU source generations.
  const auto componentSourcesBeforePlacement = componentInitial.sources;
  moveComponentOccurrence(*componentFixture.product,
                          componentFixture.occurrences.front(), -24.0);
  CHECK(componentCoordinator.rebuildAndPublish(
      *componentFixture.product, solidar::ProductPresentationEpoch{601},
      normalProfile));
  CHECK(componentAdapter.publish(componentCoordinator.active()));
  CHECK(assemblyRenderer.renderComponent(
      componentAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges,
      selectedComponent, hoveredComponent));
  const auto componentPlacement = assemblyRenderer.resourceDiagnostics();
  CHECK(componentPlacement.sourceUploadAttempts ==
        componentInitial.sourceUploadAttempts);
  CHECK(componentPlacement.highlightedComponentInstances == 0);
  CHECK(componentPlacement.sources.size() ==
        componentSourcesBeforePlacement.size());
  for (std::size_t index = 0; index < componentPlacement.sources.size();
       ++index) {
    CHECK(componentPlacement.sources[index].key ==
          componentSourcesBeforePlacement[index].key);
    CHECK(componentPlacement.sources[index].uploadGeneration ==
          componentSourcesBeforePlacement[index].uploadGeneration);
  }

  // A foreign target with the same numeric generation cannot tint this scene.
  auto foreignComponentFixture = componentGpuFixture();
  solidar::ComponentScenePublicationCoordinator foreignComponentCoordinator;
  solidar::ComponentViewportSceneAdapter foreignComponentAdapter;
  do {
    CHECK(foreignComponentCoordinator.rebuildAndPublish(
        *foreignComponentFixture.product,
        solidar::ProductPresentationEpoch{601}, normalProfile));
    CHECK(foreignComponentAdapter.publish(
        foreignComponentCoordinator.active()));
  } while (foreignComponentCoordinator.active().generation().value() <
           componentCoordinator.active().generation().value());
  CHECK(foreignComponentCoordinator.active().generation() ==
        componentCoordinator.active().generation());
  const auto foreignComponentTarget =
      solidar::ComponentInstanceHighlightTarget::create(
          foreignComponentAdapter.active()->publicationHandle(),
          foreignComponentAdapter.active()
              ->instances()
              .front()
              .bodyIdentity.component);
  CHECK(foreignComponentTarget);
  CHECK(!foreignComponentTarget->isCurrentFor(*componentAdapter.active()));
  CHECK(assemblyRenderer.renderComponent(
      componentAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges,
      foreignComponentTarget));
  CHECK(assemblyRenderer.resourceDiagnostics()
            .highlightedComponentInstances == 0);
  const auto foreignPreviewInstance = std::find_if(
      foreignComponentAdapter.active()->instances().begin(),
      foreignComponentAdapter.active()->instances().end(),
      [](const auto& instance) {
        return !instance.bodyIdentity.component.rootInstance();
      });
  CHECK(foreignPreviewInstance !=
        foreignComponentAdapter.active()->instances().end());
  auto foreignPreview = solidar::ComponentPlacementPreview::create(
      foreignComponentAdapter.active()->publicationHandle(),
      foreignPreviewInstance->bodyIdentity.component, {12.0, 0.0, 0.0},
      99);
  CHECK(foreignPreview);
  const auto beforeForeignPreview = assemblyRenderer.resourceDiagnostics();
  CHECK(!assemblyRenderer.renderComponent(
      componentAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges,
      std::nullopt, std::nullopt, solidar::lightThemeColors(),
      &*foreignPreview, 99));
  const auto afterForeignPreview = assemblyRenderer.resourceDiagnostics();
  CHECK(afterForeignPreview.staleComponentRejections ==
        beforeForeignPreview.staleComponentRejections + 1);
  CHECK(afterForeignPreview.activeComponentPublicationGeneration ==
        beforeForeignPreview.activeComponentPublicationGeneration);
  CHECK(afterForeignPreview.sourceUploadSuccesses ==
        beforeForeignPreview.sourceUploadSuccesses);
  CHECK(afterForeignPreview.componentCameraCenter.x ==
        beforeForeignPreview.componentCameraCenter.x);
  CHECK(afterForeignPreview.componentCameraCenter.y ==
        beforeForeignPreview.componentCameraCenter.y);
  CHECK(afterForeignPreview.componentCameraCenter.z ==
        beforeForeignPreview.componentCameraCenter.z);

  // Both upload and draw-preparation failures preserve the exact incumbent
  // component frame and its source generations.
  const auto componentIncumbent = assemblyRenderer.resourceDiagnostics();
  const auto staleComponentScene = componentAdapter.active();
  CHECK(componentCoordinator.rebuildAndPublish(
      *componentFixture.product, solidar::ProductPresentationEpoch{602},
      normalProfile));
  CHECK(componentAdapter.publish(componentCoordinator.active()));
  solidar::ViewportRendererTestAdapter::failNthUploadAfterVertexBuffer(
      assemblyRenderer, 2);
  CHECK(!assemblyRenderer.renderComponent(
      componentAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  auto componentFailure = assemblyRenderer.resourceDiagnostics();
  CHECK(componentFailure.activeInstancedSceneKind ==
        solidar::RendererInstancedSceneKind::Component);
  CHECK(componentFailure.activeComponentPublicationGeneration ==
        componentIncumbent.activeComponentPublicationGeneration);
  CHECK(componentFailure.sourceMeshes == componentIncumbent.sourceMeshes);
  CHECK(componentFailure.sources.size() == componentIncumbent.sources.size());
  CHECK(assemblyRenderer.renderComponent(
      componentAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto componentRecovered = assemblyRenderer.resourceDiagnostics();
  solidar::ViewportRendererTestAdapter::failNthVaoBind(assemblyRenderer, 1);
  CHECK(!assemblyRenderer.renderComponent(
      componentAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  componentFailure = assemblyRenderer.resourceDiagnostics();
  CHECK(componentFailure.activeComponentPublicationGeneration ==
        componentRecovered.activeComponentPublicationGeneration);
  CHECK(componentFailure.sourceMeshes == componentRecovered.sourceMeshes);

  const auto staleComponentCount =
      componentFailure.staleComponentRejections;
  CHECK(!assemblyRenderer.renderComponent(
      staleComponentScene, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  componentFailure = assemblyRenderer.resourceDiagnostics();
  CHECK(componentFailure.staleComponentRejections ==
        staleComponentCount + 1);
  CHECK(componentFailure.activeComponentPublicationGeneration ==
        componentRecovered.activeComponentPublicationGeneration);

  // Successful frames atomically replace the other instanced scene kind.
  CHECK(assemblyRenderer.renderAssembly(
      emptyAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F,
      {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  CHECK(assemblyRenderer.resourceDiagnostics().activeInstancedSceneKind ==
        solidar::RendererInstancedSceneKind::LegacyAssembly);
  CHECK(assemblyRenderer.resourceDiagnostics().activeComponentUsers == 0);
  CHECK(assemblyRenderer.renderComponent(
      componentAdapter.active(), QSize(320, 240), 1.0F, -35.0F, 25.0F,
      1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges));
  CHECK(assemblyRenderer.resourceDiagnostics().activeInstancedSceneKind ==
        solidar::RendererInstancedSceneKind::Component);
  CHECK(assemblyRenderer.resourceDiagnostics().activeAssemblyUsers == 0);

  // Prefix matching is applied by the renderer as well: moving the direct
  // middle component also moves its nested leaf, but not the second shared
  // leaf instance under the root.
  auto nestedComponent = nestedComponentGpuProduct();
  solidar::ComponentScenePublicationCoordinator nestedCoordinator;
  solidar::ComponentViewportSceneAdapter nestedAdapter;
  CHECK(nestedCoordinator.rebuildAndPublish(
      *nestedComponent, solidar::ProductPresentationEpoch{604},
      normalProfile));
  CHECK(nestedAdapter.publish(nestedCoordinator.active()));
  const auto nestedScene = nestedAdapter.active();
  const solidar::ComponentInstanceAddress* middleAddress = nullptr;
  const solidar::ComponentInstanceAddress* nestedLeafAddress = nullptr;
  const solidar::ComponentInstanceAddress* siblingLeafAddress = nullptr;
  for (const auto& instance : nestedScene->instances()) {
    const auto& address = instance.bodyIdentity.component;
    if (!address.occurrencePath) continue;
    const auto& entries = address.occurrencePath->entries();
    if (entries.size() == 1 && entries[0] == solidar::OccurrenceId{10})
      middleAddress = &address;
    else if (entries.size() == 2 &&
             entries[0] == solidar::OccurrenceId{10} &&
             entries[1] == solidar::OccurrenceId{20})
      nestedLeafAddress = &address;
    else if (entries.size() == 1 &&
             entries[0] == solidar::OccurrenceId{11})
      siblingLeafAddress = &address;
  }
  CHECK(middleAddress && nestedLeafAddress && siblingLeafAddress);
  auto nestedPreview = solidar::ComponentPlacementPreview::create(
      nestedScene->publicationHandle(), *middleAddress,
      {75.0, -15.0, 5.0}, 77);
  CHECK(nestedPreview);
  CHECK(nestedPreview->moves(*nestedLeafAddress));
  CHECK(!nestedPreview->moves(*siblingLeafAddress));
  const auto expectedNestedBounds = previewBounds(*nestedScene, *nestedPreview);
  CHECK(expectedNestedBounds);
  CHECK(assemblyRenderer.renderComponent(
      nestedScene, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F, {},
      solidar::ViewportDisplayMode::ShadedWithEdges, std::nullopt,
      std::nullopt, solidar::lightThemeColors(), &*nestedPreview, 77));
  const auto nestedPreviewFrame = assemblyRenderer.resourceDiagnostics();
  CHECK(nestedPreviewFrame.componentCameraCenter.x ==
        expectedNestedBounds->center().x);
  CHECK(nestedPreviewFrame.componentCameraCenter.y ==
        expectedNestedBounds->center().y);
  CHECK(nestedPreviewFrame.componentCameraCenter.z ==
        expectedNestedBounds->center().z);
  const auto nestedUploads = nestedPreviewFrame.sourceUploadSuccesses;
  CHECK(assemblyRenderer.renderComponent(
      nestedScene, QSize(320, 240), 1.0F, -35.0F, 25.0F, 1.0F, {},
      solidar::ViewportDisplayMode::ShadedWithEdges, std::nullopt,
      std::nullopt, solidar::lightThemeColors(), &*nestedPreview, 77));
  CHECK(assemblyRenderer.resourceDiagnostics().sourceUploadSuccesses ==
        nestedUploads);

  // An active root without local bodies still owns the complete component
  // workspace. Its visible descendants define the camera bounds; using the
  // empty root document's unit depth would clip their surfaces into artifacts.
  auto emptyRootComponent = emptyRootComponentGpuProduct();
  solidar::ComponentScenePublicationCoordinator emptyRootCoordinator;
  solidar::ComponentViewportSceneAdapter emptyRootAdapter;
  CHECK(emptyRootCoordinator.rebuildAndPublish(
      *emptyRootComponent, solidar::ProductPresentationEpoch{605},
      normalProfile));
  CHECK(emptyRootAdapter.publish(emptyRootCoordinator.active()));
  const auto emptyRootScene = emptyRootAdapter.active();
  CHECK(emptyRootScene && emptyRootScene->worldBounds());
  CHECK(!emptyRootScene->instances().empty());
  const auto& childAddress =
      emptyRootScene->instances().front().bodyIdentity.component;
  const solidar::ComponentInstanceAddress emptyRootAddress{
      childAddress.epoch, childAddress.rootDefinitionId,
      childAddress.rootDefinitionId, std::nullopt};
  CHECK(emptyRootAddress.rootInstance());
  CHECK(assemblyRenderer.renderComponentEditContext(
      emptyRootScene, emptyRootAddress, QSize(320, 240), 1.0F, -35.0F,
      25.0F, 1.0F, {}, solidar::ViewportDisplayMode::ShadedWithEdges, {},
      1.0, false));
  const auto emptyRootFrame = assemblyRenderer.resourceDiagnostics();
  CHECK(emptyRootFrame.activeInstancedSceneKind ==
        solidar::RendererInstancedSceneKind::Component);
  CHECK(emptyRootFrame.drawnComponentInstances ==
        emptyRootScene->instances().size());
  CHECK(emptyRootFrame.componentWorldBoundsValid);
  CHECK(emptyRootFrame.componentCameraCenter.x ==
        emptyRootScene->worldBounds()->center().x);
  CHECK(emptyRootFrame.componentCameraCenter.y ==
        emptyRootScene->worldBounds()->center().y);
  CHECK(emptyRootFrame.componentCameraCenter.z ==
        emptyRootScene->worldBounds()->center().z);
  CHECK(emptyRootFrame.componentCameraDepthExtent ==
        std::max(1.0, emptyRootScene->worldBounds()->diagonal() * 3.0));

  auto emptyComponent = emptyComponentGpuProduct();
  solidar::ComponentScenePublicationCoordinator emptyComponentCoordinator;
  solidar::ComponentViewportSceneAdapter emptyComponentAdapter;
  CHECK(emptyComponentCoordinator.rebuildAndPublish(
      *emptyComponent, solidar::ProductPresentationEpoch{603},
      normalProfile));
  CHECK(emptyComponentAdapter.publish(emptyComponentCoordinator.active()));
  CHECK(assemblyRenderer.renderComponent(
      emptyComponentAdapter.active(), QSize(320, 240), 1.0F, -35.0F,
      25.0F, 1.0F, {},
      solidar::ViewportDisplayMode::ShadedWithEdges));
  const auto emptyComponentFrame = assemblyRenderer.resourceDiagnostics();
  CHECK(emptyComponentFrame.activeInstancedSceneKind ==
        solidar::RendererInstancedSceneKind::Component);
  CHECK(emptyComponentFrame.sourceMeshes == 0);
  CHECK(emptyComponentFrame.activeComponentUsers == 0);
  CHECK(!emptyComponentFrame.componentWorldBoundsValid);
  assemblyRenderer.release();
  }

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
  // Viewport cleanup may be requested by aboutToBeDestroyed and then again by
  // owner destruction. The second request must not touch released GPU state.
  renderer.release();
  CHECK(!renderer.resourceDiagnostics().initialized);
  // A share-group context is not an owner context because VAOs are not
  // shareable. Refusal must leave the renderer initialized and untouched.
  CHECK(renderer.initialize());
  renderer.render({instances.front()}, nullptr, QSize(320, 240), 1.0F,
                  -35.0F, 25.0F, 1.0F, {},
                  solidar::ViewportDisplayMode::ShadedWithEdges, {}, {}, {},
                  std::size_t(-1));
  CHECK(renderer.error().isEmpty());
  const auto beforeContextRefusal = renderer.resourceDiagnostics();
  const QString errorBeforeContextRefusal = renderer.error();
  CHECK(beforeContextRefusal.sourceMeshes == 1);

  // A missing current context is a pure teardown rejection. All wrappers and
  // authoritative diagnostics remain available for an owner-context retry.
  framebuffer.release();
  context.doneCurrent();
  renderer.release();
  const auto afterNoContextRefusal = renderer.resourceDiagnostics();
  CHECK(sameRendererDiagnostics(beforeContextRefusal,
                                afterNoContextRefusal));
  CHECK(renderer.error() == errorBeforeContextRefusal);
  CHECK(context.makeCurrent(&surface));
  CHECK(framebuffer.bind());

  {
    QOpenGLContext relatedContext;
    relatedContext.setFormat(surface.format());
    relatedContext.setShareContext(&context);
    CHECK(relatedContext.create());
    context.doneCurrent();
    CHECK(relatedContext.makeCurrent(&surface));
    renderer.release();
    const auto afterContextRefusal = renderer.resourceDiagnostics();
    CHECK(afterContextRefusal.initialized);
    CHECK(afterContextRefusal.sourceMeshes ==
          beforeContextRefusal.sourceMeshes);
    CHECK(afterContextRefusal.sources.size() == 1);
    CHECK(afterContextRefusal.sources[0].key ==
          beforeContextRefusal.sources[0].key);
    CHECK(afterContextRefusal.sources[0].uploadGeneration ==
          beforeContextRefusal.sources[0].uploadGeneration);
    CHECK(afterContextRefusal.sources[0].contentFingerprint ==
          beforeContextRefusal.sources[0].contentFingerprint);
    CHECK(sameRendererDiagnostics(beforeContextRefusal,
                                  afterContextRefusal));
    CHECK(renderer.error() == errorBeforeContextRefusal);
    relatedContext.doneCurrent();
    CHECK(context.makeCurrent(&surface));
  }
  CHECK(framebuffer.bind());
  renderer.release();
  CHECK(!renderer.resourceDiagnostics().initialized);
  }
  framebuffer.release();
  // Keep the owning context current through automatic destruction of the
  // framebuffer and renderer-side Qt wrappers. Production teardown follows
  // the same QOpenGLWidget aboutToBeDestroyed/current-context ordering.

  return EXIT_SUCCESS;
}

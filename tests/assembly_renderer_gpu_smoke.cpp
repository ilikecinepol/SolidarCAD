#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QThread>
#include <QWindow>

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AssemblyGpuSmokeClassification.h"
#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/AssemblyScenePublicationCoordinator.h"
#include "ui/AssemblyViewportScene.h"
#include "ui/AssemblyViewportFit.h"
#include "ui/AssemblyViewportPicking.h"
#include "ui/AssemblyViewportRuler.h"
#include "ui/ViewportRenderer.h"
#include "ui/ViewportCamera.h"

namespace {

using namespace solidar;
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

OccurrencePath path(OccurrenceId id) {
  auto made = OccurrencePath::create({id});
  if (!made) std::abort();
  return std::move(*made.path);
}

std::unique_ptr<ProductDocument> product(double scale,
                                         bool withOccurrences = true) {
  auto document = std::make_unique<Document>();
  Body& boxBody = document->addBody(1, "box body");
  boxBody.addFeature(std::make_unique<ImportedShapeFeature>(
      1, std::make_shared<const TopoDS_Shape>(
             BRepPrimAPI_MakeBox(10.0 * scale, 12.0 * scale,
                                 14.0 * scale)
                 .Shape()),
      "box"));
  Body& cylinderBody = document->addBody(2, "cylinder body");
  cylinderBody.addFeature(std::make_unique<ImportedShapeFeature>(
      2, std::make_shared<const TopoDS_Shape>(
             BRepPrimAPI_MakeCylinder(4.0 * scale, 13.0 * scale).Shape()),
      "cylinder"));
  if (!document->recompute()) std::abort();

  DefinitionRegistry definitions;
  if (definitions.insert(
          PartDefinition(DefinitionId{10}, std::move(document))) !=
      DefinitionInsertStatus::Inserted)
    std::abort();
  AssemblyDefinition root(DefinitionId{100});
  if (withOccurrences) {
    for (std::uint64_t ordinal = 0; ordinal < 3; ++ordinal) {
      auto placement = RigidTransform3d::create(
          {static_cast<double>(ordinal) * 24.0, 0.0, 0.0},
          ordinal == 2
              ? std::array<double, 4>{std::sqrt(0.5), 0.0, 0.0,
                                      std::sqrt(0.5)}
              : std::array<double, 4>{1.0, 0.0, 0.0, 0.0});
      if (!placement ||
          root.insert(ComponentOccurrence(OccurrenceId{1001 + ordinal},
                                          DefinitionId{10},
                                          *placement.transform)) !=
              AssemblyOccurrenceInsertStatus::Inserted)
        std::abort();
    }
  }
  if (definitions.insert(std::move(root)) !=
      DefinitionInsertStatus::Inserted)
    std::abort();
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{100});
  if (!made) std::abort();
  return std::move(made.product);
}

std::shared_ptr<const AssemblyViewportScene> publish(
    ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyScenePublicationCoordinator& coordinator,
    AssemblyViewportSceneAdapter& adapter) {
  const auto publication =
      coordinator.rebuildAndPublish(product, epoch, kNormal);
  if (!publication || !adapter.publish(coordinator.active())) return {};
  return adapter.active();
}

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(3, 0);
  format.setDepthBufferSize(24);
  QWindow window;
  window.setTitle(QStringLiteral("SolidarCAD A4c3 Assembly GPU Smoke"));
  window.setSurfaceType(QWindow::OpenGLSurface);
  window.setFormat(format);
  window.resize(960, 640);
  window.show();
  application.processEvents();

  QOpenGLContext context;
  context.setFormat(window.requestedFormat());
  if (!context.create() || !context.makeCurrent(&window)) {
    std::cerr << "CONTEXT=FAIL\n";
    return EXIT_FAILURE;
  }
  auto* gl = context.functions();
  const auto text = [gl](GLenum name) {
    const auto* value = gl->glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : "<unavailable>";
  };
  const std::string vendor = text(GL_VENDOR);
  const std::string rendererName = text(GL_RENDERER);
  const std::string version = text(GL_VERSION);
  const std::string platform =
      QGuiApplication::platformName().toStdString();
  std::cout << "GL_VENDOR=" << vendor << '\n'
            << "GL_RENDERER=" << rendererName << '\n'
            << "GL_VERSION=" << version << '\n'
            << "QT_PLATFORM=" << platform << '\n';
  const auto classification = solidar::test::classifyGpuSmokeRuntime(
      vendor, rendererName, version, platform);
  if (!classification.realGpu) {
    std::cerr << "REAL_GPU=FAIL reason=" << classification.reason << '\n';
    return EXIT_FAILURE;
  }
  bool passed = true;
  const auto present = [&application, &context, &window]() {
    context.swapBuffers(&window);
    application.processEvents();
    QThread::msleep(350);
    application.processEvents();
  };

  auto firstProduct = product(1.0);
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  auto scene = publish(*firstProduct, ProductPresentationEpoch{701},
                       coordinator, adapter);
  if (!scene) return EXIT_FAILURE;
  AssemblyViewportPicking picking;
  if (!picking.publish(scene)) return EXIT_FAILURE;
  ViewportRenderer renderer;
  const auto initialFit = fitAssemblyViewport(
      scene, window.size(), -35.0F, 25.0F);
  if (!initialFit || !initialFit.fit->fullyContained) return EXIT_FAILURE;
  const ViewportCameraState initialCamera{
      -35.0F, 25.0F, initialFit.fit->zoom, initialFit.fit->pan, window.size(),
      1.0F, initialFit.fit->center, initialFit.fit->depthExtent};
  const auto firstFace =
      picking.faceAt(initialCamera, initialCamera.worldToScreen({5, 6, 7}));
  const auto secondFace =
      picking.faceAt(initialCamera, initialCamera.worldToScreen({29, 6, 7}));
  if (!firstFace || !secondFace) return EXIT_FAILURE;
  const auto selectedTarget = firstFace.hit->highlightTarget();
  if (!selectedTarget) return EXIT_FAILURE;
  if (!renderer.renderAssembly(scene, window.size(), window.devicePixelRatio(),
                               -35.0F, 25.0F, initialFit.fit->zoom,
                               initialFit.fit->pan,
                                ViewportDisplayMode::ShadedWithEdges,
                                selectedTarget))
    return EXIT_FAILURE;
  present();
  auto diagnostics = renderer.resourceDiagnostics();
  const bool sharedMultiBody = diagnostics.sourceMeshes == 2 &&
                               diagnostics.drawnAssemblyInstances == 6;
  passed = passed && sharedMultiBody;
  std::cout << "SHARED_MULTI_BODY="
            << (sharedMultiBody ? "PASS" : "FAIL")
            << " sources=" << diagnostics.sourceMeshes
            << " draws=" << diagnostics.drawnAssemblyInstances << '\n';
  const bool oneOccurrenceTint =
      diagnostics.highlightedAssemblyInstances == 2;
  passed = passed && oneOccurrenceTint;
  std::cout << "ONE_OCCURRENCE_TINT="
            << (oneOccurrenceTint ? "PASS" : "FAIL")
            << '\n';
  const bool facePathIdentity =
      firstFace.hit->address.face == secondFace.hit->address.face &&
      firstFace.hit->address.occurrence !=
          secondFace.hit->address.occurrence;
  passed = passed && facePathIdentity;
  std::cout << "A4C2_FACE_PATH_IDENTITY="
            << (facePathIdentity ? "PASS" : "FAIL") << '\n';
  const auto firstRectangle = picking.facesInRectangle(
      initialCamera, initialCamera.worldToScreen({5, 6, 14}) - QPointF{6, 6},
      initialCamera.worldToScreen({5, 6, 14}) + QPointF{6, 6});
  const auto secondRectangle = picking.facesInRectangle(
      initialCamera, initialCamera.worldToScreen({29, 6, 14}) - QPointF{6, 6},
      initialCamera.worldToScreen({29, 6, 14}) + QPointF{6, 6});
  const auto findRectanglePath = [](const auto& result, OccurrenceId id) {
    return std::find_if(result.hits.begin(), result.hits.end(),
                        [id](const auto& hit) {
                          return hit.address.occurrence.path == path(id);
                        });
  };
  const auto firstRectangleHit =
      findRectanglePath(firstRectangle, OccurrenceId{1001});
  const auto secondRectangleHit =
      findRectanglePath(secondRectangle, OccurrenceId{1002});
  const bool rectanglePathIdentity =
      firstRectangle && secondRectangle &&
      firstRectangleHit != firstRectangle.hits.end() &&
      secondRectangleHit != secondRectangle.hits.end() &&
      firstRectangleHit->address.occurrence !=
          secondRectangleHit->address.occurrence;
  passed = passed && rectanglePathIdentity;
  std::cout << "A4C3_RECT_PATH_IDENTITY="
            << (rectanglePathIdentity ? "PASS" : "FAIL") << '\n';

  const auto firstSnap = picking.snapAt(
      initialCamera, initialCamera.worldToScreen({0, 0, 14}), 9.0);
  const auto secondSnap = picking.snapAt(
      initialCamera, initialCamera.worldToScreen({24, 0, 14}), 9.0);
  const bool snapWorld =
      firstSnap && secondSnap &&
      firstSnap.hit->kind == AssemblySnapKind::Vertex &&
      secondSnap.hit->kind == AssemblySnapKind::Vertex &&
      firstSnap.hit->occurrence.path == path(OccurrenceId{1001}) &&
      secondSnap.hit->occurrence.path == path(OccurrenceId{1002}) &&
      std::abs(firstSnap.hit->worldPoint.z - 14.0) < 1e-6 &&
      std::abs(secondSnap.hit->worldPoint.x - 24.0) < 1e-6;
  passed = passed && snapWorld;
  std::cout << "A4C3_SNAP_WORLD=" << (snapWorld ? "PASS" : "FAIL")
            << '\n';
  AssemblyViewportRuler ruler;
  const bool rulerWorld =
      firstSnap && secondSnap &&
      ruler.commit(firstSnap) ==
          AssemblyViewportRulerCommitStatus::AcceptedFirstPoint &&
      ruler.commit(secondSnap) ==
          AssemblyViewportRulerCommitStatus::AcceptedSecondPoint &&
      ruler.distance() && std::abs(*ruler.distance() - 24.0) < 1e-6;
  passed = passed && rulerWorld;
  std::cout << "A4C3_RULER_WORLD=" << (rulerWorld ? "PASS" : "FAIL")
            << '\n';
  bool dprInvariant = true;
  for (const float dpr : {1.0F, 1.25F, 1.5F, 2.0F}) {
    auto dprCamera = initialCamera;
    dprCamera.devicePixelRatio = dpr;
    const auto snapped = picking.snapAt(
        dprCamera, dprCamera.worldToScreen({0, 0, 14}), 9.0);
    dprInvariant = dprInvariant && snapped &&
                   snapped.hit->occurrence.path == path(OccurrenceId{1001}) &&
                   std::abs(snapped.hit->worldPoint.z - 14.0) < 1e-6;
  }
  passed = passed && dprInvariant;
  std::cout << "A4C3_LOGICAL_DPR_INVARIANT="
            << (dprInvariant ? "PASS" : "FAIL") << '\n';
  const auto initialEdge = picking.edgeAt(
      initialCamera, initialCamera.worldToScreen({0, 0, 14}), 9.0);
  const bool edgePick = static_cast<bool>(initialEdge);
  passed = passed && edgePick;
  std::cout << "A4C2_EDGE_PICK=" << (edgePick ? "PASS" : "FAIL")
            << '\n';
  const bool independentHighlight = oneOccurrenceTint &&
                                    diagnostics.drawnAssemblyInstances == 6;
  passed = passed && independentHighlight;
  std::cout << "A4C2_INDEPENDENT_HIGHLIGHT="
            << (independentHighlight ? "PASS" : "FAIL") << '\n';
  const bool transformedBounds = scene->worldBounds().has_value() &&
                                 diagnostics.assemblyWorldBoundsValid &&
                                 diagnostics.assemblyCameraDepthExtent ==
                                     scene->worldBounds()->diagonal() * 3.0;
  passed = passed && transformedBounds;
  std::cout << "TRANSFORMED_WORLD_BOUNDS="
            << (transformedBounds ? "PASS" : "FAIL") << '\n';
  const bool fitToView = initialFit.fit->fullyContained &&
                         initialFit.fit->generation == scene->generation();
  passed = passed && fitToView;
  std::cout << "FIT_TO_VIEW=" << (fitToView ? "PASS" : "FAIL") << '\n';

  auto overlap = RigidTransform3d::create({0.0, 0.0, 20.0},
                                          {1.0, 0.0, 0.0, 0.0});
  if (!overlap ||
      firstProduct->setRootOccurrencePlacement(path(OccurrenceId{1002}),
                                               *overlap.transform) !=
          ProductOccurrenceMutationStatus::Committed)
    return EXIT_FAILURE;
  scene = publish(*firstProduct, ProductPresentationEpoch{701}, coordinator,
                  adapter);
  if (!scene || !picking.publish(scene)) return EXIT_FAILURE;
  const auto overlapFit =
      fitAssemblyViewport(scene, window.size(), 0.0F, 0.0F);
  if (!overlapFit) return EXIT_FAILURE;
  const ViewportCameraState overlapCamera{
      0.0F, 0.0F, overlapFit.fit->zoom, overlapFit.fit->pan, window.size(),
      1.0F, overlapFit.fit->center, overlapFit.fit->depthExtent};
  const auto overlapHit = picking.faceAt(
      overlapCamera, overlapCamera.worldToScreen({5, 6, 34}));
  const bool frontmostOcclusion =
      overlapHit && overlapHit.hit->address.occurrence.path ==
                        path(OccurrenceId{1002});
  const auto overlapTarget =
      overlapHit ? overlapHit.hit->highlightTarget()
                 : std::optional<AssemblyOccurrenceHighlightTarget>{};
  if (!overlapTarget ||
      !renderer.renderAssembly(scene, window.size(), window.devicePixelRatio(),
                               0.0F, 0.0F, overlapFit.fit->zoom,
                               overlapFit.fit->pan,
                               ViewportDisplayMode::ShadedWithEdges,
                               overlapTarget))
    return EXIT_FAILURE;
  present();
  passed = passed && frontmostOcclusion;
  std::cout << "A4C2_FRONTMOST_OCCLUSION="
            << (frontmostOcclusion ? "PASS" : "FAIL") << '\n';
  const QPointF overlapPixel = overlapCamera.worldToScreen({5, 6, 34});
  const auto overlapRectangle = picking.facesInRectangle(
      overlapCamera, overlapPixel - QPointF{5, 5},
      overlapPixel + QPointF{5, 5});
  const bool rectangleOcclusion =
      overlapRectangle &&
      findRectanglePath(overlapRectangle, OccurrenceId{1002}) !=
          overlapRectangle.hits.end() &&
      findRectanglePath(overlapRectangle, OccurrenceId{1001}) ==
          overlapRectangle.hits.end();
  passed = passed && rectangleOcclusion;
  std::cout << "A4C3_RECT_OCCLUSION="
            << (rectangleOcclusion ? "PASS" : "FAIL") << '\n';

  diagnostics = renderer.resourceDiagnostics();
  const auto uploadsBeforePlacement = diagnostics.sourceUploadAttempts;
  const auto topologyBuildsBeforePlacement =
      picking.diagnostics().topologySourceBuildAttempts;
  auto moved = RigidTransform3d::create({-35.0, 8.0, 5.0},
                                        {1.0, 0.0, 0.0, 0.0});
  if (!moved ||
      firstProduct->setRootOccurrencePlacement(path(OccurrenceId{1002}),
                                               *moved.transform) !=
          ProductOccurrenceMutationStatus::Committed)
    return EXIT_FAILURE;
  scene = publish(*firstProduct, ProductPresentationEpoch{701}, coordinator,
                  adapter);
  if (!scene || !picking.publish(scene)) return EXIT_FAILURE;
  const auto placementFit = fitAssemblyViewport(
      scene, window.size(), 55.0F, -20.0F);
  if (!scene ||
      !placementFit ||
      !renderer.renderAssembly(scene, window.size(), window.devicePixelRatio(),
                               55.0F, -20.0F, placementFit.fit->zoom,
                               placementFit.fit->pan,
                               ViewportDisplayMode::ShadedWithEdges))
    return EXIT_FAILURE;
  present();
  diagnostics = renderer.resourceDiagnostics();
  const ViewportCameraState placementCamera{
      55.0F, -20.0F, placementFit.fit->zoom, placementFit.fit->pan,
      window.size(), 1.0F, placementFit.fit->center,
      placementFit.fit->depthExtent};
  const auto movedPick = picking.faceAt(
      placementCamera, placementCamera.worldToScreen({-30, 14, 12}));
  const bool orbitZoomPick =
      movedPick && movedPick.hit->address.occurrence.path ==
                       path(OccurrenceId{1002});
  passed = passed && orbitZoomPick;
  std::cout << "A4C2_ORBIT_ZOOM_PICK="
            << (orbitZoomPick ? "PASS" : "FAIL") << '\n';
  const QPointF movedPixel = placementCamera.worldToScreen({-30, 14, 12});
  const auto movedRectangle = picking.facesInRectangle(
      placementCamera, movedPixel - QPointF{7, 7},
      movedPixel + QPointF{7, 7});
  const auto movedSnap = picking.snapAt(placementCamera, movedPixel, 9.0);
  const bool placementSpatial =
      movedRectangle &&
      findRectanglePath(movedRectangle, OccurrenceId{1002}) !=
          movedRectangle.hits.end() &&
      movedSnap && movedSnap.hit->occurrence.path == path(OccurrenceId{1002});
  const bool placementReuse =
      diagnostics.sourceUploadAttempts == uploadsBeforePlacement;
  passed = passed && placementReuse;
  std::cout << "PLACEMENT_ORBIT_ZOOM_NO_UPLOAD="
            << (placementReuse ? "PASS" : "FAIL")
            << '\n';
  const bool placementNoRemeshUpload =
      placementReuse && picking.diagnostics().topologySourceBuildAttempts ==
                            topologyBuildsBeforePlacement;
  passed = passed && placementNoRemeshUpload;
  std::cout << "A4C2_PLACEMENT_NO_REMESH_UPLOAD="
            << (placementNoRemeshUpload ? "PASS" : "FAIL") << '\n';
  const bool a4c3PlacementNoRemeshUpload =
      placementNoRemeshUpload && placementSpatial;
  passed = passed && a4c3PlacementNoRemeshUpload;
  std::cout << "A4C3_PLACEMENT_NO_REMESH_UPLOAD="
            << (a4c3PlacementNoRemeshUpload ? "PASS" : "FAIL") << '\n';
  const bool placementFitNoUpload = placementReuse &&
                                    placementFit.fit->fullyContained &&
                                    (placementFit.fit->pan !=
                                         initialFit.fit->pan ||
                                     placementFit.fit->zoom !=
                                         initialFit.fit->zoom);
  passed = passed && placementFitNoUpload;
  std::cout << "PLACEMENT_FIT_NO_UPLOAD="
            << (placementFitNoUpload ? "PASS" : "FAIL") << '\n';

  const double visibleMaximumX = scene->worldBounds()->maximum().x;
  if (firstProduct->setRootOccurrenceFlags(
          path(OccurrenceId{1003}), OccurrenceFlags{true, true, false}) !=
      ProductOccurrenceMutationStatus::Committed)
    return EXIT_FAILURE;
  scene = publish(*firstProduct, ProductPresentationEpoch{701}, coordinator,
                  adapter);
  if (!scene || !picking.publish(scene)) return EXIT_FAILURE;
  const auto hiddenFit = fitAssemblyViewport(
      scene, window.size(), 55.0F, -20.0F);
  if (!scene || !hiddenFit ||
      !renderer.renderAssembly(scene, window.size(), window.devicePixelRatio(),
                               55.0F, -20.0F, hiddenFit.fit->zoom,
                               hiddenFit.fit->pan,
                               ViewportDisplayMode::ShadedWithEdges))
    return EXIT_FAILURE;
  present();
  const bool hiddenSuppressedBounds = scene->instances().size() == 4 &&
                                      scene->worldBounds() &&
                                      scene->worldBounds()->maximum().x <
                                          visibleMaximumX;
  passed = passed && hiddenSuppressedBounds;
  std::cout << "HIDDEN_SUPPRESSED_BOUNDS="
            << (hiddenSuppressedBounds ? "PASS" : "FAIL") << '\n';
  bool hiddenAbsent = true;
  for (const auto& instance : scene->instances())
    hiddenAbsent = hiddenAbsent &&
                   instance.occurrenceIdentity.path != path(OccurrenceId{1003});
  const ViewportCameraState hiddenCamera{
      55.0F, -20.0F, hiddenFit.fit->zoom, hiddenFit.fit->pan, window.size(),
      1.0F, hiddenFit.fit->center, hiddenFit.fit->depthExtent};
  const bool hiddenSuppressedExcluded =
      hiddenAbsent &&
      picking.faceAt(hiddenCamera, hiddenCamera.worldToScreen({42, 5, 7}))
              .status == AssemblyViewportPickingQueryStatus::Miss;
  passed = passed && hiddenSuppressedExcluded;
  std::cout << "A4C2_HIDDEN_SUPPRESSED_EXCLUDED="
            << (hiddenSuppressedExcluded ? "PASS" : "FAIL") << '\n';
  const QPointF hiddenPixel = hiddenCamera.worldToScreen({42, 5, 7});
  const auto hiddenRectangle = picking.facesInRectangle(
      hiddenCamera, hiddenPixel - QPointF{3, 3},
      hiddenPixel + QPointF{3, 3});
  const auto hiddenSnap = picking.snapAt(hiddenCamera, hiddenPixel, 2.0);
  const auto evidenceStatus = [](AssemblyViewportPickingQueryStatus status) {
    if (status == AssemblyViewportPickingQueryStatus::Hit)
      return solidar::test::SpatialEvidenceStatus::Hit;
    if (status == AssemblyViewportPickingQueryStatus::Miss)
      return solidar::test::SpatialEvidenceStatus::Miss;
    return solidar::test::SpatialEvidenceStatus::Other;
  };
  const bool rectangleHitsCurrent =
      !hiddenRectangle.hits.empty() &&
      std::all_of(hiddenRectangle.hits.begin(), hiddenRectangle.hits.end(),
                  [&scene](const auto& hit) {
                    return scene && hit.publication.isSameCurrentPublication(
                                        scene->publicationHandle());
                  });
  const bool rectangleExcludesHidden = std::none_of(
      hiddenRectangle.hits.begin(), hiddenRectangle.hits.end(),
      [](const auto& hit) {
        return hit.address.occurrence.path == path(OccurrenceId{1003});
      });
  const bool snapHitCurrent =
      hiddenSnap.hit && scene &&
      hiddenSnap.hit->publication.isSameCurrentPublication(
          scene->publicationHandle());
  const bool snapExcludesHidden =
      hiddenSnap.hit && hiddenSnap.hit->occurrence.path !=
                            path(OccurrenceId{1003});
  const bool a4c3HiddenSuppressedExcluded =
      hiddenAbsent &&
      solidar::test::validA4c3HiddenSuppressedEvidence(
          scene && scene->isCurrent(), evidenceStatus(hiddenRectangle.status),
          rectangleHitsCurrent, rectangleExcludesHidden,
          evidenceStatus(hiddenSnap.status), snapHitCurrent,
          snapExcludesHidden);
  passed = passed && a4c3HiddenSuppressedExcluded;
  std::cout << "A4C3_HIDDEN_SUPPRESSED_EXCLUDED="
            << (a4c3HiddenSuppressedExcluded ? "PASS" : "FAIL") << '\n';

  auto changedGeometry = product(1.7);
  AssemblyScenePublicationCoordinator changedCoordinator;
  scene = publish(*changedGeometry, ProductPresentationEpoch{702},
                  changedCoordinator, adapter);
  if (!scene || !picking.publish(scene)) return EXIT_FAILURE;
  if (!scene ||
      !renderer.renderAssembly(scene, window.size(), window.devicePixelRatio(),
                               -15.0F, 35.0F, 0.9F, {},
                               ViewportDisplayMode::ShadedWithEdges))
    return EXIT_FAILURE;
  const auto changedScene = scene;
  present();
  diagnostics = renderer.resourceDiagnostics();
  const bool epochReupload =
      diagnostics.sourceUploadAttempts >= uploadsBeforePlacement + 2;
  passed = passed && epochReupload;
  std::cout << "EPOCH_GEOMETRY_REUPLOAD="
            << (epochReupload ? "PASS" : "FAIL")
            << '\n';

  auto emptyProduct = product(1.0, false);
  AssemblyScenePublicationCoordinator emptyCoordinator;
  scene = publish(*emptyProduct, ProductPresentationEpoch{703},
                  emptyCoordinator, adapter);
  if (!scene || !picking.publish(scene)) return EXIT_FAILURE;
  if (!scene ||
      !renderer.renderAssembly(scene, window.size(), window.devicePixelRatio(),
                               0.0F, 0.0F, 1.0F, {},
                               ViewportDisplayMode::ShadedWithEdges))
    return EXIT_FAILURE;
  diagnostics = renderer.resourceDiagnostics();
  const bool lastUserPrune = diagnostics.sourceMeshes == 0 &&
                             diagnostics.activeAssemblyUsers == 0;
  passed = passed && lastUserPrune;
  std::cout << "LAST_USER_PRUNE="
            << (lastUserPrune ? "PASS" : "FAIL")
            << '\n';

  renderer.release();
  context.doneCurrent();
  QOpenGLContext recreated;
  recreated.setFormat(window.requestedFormat());
  if (!recreated.create() || !recreated.makeCurrent(&window) ||
      !renderer.initialize() ||
      !renderer.renderAssembly(changedScene, window.size(),
                               window.devicePixelRatio(), 20.0F, 10.0F, 1.0F,
                               {}, ViewportDisplayMode::ShadedWithEdges))
    return EXIT_FAILURE;
  recreated.swapBuffers(&window);
  application.processEvents();
  QThread::msleep(350);
  std::cout << "CONTEXT_TEARDOWN_RECREATE=PASS\n";
  renderer.release();
  recreated.doneCurrent();
  if (passed)
    std::cout << "REAL_GPU=PASS\n";
  else
    std::cerr << "REAL_GPU=FAIL reason=smoke checklist failure\n";
  std::cout << "A4B_GPU_SMOKE=" << (passed ? "PASS" : "FAIL") << '\n';
  std::cout << "A4C1_GPU_SMOKE=" << (passed ? "PASS" : "FAIL") << '\n';
  std::cout << "A4C2_GPU_SMOKE=" << (passed ? "PASS" : "FAIL") << '\n';
  std::cout << "A4C3_GPU_SMOKE=" << (passed ? "PASS" : "FAIL") << '\n';
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

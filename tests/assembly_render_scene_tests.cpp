#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdlib>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <tuple>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/AssemblyScenePublicationCoordinator.h"
#include "ui/AssemblyViewportScene.h"
#include "ui/AssemblyViewportFit.h"
#include "ui/RigidTransform3dOps.h"
#include "ui/ViewportCamera.h"
#include "ui/ViewportRenderer.h"

namespace solidar {

class AssemblyViewportSceneAdapterTestAccess final {
 public:
  static void failNthNewMesh(AssemblyViewportSceneAdapter& adapter,
                             std::size_t ordinal) noexcept {
    adapter.failMeshBuildCountdown_ = ordinal;
  }
};

}  // namespace solidar

namespace {

using namespace solidar;

constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

ShapeFeature::ShapePtr box(double x, double y, double z) {
  TopoDS_Shape shape = BRepPrimAPI_MakeBox(x, y, z).Shape();
  BRepTools::Clean(shape);
  return std::make_shared<const TopoDS_Shape>(std::move(shape));
}

bool hasTriangulation(const TopoDS_Shape& shape) {
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    TopLoc_Location location;
    if (!BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location)
             .IsNull())
      return true;
  }
  return false;
}

OccurrencePath path(OccurrenceId id) {
  auto made = OccurrencePath::create({id});
  CHECK(made);
  return std::move(*made.path);
}

RigidTransform3d placement(double x, double y, double z) {
  auto made = RigidTransform3d::create({x, y, z}, {1.0, 0.0, 0.0, 0.0});
  CHECK(made);
  return *made.transform;
}

RigidTransform3d z90(double x, double y, double z) {
  const double half = std::sqrt(0.5);
  auto made = RigidTransform3d::create({x, y, z}, {half, 0.0, 0.0, half});
  CHECK(made);
  return *made.transform;
}

bool near(double left, double right, double tolerance = 2e-5) {
  return std::abs(left - right) <= tolerance;
}

bool samePoint(Point3d left, Point3d right, double tolerance = 2e-5) {
  return near(left.x, right.x, tolerance) &&
         near(left.y, right.y, tolerance) &&
         near(left.z, right.z, tolerance);
}

void checkFitContains(const AssemblyViewportScene& scene,
                      const AssemblyViewportFit& fit, const QSize& size,
                      float yaw, float pitch) {
  CHECK(scene.worldBounds());
  const ViewportCameraState camera{yaw, pitch, fit.zoom, fit.pan, size, 1.0F,
                                   fit.center, fit.depthExtent};
  const double anchorX = size.width() * 0.5;
  const double anchorY = size.height() * 0.52;
  const double halfContentX =
      size.width() * kAssemblyFitContentFraction * 0.5;
  const double halfContentY =
      size.height() * kAssemblyFitContentFraction * 0.5;
  for (const Point3d corner : scene.worldBounds()->corners()) {
    const QPointF screen = camera.worldToScreen(corner);
    CHECK(screen.x() >= anchorX - halfContentX - 1e-3);
    CHECK(screen.x() <= anchorX + halfContentX + 1e-3);
    CHECK(screen.y() >= anchorY - halfContentY - 1e-3);
    CHECK(screen.y() <= anchorY + halfContentY + 1e-3);
  }
}

struct ProductFixture final {
  std::unique_ptr<ProductDocument> product;
  Body* secondBody{};
  ShapeFeature::ShapePtr firstShape;
  ShapeFeature::ShapePtr secondShape;
};

ProductFixture threeOccurrenceTwoBodyProduct() {
  ProductFixture fixture;
  fixture.firstShape = box(10.0, 20.0, 30.0);
  fixture.secondShape = box(5.0, 7.0, 11.0);
  auto document = std::make_unique<Document>();
  Body& firstBody = document->addBody(11, "first");
  firstBody.addFeature(std::make_unique<ImportedShapeFeature>(
      101, fixture.firstShape, "first feature"));
  fixture.secondBody = &document->addBody(12, "second");
  fixture.secondBody->addFeature(std::make_unique<ImportedShapeFeature>(
      102, fixture.secondShape, "second feature"));
  CHECK(document->recompute());

  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(DefinitionId{10},
                                          std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  for (std::uint64_t ordinal = 1; ordinal <= 3; ++ordinal) {
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{1000 + ordinal}, DefinitionId{10},
              placement(static_cast<double>(ordinal) * 20.0, 0.0, 0.0))) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  }
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{100});
  CHECK(made);
  fixture.product = std::move(made.product);
  return fixture;
}

std::unique_ptr<ProductDocument> emptyAssemblyProduct() {
  DefinitionRegistry definitions;
  AssemblyDefinition root(DefinitionId{900});
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{900});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> tinyAssemblyProduct(double extent) {
  auto document = std::make_unique<Document>();
  Body& body = document->addBody(1, "tiny");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      1, box(extent, extent * 2.0, extent * 3.0), "tiny feature"));
  CHECK(document->recompute());
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(DefinitionId{1},
                                          std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{2});
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{1}, DefinitionId{1},
                                        RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{2});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> duplicateLocalIdsAcrossDefinitions() {
  DefinitionRegistry definitions;
  for (const auto [definition, extent] :
       {std::pair{20ULL, 4.0}, std::pair{30ULL, 9.0}}) {
    auto document = std::make_unique<Document>();
    Body& body = document->addBody(1, "same local body");
    body.addFeature(std::make_unique<ImportedShapeFeature>(
        1, box(extent, extent + 1.0, extent + 2.0), "same local feature"));
    CHECK(document->recompute());
    CHECK(definitions.insert(
              PartDefinition(DefinitionId{definition}, std::move(document))) ==
          DefinitionInsertStatus::Inserted);
  }
  AssemblyDefinition root(DefinitionId{200});
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{1}, DefinitionId{20},
                                        RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{2}, DefinitionId{30},
                                        RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{200});
  CHECK(made);
  return std::move(made.product);
}

}  // namespace

int main() {
  auto fixture = threeOccurrenceTwoBodyProduct();
  CHECK(!hasTriangulation(*fixture.firstShape));
  CHECK(!hasTriangulation(*fixture.secondShape));

  AssemblyScenePublicationCoordinator coordinator;
  auto published = coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{41}, kNormal);
  CHECK(published);

  AssemblyViewportSceneAdapter adapter;
  const auto initial = adapter.publish(coordinator.active());
  CHECK(initial);
  const auto initialScene = adapter.active();
  CHECK(initialScene);
  CHECK(initialScene->sources().size() == 2);
  CHECK(initialScene->instances().size() == 6);
  CHECK(initialScene->worldBounds());
  CHECK(samePoint(initialScene->worldBounds()->minimum(),
                  {20.0, 0.0, 0.0}));
  CHECK(samePoint(initialScene->worldBounds()->maximum(),
                  {70.0, 20.0, 30.0}));
  CHECK(samePoint(initialScene->instances()[0].worldBounds.minimum(),
                  {20.0, 0.0, 0.0}));
  CHECK(samePoint(initialScene->instances()[0].worldBounds.maximum(),
                  {30.0, 20.0, 30.0}));
  CHECK(initialScene->instances()[0].mesh);
  CHECK(!hasTriangulation(*fixture.firstShape));
  CHECK(!hasTriangulation(*fixture.secondShape));
  const auto initialDiagnostics = adapter.diagnostics();
  CHECK(initialDiagnostics.meshBuildAttempts == 2);
  CHECK(initialDiagnostics.meshBuildSuccesses == 2);
  CHECK(initialDiagnostics.residentSources == 2);
  const auto initialFit = fitAssemblyViewport(
      initialScene, QSize{960, 640}, -35.0F, 25.0F);
  CHECK(initialFit);

  std::vector<std::shared_ptr<const BodyRenderMesh>> originalMeshes;
  for (const auto& source : initialScene->sources())
    originalMeshes.push_back(source.mesh);
  CHECK(fixture.product->setRootOccurrencePlacement(
            path(OccurrenceId{1002}), placement(-17.0, 6.0, 3.0)) ==
        ProductOccurrenceMutationStatus::Committed);
  published = coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{41}, kNormal);
  CHECK(published);
  CHECK(adapter.publish(coordinator.active()));
  CHECK(adapter.active()->sources().size() == 2);
  CHECK(adapter.active()->instances().size() == 6);
  CHECK(adapter.diagnostics().meshBuildAttempts ==
        initialDiagnostics.meshBuildAttempts);
  for (std::size_t i = 0; i < originalMeshes.size(); ++i)
    CHECK(adapter.active()->sources()[i].mesh == originalMeshes[i]);
  CHECK(adapter.active()->worldBounds());
  CHECK(samePoint(adapter.active()->worldBounds()->minimum(),
                  {-17.0, 0.0, 0.0}));
  CHECK(samePoint(adapter.active()->worldBounds()->maximum(),
                  {70.0, 26.0, 33.0}));
  const auto placementFit = fitAssemblyViewport(
      adapter.active(), QSize{960, 640}, -35.0F, 25.0F);
  CHECK(placementFit);
  CHECK(placementFit.fit->pan != initialFit.fit->pan ||
        placementFit.fit->zoom != initialFit.fit->zoom);

  // Quaternion convention golden: WXYZ +90 degrees around Z maps the local
  // +X axis to world +Y in both the double CPU helper and renderer matrix.
  const RigidTransform3d rotated = z90(7.0, 11.0, 13.0);
  const auto transformed = transformPoint(rotated, {10.0, 0.0, 0.0});
  CHECK(transformed);
  CHECK(samePoint(*transformed, {7.0, 21.0, 13.0}, 1e-9));
  const auto renderMatrix = ViewportRenderer::modelMatrix(rotated);
  CHECK(renderMatrix);
  const QVector4D rendered =
      *renderMatrix * QVector4D(10.0F, 0.0F, 0.0F, 1.0F);
  CHECK(near(rendered.x(), 7.0, 1e-5));
  CHECK(near(rendered.y(), 21.0, 1e-5));
  CHECK(near(rendered.z(), 13.0, 1e-5));

  CHECK(fixture.product->setRootOccurrencePlacement(
            path(OccurrenceId{1001}), rotated) ==
        ProductOccurrenceMutationStatus::Committed);
  auto arbitrary = RigidTransform3d::create(
      {-9.0, 4.0, 23.0}, {0.35, -0.2, 0.8, 0.42});
  CHECK(arbitrary);
  CHECK(fixture.product->setRootOccurrencePlacement(
            path(OccurrenceId{1003}), *arbitrary.transform) ==
        ProductOccurrenceMutationStatus::Committed);
  CHECK(coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{41}, kNormal));
  CHECK(adapter.publish(coordinator.active()));
  const auto transformedScene = adapter.active();
  CHECK(transformedScene->instances().size() == 6);
  CHECK(transformedScene->worldBounds());
  // First body of the Z90 occurrence: [0,10]x[0,20]x[0,30] becomes
  // [-20,0]x[0,10]x[0,30], then receives the translation.
  CHECK(samePoint(transformedScene->instances()[0].worldBounds.minimum(),
                  {-13.0, 11.0, 13.0}));
  CHECK(samePoint(transformedScene->instances()[0].worldBounds.maximum(),
                  {7.0, 21.0, 43.0}));
  std::optional<Bounds3d> independentAggregate;
  for (const auto& instance : transformedScene->instances())
    independentAggregate = independentAggregate
                               ? independentAggregate->united(instance.worldBounds)
                               : std::optional<Bounds3d>{instance.worldBounds};
  CHECK(independentAggregate);
  CHECK(samePoint(transformedScene->worldBounds()->minimum(),
                  independentAggregate->minimum()));
  CHECK(samePoint(transformedScene->worldBounds()->maximum(),
                  independentAggregate->maximum()));

  // Pure Fit is deterministic in logical pixels for wide/tall viewports and
  // distinct orientations. The fixed anchor is (w/2,h*0.52), and DPR never
  // enters either the helper or ViewportCameraState projection.
  for (const auto& fitCase :
       {std::tuple{QSize{1200, 500}, -35.0F, 25.0F},
        std::tuple{QSize{420, 1000}, 55.0F, -20.0F},
        std::tuple{QSize{777, 555}, 0.0F, 90.0F}}) {
    const auto& [size, yaw, pitch] = fitCase;
    const auto fit = fitAssemblyViewport(transformedScene, size, yaw, pitch);
    CHECK(fit);
    CHECK(fit.fit->generation == transformedScene->generation());
    CHECK(fit.fit->fullyContained);
    CHECK(!fit.fit->minimumZoomClamped);
    checkFitContains(*transformedScene, *fit.fit, size, yaw, pitch);
    const auto repeated =
        fitAssemblyViewport(transformedScene, size, yaw, pitch);
    CHECK(repeated);
    CHECK(repeated.fit->zoom == fit.fit->zoom);
    CHECK(repeated.fit->pan == fit.fit->pan);
    CHECK(samePoint(repeated.fit->center, fit.fit->center, 0.0));
    CHECK(repeated.fit->depthExtent == fit.fit->depthExtent);

    const ViewportCameraState dprOne{
        yaw, pitch, fit.fit->zoom, fit.fit->pan, size, 1.0F,
        fit.fit->center, fit.fit->depthExtent};
    const ViewportCameraState dprTwo{
        yaw, pitch, fit.fit->zoom, fit.fit->pan, size, 2.0F,
        fit.fit->center, fit.fit->depthExtent};
    CHECK(dprOne.worldToScreen(fit.fit->center) ==
          dprTwo.worldToScreen(fit.fit->center));
    const QPointF anchor = dprOne.worldToScreen(fit.fit->center);
    CHECK(near(anchor.x(), size.width() * 0.5, 1e-3));
    CHECK(near(anchor.y(), size.height() * 0.52, 1e-3));
  }
  CHECK(fitAssemblyViewport(transformedScene, QSize{}, 0.0F, 0.0F).status ==
        AssemblyViewportFitStatus::InvalidInput);
  CHECK(fitAssemblyViewport(
            transformedScene, QSize{800, 600},
            std::numeric_limits<float>::quiet_NaN(), 0.0F)
            .status == AssemblyViewportFitStatus::InvalidInput);

  // Hidden/suppressed occurrences and body-hidden sources are not active
  // render users and therefore cannot keep a CPU source resident.
  fixture.secondBody->setVisible(false);
  CHECK(fixture.product->setRootOccurrenceFlags(
            path(OccurrenceId{1003}), OccurrenceFlags{true, true, false}) ==
        ProductOccurrenceMutationStatus::Committed);
  published = coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{41}, kNormal);
  CHECK(published);
  CHECK(adapter.publish(coordinator.active()));
  CHECK(adapter.active()->sources().size() == 1);
  CHECK(adapter.active()->instances().size() == 2);
  CHECK(adapter.diagnostics().residentSources == 1);
  CHECK(adapter.active()->worldBounds());
  // Only the effectively visible first-body instances contribute. The
  // suppressed occurrence and hidden body cannot inflate Fit bounds.
  for (const auto& instance : adapter.active()->instances())
    CHECK(instance.sourceKey.bodyId == 11);
  const auto visibleOnlyBounds =
      adapter.active()->instances()[0].worldBounds.united(
          adapter.active()->instances()[1].worldBounds);
  CHECK(visibleOnlyBounds);
  CHECK(samePoint(adapter.active()->worldBounds()->minimum(),
                  visibleOnlyBounds->minimum()));
  CHECK(samePoint(adapter.active()->worldBounds()->maximum(),
                  visibleOnlyBounds->maximum()));

  // Epoch is part of identity even with identical IDs, revisions and raw
  // shape pointers. Inject failure on the second new source: the first staged
  // tessellation and cache changes must not replace the committed scene.
  fixture.secondBody->setVisible(true);
  published = coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{42}, kNormal);
  CHECK(published);
  const auto beforeFailure = adapter.active();
  const auto residentBeforeFailure = adapter.diagnostics().residentSources;
  AssemblyViewportSceneAdapterTestAccess::failNthNewMesh(adapter, 2);
  const auto failed = adapter.publish(coordinator.active());
  CHECK(!failed);
  CHECK(failed.status == AssemblyViewportSceneStatus::MeshBuildFailed);
  CHECK(adapter.active() == beforeFailure);
  CHECK(adapter.diagnostics().residentSources == residentBeforeFailure);
  CHECK(adapter.publish(coordinator.active()));
  CHECK(adapter.active()->sources().size() == 2);
  CHECK(adapter.active()->sources()[0].key.productEpoch == 42);
  CHECK(adapter.active()->sources()[0].key != initialScene->sources()[0].key);

  const auto current = coordinator.active();
  CHECK(coordinator.clear() == AssemblySceneClearStatus::Cleared);
  const auto stale = adapter.publish(current);
  CHECK(!stale);
  CHECK(stale.status == AssemblyViewportSceneStatus::StalePublication);

  // Identical local IDs in distinct definitions remain distinct sources.
  auto duplicateIds = duplicateLocalIdsAcrossDefinitions();
  AssemblyScenePublicationCoordinator secondCoordinator;
  CHECK(secondCoordinator.rebuildAndPublish(
      *duplicateIds, ProductPresentationEpoch{77}, kNormal));
  CHECK(adapter.publish(secondCoordinator.active()));
  CHECK(adapter.active()->sources().size() == 2);
  CHECK(adapter.active()->sources()[0].key.bodyId == 1);
  CHECK(adapter.active()->sources()[1].key.bodyId == 1);
  CHECK(adapter.active()->sources()[0].key.definitionId !=
        adapter.active()->sources()[1].key.definitionId);

  // A valid empty publication is a real commit and prunes the final user.
  auto empty = emptyAssemblyProduct();
  AssemblyScenePublicationCoordinator emptyCoordinator;
  CHECK(emptyCoordinator.rebuildAndPublish(
      *empty, ProductPresentationEpoch{88}, kNormal));
  CHECK(adapter.publish(emptyCoordinator.active()));
  CHECK(adapter.active());
  CHECK(adapter.active()->sources().empty());
  CHECK(adapter.active()->instances().empty());
  CHECK(!adapter.active()->worldBounds());
  const auto emptyFit = fitAssemblyViewport(
      adapter.active(), QSize{800, 600}, 0.0F, 0.0F);
  CHECK(emptyFit.status == AssemblyViewportFitStatus::EmptyBounds);
  CHECK(!emptyFit.fit);
  CHECK(adapter.diagnostics().residentSources == 0);
  CHECK(adapter.diagnostics().activeSources == 0);
  CHECK(adapter.diagnostics().activeInstances == 0);

  // Presentation quality is mapped explicitly into the viewport domain; the
  // two enums intentionally do not rely on matching ordinal values.
  auto highQuality = threeOccurrenceTwoBodyProduct();
  AssemblyScenePublicationCoordinator highCoordinator;
  constexpr AssemblyMeshProfile highProfile{AssemblyMeshQuality::High};
  CHECK(highCoordinator.rebuildAndPublish(
      *highQuality.product, ProductPresentationEpoch{89}, highProfile));
  AssemblyViewportSceneAdapter highAdapter;
  CHECK(highAdapter.publish(highCoordinator.active()));
  CHECK(highAdapter.active()->sources().size() == 2);
  for (const auto& source : highAdapter.active()->sources()) {
    CHECK(source.key.quality == ViewportMeshQuality::High);
    CHECK(source.mesh->quality() == ViewportMeshQuality::High);
  }

  auto tiny = tinyAssemblyProduct(0.001);
  AssemblyScenePublicationCoordinator tinyCoordinator;
  AssemblyViewportSceneAdapter tinyAdapter;
  CHECK(tinyCoordinator.rebuildAndPublish(
      *tiny, ProductPresentationEpoch{91}, kNormal));
  CHECK(tinyAdapter.publish(tinyCoordinator.active()));
  const auto maximumClampFit = fitAssemblyViewport(
      tinyAdapter.active(), QSize{800, 600}, -35.0F, 25.0F);
  CHECK(maximumClampFit);
  CHECK(maximumClampFit.fit->maximumZoomClamped);
  CHECK(!maximumClampFit.fit->minimumZoomClamped);
  CHECK(maximumClampFit.fit->fullyContained);

  auto hugeSpread = threeOccurrenceTwoBodyProduct();
  CHECK(hugeSpread.product->setRootOccurrencePlacement(
            path(OccurrenceId{1001}), placement(-1.0e9, 0.0, 0.0)) ==
        ProductOccurrenceMutationStatus::Committed);
  CHECK(hugeSpread.product->setRootOccurrencePlacement(
            path(OccurrenceId{1003}), placement(1.0e9, 0.0, 0.0)) ==
        ProductOccurrenceMutationStatus::Committed);
  AssemblyScenePublicationCoordinator hugeCoordinator;
  AssemblyViewportSceneAdapter hugeAdapter;
  CHECK(hugeCoordinator.rebuildAndPublish(
      *hugeSpread.product, ProductPresentationEpoch{92}, kNormal));
  CHECK(hugeAdapter.publish(hugeCoordinator.active()));
  const auto minimumClampFit = fitAssemblyViewport(
      hugeAdapter.active(), QSize{640, 480}, 0.0F, 0.0F);
  CHECK(minimumClampFit);
  CHECK(minimumClampFit.fit->minimumZoomClamped);
  CHECK(!minimumClampFit.fit->maximumZoomClamped);
  CHECK(!minimumClampFit.fit->fullyContained);

  // An aggregate whose opposite finite translations make the diagonal
  // overflow is a typed bounds failure. Neither the last scene nor its cache
  // publication is replaced.
  auto overflowFixture = threeOccurrenceTwoBodyProduct();
  const double maximum = std::numeric_limits<double>::max();
  CHECK(overflowFixture.product->setRootOccurrencePlacement(
            path(OccurrenceId{1001}), placement(maximum, 0.0, 0.0)) ==
        ProductOccurrenceMutationStatus::Committed);
  CHECK(overflowFixture.product->setRootOccurrencePlacement(
            path(OccurrenceId{1002}), placement(-maximum, 0.0, 0.0)) ==
        ProductOccurrenceMutationStatus::Committed);
  AssemblyScenePublicationCoordinator overflowCoordinator;
  CHECK(overflowCoordinator.rebuildAndPublish(
      *overflowFixture.product, ProductPresentationEpoch{90}, kNormal));
  const auto beforeInvalidBounds = highAdapter.active();
  const auto invalidBounds = highAdapter.publish(overflowCoordinator.active());
  CHECK(!invalidBounds);
  CHECK(invalidBounds.status == AssemblyViewportSceneStatus::InvalidBounds);
  CHECK(highAdapter.active() == beforeInvalidBounds);

  const auto staleFitScene = transformedScene;
  CHECK(fitAssemblyViewport(staleFitScene, QSize{800, 600}, 0.0F, 0.0F)
            .status == AssemblyViewportFitStatus::StalePublication);

  return EXIT_SUCCESS;
}

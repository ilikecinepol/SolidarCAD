#include "TestAssertions.h"

#include "ui/SketchRenderer.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <array>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <type_traits>
#include <vector>

namespace {

using namespace solidar;

static_assert(!std::is_copy_constructible_v<SketchRenderGeometry>);
static_assert(!std::is_copy_assignable_v<SketchRenderGeometry>);
static_assert(std::is_nothrow_move_constructible_v<SketchRenderGeometry>);
static_assert(std::is_nothrow_move_assignable_v<SketchRenderGeometry>);

QPalette widgetPalette(const ThemeColors& theme) {
  QPalette palette;
  palette.setColor(QPalette::Window, theme.window);
  palette.setColor(QPalette::Base, theme.surface);
  palette.setColor(QPalette::Text, theme.textPrimary);
  palette.setColor(QPalette::Mid, theme.textSecondary);
  palette.setColor(QPalette::Highlight, theme.accent);
  return palette;
}

SketchRenderSnapshot representativeSnapshot(const ThemeColors& theme) {
  SketchRenderSnapshot snapshot;
  snapshot.viewportSize = QSize(360, 280);
  snapshot.palette = sketchRenderPalette(theme, widgetPalette(theme));
  snapshot.pixelsPerMm = 5.0;
  snapshot.snapStepMm = 5.0;
  snapshot.gridVisible = true;

  sketch::Sketch sketch;
  sketch.addLine({-18.0, 0.0}, {18.0, 0.0});
  sketch.addCircle({0.0, 10.0}, 5.0);
  sketch.setCircleDashedById(sketch.circleId(0), true);
  sketch.addArc({0.0, -10.0}, 6.0, 0.0, std::numbers::pi, false);
  const auto centeredStart = sketch.lines().size();
  sketch.addRectangle({-22.0, -22.0}, {-16.0, -16.0});
  sketch.markElementCenterNode(sketch.lines()[centeredStart].elementId);
  sketch::Constraint projectedLock;
  projectedLock.type = sketch::ConstraintType::Lock;
  projectedLock.firstGeometry = sketch.circleId(0);
  projectedLock.value = 0.0;
  if (sketch.addConstraint(projectedLock) ==
      sketch::kInvalidConstraintId)
    std::abort();
  snapshot.selectedLineIds.push_back(sketch.lineId(0));

  sketch::Dimension dimension;
  dimension.kind = sketch::DimensionKind::LineLength;
  dimension.geometryId = sketch.lineId(0);
  dimension.valueMm = 36.0;
  dimension.offsetMm = 7.0;
  sketch.addDimension(dimension);

  sketch::Sketch referenceProfile;
  std::vector<SketchSceneReference> sceneSketches;
  std::vector<std::shared_ptr<const BodyRenderMesh>> sceneMeshes;
  SketchRenderSceneCache cache;
  snapshot.scene = cache.resolve(
      1, sketch, referenceProfile, sceneSketches,
      std::make_shared<BodyRenderMesh>(),
      std::make_shared<BodyRenderMesh>(), sceneMeshes,
      SketchPlacement::xy(), 0.0, 0.0, 0.0, false, false, false);

  snapshot.interaction.tool = SketchInteractionTool::Arc;
  snapshot.interaction.creation.arcPoints.push_back({-12.0, 18.0});
  snapshot.hoverPoint = {12.0, 18.0};
  snapshot.constructionHover = SketchRenderSnap{
      {0.0, 0.0}, SketchRenderSnapKind::Origin,
      sketch::kInvalidGeometryId, 0, {}};
  return snapshot;
}

QImage render(const SketchRenderer& renderer,
              const SketchRenderSnapshot& snapshot) {
  QImage image(snapshot.viewportSize, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(painter, renderer.buildFrame(snapshot));
  painter.end();
  return image;
}

bool imageHasInk(const QImage& image) {
  for (int y = 0; y < image.height(); y += 7)
    for (int x = 0; x < image.width(); x += 7)
      if (qAlpha(image.pixel(x, y)) != 0) return true;
  return false;
}

int colorDistance(QColor first, QColor second) {
  return std::abs(first.red() - second.red()) +
         std::abs(first.green() - second.green()) +
         std::abs(first.blue() - second.blue());
}

bool imageContainsColor(const QImage& image, QColor expected,
                        int tolerance = 24) {
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      if (colorDistance(image.pixelColor(x, y), expected) <= tolerance)
        return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  solidar::SketchRenderer renderer;

  const auto lightTheme = lightThemeColors();
  const auto darkTheme = darkThemeColors();
  const QColor lightConstraintRole(241, 17, 191);
  const QColor lightTransientRole(19, 97, 253);
  const QColor lightTrimRole(3, 250, 117);
  const QColor darkConstraintRole(251, 91, 13);
  const QColor darkTransientRole(127, 17, 239);
  const QColor darkTrimRole(17, 239, 89);
  auto lightSnapshot = representativeSnapshot(lightTheme);
  lightSnapshot.palette.constraint = lightConstraintRole;
  lightSnapshot.palette.transient = lightTransientRole;
  lightSnapshot.palette.trim = lightTrimRole;
  CHECK(lightConstraintRole != lightTransientRole &&
        lightConstraintRole != lightTrimRole &&
        lightTransientRole != lightTrimRole &&
        lightConstraintRole != lightSnapshot.palette.background &&
        lightTransientRole != lightSnapshot.palette.background &&
        lightTrimRole != lightSnapshot.palette.background);
  const auto lightFrame = renderer.buildFrame(lightSnapshot);
  CHECK(lightFrame.hasCanonicalOrder());
  CHECK(lightFrame.layers.size() == kSketchRenderPassOrder.size());
  for (const auto pass : kSketchRenderPassOrder)
    CHECK(lightFrame.layerCount(pass) == 1);

  QImage lightImage(lightSnapshot.viewportSize,
                    QImage::Format_ARGB32_Premultiplied);
  lightImage.fill(Qt::transparent);
  {
    QPainter painter(&lightImage);
    renderer.render(painter, lightFrame);
  }
  CHECK(imageHasInk(lightImage));
  CHECK(lightImage.pixelColor(100, 100) !=
        sketchRenderPalette(darkTheme, widgetPalette(darkTheme)).background);
  // Selected geometry, a Lock-constrained circle, and its dimension remain
  // visible with semantic contrast instead of relying on mere opacity.
  const QColor selectedPixel = lightImage.pixelColor(220, 155);
  CHECK(colorDistance(selectedPixel, lightSnapshot.palette.background) > 90);
  CHECK(imageContainsColor(lightImage, lightSnapshot.palette.committedLocked));
  CHECK(imageContainsColor(lightImage, lightSnapshot.palette.dimension));
  CHECK(imageContainsColor(lightImage, lightConstraintRole, 0));
  CHECK(imageContainsColor(lightImage, lightTransientRole, 110));
  auto lightTransientControl = lightSnapshot;
  lightTransientControl.palette.transient = darkTransientRole;
  CHECK(render(renderer, lightTransientControl) != lightImage);

  auto darkSnapshot = representativeSnapshot(darkTheme);
  darkSnapshot.palette.constraint = darkConstraintRole;
  darkSnapshot.palette.transient = darkTransientRole;
  darkSnapshot.palette.trim = darkTrimRole;
  CHECK(darkConstraintRole != darkTransientRole &&
        darkConstraintRole != darkTrimRole &&
        darkTransientRole != darkTrimRole &&
        darkConstraintRole != darkSnapshot.palette.background &&
        darkTransientRole != darkSnapshot.palette.background &&
        darkTrimRole != darkSnapshot.palette.background);
  const auto darkImage = render(renderer, darkSnapshot);
  CHECK(imageHasInk(darkImage));
  CHECK(lightSnapshot.palette.background != darkSnapshot.palette.background);
  CHECK(colorDistance(darkImage.pixelColor(220, 155),
                      darkSnapshot.palette.background) > 90);
  CHECK(imageContainsColor(darkImage, darkSnapshot.palette.committedLocked));
  CHECK(imageContainsColor(darkImage, darkSnapshot.palette.dimension));
  CHECK(imageContainsColor(darkImage, darkConstraintRole, 0));
  CHECK(imageContainsColor(darkImage, darkTransientRole, 110));
  auto darkTransientControl = darkSnapshot;
  darkTransientControl.palette.transient = lightTransientRole;
  CHECK(render(renderer, darkTransientControl) != darkImage);

  // Visible reference images belong to the immutable sketch scene and render
  // as a dimmed, non-interactive backdrop. Their original pixels are retained
  // independently of the mutable Document/model object.
  QImage referencePixels(8, 8, QImage::Format_ARGB32_Premultiplied);
  referencePixels.fill(QColor(7, 231, 149));
  std::vector<SketchSceneImageReference> sceneImages{{
      QStringLiteral("Reference"), referencePixels, SketchPlacement::xy(),
      8.0, 6.0, 0.0, 1.0, 100, 80}};
  sketch::Sketch imageSketch;
  sketch::Sketch imageProfile;
  std::vector<SketchSceneReference> imageSketches;
  std::vector<std::shared_ptr<const BodyRenderMesh>> imageMeshes;
  auto imageEmptyMesh = std::make_shared<BodyRenderMesh>();
  SketchRenderSceneCache imageCache;
  auto imageScene = imageCache.resolve(
      1, imageSketch, imageProfile, imageSketches, imageEmptyMesh,
      imageEmptyMesh, imageMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0,
      false, false, false, sceneImages);
  CHECK(imageScene->sceneImages.size() == 1);
  CHECK(imageScene->sceneImages.front().pixels == referencePixels);
  SketchRenderSnapshot imageSnapshot;
  imageSnapshot.viewportSize = QSize(260, 220);
  imageSnapshot.palette = sketchRenderPalette(lightTheme,
                                               widgetPalette(lightTheme));
  imageSnapshot.scene = imageScene;
  imageSnapshot.pixelsPerMm = 4.0;
  imageSnapshot.snapStepMm = 5.0;
  imageSnapshot.gridVisible = false;
  const auto imageBackdrop = render(renderer, imageSnapshot);
  SketchRenderSceneCache noImageCache;
  imageSnapshot.scene = noImageCache.resolve(
      1, imageSketch, imageProfile, imageSketches, imageEmptyMesh,
      imageEmptyMesh, imageMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0,
      false, false, false);
  CHECK(render(renderer, imageSnapshot) != imageBackdrop);

  auto trim = representativeSnapshot(lightTheme);
  trim.palette.trim = lightTrimRole;
  trim.interaction.tool = SketchInteractionTool::Trim;
  trim.interaction.trim.preview = SketchTrimPreview{
      SketchTrimGeometryKind::Line, trim.scene->sketch.lineId(0),
      0.2, 0.8, false};
  CHECK(imageContainsColor(render(renderer, trim), lightTrimRole, 0));

  auto darkTrim = representativeSnapshot(darkTheme);
  darkTrim.palette.trim = darkTrimRole;
  darkTrim.interaction.tool = SketchInteractionTool::Trim;
  darkTrim.interaction.trim.preview = SketchTrimPreview{
      SketchTrimGeometryKind::Line, darkTrim.scene->sketch.lineId(0),
      0.2, 0.8, false};
  const auto darkTrimImage = render(renderer, darkTrim);
  CHECK(imageContainsColor(darkTrimImage, darkTrimRole, 0));
  CHECK(!imageContainsColor(darkTrimImage, lightTrimRole, 0));

  // The supplied semantic palette controls deterministic pixels.
  auto custom = representativeSnapshot(lightTheme);
  custom.gridVisible = false;
  custom.palette.background = lightTheme.danger;
  const auto customImage = render(renderer, custom);
  CHECK(customImage.pixelColor(100, 100) == custom.palette.background);

  // Same committed revision reuses its immutable scene even when transient
  // state changes. A model revision rebuilds; a project replacement cannot
  // accidentally resolve to the old scene.
  sketch::Sketch cachedSketch;
  cachedSketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch::Sketch emptyReference;
  std::vector<SketchSceneReference> noSketchReferences;
  std::vector<std::shared_ptr<const BodyRenderMesh>> noMeshes;
  auto emptyMesh = std::make_shared<BodyRenderMesh>();
  SketchRenderSceneCache cache;
  const auto cachedFirst = cache.resolve(
      41, cachedSketch, emptyReference, noSketchReferences, emptyMesh,
      emptyMesh, noMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0,
      false, false, false);
  const auto cachedAgain = cache.resolve(
      41, cachedSketch, emptyReference, noSketchReferences, emptyMesh,
      emptyMesh, noMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0,
      false, false, false);
  CHECK(cachedFirst == cachedAgain);
  CHECK(cache.buildCount() == 1);
  cachedSketch.addCircle({5.0, 5.0}, 2.0);
  const auto unchangedRevision = cache.resolve(
      41, cachedSketch, emptyReference, noSketchReferences, emptyMesh,
      emptyMesh, noMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0,
      false, false, false);
  CHECK(unchangedRevision == cachedFirst);
  CHECK(unchangedRevision->sketch.circles().empty());
  const auto modelChanged = cache.resolve(
      42, cachedSketch, emptyReference, noSketchReferences, emptyMesh,
      emptyMesh, noMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0,
      false, false, false);
  CHECK(modelChanged != cachedFirst);
  CHECK(modelChanged->sketch.circles().size() == 1);
  CHECK(cache.buildCount() == 2);
  auto transientOnly = lightSnapshot;
  transientOnly.hoverPoint = {99.0, 99.0};
  CHECK(cache.resolve(42, cachedSketch, emptyReference, noSketchReferences,
                      emptyMesh, emptyMesh, noMeshes, SketchPlacement::xy(),
                      0.0, 0.0, 0.0, false, false, false) == modelChanged);
  CHECK(cache.buildCount() == 2);
  sketch::Sketch replacement;
  replacement.addArc({0.0, 0.0}, 4.0, 0.0, std::numbers::pi, false);
  const auto replaced = cache.resolve(
      43, replacement, emptyReference, noSketchReferences, emptyMesh,
      emptyMesh, noMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0,
      false, false, false);
  CHECK(replaced != modelChanged);
  CHECK(replaced->sketch.lines().empty());
  CHECK(replaced->sketch.arcs().size() == 1);
  CHECK(modelChanged->sketch.lines().size() == 1);
  CHECK(cache.buildCount() == 3);

  // SketchRenderGeometry is move-only (asserted above), so transferring the
  // validated capture results into the immutable scene cannot deep-copy any
  // vector or ID map. Exercise all three rebuild lanes with 10k primitives:
  // committed sketch, reference profile, and a scene-sketch reference.
  constexpr std::size_t denseGeometryCount = 10000;
  sketch::Sketch denseMain;
  for (std::size_t index = 0; index < denseGeometryCount; ++index) {
    const double x = static_cast<double>(index);
    denseMain.addLine({x, 0.0}, {x, 1.0});
  }
  sketch::Sketch denseProfile = denseMain;
  sketch::Sketch denseReference = denseMain;
  std::vector<SketchSceneReference> denseReferences;
  denseReferences.push_back(
      {std::move(denseReference), SketchPlacement::xy()});
  SketchRenderSceneCache denseCache;
  const auto denseScene = denseCache.resolve(
      1, denseMain, denseProfile, denseReferences, emptyMesh, emptyMesh,
      noMeshes, SketchPlacement::xy(), 0.0, 0.0, 0.0, false, false, false);
  CHECK(denseScene && denseScene->valid);
  CHECK(denseScene->sketch.lines().size() == denseGeometryCount);
  CHECK(denseScene->referenceProfile.lines().size() == denseGeometryCount);
  CHECK(denseScene->sceneSketches.size() == 1);
  CHECK(denseScene->sceneSketches.front().geometry.lines().size() ==
        denseGeometryCount);
  CHECK(denseScene->sketch.hasCompleteIndexInvariant());
  CHECK(denseScene->referenceProfile.hasCompleteIndexInvariant());
  CHECK(denseScene->sceneSketches.front().geometry
            .hasCompleteIndexInvariant());

  // 10k primitives are indexed once; each stable-ID lookup performs one
  // hash-table probe path instead of scanning the geometry vector. Style
  // metadata is precomputed in the same linear build pass.
  constexpr std::size_t geometryCount = 10000;
  const auto makeLargeGeometry = [=] {
    SketchRenderGeometry result;
    result.linePrimitives.reserve(geometryCount);
    result.lineIds.reserve(geometryCount);
    for (std::size_t index = 0; index < geometryCount; ++index) {
      result.linePrimitives.push_back(
          {{static_cast<double>(index), 0.0},
           {static_cast<double>(index), 1.0}, index + 1,
           index < 2});
      result.lineIds.push_back(index + 1);
    }
    for (std::size_t index = 1; index < 4; ++index)
      result.linePrimitives[index].elementId =
          result.linePrimitives[0].elementId;
    result.centerNodeElementIdsData = {result.linePrimitives[0].elementId};
    sketch::Dimension dimension;
    dimension.id = 1;
    dimension.kind = sketch::DimensionKind::LineLength;
    dimension.geometryId = 1;
    dimension.valueMm = 1.0;
    result.dimensionPrimitives.push_back(dimension);
    for (std::size_t index = 0; index < 100; ++index) {
      sketch::Constraint lock;
      lock.id = index + 1;
      lock.type = sketch::ConstraintType::Lock;
      lock.firstGeometry = index * 100 + 1;
      lock.value = 0.0;
      result.constraintPrimitives.push_back(lock);
    }
    return result;
  };
  auto large = makeLargeGeometry();
  CHECK(large.buildIndexesAndMetadata());
  CHECK(large.indexStats().indexedGeometry == geometryCount);
  CHECK(large.indexStats().constraintVisits ==
        large.constraintPrimitives.size());
  CHECK(large.indexStats().centerLineVisits == geometryCount);
  CHECK(large.indexStats().indexedDimensions == 1);
  CHECK(large.hasCompleteIndexInvariant());
  const auto projectedStyle = large.style(1);
  const auto siblingStyle = large.style(2);
  CHECK(projectedStyle && projectedStyle->locked && projectedStyle->projected &&
        !projectedStyle->construction);
  CHECK(siblingStyle && siblingStyle->locked && !siblingStyle->projected &&
        siblingStyle->construction);
  for (std::size_t index = 0; index < geometryCount; ++index)
    CHECK(large.lineIndex(index + 1) == index);
  CHECK(large.dimensionIndex(1) == 0);
  CHECK(large.elementCenterPoint(large.linePrimitives[0].elementId));

  auto duplicateIds = makeLargeGeometry();
  duplicateIds.lineIds.back() = duplicateIds.lineIds.front();
  CHECK(!duplicateIds.buildIndexesAndMetadata());
  CHECK(!duplicateIds.lineIndex(duplicateIds.lineIds.front()));
  auto invalidId = makeLargeGeometry();
  invalidId.lineIds.front() = sketch::kInvalidGeometryId;
  CHECK(!invalidId.buildIndexesAndMetadata());
  CHECK(!invalidId.lineIndex(sketch::kInvalidGeometryId));
  auto duplicateDimensions = makeLargeGeometry();
  duplicateDimensions.dimensionPrimitives.push_back(
      duplicateDimensions.dimensionPrimitives.front());
  CHECK(!duplicateDimensions.buildIndexesAndMetadata());
  auto invalidDimension = makeLargeGeometry();
  invalidDimension.dimensionPrimitives.front().id =
      sketch::kInvalidDimensionId;
  CHECK(!invalidDimension.buildIndexesAndMetadata());

  // Deleted/stale stable IDs fail closed while the remaining frame still
  // renders. No positional index is retained in the resulting plan.
  auto stale = representativeSnapshot(lightTheme);
  stale.selectedLineIds = {999999};
  stale.interaction.tool = SketchInteractionTool::Trim;
  stale.interaction.trim.preview = SketchTrimPreview{
      SketchTrimGeometryKind::Arc, 999999, 0.1, 0.8, false};
  const auto staleFrame = renderer.buildFrame(stale);
  CHECK(staleFrame.hasCanonicalOrder());
  CHECK(imageHasInk(render(renderer, stale)));

  // A malformed/reordered plan is rejected atomically: not even its first
  // layer may leak to the target image.
  auto malformed = lightFrame;
  std::swap(malformed.layers[0], malformed.layers[1]);
  CHECK(!malformed.hasCanonicalOrder());
  QImage rejected(lightSnapshot.viewportSize, QImage::Format_ARGB32);
  rejected.fill(QColor(1, 2, 3));
  {
    QPainter painter(&rejected);
    renderer.render(painter, malformed);
  }
  CHECK(rejected.pixelColor(20, 20) == QColor(1, 2, 3));

  return EXIT_SUCCESS;
}

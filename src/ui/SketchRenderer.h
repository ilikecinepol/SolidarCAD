#pragma once

#include <QColor>
#include <QPalette>
#include <QSize>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "model/SketchPlacement.h"
#include "sketch/Sketch.h"
#include "ui/BodyRenderMesh.h"
#include "ui/SketchHitTester.h"
#include "ui/SketchInteractionController.h"
#include "ui/ThemeColors.h"

class QPainter;

namespace solidar {

struct SketchSceneReference {
  sketch::Sketch geometry;
  SketchPlacement placement{SketchPlacement::xy()};
};

// Render-only geometry detached from the mutable Sketch model. Stable IDs are
// retained so transient tool state can be revalidated against an immutable
// frame scene without positional identities escaping the frame.
struct SketchRenderGeometry {
  SketchRenderGeometry() = default;
  SketchRenderGeometry(const SketchRenderGeometry&) = delete;
  SketchRenderGeometry& operator=(const SketchRenderGeometry&) = delete;
  SketchRenderGeometry(SketchRenderGeometry&&) noexcept = default;
  SketchRenderGeometry& operator=(SketchRenderGeometry&&) noexcept = default;

  struct Style {
    bool locked{};
    bool projected{};
    bool construction{};
  };
  struct IndexStats {
    std::size_t indexedGeometry{};
    std::size_t indexedDimensions{};
    std::size_t constraintVisits{};
    std::size_t centerLineVisits{};
  };
  std::vector<sketch::Line> linePrimitives;
  std::vector<sketch::Circle> circlePrimitives;
  std::vector<sketch::Arc> arcPrimitives;
  std::vector<sketch::GeometryId> lineIds;
  std::vector<sketch::GeometryId> circleIds;
  std::vector<sketch::GeometryId> arcIds;
  std::vector<sketch::Dimension> dimensionPrimitives;
  std::vector<sketch::Constraint> constraintPrimitives;
  std::vector<std::size_t> centerNodeElementIdsData;

  [[nodiscard]] const std::vector<sketch::Line>& lines() const noexcept {
    return linePrimitives;
  }
  [[nodiscard]] const std::vector<sketch::Circle>& circles() const noexcept {
    return circlePrimitives;
  }
  [[nodiscard]] const std::vector<sketch::Arc>& arcs() const noexcept {
    return arcPrimitives;
  }
  [[nodiscard]] const std::vector<sketch::Dimension>& dimensions()
      const noexcept {
    return dimensionPrimitives;
  }
  [[nodiscard]] const std::vector<sketch::Constraint>& constraints()
      const noexcept {
    return constraintPrimitives;
  }
  [[nodiscard]] const std::vector<std::size_t>& centerNodeElementIds()
      const noexcept {
    return centerNodeElementIdsData;
  }

  [[nodiscard]] std::optional<std::size_t> lineIndex(
      sketch::GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> circleIndex(
      sketch::GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> arcIndex(
      sketch::GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> dimensionIndex(
      sketch::DimensionId id) const noexcept;
  [[nodiscard]] sketch::GeometryId lineId(std::size_t index) const noexcept;
  [[nodiscard]] sketch::GeometryId circleId(std::size_t index) const noexcept;
  [[nodiscard]] sketch::GeometryId arcId(std::size_t index) const noexcept;
  [[nodiscard]] bool isGeometryLocked(sketch::GeometryId id) const noexcept;
  [[nodiscard]] std::optional<sketch::Point> elementCenterPoint(
      std::size_t elementId) const noexcept;
  [[nodiscard]] std::optional<sketch::Point> referencedPoint(
      sketch::PointReference reference) const noexcept;
  [[nodiscard]] std::optional<Style> style(
      sketch::GeometryId id) const noexcept;
  [[nodiscard]] bool buildIndexesAndMetadata() noexcept;
  [[nodiscard]] const IndexStats& indexStats() const noexcept;
  [[nodiscard]] bool hasCompleteIndexInvariant() const noexcept;

 private:
  std::unordered_map<sketch::GeometryId, std::size_t> lineIndexById_;
  std::unordered_map<sketch::GeometryId, std::size_t> circleIndexById_;
  std::unordered_map<sketch::GeometryId, std::size_t> arcIndexById_;
  std::unordered_map<sketch::DimensionId, std::size_t> dimensionIndexById_;
  std::unordered_map<sketch::GeometryId, Style> styleById_;
  std::unordered_map<std::size_t, sketch::Point> elementCenterById_;
  IndexStats indexStats_;
};

struct SketchRenderSceneReference {
  SketchRenderGeometry geometry;
  SketchPlacement placement{SketchPlacement::xy()};
};

// Immutable committed/reference scene. It owns only render data and immutable
// mesh handles; no mutable Sketch, Document, Canvas, controller, or callback.
struct SketchRenderScene {
  std::uint64_t sourceRevision{};
  bool valid{};
  SketchRenderGeometry sketch;
  SketchRenderGeometry referenceProfile;
  std::vector<SketchRenderSceneReference> sceneSketches;
  std::shared_ptr<const BodyRenderMesh> referenceBodyMesh;
  std::shared_ptr<const BodyRenderMesh> referenceFaceMesh;
  std::vector<std::shared_ptr<const BodyRenderMesh>> sceneBodyMeshes;
  SketchPlacement referencePlacement{SketchPlacement::xy()};
  double referenceWidthMm{};
  double referenceDepthMm{};
  double referenceHeightMm{};
  bool referenceBodyVisible{};
  bool realReferenceBodyVisible{};
  bool referenceProfileVisible{};
};

// Renderer-side adapter/cache. Source references are consumed synchronously
// during a rebuild and are never retained. A monotonic owner revision makes a
// project replacement distinct even when its geometry happens to be equal.
class SketchRenderSceneCache final {
 public:
  [[nodiscard]] std::shared_ptr<const SketchRenderScene> resolve(
      std::uint64_t sourceRevision,
      const sketch::Sketch& sketch,
      const sketch::Sketch& referenceProfile,
      std::span<const SketchSceneReference> sceneSketches,
      std::shared_ptr<const BodyRenderMesh> referenceBodyMesh,
      std::shared_ptr<const BodyRenderMesh> referenceFaceMesh,
      std::span<const std::shared_ptr<const BodyRenderMesh>> sceneBodyMeshes,
      const SketchPlacement& referencePlacement,
      double referenceWidthMm, double referenceDepthMm,
      double referenceHeightMm, bool referenceBodyVisible,
      bool realReferenceBodyVisible, bool referenceProfileVisible);
  void invalidate() noexcept;
  [[nodiscard]] std::size_t buildCount() const noexcept;

 private:
  std::shared_ptr<const SketchRenderScene> cached_;
  std::size_t buildCount_{};
};

// Complete semantic paint contract. The renderer never invents theme colors;
// the application resolves System/Light/Dark through ThemeManager and supplies
// these roles once per frame.
struct SketchRenderPalette {
  QColor background;
  QColor gridMinor;
  QColor rulerBackground;
  QColor rulerBorder;
  QColor rulerText;
  QColor axisX;
  QColor axisY;
  QColor datum;
  QColor sceneFill;
  QColor sceneEdge;
  QColor sceneHover;
  QColor sceneSketch;
  QColor referenceFill;
  QColor referenceEdge;
  QColor committed;
  QColor committedLocked;
  QColor selected;
  QColor selectedLocked;
  QColor endpointFill;
  QColor trim;
  QColor projection;
  QColor constraint;
  QColor dimension;
  QColor dimensionSelected;
  QColor dimensionLabelBackground;
  QColor dimensionText;
  QColor dimensionSelectedText;
  QColor transient;
  QColor transientFill;
  QColor transientSurface;
  QColor selectionBoxOutline;
  QColor selectionBoxFill;
  QColor hudText;
};

[[nodiscard]] SketchRenderPalette sketchRenderPalette(
    const ThemeColors& theme, const QPalette& widgetPalette);

enum class SketchRenderCircleMode {
  CenterRadius,
  TwoPoints,
  ThreePoints,
  ThreeTangents,
  TwoTangentsRadius,
};

enum class SketchRenderRectangleMode { TwoPoints, ThreePoints, FromCenter };
enum class SketchRenderSelectionKind { None, Line, Circle, Arc };
enum class SketchRenderSnapKind {
  None,
  LinePoint,
  LineMidpoint,
  CircleCenter,
  ElementCenter,
  LineBody,
  CircleBody,
  XAxis,
  YAxis,
  Origin,
};

struct SketchRenderSnap {
  sketch::Point point{};
  SketchRenderSnapKind kind{SketchRenderSnapKind::None};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  std::size_t elementId{};
  sketch::PointReference pointReference{};
};

struct SketchRenderTransientState {
  SketchInteractionTool tool{SketchInteractionTool::Select};
  SketchDimensionInteractionState dimension;
  SketchAutoDimensionState autoDimension;
  SketchConstraintInteractionState constraint;
  SketchCreationGestureState creation;
  SketchTrimInteractionState trim;
  SketchCameraGestureState camera;
  SketchSelectionBoxState selectionBox;
  bool twoTangentRadiusPreviewActive{};
};

// Lean per-frame state. Committed/reference data lives in a revisioned,
// immutable scene shared across paints; only render-relevant interaction and
// view deltas are copied for the current frame.
struct SketchRenderSnapshot {
  QSize viewportSize;
  SketchRenderPalette palette;
  std::shared_ptr<const SketchRenderScene> scene;
  SketchRenderTransientState interaction;
  SketchRenderSelectionKind selectionKind{SketchRenderSelectionKind::None};
  sketch::GeometryId selectionCircleId{sketch::kInvalidGeometryId};
  sketch::GeometryId selectionLineId{sketch::kInvalidGeometryId};
  sketch::GeometryId selectionArcId{sketch::kInvalidGeometryId};
  std::size_t selectionElementId{};
  std::vector<sketch::GeometryId> selectedLineIds;
  std::vector<std::size_t> selectedElementIds;
  std::vector<sketch::GeometryId> selectedCircleIds;
  std::vector<sketch::GeometryId> selectedArcIds;
  sketch::Point hoverPoint{};
  std::optional<SketchRenderSnap> constructionHover;
  std::optional<SketchProjectionEdgeToken> hoveredProjectionEdge;
  double pixelsPerMm{5.0};
  double snapStepMm{5.0};
  bool snapEnabled{};
  bool gridVisible{true};
  double viewRotationDeg{};
  double viewYawDeg{};
  double viewPitchDeg{};
  SketchRenderCircleMode circleMode{SketchRenderCircleMode::CenterRadius};
  double circleDiameterMm{20.0};
  SketchRenderRectangleMode rectangleMode{SketchRenderRectangleMode::TwoPoints};
  bool primaryDimensionVisible{};
  double primaryDimensionValue{};
};

// Stable, exhaustive paint ordering for a captured Sketcher frame.  The
// frame owns its typed snapshot; it does not retain the Canvas, controller,
// widgets, mutable references, or frame-local container indexes.
enum class SketchRenderPass {
  BackgroundGridDatum,
  ReferenceGeometry,
  CommittedGeometry,
  TransientUnderlay,
  ConstraintsDimensions,
  TransientTools,
  SelectionHover,
  HudOverlays,
};

inline constexpr std::array<SketchRenderPass, 8> kSketchRenderPassOrder{
    SketchRenderPass::BackgroundGridDatum,
    SketchRenderPass::ReferenceGeometry,
    SketchRenderPass::CommittedGeometry,
    SketchRenderPass::TransientUnderlay,
    SketchRenderPass::ConstraintsDimensions,
    SketchRenderPass::TransientTools,
    SketchRenderPass::SelectionHover,
    SketchRenderPass::HudOverlays,
};

struct SketchRenderLayer {
  SketchRenderPass pass{SketchRenderPass::BackgroundGridDatum};
};

struct SketchRenderFrame {
  SketchRenderSnapshot snapshot;
  std::vector<SketchRenderLayer> layers;

  [[nodiscard]] bool hasCanonicalOrder() const noexcept;
  [[nodiscard]] std::size_t layerCount(SketchRenderPass pass) const noexcept;
};

// Pure QPainter rendering. SketchCanvas captures a self-contained frame before
// calling render(); no model/controller/UI object is observed during paint,
// so rendering cannot cause callbacks or mutation.
class SketchRenderer final {
 public:
  [[nodiscard]] SketchRenderFrame buildFrame(
      SketchRenderSnapshot snapshot) const;
  void render(QPainter& painter, const SketchRenderFrame& frame) const;
};

}  // namespace solidar

#pragma once

#include "model/EdgeReference.h"

#include <QPoint>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QOpenGLWidget>
#include <QImage>
#include <QMetaObject>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <optional>
#include <string>

#include "model/Document.h"
#include "sketch/Sketch.h"
#include "ui/BodyRenderMesh.h"
#include "ui/AssemblyViewportSession.h"
#include "ui/ComponentViewportSession.h"
#include "ui/AssemblyPlacementPreview.h"
#include "ui/ComponentPlacementDrag.h"
#include "ui/ViewportPicking.h"
#include "ui/ManipulatorLayout.h"
#include "ui/ViewportRenderer.h"
#include "ui/ViewportRuler.h"
#include "ui/ViewCube.h"
#include "ui/ViewportInteractionTypes.h"
#include "model/ToolSession.h"

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class QKeyEvent;
class QDoubleSpinBox;
class QVariantAnimation;
class QTimer;
class QPainter;

namespace solidar {

class ToolParameterHud;

struct ViewportNavigationState final {
  float yawDeg{};
  float pitchDeg{};
  float zoom{};
  QPointF pan;
  friend bool operator==(const ViewportNavigationState&,
                         const ViewportNavigationState&) = default;
};

struct BodyViewShape {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeFeature::ShapePtr shape;
  std::shared_ptr<const TopologyIndex> topologyIndex;
  ShapeRevision shapeRevision{kInvalidShapeRevision};
};

enum class BodyDisplayPrepareStatus : std::uint8_t {
  Prepared,
  InvalidInput,
  TopologyFailed,
  MeshFailed,
  BoundsFailed,
  ResourceLimit,
  ResourceFailure,
};

enum class BodyDisplayPrepareFailurePoint : std::uint8_t {
  None,
  Topology,
  Meshing,
  Bounds,
  Resource,
  Finalization,
};

struct PreparedBodyTopologyRange final {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeFeature::ShapePtr shape;
  std::shared_ptr<const TopologyIndex> topologyIndex;
  std::size_t firstFace{};
  std::size_t faceCount{};
  std::size_t firstEdge{};
  std::size_t edgeCount{};
};

struct PreparedBodyDisplayMesh final {
  std::shared_ptr<const BodyRenderMesh> mesh;
  BodyMeshKey identity;
  std::size_t firstFace{};
  std::size_t firstEdge{};
};

class PreparedBodyDisplay final {
 public:
  PreparedBodyDisplay() = default;
  ~PreparedBodyDisplay() = default;
  PreparedBodyDisplay(const PreparedBodyDisplay&) = delete;
  PreparedBodyDisplay& operator=(const PreparedBodyDisplay&) = delete;
  PreparedBodyDisplay(PreparedBodyDisplay&&) noexcept = default;
  PreparedBodyDisplay& operator=(PreparedBodyDisplay&&) noexcept = default;

  [[nodiscard]] std::size_t bodyCount() const noexcept {
    return meshes_.size();
  }
  [[nodiscard]] Point3d center() const noexcept { return center_; }
  [[nodiscard]] double diagonal() const noexcept { return diagonal_; }
  [[nodiscard]] ViewportMeshQuality quality() const noexcept { return quality_; }

 private:
  friend class Viewport;

  std::vector<BodyViewShape> viewShapes_;
  std::vector<PreparedBodyTopologyRange> topologyRanges_;
  std::vector<PreparedBodyDisplayMesh> meshes_;
  BodyRenderMeshCache meshCache_;
  ShapeFeature::ShapePtr representativeShape_;
  BodyId representativeBodyId_{kInvalidBodyId};
  FeatureId representativeFeatureId_{kInvalidFeatureId};
  BoxParameters box_;
  bool hasBox_{};
  Point3d center_{};
  double diagonal_{};
  ViewportMeshQuality quality_{ViewportMeshQuality::Normal};
};

struct PreparedBodyDisplayResult final {
  BodyDisplayPrepareStatus status{BodyDisplayPrepareStatus::InvalidInput};
  std::unique_ptr<PreparedBodyDisplay> display;
  GeometryFailure failure;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == BodyDisplayPrepareStatus::Prepared && display;
  }
};

// Immutable-by-convention presentation snapshot rebuilt from Document.
// Viewport owns the copies strictly for rendering and picking.
struct SketchPresentationSnapshot {
  SketchId sketchId{kInvalidSketchId};
  sketch::Sketch geometry;
  QString presentationLabel;
  SketchPlacement placement{SketchPlacement::xy()};
};

enum class SelectionFilter { Any, Face, Edge, Plane };
enum class ToolPreviewPresentation {
  // Keep selected/hovered source topology visible over the preview.
  OverlaySourceSelection,
  // The preview is the future body state; do not re-draw source selection over it.
  ReplaceSource
};

// Minimum marquee extent on release below which a drag is treated as a click,
// mirroring the Sketcher selection-box threshold.
inline constexpr double kMarqueeMinSizePx = 3.0;

class Viewport final : public QOpenGLWidget {
  Q_OBJECT

 public:
  static constexpr qulonglong kGlobalXAxisToken = 0xfffffffffffffff0ULL;
  static constexpr qulonglong kGlobalYAxisToken = 0xfffffffffffffff1ULL;
  static constexpr qulonglong kGlobalZAxisToken = 0xfffffffffffffff2ULL;
  explicit Viewport(QWidget* parent = nullptr);
  ~Viewport() override;
  void installComponentSession(
      std::unique_ptr<ComponentViewportSession> session);
  void clearComponentSession() noexcept;
  [[nodiscard]] std::unique_ptr<ComponentViewportSession>
  takeComponentSession() noexcept;
  void restoreComponentSession(
      std::unique_ptr<ComponentViewportSession> session);
  // Replaces only the product-side component session while the ordinary Part
  // presentation remains installed. This is the hybrid root-component
  // authoring path: child occurrences stay pickable/draggable without
  // discarding the active root Document rendered by the Part path.
  void replaceComponentSessionDuringEdit(
      std::unique_ptr<ComponentViewportSession> session) noexcept;
  [[nodiscard]] const ComponentViewportSession* componentSession()
      const noexcept { return componentSession_.get(); }
  void setComponentEditContext(
      std::shared_ptr<const ComponentViewportScene> scene,
      ComponentInstanceAddress activeComponent);
  void clearComponentEditContext() noexcept;
  [[nodiscard]] bool componentEditContextActive() const noexcept {
    return componentEditContextScene_ && componentEditActiveComponent_;
  }
  [[nodiscard]] bool componentEditContextGhostsInactiveComponents()
      const noexcept {
    return componentEditActiveComponent_ &&
           !componentEditActiveComponent_->rootInstance();
  }
  void setComponentRootVisible(bool visible) noexcept;
  [[nodiscard]] bool componentRootVisible() const noexcept {
    return componentRootVisible_;
  }
  [[nodiscard]] bool setSelectedComponent(
      const std::optional<ComponentInstanceSelection>& component,
      bool notify = false) noexcept;
  [[nodiscard]] bool setHoveredComponent(
      const std::optional<ComponentInstanceSelection>& component,
      bool notify = false) noexcept;
  [[nodiscard]] const std::optional<ComponentInstanceSelection>&
  selectedComponentView() const noexcept;
  [[nodiscard]] const std::optional<ComponentPlacementPreview>&
  componentPlacementPreview() const noexcept {
    return componentPlacementPreview_;
  }
  [[nodiscard]] bool componentPlacementDragArmed() const noexcept {
    return componentPlacementDragArm_.has_value();
  }
  [[nodiscard]] ComponentFaceRayQueryResult componentFaceAt(
      QPointF logicalPosition) const noexcept;
  [[nodiscard]] ComponentEdgeRayQueryResult componentEdgeAt(
      QPointF logicalPosition,
      double radiusLogicalPixels = 9.0) const noexcept;
  void installAssemblySession(std::unique_ptr<AssemblyViewportSession> session);
  // Visibility keeps the camera and renderer caches intact.  The candidate is
  // fully published and selection-prepared before this no-throw swap.
  void installAssemblyVisibilitySession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  void installAssemblyGroundingSession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  void installAssemblySuppressionSession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  void installAssemblyDuplicateSession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  void installAssemblyRemoveSession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  void installAssemblyDefinitionSession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  [[nodiscard]] bool installPreparedAssemblyIsolation(
      PreparedAssemblyIsolationState&& prepared) noexcept;
  void clearAssemblySession() noexcept;
  [[nodiscard]] std::unique_ptr<AssemblyViewportSession>
  suspendAssemblySession() noexcept;
  void resumeAssemblySession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  [[nodiscard]] const AssemblyViewportSession* assemblySession() const noexcept {
    return assemblySession_.get();
  }
  void setAssemblyMatePickMode(bool active) noexcept;
  [[nodiscard]] bool assemblyMatePickMode() const noexcept {
    return assemblyMatePickMode_;
  }
  [[nodiscard]] bool installAssemblyMatePreview(
      std::shared_ptr<const AssemblyViewportScene> scene) noexcept;
  void clearAssemblyMatePreview() noexcept;
  [[nodiscard]] const std::shared_ptr<const AssemblyViewportScene>&
  assemblyMatePreviewScene() const noexcept {
    return assemblyMatePreviewScene_;
  }
  [[nodiscard]] ViewportNavigationState navigationState() const noexcept {
    return {yaw_, pitch_, zoom_, cameraPan_};
  }
  void restoreNavigationState(ViewportNavigationState state) noexcept;
  [[nodiscard]] bool setSelectedAssemblyOccurrence(
      const std::optional<AssemblyOccurrenceSelection>& occurrence,
      bool notify = false) noexcept;
  void installPreparedAssemblySelection(
      PreparedAssemblySelectionState&& prepared) noexcept;
  // Non-owning immutable view.  In particular, noexcept availability and
  // semantic-state probes must not copy the OccurrencePath vector.
  [[nodiscard]] const std::optional<AssemblyOccurrenceSelection>&
  selectedAssemblyOccurrenceView() const noexcept;
  [[nodiscard]] bool prepareAssemblyPlacementPreview(
      const Document& source) noexcept;
  [[nodiscard]] bool setAssemblyPlacementPreviewTransform(
      RigidTransform3d placement) noexcept;
  void clearAssemblyPlacementPreview() noexcept;
  [[nodiscard]] const AssemblyPlacementPreview& assemblyPlacementPreview()
      const noexcept { return assemblyPlacementPreview_; }
  // Read-only renderer counters make no-upload publication contracts
  // observable without exposing OpenGL object names or renderer ownership.
  [[nodiscard]] RendererResourceDiagnostics rendererResourceDiagnostics()
      const noexcept { return renderer_.resourceDiagnostics(); }
  void setBox(BoxParameters parameters);
  void setBodyShape(ShapeFeature::ShapePtr shape,
                    BodyId bodyId = kInvalidBodyId,
                    FeatureId featureId = kInvalidFeatureId);
  void setBodyShapes(std::vector<BodyViewShape> shapes);
  [[nodiscard]] static PreparedBodyDisplayResult prepareBodyDisplay(
      std::vector<BodyViewShape> shapes, ViewportMeshQuality quality,
      ProductPresentationEpoch epoch = {}, DefinitionId definitionId = {},
      BodyDisplayPrepareFailurePoint failurePoint =
          BodyDisplayPrepareFailurePoint::None) noexcept;
  [[nodiscard]] bool installPreparedBodyDisplay(
      PreparedBodyDisplay&& prepared) noexcept;
  [[nodiscard]] std::size_t displayedBodyCount() const noexcept {
    return bodyDisplayMeshes_.size();
  }
  void setSketch(const sketch::Sketch& sketch);
  void setSketch(const sketch::Sketch& sketch,
                 const SketchPlacement& placement);
  void setSolidSketch(const sketch::Sketch& sketch,
                      const SketchPlacement& placement);
  void setSolidVisible(bool visible);
  void setSolidSupport(const QString& supportName);
  void setSketchVisible(bool visible);
  void addSketch(SketchId sketchId, const sketch::Sketch& sketch,
                 const QString& supportName,
                 const SketchPlacement& placement);
  void updateSketch(std::size_t index, const sketch::Sketch& sketch,
                    const QString& supportName,
                    const SketchPlacement& placement);
  void removeSketch(std::size_t index);
  void replaceSketchPresentations(
      std::vector<SketchPresentationSnapshot> sketches);
  void setReferenceImages(const std::vector<ReferenceImage>& images);
  void setSketchVisible(std::size_t index, bool visible);
  void setOriginVisible(bool visible);
  void setBasePlaneVisible(int plane, bool visible);
  [[nodiscard]] bool sketchVisible() const noexcept;
  [[nodiscard]] bool sketchVisible(std::size_t index) const noexcept;
  [[nodiscard]] bool originVisible() const noexcept;
  [[nodiscard]] bool basePlaneVisible(int plane) const noexcept;
  [[nodiscard]] bool sketchPlaneSelectionActive() const noexcept;
  void resetScene();
  // Used only while an assembly session is suspended for embedded-definition
  // editing. It clears Part/tool state without cancelling renderer retention
  // of the hidden assembly's resident source keys.
  void resetScenePreservingSuspendedAssemblyResources();
  void beginSketchPlaneSelection();
  void beginImagePlaneSelection();
  void beginMirrorBodySelection();
  void beginMirrorPlaneSelection();
  void showMirrorPlaneSelection(int planeIndex);
  void showMirrorFaceSelection(const FaceReference& face);
  [[nodiscard]] bool mirrorBodySelectionActive() const noexcept;
  [[nodiscard]] bool mirrorPlaneSelectionActive() const noexcept;
  void beginMoveBodySelection();
  void beginJoinBodiesSelection();
  void showMovePreview();
  [[nodiscard]] bool moveBodySelectionActive() const noexcept;
  void beginLinearPatternBodySelection();
  void beginLinearPatternAxisSelection();
  void showLinearPatternAxisSelection(int axisIndex);
  [[nodiscard]] bool linearPatternBodySelectionActive() const noexcept;
  [[nodiscard]] bool linearPatternAxisSelectionActive() const noexcept;
  void beginCircularPatternBodySelection();
  void beginCircularPatternAxisSelection();
  void showCircularPatternAxisSelection(int axisIndex);
  [[nodiscard]] bool circularPatternBodySelectionActive() const noexcept;
  [[nodiscard]] bool circularPatternAxisSelectionActive() const noexcept;
  void beginDraftFaceSelection();
  void beginDraftAxisSelection();
  void showDraftAxisSelection(int axisIndex);
  void showDraftEdgeAxisSelection(const EdgeReference& edge);
  [[nodiscard]] bool draftFaceSelectionActive() const noexcept;
  [[nodiscard]] bool draftAxisSelectionActive() const noexcept;
  void beginRulerMeasurement();
  void cancelRulerMeasurement();
  [[nodiscard]] bool rulerMeasurementActive() const noexcept;
  [[nodiscard]] std::optional<double> rulerDistanceMm() const noexcept;
  void beginExtrusionSurfaceSelection();
  void beginRevolveAxisSelection(std::size_t sketchIndex);
  void showExtrusionManipulator(double lengthMm);
  void hideExtrusionManipulator();
  // Clears every piece of the legacy QPainter extrusion preview (sketch
  // extrude) so it can never overlap the modern Face Extrude native preview.
  void clearLegacyExtrusionPreview();
  // Clears every transient modeling interaction without emitting the Escape
  // notification. MainWindow uses this before starting another tool so a
  // legacy pick mode/HUD cannot survive alongside the new tool's UI.
  void resetToolInteraction();
  void resetSceneImpl(bool preserveSuspendedAssemblyResources);
  // True when the active tool manipulator is a directional (Face Extrude)
  // linear manipulator; paintGL suppresses the legacy prism/arrow/spinbox in
  // that case so the two preview generations can never render simultaneously.
  [[nodiscard]] bool legacyExtrusionPreviewSuppressed() const noexcept;
  [[nodiscard]] bool extrusionManipulatorVisible() const noexcept;
  void setExtrusionPreviewLength(double lengthMm);
  [[nodiscard]] bool hasSelectedFace() const noexcept;
  [[nodiscard]] QString selectedFaceName() const;
  [[nodiscard]] std::optional<std::size_t> selectedBodyFaceIndex() const noexcept;
  [[nodiscard]] std::optional<FaceReference> selectedBodyFace() const;
  [[nodiscard]] std::vector<FaceReference> selectedBodyFaces() const;
  void setSelectedBodyFaces(const std::vector<FaceReference>& faces);
  void setFaceMultiSelectionMode(bool enabled) noexcept;
  [[nodiscard]] bool faceMultiSelectionMode() const noexcept;
  [[nodiscard]] std::optional<EdgeReference> selectedBodyEdge() const noexcept;
  [[nodiscard]] std::vector<EdgeReference> selectedBodyEdges() const;
  void setSelectedBodyEdges(const std::vector<EdgeReference>& edges);
  void setEdgeMultiSelectionMode(bool enabled) noexcept;
  [[nodiscard]] bool edgeMultiSelectionMode() const noexcept;
  [[nodiscard]] const std::vector<BodyId>& selectedBodies() const noexcept;
  void setSelectedBodies(std::vector<BodyId> ids);
  // Face ordinals passed to the renderer as the selected-faces set. When whole
  // bodies are selected this is the union of every selected body's face
  // ordinals (so a body selection renders with the standard selected-face
  // tint); otherwise it is the sub-element face selection.
  [[nodiscard]] std::vector<std::size_t> effectiveSelectedFaceIndices() const;
  [[nodiscard]] std::vector<std::size_t> effectiveHoveredFaceIndices() const;
  [[nodiscard]] std::optional<std::size_t> hoveredBodyEdgeIndex() const noexcept;
  void setSelectionFilter(SelectionFilter filter) noexcept;
  [[nodiscard]] SelectionFilter selectionFilter() const noexcept;
  [[nodiscard]] bool marqueeActive() const noexcept;
  void setToolPreviewShape(BodyId bodyId, FeatureId featureId,
                           ShapeFeature::ShapePtr shape);
  // ReplaceSource normally hides only bodyId. Multi-source operations such as
  // Join explicitly provide both inputs; invalid/NewBody previews replace none.
  void setToolPreviewReplacedBodies(std::vector<BodyId> bodyIds);
  void setToolCutPreviewShape(ShapeFeature::ShapePtr shape);
  void setToolPreviewPresentation(
      ToolPreviewPresentation presentation) noexcept;
  void clearToolPreviewShape();
  void setToolManipulator(const LinearToolManipulator& manipulator);
  [[nodiscard]] double toolManipulatorHudValue() const noexcept;
  [[nodiscard]] const std::optional<LinearToolManipulator>&
  linearToolManipulator() const noexcept { return toolManipulator_; }
  void setTranslationToolManipulator(
      const TranslationToolManipulator& manipulator);
  void setReferenceImageManipulator(const ReferenceImage& image);
  [[nodiscard]] bool referenceImageManipulatorActive() const noexcept {
    return referenceImageManipulatorActive_;
  }
  [[nodiscard]] const std::optional<TranslationToolManipulator>&
  translationToolManipulator() const noexcept {
    return translationToolManipulator_;
  }
  void setAngularToolManipulator(const AngularToolManipulator& manipulator);
  [[nodiscard]] const std::optional<AngularToolManipulator>&
  angularToolManipulator() const noexcept { return angularToolManipulator_; }
  void clearToolManipulator();
  // CAD keyboard traversal targets only numeric fields rendered in the
  // viewport. Right-hand tool panels may call this to hand Tab into the HUD.
  [[nodiscard]] bool focusToolParameterField(bool backward = false);
  // Seeds the tool-manipulator drag from an arbitrary scene point and world
  // axis so a direct profile press can continue into the same held-mouse drag.
  void seedToolManipulatorDrag(QPointF scenePosition, Vector3d worldAxis);
  void fitAll();
  // Docked tool panels resize the central viewport. Preserve the real world
  // origin at its current screen position for that single layout change so a
  // preview cannot appear to rotate or flip when its panel opens/closes.
  void preserveWorldOriginOnNextResize();
  void viewTop();
  void viewBottom();
  void viewFront();
  void viewBack();
  void viewRight();
  void viewLeft();
  void viewIsometric();
  // Smoothly matches the 3D camera to the already prepared Sketcher view.
  // The placement origin, screen position and scale converge together so the
  // coordinate centre does not jump when the workspace widget changes.
  void animateToSketchView(CameraOrientation target, Point3d placementOrigin,
                           QPointF targetOriginScreen,
                           double targetPixelsPerMm,
                           std::function<void()> finished);
  // Restores the same global camera when leaving Sketcher.  The 3D viewport
  // is hidden at this point, so this is intentionally immediate: its first
  // visible frame, the ViewCube and any tool preview must all agree.
  void adoptSketchView(CameraOrientation target, Point3d placementOrigin,
                       QPointF targetOriginScreen,
                       double targetPixelsPerMm);
  void setDisplayMode(ViewportDisplayMode mode);
  void setMeshQuality(ViewportMeshQuality quality);
  [[nodiscard]] ViewportDisplayMode displayMode() const noexcept;
  [[nodiscard]] ViewportMeshQuality meshQuality() const noexcept;
  [[nodiscard]] float cameraYawDegrees() const noexcept;
  [[nodiscard]] float cameraPitchDegrees() const noexcept;
  [[nodiscard]] Vector3d cameraViewDirection() const noexcept;
  [[nodiscard]] Vector3d cameraScreenUpDirection() const noexcept;
  [[nodiscard]] const sketch::Sketch& extrusionCandidateSketch() const noexcept;
  [[nodiscard]] std::size_t selectedProfileRegionCount() const noexcept;
  [[nodiscard]] QRectF extrusionPreviewBaseBounds() const noexcept;
  [[nodiscard]] QRectF extrusionHoverBounds() const noexcept;
  [[nodiscard]] QString extrusionCandidateSupport() const;
  [[nodiscard]] std::size_t extrusionCandidateSketchIndex() const noexcept;
  [[nodiscard]] bool extrusionCandidateOnBodyCap() const noexcept;
  [[nodiscard]] const sketch::Sketch& solidSketch() const noexcept;
  [[nodiscard]] QString solidSupport() const;
  [[nodiscard]] QPointF bodyPosition() const noexcept;
  void setBodyPosition(QPointF position);
  void setWorkGridPlacement(const SketchPlacement& placement);
  void resetWorkGridPlacement();
  void setWorkGridVisible(bool visible);
  [[nodiscard]] bool workGridVisible() const noexcept;

 signals:
  void selectionChanged(const QString& description);
  void assemblyMateFacePicked(const AssemblyFaceRayHit& hit);
  void interactionCancelled(ViewportCancelReason reason);
  void sketchPlanePicked(const SketchPlanePick& pick);
  void imagePlanePicked(const SketchPlanePick& pick);
  void mirrorBodyPicked(BodyId bodyId);
  void mirrorPlanePicked(int planeIndex);
  void mirrorPlaneFacePicked(const FaceReference& face);
  void moveBodyPicked(BodyId bodyId);
  void linearPatternBodyPicked(BodyId bodyId);
  void linearPatternAxisPicked(int axisIndex);
  void circularPatternBodyPicked(BodyId bodyId);
  void circularPatternAxisPicked(int axisIndex);
  void draftAxisPicked(int axisIndex);
  void draftEdgeAxisPicked(const EdgeReference& edge);
  void extrusionSourcePicked(const ExtrusionSourcePick& pick);
  void extrusionPreviewLengthChanged(double lengthMm);
  void bodyMoveCommitted(QPointF previous, QPointF current);
  void bodyEdgeSelectionChanged();
  void bodyFaceSelectionChanged();
  // Published whenever a whole-body (model-level) selection is committed via
  // setSelectedBodies; MainWindow consumption is intentionally deferred.
  void bodiesSelected(const std::vector<BodyId>& ids);
  void toolManipulatorValueChanged(double valueMm);
  void translationToolManipulatorValueChanged(int axisIndex, double valueMm);
  // parameterIndex: 0/1/2 = local X/Y/Z offset in millimetres, 3 = scale in
  // percent. Continuous drag/editor updates are committed by MainWindow when
  // the gesture or HUD edit finishes.
  void referenceImageParameterChanged(int parameterIndex, double value);
  void angularToolManipulatorValueChanged(double angleDeg);
  void toolManipulatorDragFinished();
  void extrusionManipulatorDragFinished();
  // Emitted after a HUD field commit via Enter (after the value has been routed
  // to the session preview). MainWindow uses it to perform the active tool's
  // Accept without re-interpreting the already-committed value.
  void toolParameterCommitted();
  // Matches the Revolve axis combo data: 1/2 are sketch X/Y axes,
  // values >= 3 encode a sketch line id plus three.
  void revolveAxisPicked(qulonglong axisToken);
  // Emitted while Revolve is already waiting for an axis and the user
  // Ctrl-adds/removes another profile region. The candidate geometry and
  // sketch index are read through the public profile-selection accessors.
  void revolveProfileSelectionChanged(const ExtrusionSourcePick& pick);
  // Direct interaction: a visible closed sketch profile was pressed in normal
  // mode. MainWindow maps the display index to a SketchId and starts the
  // sketch Extrude session (contract 4).
  void directProfilePicked(const ExtrusionSourcePick& pick);
  void rulerActiveChanged(bool active);
  void rulerPointPicked(int selectedPointCount);
  void rulerMeasurementChanged(double distanceMm);
  void assemblyOccurrenceSelected(
      const solidar::AssemblyOccurrenceSelection& occurrence);
  void assemblyOccurrenceSelectionCleared(
      const solidar::AssemblyScenePublicationHandle& publication);
  void componentSelected(
      const solidar::ComponentInstanceSelection& component);
  void componentSelectionCleared(
      const solidar::ComponentScenePublicationHandle& publication);
  void componentHovered(
      const solidar::ComponentInstanceSelection& component);
  void componentHoverCleared(
      const solidar::ComponentScenePublicationHandle& publication);
  void componentFacePicked(const solidar::ComponentFaceRayHit& hit);
  void componentEdgePicked(const solidar::ComponentEdgeRayHit& hit);
  void componentPlacementDragRequested(
      solidar::ComponentPlacementDragRequest request);

 protected:
  bool event(QEvent* event) override;
  void initializeGL() override;
  void paintGL() override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  struct ComponentNotificationExpectation final {
    const ComponentViewportSession* session{};
    const ComponentViewportScene* scene{};
    ComponentScenePublicationHandle publication;
    std::uint64_t ownershipRevision{};
  };

  [[nodiscard]] std::optional<ComponentNotificationExpectation>
  componentNotificationExpectation() const noexcept;
  [[nodiscard]] bool matchesComponentNotificationExpectation(
      const ComponentNotificationExpectation& expected) const noexcept;
  void installAssemblyOccurrenceSession(
      std::unique_ptr<AssemblyViewportSession> session) noexcept;
  friend class MainWindowUndoTestAccess;

  void animateOrientation(CameraOrientation target);
  void paintComponentFrame(QPainter& painter, const ThemeColors& theme);
  void paintAssemblyFrame(QPainter& painter, const ThemeColors& theme);
  [[nodiscard]] ViewportCameraState componentCamera() const noexcept;
  [[nodiscard]] ViewportCameraState assemblyCamera() const noexcept;
  [[nodiscard]] bool handleComponentPress(QMouseEvent* event);
  [[nodiscard]] bool handleAssemblyPress(QMouseEvent* event);
  [[nodiscard]] bool updateComponentHover(QPointF logicalPosition);
  [[nodiscard]] bool rootComponentAuthoringActive() const noexcept;
  void armComponentPlacementDrag(
      const ComponentInstanceSelection& selection,
      QPointF logicalPosition) noexcept;
  [[nodiscard]] bool updateComponentPlacementDrag(
      QPointF logicalPosition) noexcept;
  void cancelComponentPlacementDrag() noexcept;
  void updateAssemblyHover(QPointF logicalPosition);
  void setStandardView(StandardView view);
  void clearCubeHover();
  QVariantAnimation* orientationAnimation_{nullptr};
  ViewCubeHit cubeHover_, cubePressed_;
  Qt::CursorShape cursorBeforeCube_{Qt::ArrowCursor};
  void updateSketchPlaneHover(QPointF position);
  void updateToolBodyHover(QPointF position);
  void updateMirrorPlaneHover(QPointF position);
  void updateExtrusionHover(QPointF position);
  [[nodiscard]] qulonglong revolveAxisTokenAt(QPointF scenePosition) const;
  [[nodiscard]] qulonglong principalAxisTokenAt(QPointF scenePosition) const;
  void pickFallbackBodyFace(QPointF position);
  void refreshSelectedExtrusionPolygon();
  [[nodiscard]] QPointF extrusionScreenOffset(double lengthMm) const;
  // Shared presentation helpers. A single layout/radius is computed once and
  // used by both the draw pass and the hit-test so the two can never diverge.
  struct AngularVisual {
    QPointF origin;
    Vector3d u{1.0, 0.0, 0.0};
    Vector3d v{0.0, 1.0, 0.0};
    Vector3d axis{0.0, 0.0, 1.0};
    double visualRadiusMm{};
  };
  [[nodiscard]] std::optional<ManipulatorLayoutResult> toolManipulatorLayout()
      const;
  [[nodiscard]] std::array<ManipulatorLayoutResult, 3>
  translationManipulatorLayouts() const;
  [[nodiscard]] std::optional<AngularVisual> angularVisual() const;
  void rebuildSelectedExtrusionSketch();
  void updateBodyHover(QPointF position, bool exact = false);
  [[nodiscard]] bool rebuildBodyDisplay(
      const std::vector<BodyViewShape>& shapes, bool clearSelection,
      ViewportMeshQuality quality);
  [[nodiscard]] Point3d displayedBodyCenter() const noexcept;
  [[nodiscard]] double displayedBodyDiagonal() const noexcept;
  [[nodiscard]] bool hasDisplayedBodyTriangles() const noexcept;
  [[nodiscard]] QRectF projectedDisplayedBodyBounds() const;
  [[nodiscard]] const ProjectedPickingScene& pickingScene() const;
  [[nodiscard]] std::vector<PickingMeshInput> pickingMeshInputs() const;
  void releaseRendererResources() noexcept;
  void synchronizeRendererResources();
  void scheduleHover(QPointF viewPosition);
  [[nodiscard]] bool flushPendingHover(
      std::optional<QPointF> exactPosition = std::nullopt);
  void invalidatePendingHover();
  [[nodiscard]] bool processHoverAt(QPointF viewPosition,
                                    bool exact = false);
  void selectInRect(const QRectF& rect, bool additive, bool singleOnly = false);
  void cancelMarquee();
  void cancelActiveInteraction();
  // Clears only whole-body selection and publishes bodiesSelected({}) when the
  // state actually changes. Face/edge state is intentionally left untouched.
  void clearWholeBodySelection() noexcept;
  void commitEdgeSelection(std::size_t globalIndex, bool toggle);
  void commitFaceSelection(std::size_t globalIndex, bool toggle);
  [[nodiscard]] std::optional<EdgeReference> edgeReferenceForGlobalIndex(
      std::size_t index) const;
  [[nodiscard]] std::optional<FaceReference> faceReferenceForGlobalIndex(
      std::size_t index) const;
  enum class PickMode {
    None,
    SketchPlane,
    ImagePlane,
    ExtrusionSurface,
    RevolveAxis,
    MirrorBody,
    MirrorPlane,
    MirrorPreview,
    MoveBody,
    MovePreview,
    JoinBodies,
    LinearPatternBody,
    LinearPatternAxis,
    LinearPatternPreview,
    CircularPatternBody,
    CircularPatternAxis,
    CircularPatternPreview,
    DraftFace,
    DraftAxis,
    DraftPreview,
    Ruler
  };
  BoxParameters box_;
  ShapeFeature::ShapePtr bodyShape_;
  std::vector<PreparedBodyDisplayMesh> bodyDisplayMeshes_;
  BodyRenderMeshCache bodyMeshCache_;
  // Legacy callers do not own a model ShapeRevision. Give every explicit
  // replacement a fresh structural revision so reusing and mutating the same
  // shared_ptr can never hit a stale CPU/GPU/projection cache entry.
  ShapeRevision fallbackBodyShapeRevision_{1};
  mutable ProjectedPickingScene pickingScene_;
  Point3d displayedBodyCenter_{};
  double displayedBodyDiagonal_{};
  ShapeFeature::ShapePtr toolPreviewShape_;
  BodyRenderMesh toolPreviewRenderMesh_;
  // Presentation revisions are owned by Viewport instead of BodyRenderMesh:
  // each transaction builds a fresh mesh whose local revision starts at one.
  // These monotonic values keep a stable preview GPU slot from mistaking new
  // geometry for the previously uploaded transaction.
  std::uint64_t toolPreviewPresentationRevision_{};
  ShapeFeature::ShapePtr toolCutPreviewShape_;
  BodyRenderMesh toolCutPreviewRenderMesh_;
  std::uint64_t toolCutPreviewPresentationRevision_{};
  ToolPreviewPresentation toolPreviewPresentation_{
      ToolPreviewPresentation::OverlaySourceSelection};
  ViewportRenderer renderer_;
  std::unique_ptr<ComponentViewportSession> componentSession_;
  std::shared_ptr<const ComponentViewportScene> componentEditContextScene_;
  std::optional<ComponentInstanceAddress> componentEditActiveComponent_;
  bool componentRootVisible_{true};
  std::uint64_t componentSessionOwnershipRevision_{};
  struct ComponentPlacementDragArm final {
    ComponentScenePublicationHandle publication;
    ComponentInstanceAddress component;
    QPointF pressLogical;
    ViewportCameraState camera;
    std::uint64_t ownershipRevision{};
  };
  std::optional<ComponentPlacementDragArm> componentPlacementDragArm_;
  std::optional<ComponentPlacementPreview> componentPlacementPreview_;
  std::unique_ptr<AssemblyViewportSession> assemblySession_;
  std::shared_ptr<const AssemblyViewportScene> assemblyMatePreviewScene_;
  bool assemblyMatePickMode_{};
  AssemblyPlacementPreview assemblyPlacementPreview_;
  QMetaObject::Connection contextAboutToBeDestroyedConnection_;
  bool destructionStarted_{};
  ViewportDisplayMode displayMode_{ViewportDisplayMode::ShadedWithEdges};
  ViewportMeshQuality meshQuality_{ViewportMeshQuality::Normal};
  BodyId toolPreviewBodyId_{kInvalidBodyId};
  FeatureId toolPreviewFeatureId_{kInvalidFeatureId};
  std::vector<BodyId> toolPreviewReplacedBodyIds_;
  std::vector<BodyViewShape> bodyViewShapes_;
  std::vector<PreparedBodyTopologyRange> bodyTopologyRanges_;
  BodyId bodyId_{kInvalidBodyId};
  FeatureId bodyFeatureId_{kInvalidFeatureId};
  sketch::Sketch sketch_;
  SketchPlacement sketchPlacement_{SketchPlacement::xy()};
  sketch::Sketch solidSketch_;
  SketchPlacement solidSketchPlacement_{SketchPlacement::xy()};
  bool solidVisible_{false};
  QString solidSupportName_{QStringLiteral("XY")};
  bool sketchVisible_{true};
  struct DisplaySketch {
    SketchId sketchId{kInvalidSketchId};
    sketch::Sketch geometry;
    QString supportName;
    SketchPlacement placement;
    bool visible{true};
  };
  std::vector<DisplaySketch> displaySketches_;
  struct DisplayReferenceImage {
    ReferenceImage model;
    QImage pixels;
  };
  std::vector<DisplayReferenceImage> referenceImages_;
  bool originVisible_{true};
  bool basePlanesVisible_[3]{false, false, false};
  PickMode pickMode_{PickMode::None};
  int selectedFace_{-1};
  std::vector<std::size_t> selectedBodyFaceIndices_;
  std::vector<FaceReference> selectedBodyFaceReferences_;
  bool faceMultiSelectionMode_{false};
  std::size_t hoveredBodyFaceIndex_{static_cast<std::size_t>(-1)};
  std::size_t hoveredBodyEdgeIndex_{static_cast<std::size_t>(-1)};
  std::size_t selectedBodyEdgeIndex_{static_cast<std::size_t>(-1)};
  std::vector<std::size_t> selectedBodyEdgeIndices_;
  std::vector<EdgeReference> selectedBodyEdgeReferences_;
  bool edgeMultiSelectionMode_{false};
  // Transient whole-body (model-level) selection published via bodiesSelected.
  std::vector<BodyId> selectedBodyIds_;
  BodyId hoveredToolBodyId_{kInvalidBodyId};
  SelectionFilter selectionFilter_{SelectionFilter::Any};
  int selectedBasePlane_{-1};
  int selectedVertex_{-1};
  bool selectedOrigin_{false};
  QPolygonF extrusionHoverPolygon_;
  QPainterPath extrusionHoverPath_;
  QPolygonF selectedExtrusionPolygon_;
  std::vector<QPolygonF> selectedExtrusionPolygons_;
  std::vector<QPainterPath> selectedExtrusionPaths_;
  std::vector<sketch::Sketch> selectedExtrusionRegionSketches_;
  sketch::Sketch hoveredExtrusionSketch_;
  sketch::Sketch selectedExtrusionSketch_;
  SketchPlacement hoveredExtrusionPlacement_{SketchPlacement::xy()};
  SketchPlacement selectedExtrusionPlacement_{SketchPlacement::xy()};
  QString hoveredExtrusionSupport_;
  QString selectedExtrusionSupport_;
  QString hoveredExtrusionSurface_;
  std::size_t hoveredExtrusionSketchIndex_{static_cast<std::size_t>(-1)};
  std::size_t selectedExtrusionSketchIndex_{static_cast<std::size_t>(-1)};
  std::size_t revolveAxisSketchIndex_{static_cast<std::size_t>(-1)};
  qulonglong hoveredRevolveAxisToken_{0};
  int selectedPatternAxis_{-1};
  std::optional<EdgeReference> selectedDraftAxisEdge_;
  QDoubleSpinBox* extrusionLengthEditor_{nullptr};
  ToolParameterHud* toolParameterHud_{nullptr};
  double toolHudMinimum_{};
  double toolHudMaximum_{};
  std::string toolHudParameterId_;
  QPointF extrusionManipulatorAnchor_;
  double extrusionPreviewLengthMm_{25.0};
  bool extrusionManipulatorVisible_{false};
  bool selectedExtrusionBodyFace_{false};
  bool hoveredExtrusionOnBodyCap_{false};
  bool selectedExtrusionOnBodyCap_{false};
  bool hoveredExtrusionReverse_{false};
  bool selectedExtrusionReverse_{false};
  std::optional<LegacySolidFacePick> hoveredLegacySolidFace_;
  std::optional<LegacySolidFacePick> selectedLegacySolidFace_;
  bool draggingExtrusionHandle_{false};
  std::optional<LinearToolManipulator> toolManipulator_;
  std::optional<TranslationToolManipulator> translationToolManipulator_;
  bool referenceImageManipulatorActive_{false};
  std::optional<AngularToolManipulator> angularToolManipulator_;
  ManipulatorStyle manipulatorStyle_;
  bool draggingToolManipulator_{false};
  bool draggingTranslationToolManipulator_{false};
  int activeTranslationAxis_{-1};
  std::optional<LinearDragSnapshot> linearDragSnapshot_;
  bool draggingAngularToolManipulator_{false};
  bool panningView_{false};
  QPointF cameraPan_;
  std::optional<QPointF> pendingResizeOriginScreen_;
  bool draggingBody_{false};
  QPointF bodyDragStart_;
  bool marqueeActive_{false};
  QPointF marqueeStart_{};
  QPointF marqueeCurrent_{};
  bool marqueeAdditive_{false};
  float offsetX_{0.0F};
  float offsetY_{0.0F};
  QPoint lastMousePosition_;
  float yaw_{-35.0F};
  float pitch_{25.0F};
  float zoom_{1.0F};
  bool sketchViewTransitionActive_{};
  ViewportRuler ruler_;
  QTimer* hoverFrameTimer_{nullptr};
  std::optional<QPointF> pendingHoverPosition_;
  std::uint64_t sceneGeneration_{0};
  std::uint64_t pendingHoverGeneration_{0};
  SketchPlacement workGridPlacement_{SketchPlacement::xy()};
  bool workGridVisible_{true};
};

}  // namespace solidar

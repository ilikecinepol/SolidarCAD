#pragma once

#include <QMainWindow>
#include <QRectF>
#include <optional>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "ui/PartDesignHistory.h"
#include "ui/PreviewUpdateCoordinator.h"
#include "ui/ViewportInteractionTypes.h"
#include "ui/application/ModelCommandHistory.h"
#include "ui/application/PartDesignCoordinator.h"
#include "ui/application/ProjectApplicationService.h"
#include "sketch/Sketch.h"
#include "app/AppSettings.h"

class QTreeWidget;
class QCloseEvent;
class QStackedWidget;
class QAction;
class QHBoxLayout;
class QDockWidget;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QComboBox;
class QCheckBox;
class QLabel;
class QScrollArea;
class QTreeWidget;

namespace solidar {

class Viewport;
class SketchCanvas;
class DrawingSheetView;
class SketchRibbon;
class ModelRibbon;
class ToolParametersPanel;
class HistoryTimelineWidget;
class MainWindow final : public QMainWindow {
  Q_OBJECT

  friend class MainWindowUndoTestAccess;

 public:
  explicit MainWindow(AppSettings& settings, QWidget* parent = nullptr);
  ~MainWindow() override;
  void setProjectPath(const QString& path);
  bool loadProject(const QString& path, QString* error = nullptr);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  struct SketchViewEntry;
  using HistorySelectionState = EditorCommittedState;
  enum class DocumentReplacementOrigin { OpenProject, ImportedStep };
  struct ModernSolidPresentation {
    const ShapeFeature* owner{};
    ShapeFeature::ShapePtr shape;
    std::shared_ptr<const TopologyIndex> topologyIndex;
  };
  void buildUi();
  void buildMenus();
  void applyActivePartDesignTool();
  void applyPartDesignUiEffect(PartDesignUiEffect);
  [[nodiscard]] bool applyPartDesignBeginResult(
      PartDesignTransitionOutcome, PartDesignToolKind);
  void openSettings();
  void createProject();
  void openProject();
  void saveProject();
  [[nodiscard]] bool confirmProjectReplacement();
  void importStep();
  void applyImportedDocument(Document staged, const QString& importedName);
  void commitDocumentReplacement(StagedApplicationDocument staged,
                                 DocumentReplacementOrigin origin,
                                 const QString& pathOrLabel);
  void exportStep();
  void exportStl();
  void updateFromSketch(double widthMm, double heightMm);
  void finishSketch();
  void extrudeSketch();
  void updateAutomaticExtrudeOperation();
  void scheduleAutomaticExtrudeOperation();
  [[nodiscard]] std::optional<ExtrudeOperation>
  detectAutomaticExtrudeOperation() const;
  void normalizeExtrusionDistance();
  void cancelLegacyExtrusion();
  void createPocket();
  void createRevolve();
  void updateRevolveToolPreview();
  void acceptRevolveTool();
  void cancelRevolveTool();
  void rebuildRevolveAxisChoices();
  void updateRevolveProfileSelection(const ExtrusionSourcePick& pick);
  void createFillet();
  void updateFilletToolPreview();
  void acceptFilletTool();
  void cancelFilletTool();
  void createChamfer();
  void createJoinBodies();
  void updateJoinBodiesToolPreview();
  void acceptJoinBodiesTool();
  void cancelJoinBodiesTool();
  void createMirror();
  void updateMirrorToolPreview();
  void acceptMirrorTool();
  void cancelMirrorTool();
  void createMove();
  void updateMoveToolPreview();
  void acceptMoveTool();
  void cancelMoveTool();
  void createLinearPattern();
  void updateLinearPatternToolPreview();
  void acceptLinearPatternTool();
  void cancelLinearPatternTool();
  void createCircularPattern();
  void updateCircularPatternToolPreview();
  void acceptCircularPatternTool();
  void cancelCircularPatternTool();
  void editPatternFeature(FeatureId featureId, FeatureEditorRoute route);
  void updateChamferToolPreview();
  void acceptChamferTool();
  void cancelChamferTool();
  void createShell();
  void updateShellToolPreview();
  void acceptShellTool();
  void cancelShellTool();
  void createDraft();
  void updateDraftToolPreview();
  void acceptDraftTool();
  void cancelDraftTool();
  void createFaceExtrude(const FaceReference& face);
  void createSketchExtrude(const ExtrusionSourcePick& pick);
  void updateFaceExtrudeToolPreview();
  void acceptFaceExtrudeTool();
  void cancelFaceExtrudeTool();
  void editFaceExtrudeStep(Body* body, ExtrudeFeature* extrude,
                           std::size_t extrudeIndex);
  void refreshBodyViewFromDocument(
      const std::vector<BodyId>& transientVisibleBodies = {});
  [[nodiscard]] bool hasCommittedModernSolid() const noexcept;
  [[nodiscard]] bool hasDisplayableModernSolid() const;
  [[nodiscard]] static std::optional<ModernSolidPresentation>
  resolveModernSolidPresentation(
      const Body& body, const ShapeFeature::ShapePtr& candidate,
      bool effectivelyVisible);
  [[nodiscard]] bool hasExportableModernSolid(
      QString* error = nullptr) const;
  [[nodiscard]] bool hasHistoricalLegacyExtrusion() const noexcept;
  void syncSketchPresentationFromDocument();
  void rebuildFeatureTree();
  void rebuildHistoryPanel();
  void applyHistoryPosition(int position);
  void editSketchStep(std::size_t index);
  void editSketchById(SketchId sketchId);
  void editHistoryFeature(BodyId bodyId, FeatureId featureId);
  void removeHistoryStep(const HistoryStep& step);
  void removeBody(BodyId bodyId);
  bool ensureHistoryAtEnd();
  void moveHistoryToEnd();
  [[nodiscard]] bool isHistoryAtEnd() const;
  void editExtrusionStep(BodyId bodyId, FeatureId featureId);
  void editPocketStep(BodyId bodyId, FeatureId featureId);
  bool acceptPocketTransition(
      SketchId profileId, double depthMm,
      std::optional<FeatureId> editingFeatureId = std::nullopt);
  void editFilletStep(BodyId bodyId, FeatureId featureId);
  void editChamferStep(BodyId bodyId, FeatureId featureId);
  void editJoinBodiesStep(BodyId bodyId, FeatureId featureId);
  void exportPdf();
  void printDrawing();
  void undoLastAction();
  void redoLastAction();
  void updateUndoAvailability();
  void updateSketchConstraintPanel();
  void resetTransientModelingUi();
  void resetTransientModelingPresentation();
  void schedulePreviewUpdate(BodyId bodyId, FeatureId featureId,
                             std::function<void()> rebuild,
                             std::function<void()> publish);
  void flushPreviewUpdate();
  void invalidatePreviewUpdates() noexcept;
  bool configureSketchEditContext(bool autoProjectSupportFace = false);
  void configureSketchSceneReferences(
      SketchId excludedSketchId = kInvalidSketchId);

  Document document_;
  ProjectApplicationService projectApplicationService_;
  AppSettings& settings_;
  Viewport* viewport_{nullptr};
  SketchCanvas* sketchCanvas_{nullptr};
  DrawingSheetView* drawingSheet_{nullptr};
  SketchRibbon* sketchRibbon_{nullptr};
  ModelRibbon* modelRibbon_{nullptr};
  QStackedWidget* ribbonStack_{nullptr};
  QStackedWidget* workspaceStack_{nullptr};
  QDockWidget* modelTreeDock_{nullptr};
  QDockWidget* historyDock_{nullptr};
  QTreeWidget* featureTree_{nullptr};
  // UI-only compatibility presentation for a loaded v1 extrusion. Presence
  // of this stable source ID is the entire state; it is consumed once a modern
  // Body is committed and can never disagree with a separate active flag.
  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId_;
  std::optional<ExtrusionSourcePick> selectedExtrusionSource_;
  QString currentSketchSupport_{QStringLiteral("XY")};
  SketchPlacement currentSketchPlacement_{SketchPlacement::xy()};
  std::optional<FaceReference> currentSketchFaceReference_;
  QAction* undoAction_{nullptr};
  QAction* redoAction_{nullptr};
  QWidget* historyContent_{nullptr};
  QHBoxLayout* historyLayout_{nullptr};
  QScrollArea* historyScroll_{nullptr};
  HistoryTimelineWidget* historyTimeline_{nullptr};
  std::vector<HistoryStep> historySteps_;
  QDockWidget* extrusionDock_{nullptr};
  QDockWidget* toolParametersDock_{nullptr};
  ToolParametersPanel* toolParametersPanel_{nullptr};
  // Mirror makes construction planes temporarily visible while picking and
  // previewing.  Keep the committed presentation separate from that tool UI
  // so Apply/Cancel/NoChange can restore the exact pre-tool state.
  std::optional<EditorPresentationState> mirrorPresentationBefore_;
  PartDesignCoordinator partDesignCoordinator_;
  std::unique_ptr<PreviewUpdateCoordinator> previewUpdates_;
  struct PreviewSourceSnapshot {
    std::uint64_t documentGeneration{};
    PartDesignRevisionToken toolRevision;
    BodyId bodyId{kInvalidBodyId};
    FeatureId featureId{kInvalidFeatureId};
    ShapeRevision shapeRevision{kInvalidShapeRevision};
    ShapeFeature::ShapePtr shape;
  };
  [[nodiscard]] PreviewSourceSnapshot capturePreviewSource(
      BodyId bodyId, FeatureId featureId) const;
  [[nodiscard]] bool previewSourceIsCurrent(
      const PreviewSourceSnapshot& snapshot) const;
  std::uint64_t previewDocumentGeneration_{1};
  struct AutomaticExtrudeDetectionKey {
    std::uint64_t documentGeneration{};
    BodyId bodyId{kInvalidBodyId};
    FeatureId featureId{kInvalidFeatureId};
    ShapeRevision shapeRevision{kInvalidShapeRevision};
    std::size_t sketchIndex{static_cast<std::size_t>(-1)};
    QString support;
    QRectF regionBounds;
    double distanceMm{};
    bool reversed{};
    bool operator==(const AutomaticExtrudeDetectionKey&) const = default;
  };
  std::optional<AutomaticExtrudeDetectionKey> automaticExtrudeDetectionKey_;
  std::uint64_t automaticExtrudeDetectionCount_{};
  QDockWidget* revolveDock_{nullptr};
  QComboBox* revolveProfileCombo_{nullptr};
  QLabel* revolveProfileSummary_{nullptr};
  QComboBox* revolveAxisCombo_{nullptr};
  QDoubleSpinBox* revolveAngleSpin_{nullptr};
  QComboBox* revolveOperationCombo_{nullptr};
  QCheckBox* revolveReverseCheck_{nullptr};
  QPushButton* revolveAcceptButton_{nullptr};
  QLabel* revolveStepHint_{nullptr};
  QDockWidget* mirrorDock_{nullptr};
  QLabel* mirrorBodyValue_{nullptr};
  QLabel* mirrorPlaneValue_{nullptr};
  QPushButton* mirrorBodySelectButton_{nullptr};
  QPushButton* mirrorPlaneSelectButton_{nullptr};
  QPushButton* mirrorAcceptButton_{nullptr};
  QLabel* mirrorStepHint_{nullptr};
  QDockWidget* moveDock_{nullptr};
  QLabel* moveBodyValue_{nullptr};
  QPushButton* moveBodySelectButton_{nullptr};
  QDoubleSpinBox* moveXSpin_{nullptr};
  QDoubleSpinBox* moveYSpin_{nullptr};
  QDoubleSpinBox* moveZSpin_{nullptr};
  QPushButton* moveAcceptButton_{nullptr};
  QLabel* moveStepHint_{nullptr};
  QDockWidget* linearPatternDock_{nullptr};
  QLabel* linearPatternBodyValue_{nullptr};
  QLabel* linearPatternAxisValue_{nullptr};
  QPushButton* linearPatternBodySelectButton_{nullptr};
  QPushButton* linearPatternAxisSelectButton_{nullptr};
  QDoubleSpinBox* linearPatternSpacingSpin_{nullptr};
  QSpinBox* linearPatternCountSpin_{nullptr};
  QComboBox* linearPatternOperationCombo_{nullptr};
  QPushButton* linearPatternAcceptButton_{nullptr};
  QLabel* linearPatternStepHint_{nullptr};
  QDockWidget* circularPatternDock_{nullptr};
  QLabel* circularPatternBodyValue_{nullptr};
  QLabel* circularPatternAxisValue_{nullptr};
  QPushButton* circularPatternBodySelectButton_{nullptr};
  QPushButton* circularPatternAxisSelectButton_{nullptr};
  QDoubleSpinBox* circularPatternAngleSpin_{nullptr};
  QSpinBox* circularPatternCountSpin_{nullptr};
  QComboBox* circularPatternOperationCombo_{nullptr};
  QPushButton* circularPatternAcceptButton_{nullptr};
  QLabel* circularPatternStepHint_{nullptr};
  QDoubleSpinBox* extrusionLengthSpin_{nullptr};
  QComboBox* extrusionOperationCombo_{nullptr};
  QCheckBox* extrusionReverseCheck_{nullptr};
  bool extrudeOperationManuallyChanged_{false};
  QDockWidget* sketchSettingsDock_{nullptr};
  QComboBox* sketchLineTypeCombo_{nullptr};
  QTreeWidget* sketchConstraintsList_{nullptr};
  int historyPosition_{0};
  struct SketchViewEntry {
    SketchId documentSketchId{kInvalidSketchId};
  };
  // Derived presentation ordering only. Committed geometry, placement and
  // support references are owned exclusively by Document and resolved by ID.
  std::vector<SketchViewEntry> sketchViews_;
  std::optional<std::size_t> editingSketchIndex_;
  [[nodiscard]] bool pushModelTransition(
      Document previous, HistorySelectionState previousSelection,
      std::optional<HistorySelectionState> committedAfter = std::nullopt);
  [[nodiscard]] bool pushPresentationTransition(
      HistorySelectionState before, HistorySelectionState after);
  void completeModelTransitionUi();
  [[nodiscard]] HistorySelectionState captureHistorySelection() const;
  [[nodiscard]] HistorySelectionState captureCommittedEndState() const;
  void restoreHistorySelection(const HistorySelectionState& state);
  void applyPresentationState(const EditorPresentationState& state);
  void synchronizeCommittedState(const HistorySelectionState& state);
  struct EditUiTransaction {
    HistorySelectionState before;
    BodyId bodyId{kInvalidBodyId};
    FeatureId featureId{kInvalidFeatureId};
    bool modified{};
  };
  void restoreCancelledEditUiTransaction();
  void finishEditUiTransaction() noexcept;
  std::optional<EditUiTransaction> editUiTransaction_;
  std::optional<EditUiTransaction> pendingNoChangeUiRestore_;
  bool suppressEditCancelRestore_{};
  ModelCommandHistory modelHistory_;
};

}  // namespace solidar

#pragma once

#include <QMainWindow>
#include <QModelIndex>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <array>
#include <optional>
#include <functional>
#include <memory>
#include <map>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/ProductDocument.h"
#include "ui/PartDesignHistory.h"
#include "ui/PreviewUpdateCoordinator.h"
#include "ui/ViewportInteractionTypes.h"
#include "ui/application/ModelCommandHistory.h"
#include "ui/application/ModelWorkspaceProjectionSuccessor.h"
#include "ui/application/PartDesignCoordinator.h"
#include "ui/application/ProjectApplicationService.h"
#include "ui/application/AssemblyCommandHistory.h"
#include "ui/application/AssemblyInsertToolSession.h"
#include "ui/application/AssemblyMateToolSession.h"
#include "ui/AssemblyWorkspace.h"
#include "ui/ComponentWorkspace.h"
#include "ui/ComponentPlacementDrag.h"
#include "sketch/Sketch.h"
#include "app/AppSettings.h"

class QTreeWidget;
class QTreeView;
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
class QShortcut;
class QInputDialog;
class QTreeWidget;

namespace solidar {

enum class EditorProjectKind { Part, AssemblyReadOnly };

enum class AssemblyInsertTeardownStep {
  InsertCallbacksInvalidated,
  InsertControllerCancelled,
  InsertSessionCancelled,
  PartDesignPrepared,
  PartDesignSessionsCancelled,
  GhostUiDetached,
  AssemblyHistoryCleared,
  OldWorkspaceDetached,
  ProductReplaced,
  WorkspaceInstalled,
};

class Viewport;
class AssemblyTreeModel;
class ComponentTreeModel;
class SketchCanvas;
class DrawingSheetView;
class SketchRibbon;
class ModelRibbon;
class ToolParametersPanel;
class HistoryTimelineWidget;
class MainWindow final : public QMainWindow {
  Q_OBJECT

  friend class MainWindowUndoTestAccess;
  friend class ComponentWorkspaceMainWindowTestAccess;

 public:
  explicit MainWindow(AppSettings& settings, QWidget* parent = nullptr);
  ~MainWindow() override;
  void setProjectPath(const QString& path);
  bool loadProject(const QString& path, QString* error = nullptr);
  void setAssemblyReplacementTraceObserverForTesting(
      std::function<void(AssemblyInsertTeardownStep, bool)> observer) {
    assemblyInsertTeardownObserver_ = std::move(observer);
  }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  class AssemblyProjectionSuccessor final
      : public ProductDocumentSuccessor {
   public:
    AssemblyProjectionSuccessor(
        MainWindow& owner,
        std::unique_ptr<PreparedAssemblyProjection>& prepared,
        std::unique_ptr<PreparedAssemblyProjection>& visibilityPrepared,
        std::unique_ptr<PreparedAssemblyProjection>& groundingPrepared,
        std::unique_ptr<PreparedAssemblyProjection>& suppressionPrepared,
        std::unique_ptr<PreparedAssemblyProjection>& duplicatePrepared,
        std::unique_ptr<PreparedAssemblyProjection>& removePrepared,
        std::unique_ptr<PreparedAssemblyProjection>& definitionPrepared,
        std::unique_ptr<AssemblyTreeModel>& treeOnly,
        QModelIndex& treeSelectionIndex,
        std::optional<PreparedAssemblySelectionState>& preparedSelection,
        AssemblyCommandSemanticState&& semanticStateToRestore) noexcept
        : owner_(owner),
          prepared_(prepared),
          visibilityPrepared_(visibilityPrepared),
          groundingPrepared_(groundingPrepared),
          suppressionPrepared_(suppressionPrepared),
          duplicatePrepared_(duplicatePrepared),
          removePrepared_(removePrepared),
          definitionPrepared_(definitionPrepared),
          treeOnly_(treeOnly),
          treeSelectionIndex_(treeSelectionIndex),
          preparedSelection_(preparedSelection),
          semanticStateToRestore_(std::move(semanticStateToRestore)) {}

    [[nodiscard]] ProductDocumentSuccessorStatus prepare(
        const ProductDocument& candidate,
        ProductDocumentSuccessorImpact impact) noexcept override;

   private:
    MainWindow& owner_;
    std::unique_ptr<PreparedAssemblyProjection>& prepared_;
    std::unique_ptr<PreparedAssemblyProjection>& visibilityPrepared_;
    std::unique_ptr<PreparedAssemblyProjection>& groundingPrepared_;
    std::unique_ptr<PreparedAssemblyProjection>& suppressionPrepared_;
    std::unique_ptr<PreparedAssemblyProjection>& duplicatePrepared_;
    std::unique_ptr<PreparedAssemblyProjection>& removePrepared_;
    std::unique_ptr<PreparedAssemblyProjection>& definitionPrepared_;
    std::unique_ptr<AssemblyTreeModel>& treeOnly_;
    QModelIndex& treeSelectionIndex_;
    std::optional<PreparedAssemblySelectionState>& preparedSelection_;
    AssemblyCommandSemanticState semanticStateToRestore_;
  };

  struct SketchViewEntry;
  using HistorySelectionState = EditorCommittedState;
  enum class DocumentReplacementOrigin { OpenProject, ImportedStep };
  enum class ProjectReplacementConfirmation {
    Proceed,
    Cancelled,
    ReceiverDestroyed,
  };
  enum class OpenProjectCommandOutcome {
    Opened,
    Cancelled,
    Rejected,
    ReceiverDestroyed,
  };
  using ProjectReplacementConfirmationCallback =
      std::function<ProjectReplacementConfirmation(MainWindow&)>;
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
  void saveProjectAs();
  [[nodiscard]] bool saveProjectToPath(const QString& path, QString* error);
  void upgradeProjectFormat();
  void convertToProductDocument();
  void relocateProductDocument();
  void updateUpgradeProjectFormatAction() noexcept;
  [[nodiscard]] bool projectPersistenceTransitionIsQuiescent() const noexcept;
  [[nodiscard]] bool componentPersistenceTransitionIsQuiescent() const noexcept;
  [[nodiscard]] bool saveProjectAsIsEligible() const noexcept;
  [[nodiscard]] bool standaloneProductConversionIsEligible() const noexcept;
  [[nodiscard]] bool productRelocationIsEligible() const noexcept;
  using SaveProjectAsTargetChooser =
      std::function<std::optional<QString>(const QString& suggestedPath)>;
  using ProductCloneRequestFactory =
      std::function<ProductCloneRequest(const QString& targetPath)>;
  [[nodiscard]] ProjectApplicationStatus runSaveProjectAsCommand(
      const SaveProjectAsTargetChooser& chooseTarget,
      const ProductCloneRequestFactory& makeProductRequest,
      bool* saveAttempted = nullptr);
  [[nodiscard]] ProjectApplicationStatus runAssemblySaveAsCommand(
      const SaveProjectAsTargetChooser& chooseTarget,
      bool* saveAttempted = nullptr);
  [[nodiscard]] ProjectApplicationStatus runComponentSaveAsCommand(
      const SaveProjectAsTargetChooser& chooseTarget,
      bool* saveAttempted = nullptr);
  [[nodiscard]] ProjectApplicationStatus saveProductCloneToPath(
      const QString& targetPath, const ProductCloneRequest& request);
  using ProductConversionTargetChooser =
      std::function<std::optional<QString>(const QString& suggestedPath)>;
  using ProductConversionRequestFactory = std::function<
      StandaloneProductConversionRequest(const QString& targetPath,
                                         DefinitionId rootDefinitionId)>;
  [[nodiscard]] ProjectApplicationStatus runProductConversionCommand(
      const ProductConversionTargetChooser& chooseTarget,
      const ProductConversionRequestFactory& makeRequest,
      bool* conversionAttempted = nullptr);
  using ProductRelocationTargetChooser =
      std::function<std::optional<QString>(const QString& suggestedPath)>;
  using ProductRelocationExecutor = std::function<ProjectApplicationStatus(
      const QString& sourcePath, const QString& targetPath,
      const ProductRelocationPrecondition& precondition)>;
  [[nodiscard]] ProjectApplicationStatus runProductRelocationCommand(
      const ProductRelocationTargetChooser& chooseTarget,
      const ProductRelocationExecutor& relocate,
      bool* relocationAttempted = nullptr);
  [[nodiscard]] ProjectApplicationStatus convertProjectToPath(
      const QString& targetPath,
      const StandaloneProductConversionRequest& request);
  [[nodiscard]] PartDesignCommitGuard startPartDesignCommit();
  [[nodiscard]] static ProjectReplacementConfirmation
  confirmProjectReplacement(MainWindow& owner);
  [[nodiscard]] OpenProjectCommandOutcome runOpenProjectCommand(
      const QString& path,
      const ProjectReplacementConfirmationCallback& confirmReplacement,
      QString* error = nullptr);
  void importStep();
  void importReferenceImage();
  void finishReferenceImageImport(const SketchPlanePick& pick);
  void updateReferenceImageParameter(int parameterIndex, double value);
  void commitReferenceImageEdit();
  void showSelectedReferenceImageManipulator();
  void removeReferenceImage(ReferenceImageId id);
  void applyImportedDocument(Document staged, const QString& importedName);
  void commitDocumentReplacement(StagedApplicationDocument staged,
                                 DocumentReplacementOrigin origin,
                                 const QString& pathOrLabel);
  [[nodiscard]] std::unique_ptr<PreparedAssemblyWorkspace>
  prepareAssemblyWorkspace(StagedApplicationDocument staged) noexcept;
  void commitAssemblyReplacement(
      std::unique_ptr<PreparedAssemblyWorkspace> prepared,
      const QString& path) noexcept;
  [[nodiscard]] std::unique_ptr<PreparedComponentWorkspace>
  prepareComponentWorkspace(StagedApplicationDocument staged) noexcept;
  void commitComponentReplacement(
      std::unique_ptr<PreparedComponentWorkspace> prepared,
      const QString& path) noexcept;
  [[nodiscard]] std::unique_ptr<PreparedComponentProjection>
  prepareComponentIdentityProjection(
      ComponentIdentityTransition transition) noexcept;
  void installComponentIdentityProjection(
      std::unique_ptr<PreparedComponentProjection> projection) noexcept;
  void installPreparedComponentProjection(
      std::unique_ptr<PreparedComponentProjection> projection) noexcept;
  void installPreparedPartProjection(
      std::unique_ptr<PartWorkspaceProjection> projection) noexcept;
  void activateComponentDrawing(const QModelIndex& index) noexcept;
  void closeComponentDrawing() noexcept;
  void clearComponentDrawingWorkspace() noexcept;
  void enterComponentDefinitionEdit(const QModelIndex& index);
  void leaveComponentDefinitionEdit();
  [[nodiscard]] bool commitActiveComponentDocument();
  void installActiveComponentProjection(
      std::unique_ptr<PreparedComponentProjection> projection) noexcept;
  void setComponentVisibility(ComponentInstanceAddress instance,
                              bool visible);
  void setComponentTreeNodeVisibility(ComponentTreeAddressValue address,
                                      bool visible);
  void connectComponentTreeVisibilitySignals();
  [[nodiscard]] bool mutateComponentDocumentFromTree(
      const ComponentInstanceAddress& instance,
      const std::function<bool(Document&)>& mutation,
      const QString& failureMessage);
  void renameComponentTreeItem(const ComponentTreeAddressValue& address,
                               const QString& requestedName);
  void deleteComponentTreeItem(const ComponentTreeAddressValue& address);
  void updateComponentTreePresentation() noexcept;
  void createComponentFromSelectedBody();
  void createComponentFromBody(ComponentWorkspaceBodyKey selectedBody);
  void commitComponentPlacementDrag(ComponentPlacementDragRequest request);
  void updateCreateComponentFromBodyAvailability() noexcept;
  void updateComponentHomePlacementAvailability() noexcept;
  void applyComponentHomePlacement(ComponentHomePlacementOperation operation);
  [[nodiscard]] std::optional<ComponentWorkspaceBodyKey>
  selectedComponentExtractionBody() const noexcept;
  [[nodiscard]] std::optional<ComponentExtractionTransaction>
  prepareSelectedComponentExtraction() const noexcept;
  [[nodiscard]] std::optional<ComponentExtractionTransaction>
  prepareComponentExtraction(
      const ComponentWorkspaceBodyKey& body) const noexcept;
  void createNewAssembly();
  void beginAssemblyInsert();
  void beginAssemblyMate();
  void acceptAssemblyMatePick(const AssemblyFaceRayHit& hit);
  [[nodiscard]] bool presentCurrentAssemblyMatePreview(
      std::uint64_t expectedRevision) noexcept;
  void updateAssemblyMateUi() noexcept;
  void applyAssemblyMate();
  void cancelAssemblyMate() noexcept;
  [[nodiscard]] bool assemblyMateActive() const noexcept {
    return assemblyMateController_ &&
           assemblyMateController_->session().active();
  }
  void beginAssemblyRename();
  void toggleAssemblyOccurrenceVisibility();
  void toggleAssemblyOccurrenceGrounded();
  void toggleAssemblyOccurrenceSuppressed();
  void duplicateAssemblyOccurrence();
  void removeAssemblyOccurrence();
  void reloadSelectedLinkedPart();
  void repairSelectedLinkedPart();
  using LinkedPartRepairSourceChooser =
      std::function<std::optional<QString>(const QString& suggestedRoot)>;
  [[nodiscard]] bool runLinkedPartRepairCommand(
      const LinkedPartRepairSourceChooser& chooseSource,
      bool* repairAttempted = nullptr);
  [[nodiscard]] std::optional<DefinitionId>
  selectedLinkedDefinitionId() const noexcept;
  void updateLinkedPartLifecycleAvailability() noexcept;
  void enterAssemblyDefinitionEdit();
  void leaveAssemblyDefinitionEdit();
  void toggleAssemblyIsolation();
  void cancelAssemblyRename() noexcept;
  void updateAssemblyRenameAvailability() noexcept;
  void applyAssemblyInsert();
  void cancelAssemblyInsert() noexcept;
  void cancelAssemblyInsertBeforeReplacement() noexcept;
  void preparePartDesignForReplacement() noexcept;
  void detachDefinitionEditForReplacement() noexcept;
  void detachAssemblyInsertUi(bool hardReplacement) noexcept;
  void clearAssemblyCommandHistoryForReplacement() noexcept;
  void traceAssemblyReplacementStep(AssemblyInsertTeardownStep step,
                                    bool sourceAlive) noexcept;
  void resetAssemblyPlacementControls(bool enabled) noexcept;
  [[nodiscard]] std::optional<AssemblyCommandSemanticState>
  captureAssemblyCommandSemanticState() const noexcept;
  void updateAssemblyInsertPlacement() noexcept;
  [[nodiscard]] ProductDocumentSuccessorStatus prepareAssemblyProjection(
      const ProductDocument& product,
      const AssemblyCommandSemanticState& semanticStateToRestore,
      std::unique_ptr<PreparedAssemblyProjection>& projection) noexcept;
  [[nodiscard]] ProductDocumentSuccessorStatus prepareAssemblyTreeProjection(
      const ProductDocument& product,
      std::optional<OccurrencePath> selectionToRestore,
      std::optional<DefinitionId> definitionSelectionToRestore,
      std::unique_ptr<AssemblyTreeModel>& tree,
      QModelIndex& selectionIndex,
      std::optional<PreparedAssemblySelectionState>& preparedSelection);
  [[nodiscard]] ProductDocumentSuccessorStatus
  prepareAssemblyVisibilityProjection(
      const ProductDocument& product,
      std::optional<OccurrencePath> selectionToRestore,
      std::unique_ptr<PreparedAssemblyProjection>& projection,
      QModelIndex& selectionIndex) noexcept;
  [[nodiscard]] ProductDocumentSuccessorStatus
  prepareAssemblyGroundingProjection(
      const ProductDocument& product,
      std::optional<OccurrencePath> selectionToRestore,
      std::unique_ptr<PreparedAssemblyProjection>& projection,
      QModelIndex& selectionIndex) noexcept;
  [[nodiscard]] ProductDocumentSuccessorStatus
  prepareAssemblySuppressionProjection(
      const ProductDocument& product,
      std::optional<OccurrencePath> selectionToRestore,
      std::unique_ptr<PreparedAssemblyProjection>& projection,
      QModelIndex& selectionIndex) noexcept;
  [[nodiscard]] ProductDocumentSuccessorStatus
  prepareAssemblyDuplicateProjection(
      const ProductDocument& product,
      std::optional<OccurrencePath> selectionToRestore,
      std::unique_ptr<PreparedAssemblyProjection>& projection,
      QModelIndex& selectionIndex) noexcept;
  [[nodiscard]] ProductDocumentSuccessorStatus
  prepareAssemblyRemoveProjection(
      const ProductDocument& product,
      std::optional<OccurrencePath> selectionToRestore,
      std::unique_ptr<PreparedAssemblyProjection>& projection,
      QModelIndex& selectionIndex) noexcept;
  [[nodiscard]] bool prepareAssemblyIsolationProjection(
      const std::optional<AssemblyOccurrenceIdentity>& occurrence,
      std::optional<PreparedAssemblyIsolationState>& prepared,
      std::unique_ptr<AssemblyTreeModel>& tree,
      QModelIndex& selectionIndex) noexcept;
  void installAssemblyProjection(
      std::unique_ptr<PreparedAssemblyProjection> projection) noexcept;
  void installAssemblyTreeProjection(
      std::unique_ptr<AssemblyTreeModel> tree,
      const QModelIndex& selectionIndex,
      std::optional<PreparedAssemblySelectionState> preparedSelection) noexcept;
  void installAssemblyVisibilityProjection(
      std::unique_ptr<PreparedAssemblyProjection> projection,
      const QModelIndex& selectionIndex) noexcept;
  void installAssemblyGroundingProjection(
      std::unique_ptr<PreparedAssemblyProjection> projection,
      const QModelIndex& selectionIndex) noexcept;
  void installAssemblySuppressionProjection(
      std::unique_ptr<PreparedAssemblyProjection> projection,
      const QModelIndex& selectionIndex) noexcept;
  void installAssemblyDuplicateProjection(
      std::unique_ptr<PreparedAssemblyProjection> projection,
      const QModelIndex& selectionIndex) noexcept;
  void installAssemblyRemoveProjection(
      std::unique_ptr<PreparedAssemblyProjection> projection,
      const QModelIndex& selectionIndex) noexcept;
  [[nodiscard]] ProductDocumentSuccessorStatus
  prepareAssemblyDefinitionProjection(
      const ProductDocument& product,
      std::optional<OccurrencePath> selectionToRestore,
      std::optional<DefinitionId> definitionSelectionToRestore,
      std::unique_ptr<PreparedAssemblyProjection>& projection,
      QModelIndex& selectionIndex) noexcept;
  void installAssemblyDefinitionProjection(
      std::unique_ptr<PreparedAssemblyProjection> projection,
      const QModelIndex& selectionIndex) noexcept;
  void installAssemblyIsolationProjection(
      std::optional<PreparedAssemblyIsolationState> prepared,
      std::unique_ptr<AssemblyTreeModel> tree,
      const QModelIndex& selectionIndex) noexcept;
  void applyProjectKind(EditorProjectKind kind) noexcept;
  [[nodiscard]] bool modelWorkspaceActive() const noexcept {
    return projectKind_ == EditorProjectKind::Part ||
           definitionEditContext_.has_value() ||
           componentDefinitionEditContext_.has_value();
  }
  [[nodiscard]] bool standalonePartProjectActive() const noexcept {
    return projectKind_ == EditorProjectKind::Part &&
           !definitionEditContext_.has_value() &&
           !componentDefinitionEditContext_.has_value() &&
           productDocument_.rootDefinitionKind() == DefinitionKind::Part;
  }
  [[nodiscard]] bool componentProductActive() const noexcept {
    return projectKind_ == EditorProjectKind::Part &&
           !definitionEditContext_.has_value() &&
           !componentDefinitionEditContext_.has_value() &&
           productDocument_.rootDefinitionKind() == DefinitionKind::Component;
  }
  [[nodiscard]] bool rootComponentAuthoringActive() const noexcept {
    return componentDefinitionEditContext_ &&
           componentDefinitionEditContext_->targetDefinitionId ==
               componentDefinitionEditContext_->rootDefinitionId &&
           !componentDefinitionEditContext_->occurrencePath;
  }
  [[nodiscard]] bool partModelingContextActive() const noexcept {
    return standalonePartProjectActive() || definitionEditContext_.has_value() ||
           componentDefinitionEditContext_.has_value();
  }
  [[nodiscard]] bool assemblyProductActive() const noexcept {
    return projectKind_ == EditorProjectKind::AssemblyReadOnly;
  }
  [[nodiscard]] bool partProjectActive() const noexcept {
    return standalonePartProjectActive() ||
           componentDefinitionEditContext_.has_value();
  }
  [[nodiscard]] bool moveCommandContextActive() const noexcept {
    return standalonePartProjectActive() || definitionEditContext_.has_value() ||
           componentDefinitionEditContext_.has_value();
  }
  [[nodiscard]] bool activePartDesignUiContextAllowed() const noexcept {
    return standalonePartProjectActive() ||
           componentDefinitionEditContext_.has_value() ||
           (definitionEditContext_.has_value() &&
            partDesignCoordinator_.activeTool() == PartDesignToolKind::Move);
  }
  void selectAssemblyTreeIndex(const QModelIndex& current);
  void selectAssemblyOccurrenceFromViewport(
      const AssemblyOccurrenceSelection& occurrence);
  void selectComponentTreeIndex(const QModelIndex& current);
  void selectComponentFromViewport(
      const ComponentInstanceSelection& component);
  void exportStep();
  void exportStl();
  void updateFromSketch(double widthMm, double heightMm);
  void finishSketch();
  void extrudeSketch();
  void updateLegacyExtrusionPreview();
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

  ProductDocument productDocument_;
  EditorProjectKind projectKind_{EditorProjectKind::Part};
  struct AssemblyDefinitionEditContext final {
    ProductIncarnationId productIncarnation;
    DefinitionId rootDefinitionId;
    DefinitionId targetDefinitionId;
    DefinitionStateId stateAtEntry;
    AssemblyCommandRevision historyRevisionFloor;
    std::optional<DefinitionId> semanticTreeSelection;
    float assemblyYawDeg{};
    float assemblyPitchDeg{};
    float assemblyZoom{};
    QPointF assemblyPan;
    std::uint64_t callbackGeneration{};
  };
  std::optional<AssemblyDefinitionEditContext> definitionEditContext_;
  std::unique_ptr<AssemblyViewportSession> suspendedAssemblySession_;
  struct ComponentDefinitionEditContext final {
    ProductIncarnationId productIncarnation;
    DefinitionId rootDefinitionId;
    DefinitionId targetDefinitionId;
    DefinitionStateId stateAtEntry;
    std::optional<OccurrencePath> occurrencePath;
    float yawDeg{};
    float pitchDeg{};
    float zoom{};
    QPointF pan;
    QString displayName;
  };
  std::optional<ComponentDefinitionEditContext>
      componentDefinitionEditContext_;
  std::unique_ptr<Document> componentEditDocument_;
  std::unique_ptr<ComponentViewportSession> suspendedComponentSession_;
  bool rootComponentVisible_{true};
  std::uint64_t assemblyPresentationEpochClock_{};
  std::uint64_t componentPresentationEpochClock_{};
  ComponentViewportSessionFailurePoint componentPreparationFailurePoint_{
      ComponentViewportSessionFailurePoint::None};
  ComponentTreeModelFailurePoint componentTreeFailurePoint_{
      ComponentTreeModelFailurePoint::None};
  ModelWorkspaceProjectionFailurePoint
      componentExtractionProjectionFailurePoint_{
          ModelWorkspaceProjectionFailurePoint::None};
  ComponentExtractionFailurePoint componentExtractionFailurePoint_{
      ComponentExtractionFailurePoint::None};
  AssemblyViewportSessionFailurePoint assemblyPreparationFailurePoint_{
      AssemblyViewportSessionFailurePoint::None};
  bool assemblyFullProjectionSelectionResourceFailureForTesting_{};
  bool assemblyTreeResourceFailureForTesting_{};
  AssemblyIsolationFailurePoint assemblyIsolationFailurePointForTesting_{
      AssemblyIsolationFailurePoint::None};
  bool assemblyTreeSelectionCallbackResourceFailureForTesting_{};
  std::uint64_t assemblyTreeSelectionCallbackCountForTesting_{};
  bool definitionMoveRecomputeFailureForTesting_{};
  [[nodiscard]] const Document& activePartDocument() const noexcept {
    if (componentDefinitionEditContext_) {
      Q_ASSERT(componentEditDocument_);
      if (!componentEditDocument_) std::terminate();
      return *componentEditDocument_;
    }
    if (definitionEditContext_) {
      const auto* embedded = productDocument_.partDocument(
          definitionEditContext_->targetDefinitionId);
      Q_ASSERT(embedded);
      return *embedded;
    }
    Q_ASSERT(standalonePartProjectActive());
    if (!standalonePartProjectActive()) std::terminate();
    return productDocument_.rootDocument();
  }
  [[nodiscard]] Document& mutableStandaloneDocument() noexcept {
    if (componentDefinitionEditContext_) {
      Q_ASSERT(componentEditDocument_);
      if (!componentEditDocument_) std::terminate();
      return *componentEditDocument_;
    }
    Q_ASSERT(standalonePartProjectActive());
    if (!standalonePartProjectActive()) std::terminate();
    return productDocument_.rootDocument();
  }
  project::ProjectPersistenceState projectPersistenceState_;
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
  QStackedWidget* modelTreeStack_{nullptr};
  QWidget* partTreePage_{nullptr};
  QTreeView* assemblyTreeView_{nullptr};
  AssemblyTreeModel* assemblyTreeModel_{nullptr};
  QTreeView* componentTreeView_{nullptr};
  ComponentTreeModel* componentTreeModel_{nullptr};
  QWidget* assemblyRibbon_{nullptr};
  QPushButton* assemblyNewButton_{nullptr};
  QPushButton* assemblyInsertButton_{nullptr};
  QPushButton* assemblyMateButton_{nullptr};
  QPushButton* assemblyRenameButton_{nullptr};
  QPushButton* assemblyVisibilityButton_{nullptr};
  QPushButton* assemblyGroundingButton_{nullptr};
  QPushButton* assemblySuppressionButton_{nullptr};
  QPushButton* assemblyDuplicateButton_{nullptr};
  QPushButton* assemblyRemoveButton_{nullptr};
  QPushButton* assemblyEditDefinitionButton_{nullptr};
  QPushButton* assemblyIsolationButton_{nullptr};
  QPushButton* assemblyApplyButton_{nullptr};
  QPushButton* assemblyCancelButton_{nullptr};
  QCheckBox* assemblyMateOpposedCheck_{nullptr};
  QLabel* assemblyMateStatusLabel_{nullptr};
  QShortcut* assemblyInsertEscapeShortcut_{nullptr};
  QShortcut* assemblyRenameShortcut_{nullptr};
  QShortcut* assemblyVisibilityShortcut_{nullptr};
  QShortcut* assemblyGroundingShortcut_{nullptr};
  QShortcut* assemblySuppressionShortcut_{nullptr};
  QShortcut* assemblyDuplicateShortcut_{nullptr};
  QShortcut* assemblyRemoveShortcut_{nullptr};
  QAction* assemblyRenameAction_{nullptr};
  QAction* assemblyVisibilityAction_{nullptr};
  QAction* assemblyGroundingAction_{nullptr};
  QAction* assemblySuppressionAction_{nullptr};
  QAction* assemblyDuplicateAction_{nullptr};
  QAction* assemblyRemoveAction_{nullptr};
  QAction* assemblyEditDefinitionAction_{nullptr};
  QAction* assemblyIsolationAction_{nullptr};
  QAction* linkedPartReloadAction_{nullptr};
  QAction* linkedPartRepairAction_{nullptr};
  bool assemblySaveAsChooserActive_{false};
  bool componentSaveAsChooserActive_{false};
  bool linkedPartRepairChooserActive_{false};
  QLabel* definitionEditBreadcrumb_{nullptr};
  QPushButton* definitionEditBackButton_{nullptr};
  std::array<QDoubleSpinBox*, 3> assemblyTranslationSpins_{};
  std::array<QDoubleSpinBox*, 3> assemblyRotationSpins_{};
  bool assemblySelectionReentry_{};
  bool componentSelectionReentry_{};
  std::uint64_t assemblyInsertCallbackRevision_{};
  std::uint64_t assemblyRenameCallbackRevision_{};
  QPointer<QInputDialog> assemblyRenameDialog_;
  AssemblyInsertToolSession assemblyInsertSession_;
  AssemblyInsertController assemblyInsertController_;
  std::unique_ptr<AssemblyMateToolController> assemblyMateController_;
  // Narrow deterministic seams for lifecycle/presentation regressions. They
  // are inert in production and consumed once by the friend test access.
  bool assemblyMatePreviewPrepareFailureForTesting_{};
  bool assemblyMatePreviewInstallFailureForTesting_{};
  std::function<void()> assemblyMateReentryHookForTesting_;
  AssemblyCommandHistory assemblyCommandHistory_;
  LinkedPartRuntimeStates linkedPartRuntimeStates_;
  enum class MateDiagnosticsValidity : std::uint8_t {
    Unchecked,
    Current,
  };
  struct MateRegistryValiditySeal final {
    DefinitionId owner;
    std::uint64_t revision{};
    std::uint64_t fingerprint{};
    MateSolverOptions options;
    friend bool operator==(const MateRegistryValiditySeal&,
                           const MateRegistryValiditySeal&) = default;
  };
  struct MateDiagnosticsValidityStamp final {
    AssemblyCommandRevision historyRevision;
    std::size_t historyCommandCount{};
    std::size_t historyCursor{};
    std::vector<MateRegistryValiditySeal> registries;
  };
  std::vector<project::MateRuntimeDiagnostics> mateRuntimeDiagnostics_;
  std::optional<MateDiagnosticsValidityStamp> mateDiagnosticsValidityStamp_;
  [[nodiscard]] std::optional<MateDiagnosticsValidityStamp>
  captureMateDiagnosticsValidityStamp() const noexcept;
  [[nodiscard]] MateDiagnosticsValidity mateDiagnosticsValidity() const
      noexcept;
  std::function<void(AssemblyInsertTeardownStep, bool)>
      assemblyInsertTeardownObserver_;
  QMetaObject::Connection assemblyTreeSelectionConnection_;
  QMetaObject::Connection componentTreeSelectionConnection_;
  ReferenceImageId selectedReferenceImageId_{kInvalidReferenceImageId};
  QString pendingReferenceImagePath_;
  QSize pendingReferenceImagePixelSize_;
  std::optional<Document> referenceImageEditBefore_;
  std::optional<HistorySelectionState> referenceImageEditSelectionBefore_;
  // UI-only compatibility presentation for a loaded v1 extrusion. Presence
  // of this stable source ID is the entire state; it is consumed once a modern
  // Body is committed and can never disagree with a separate active flag.
  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId_;
  std::optional<ExtrusionSourcePick> selectedExtrusionSource_;
  QString currentSketchSupport_{QStringLiteral("XY")};
  SketchPlacement currentSketchPlacement_{SketchPlacement::xy()};
  std::optional<FaceReference> currentSketchFaceReference_;
  QAction* createAction_{nullptr};
  QAction* undoAction_{nullptr};
  QAction* redoAction_{nullptr};
  QAction* saveAction_{nullptr};
  QAction* importStepAction_{nullptr};
  QAction* exportStepAction_{nullptr};
  QAction* exportStlAction_{nullptr};
  QAction* historyToggleAction_{nullptr};
  QAction* saveProjectAsAction_{nullptr};
  QAction* upgradeProjectFormatAction_{nullptr};
  QAction* convertToProductDocumentAction_{nullptr};
  QAction* relocateProductDocumentAction_{nullptr};
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
  QDockWidget* sketchMirrorDock_{nullptr};
  QLabel* sketchMirrorElementsValue_{nullptr};
  QLabel* sketchMirrorAxisValue_{nullptr};
  QPushButton* sketchMirrorApplyButton_{nullptr};
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
  // The sole definition-edit authoring boundary. Callers mutate only the
  // detached candidate; this helper computes the cold delta, recomputes and
  // commits it through ProductDocument + AssemblyCommandHistory.
  [[nodiscard]] bool commitDefinitionCandidate(Document candidate);
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
  std::optional<ModelCommandHistory> suspendedComponentModelHistory_;
  std::map<DefinitionId, ModelCommandHistory> componentModelHistories_;
};

}  // namespace solidar

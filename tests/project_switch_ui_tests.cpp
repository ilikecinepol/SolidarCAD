#include "TestAssertions.h"

#include <TopoDS_Shape.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

#include <QApplication>
#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantAnimation>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <type_traits>

#include "TestGeometryUtils.h"
#include "app/AppSettings.h"
#include "model/ExtrudeFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/FilletFeature.h"
#include "model/ChamferFeature.h"
#include "model/ShellFeature.h"
#include "model/DraftFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/IdGeneration.h"
#include "model/MirrorFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/FilletToolSession.h"
#include "model/MoveFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"
#include "project/LinkedPartSourceResolver.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/ProductEnvelopeV6Encoder.h"
#include "project/ProductEnvelopeV7Encoder.h"
#include "project/ProductEnvelopeV8Encoder.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "sketch/SketchRibbon.h"
#include "ui/EditorFactory.h"
#include "ui/MainWindow.h"
#include "ui/HistoryTimelineWidget.h"
#include "ui/ModelRibbon.h"
#include "ui/PartDesignHistory.h"
#include "ui/SketchCanvas.h"
#include "ui/ToolParametersPanel.h"
#include "ui/Viewport.h"
#include "ui/tools/PartDesignToolController.h"

namespace solidar {

using AssemblySelectionViewGetter =
    const std::optional<AssemblyOccurrenceSelection>& (Viewport::*)()
        const noexcept;
static_assert(std::is_same_v<
              decltype(&Viewport::selectedAssemblyOccurrenceView),
              AssemblySelectionViewGetter>);
static_assert(noexcept(std::declval<const Viewport&>()
                           .selectedAssemblyOccurrenceView()));

template <typename T>
concept HasOwningAssemblySelectionGetter = requires(const T& viewport) {
  viewport.selectedAssemblyOccurrence();
};
static_assert(!HasOwningAssemblySelectionGetter<Viewport>);

class UnindexedPresentationFeature final : public ShapeFeature {
 public:
  explicit UnindexedPresentationFeature(ShapePtr shape)
      : ShapeFeature("Unindexed presentation fixture") {
    setShape(std::move(shape));
    markValid();
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<UnindexedPresentationFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    markValid();
    return true;
  }
};

class MainWindowUndoTestAccess {
 public:
  static_assert(
      std::is_same_v<decltype(MainWindow::productDocument_), ProductDocument>);
  static_assert(!std::is_copy_constructible_v<
                decltype(MainWindow::productDocument_)>);
  static_assert(std::is_same_v<
                decltype(std::declval<MainWindow&>().activePartDocument()),
                const Document&>);
  static_assert(std::is_same_v<
                decltype(std::declval<const MainWindow&>().activePartDocument()),
                const Document&>);

  inline static std::optional<MainWindow::HistorySelectionState>
      expectedEditUndoSelection_;
  static void reset(MainWindow& window) {
    window.modelHistory_.clear();
    window.updateUndoAvailability();
  }
  static project::ProjectPersistenceState persistenceState(
      const MainWindow& window) {
    return window.projectPersistenceState_;
  }
  static const Document* documentAddress(const MainWindow& window) {
    return &window.activePartDocument();
  }
  static std::size_t definitionCount(const MainWindow& window) {
    return window.productDocument_.definitions().size();
  }
  static DefinitionId rootDefinitionId(const MainWindow& window) {
    return window.productDocument_.rootDefinitionId();
  }
  static EditorProjectKind projectKind(const MainWindow& window) {
    return window.projectKind_;
  }
  static bool hasAssemblySession(const MainWindow& window) {
    return window.viewport_->assemblySession() != nullptr;
  }
  static const AssemblyViewportSession* assemblySessionAddress(
      const MainWindow& window) {
    return window.viewport_->assemblySession();
  }
  static const ProductDocument* productDocumentAddress(
      const MainWindow& window) {
    return &window.productDocument_;
  }
  static int assemblyTreeRootCount(const MainWindow& window) {
    return window.assemblyTreeModel_
               ? window.assemblyTreeModel_->rowCount()
               : 0;
  }
  static QModelIndex firstAssemblyOccurrenceIndex(const MainWindow& window) {
    if (!window.assemblyTreeModel_) return {};
    const QModelIndex root = window.assemblyTreeModel_->index(0, 0);
    return root.isValid() ? window.assemblyTreeModel_->index(0, 0, root)
                          : QModelIndex{};
  }
  static QModelIndex assemblyOccurrenceIndex(const MainWindow& window,
                                             int row) {
    if (!window.assemblyTreeModel_) return {};
    const QModelIndex root = window.assemblyTreeModel_->index(0, 0);
    return root.isValid() ? window.assemblyTreeModel_->index(row, 0, root)
                          : QModelIndex{};
  }
  static QModelIndex assemblyOccurrenceIndexForPath(
      const MainWindow& window, const OccurrencePath& path) {
    if (!window.assemblyTreeModel_ || !window.viewport_ ||
        !window.viewport_->assemblySession())
      return {};
    const auto* session = window.viewport_->assemblySession();
    AssemblyOccurrenceSelection selection{
        session->publicationHandle(),
        {session->epoch(), window.productDocument_.rootDefinitionId(), path}};
    return window.assemblyTreeModel_->indexForOccurrence(selection);
  }
  static QModelIndex assemblyDefinitionIndex(const MainWindow& window,
                                             DefinitionId definitionId) {
    return window.assemblyTreeModel_
               ? window.assemblyTreeModel_->indexForDefinition(definitionId)
               : QModelIndex{};
  }
  static std::optional<DefinitionId> firstPartDefinitionId(
      const MainWindow& window) {
    for (const DefinitionId id :
         window.productDocument_.definitions().definitionIds()) {
      if (window.productDocument_.definitions().kind(id) ==
          DefinitionKind::Part)
        return id;
    }
    return std::nullopt;
  }
  static QAction* linkedReloadAction(MainWindow& window) {
    return window.linkedPartReloadAction_;
  }
  static QAction* linkedRepairAction(MainWindow& window) {
    return window.linkedPartRepairAction_;
  }
  static QAction* saveAction(MainWindow& window) {
    return window.saveAction_;
  }
  static QAction* exportStepAction(MainWindow& window) {
    return window.exportStepAction_;
  }
  static QAction* exportStlAction(MainWindow& window) {
    return window.exportStlAction_;
  }
  static LinkedPartRuntimeStatus linkedRuntimeStatus(
      const MainWindow& window, DefinitionId id) {
    const auto found = window.linkedPartRuntimeStates_.find(id);
    return found == window.linkedPartRuntimeStates_.end()
               ? LinkedPartRuntimeStatus::Unchecked
               : found->second.status;
  }
  static std::size_t mateDiagnosticsCount(const MainWindow& window) {
    return window.mateRuntimeDiagnostics_.size();
  }
  static const project::MateRuntimeDiagnostics* mateDiagnosticsAddress(
      const MainWindow& window) {
    return window.mateRuntimeDiagnostics_.empty()
               ? nullptr
               : window.mateRuntimeDiagnostics_.data();
  }
  static const LinkedPartRuntimeStates& linkedRuntimeStates(
      const MainWindow& window) {
    return window.linkedPartRuntimeStates_;
  }
  static MateSolveStatus firstMateDiagnosticStatus(
      const MainWindow& window) {
    return window.mateRuntimeDiagnostics_.empty()
               ? MateSolveStatus::InvalidInput
               : window.mateRuntimeDiagnostics_.front().status;
  }
  static bool mateDiagnosticsCurrent(const MainWindow& window) {
    return window.mateDiagnosticsValidity() ==
           MainWindow::MateDiagnosticsValidity::Current;
  }
  static std::optional<LinkedPartBinding> linkedBinding(
      const MainWindow& window, DefinitionId id) {
    const auto* binding = window.productDocument_.partLinkedBinding(id);
    return binding ? std::optional<LinkedPartBinding>{*binding} : std::nullopt;
  }
  static BoxParameters linkedBox(const MainWindow& window, DefinitionId id) {
    const auto* document = window.productDocument_.partDocument(id);
    CHECK(document);
    return document->box();
  }
  static void reloadSelectedLinkedPart(MainWindow& window) {
    window.reloadSelectedLinkedPart();
  }
  static bool runLinkedPartRepairCommand(
      MainWindow& window,
      const MainWindow::LinkedPartRepairSourceChooser& chooser,
      bool* attempted = nullptr) {
    return window.runLinkedPartRepairCommand(chooser, attempted);
  }
  static void undoAssembly(MainWindow& window) { window.undoLastAction(); }
  static void redoAssembly(MainWindow& window) { window.redoLastAction(); }
  static const Document* embeddedDocumentAddress(const MainWindow& window,
                                                 DefinitionId id) {
    return window.productDocument_.partDocument(id);
  }
  static DefinitionStateId definitionStateId(const MainWindow& window,
                                             DefinitionId id) {
    return window.productDocument_.definitionStateId(id);
  }
  static bool definitionEditEnabled(const MainWindow& window) {
    return window.assemblyEditDefinitionAction_ &&
           window.assemblyEditDefinitionAction_->isEnabled();
  }
  static void enterDefinitionEdit(MainWindow& window) {
    window.enterAssemblyDefinitionEdit();
  }
  static void leaveDefinitionEdit(MainWindow& window) {
    window.leaveAssemblyDefinitionEdit();
  }
  static bool definitionEditActive(const MainWindow& window) {
    return window.definitionEditContext_.has_value();
  }
  static bool definitionEditNavigationVisible(const MainWindow& window,
                                              DefinitionId id) {
    return window.definitionEditBreadcrumb_ &&
           !window.definitionEditBreadcrumb_->isHidden() &&
           window.definitionEditBreadcrumb_->text() ==
               QString::fromUtf8("Сборка %1 › Деталь %2")
                   .arg(window.productDocument_.rootDefinitionId().value())
                   .arg(id.value()) &&
           window.definitionEditBackButton_ &&
           !window.definitionEditBackButton_->isHidden();
  }
  static bool definitionEditPersistenceDisabled(const MainWindow& window) {
    return window.saveAction_ && !window.saveAction_->isEnabled() &&
           window.saveProjectAsAction_ &&
           !window.saveProjectAsAction_->isEnabled() &&
           window.importStepAction_ && !window.importStepAction_->isEnabled() &&
           window.exportStepAction_ && !window.exportStepAction_->isEnabled() &&
           window.exportStlAction_ && !window.exportStlAction_->isEnabled();
  }
  static bool definitionEditMoveOnlyAvailable(const MainWindow& window) {
    const auto* move = window.modelRibbon_
                           ? window.modelRibbon_->findChild<QToolButton*>(
                                 QStringLiteral("moveCommand"))
                           : nullptr;
    bool allOtherCommandsDisabled = window.modelRibbon_ != nullptr;
    if (window.modelRibbon_)
      for (const auto* button :
           window.modelRibbon_->findChildren<QToolButton*>())
        if (button != move && button->isEnabled())
          allOtherCommandsDisabled = false;
    return window.modelRibbon_ && window.modelRibbon_->isEnabled() && move &&
           move->isEnabled() && allOtherCommandsDisabled &&
           window.sketchRibbon_ && !window.sketchRibbon_->isEnabled() &&
           window.sketchCanvas_ && !window.sketchCanvas_->isEnabled() &&
           window.featureTree_ && !window.featureTree_->isEnabled() &&
           window.historyTimeline_ && !window.historyTimeline_->isEnabled() &&
           window.toolParametersDock_ &&
           !window.toolParametersDock_->isEnabled() && window.moveDock_ &&
           window.moveDock_->isEnabled();
  }
  static void injectDefinitionMoveRecomputeFailure(MainWindow& window,
                                                   bool enabled) {
    window.definitionMoveRecomputeFailureForTesting_ = enabled;
  }
  static bool sketchWorkingCopyActive(const MainWindow& window) {
    return window.workspaceStack_->currentWidget() == window.sketchCanvas_ ||
           window.editingSketchIndex_.has_value();
  }
  static const AssemblyViewportSession* suspendedAssemblySessionAddress(
      const MainWindow& window) {
    return window.suspendedAssemblySession_.get();
  }
  static std::size_t assemblyHistorySize(const MainWindow& window) {
    return window.assemblyCommandHistory_.size();
  }
  static std::size_t assemblyHistoryCursor(const MainWindow& window) {
    return window.assemblyCommandHistory_.cursor();
  }
  static std::size_t assemblyHistoryRetainedBytes(const MainWindow& window) {
    return window.assemblyCommandHistory_.retainedBytes();
  }
  static bool assemblyCanRedo(const MainWindow& window) {
    return window.assemblyCommandHistory_.canRedo();
  }
  static AssemblyCommandRevision assemblyHistoryRevision(
      const MainWindow& window) {
    return window.assemblyCommandHistory_.currentRevision();
  }
  static AssemblyCommandRevision assemblyHistorySavepoint(
      const MainWindow& window) {
    return window.assemblyCommandHistory_.savepointRevision();
  }
  static ViewportNavigationState viewportNavigation(const MainWindow& window) {
    return window.viewport_->navigationState();
  }
  static void setViewportNavigation(MainWindow& window,
                                    ViewportNavigationState state) {
    window.viewport_->restoreNavigationState(state);
  }
  static bool commitEmbeddedBox(MainWindow& window, BoxParameters box) {
    if (!window.definitionEditContext_) return false;
    const Document* current = window.productDocument_.partDocument(
        window.definitionEditContext_->targetDefinitionId);
    if (!current) return false;
    Document candidate = *current;
    candidate.setBox(box);
    return window.commitDefinitionCandidate(std::move(candidate));
  }
  static std::optional<AssemblyOccurrenceSelection> occurrenceForIndex(
      const MainWindow& window, const QModelIndex& index) {
    if (!window.assemblyTreeModel_) return std::nullopt;
    const auto address = window.assemblyTreeModel_->addressForIndex(index);
    return address ? address->get().occurrence : std::nullopt;
  }
  static void selectAssemblyTreeIndex(MainWindow& window,
                                      const QModelIndex& index) {
    window.assemblyTreeView_->setCurrentIndex(index);
  }
  static bool selectAssemblyOccurrenceFromViewport(
      MainWindow& window, const AssemblyOccurrenceSelection& occurrence) {
    return window.viewport_->setSelectedAssemblyOccurrence(occurrence, true);
  }
  static bool clearAssemblyOccurrenceFromViewport(MainWindow& window) {
    return window.viewport_->setSelectedAssemblyOccurrence(std::nullopt, true);
  }
  static std::optional<AssemblyOccurrenceSelection> viewportAssemblySelection(
      const MainWindow& window) {
    return window.viewport_->selectedAssemblyOccurrenceView();
  }
  static bool setHoveredAssemblyOccurrence(
      MainWindow& window,
      const std::optional<AssemblyOccurrenceSelection>& occurrence) {
    auto* session = const_cast<AssemblyViewportSession*>(
        window.viewport_->assemblySession());
    return session && session->setHoveredOccurrence(occurrence);
  }
  static bool viewportAssemblySelectionViewAliasesSessionStorage(
      const MainWindow& window) {
    const auto* session = window.viewport_->assemblySession();
    return session &&
           &window.viewport_->selectedAssemblyOccurrenceView() ==
               &session->selectedOccurrence();
  }
  static AssemblyCommandSemanticState assemblyCommandSemanticState(
      const MainWindow& window) {
    return window.captureAssemblyCommandSemanticState().value_or(
        AssemblyCommandSemanticState{});
  }
  static void updateAssemblyRenameAvailability(MainWindow& window) noexcept {
    window.updateAssemblyRenameAvailability();
  }
  static QModelIndex assemblyTreeCurrentIndex(const MainWindow& window) {
    return window.assemblyTreeView_->currentIndex();
  }
  static std::optional<DefinitionId> assemblyTreeCurrentDefinition(
      const MainWindow& window) {
    if (!window.assemblyTreeModel_ || !window.assemblyTreeView_)
      return std::nullopt;
    const auto address = window.assemblyTreeModel_->addressForIndex(
        window.assemblyTreeView_->currentIndex());
    if (!address ||
        (address->get().kind != AssemblyTreeNodeKind::PartDefinition &&
         address->get().kind != AssemblyTreeNodeKind::AssemblyDefinition &&
         address->get().kind !=
             AssemblyTreeNodeKind::RootAssemblyDefinition))
      return std::nullopt;
    return address->get().definitionId;
  }
  static void exerciseReadOnlyMutationGuards(MainWindow& window) {
    window.createProject();
    window.applyActivePartDesignTool();
    window.importStep();
    window.importReferenceImage();
    window.finishReferenceImageImport(SketchPlanePick{});
    window.updateReferenceImageParameter(0, 1.0);
    window.commitReferenceImageEdit();
    window.removeReferenceImage(kInvalidReferenceImageId);
    window.updateFromSketch(10.0, 10.0);
    window.finishSketch();
    window.extrudeSketch();
    window.createRevolve();
    window.createPocket();
    window.createFillet();
    window.createMove();
    window.createMirror();
    window.createLinearPattern();
    window.createCircularPattern();
    window.createChamfer();
    window.createJoinBodies();
    window.createShell();
    window.createDraft();
    window.applyHistoryPosition(0);
    window.editSketchById(kInvalidSketchId);
    window.editHistoryFeature(kInvalidBodyId, kInvalidFeatureId);
    window.exportStep();
    window.exportStl();
    window.upgradeProjectFormat();
    window.convertToProductDocument();
    window.relocateProductDocument();
  }
  static std::uint64_t assemblyEpochClock(const MainWindow& window) {
    return window.assemblyPresentationEpochClock_;
  }
  static void injectAssemblyPreparationFailure(
      MainWindow& window, AssemblyViewportSessionFailurePoint point) {
    window.assemblyPreparationFailurePoint_ = point;
  }
  static void injectFullProjectionSelectionResourceFailure(
      MainWindow& window, bool enabled) {
    window.assemblyFullProjectionSelectionResourceFailureForTesting_ = enabled;
  }
  static void injectAssemblyTreeResourceFailure(MainWindow& window,
                                                bool enabled) {
    window.assemblyTreeResourceFailureForTesting_ = enabled;
  }
  static void injectAssemblyTreeSelectionCallbackResourceFailure(
      MainWindow& window, bool enabled) {
    window.assemblyTreeSelectionCallbackResourceFailureForTesting_ = enabled;
  }
  static std::uint64_t assemblyTreeSelectionCallbackCount(
      const MainWindow& window) {
    return window.assemblyTreeSelectionCallbackCountForTesting_;
  }
  static bool createNewAssembly(MainWindow& window) {
    auto staged = window.projectApplicationService_.stageNewAssembly();
    if (!staged.succeeded()) return false;
    auto prepared =
        window.prepareAssemblyWorkspace(std::move(*staged.staged));
    if (!prepared) return false;
    window.commitAssemblyReplacement(std::move(prepared), QString{});
    return window.projectKind_ == EditorProjectKind::AssemblyReadOnly;
  }
  static bool installAssemblyProduct(
      MainWindow& window, std::unique_ptr<ProductDocument> product,
      QString path = {}, LinkedPartRuntimeStates linkedStates = {}) {
    if (!product) return false;
    StagedApplicationDocument staged(
        std::move(*product), std::nullopt,
        project::ProjectPersistenceState{
            project::ProjectPersistenceFormat::V7});
    staged.linkedPartRuntimeStates = std::move(linkedStates);
    auto prepared = window.prepareAssemblyWorkspace(std::move(staged));
    if (!prepared) return false;
    window.commitAssemblyReplacement(std::move(prepared), path);
    return window.projectKind_ == EditorProjectKind::AssemblyReadOnly;
  }
  static ProductDocumentSuccessorStatus installFullProjection(
      MainWindow& window, AssemblyCommandSemanticState state) {
    std::unique_ptr<PreparedAssemblyProjection> projection;
    const auto status = window.prepareAssemblyProjection(
        window.productDocument_, state, projection);
    if (status == ProductDocumentSuccessorStatus::Prepared)
      window.installAssemblyProjection(std::move(projection));
    return status;
  }
  static AssemblyCommandHistoryStatus executeAssemblyRenameForSemanticState(
      MainWindow& window, AssemblyCommandSemanticState beforeState,
      AssemblyCommandSemanticState afterState) {
    const auto* root = window.productDocument_.rootAssemblyDefinition();
    if (!root || root->occurrences().empty())
      return AssemblyCommandHistoryStatus::InvalidCommand;
    const auto& occurrence = root->occurrences().begin()->second;
    const auto path = OccurrencePath::create({occurrence.id()});
    if (!path) return AssemblyCommandHistoryStatus::InvalidCommand;
    RootOccurrenceAddress address{
        window.productDocument_.incarnation(),
        window.productDocument_.rootDefinitionId(),
        std::move(*path.path),
        occurrence.targetDefinitionId()};
    auto delta = OccurrenceDelta::rename(
        std::move(address), occurrence.displayNameUtf8(),
        occurrence.displayNameUtf8() + " semantic");
    if (!delta) return AssemblyCommandHistoryStatus::InvalidCommand;
    AssemblyCommandSemanticState successorState = afterState;
    OccurrenceDeltaCommand command{std::move(*delta.delta),
                                   std::move(beforeState),
                                   std::move(afterState)};
    std::unique_ptr<PreparedAssemblyProjection> fullProjection;
    std::unique_ptr<PreparedAssemblyProjection> visibilityProjection;
    std::unique_ptr<PreparedAssemblyProjection> groundingProjection;
    std::unique_ptr<PreparedAssemblyProjection> suppressionProjection;
    std::unique_ptr<PreparedAssemblyProjection> duplicateProjection;
    std::unique_ptr<PreparedAssemblyProjection> removeProjection;
    std::unique_ptr<PreparedAssemblyProjection> definitionProjection;
    std::unique_ptr<AssemblyTreeModel> treeOnly;
    QModelIndex selectionIndex;
    std::optional<PreparedAssemblySelectionState> preparedSelection;
    MainWindow::AssemblyProjectionSuccessor prepare(
        window, fullProjection, visibilityProjection, groundingProjection,
        suppressionProjection, duplicateProjection, removeProjection,
        definitionProjection, treeOnly, selectionIndex, preparedSelection,
        std::move(successorState));
    const auto result = window.assemblyCommandHistory_.executeOccurrenceDelta(
        window.productDocument_, std::move(command), prepare);
    if (result.status == AssemblyCommandHistoryStatus::Committed)
      window.installAssemblyTreeProjection(
          std::move(treeOnly), selectionIndex,
          std::move(preparedSelection));
    return result.status;
  }
  static bool stageAssemblyInsert(MainWindow& window, const QString& path) {
    const auto* root = window.productDocument_.rootAssemblyDefinition();
    if (!root || window.assemblyInsertController_.active()) return false;
    std::uint64_t definitionValue = 1;
    while (window.productDocument_.definitions().contains(
        DefinitionId{definitionValue})) {
      if (definitionValue == std::numeric_limits<std::uint64_t>::max())
        return false;
      ++definitionValue;
    }
    std::uint64_t occurrenceValue = 1;
    while (root->find(OccurrenceId{occurrenceValue})) {
      if (occurrenceValue == std::numeric_limits<std::uint64_t>::max())
        return false;
      ++occurrenceValue;
    }
    window.assemblyInsertController_.activate(
        window.assemblyInsertSession_, DefinitionId{definitionValue},
        OccurrenceId{occurrenceValue});
    auto staged = window.projectApplicationService_.stageEmbeddedPart(path);
    if (!staged.succeeded()) return false;
    if (!window.viewport_->prepareAssemblyPlacementPreview(
            *staged.staged->document))
      return false;
    if (!window.assemblyInsertSession_.acceptSource(
            std::move(*staged.staged)))
      return false;
    window.resetAssemblyPlacementControls(true);
    if (window.assemblyApplyButton_)
      window.assemblyApplyButton_->setEnabled(true);
    if (window.assemblyCancelButton_)
      window.assemblyCancelButton_->setEnabled(true);
    return true;
  }
  static void beginAssemblyInsertUi(MainWindow& window) {
    window.beginAssemblyInsert();
  }
  static bool setAssemblyInsertPlacement(
      MainWindow& window, std::array<double, 3> translation,
      std::array<double, 3> rotation) {
    return window.assemblyInsertSession_.updateTranslation(translation) &&
           window.assemblyInsertSession_.updateRotationDegrees(rotation) &&
           window.viewport_->setAssemblyPlacementPreviewTransform(
               window.assemblyInsertSession_.placement());
  }
  static void applyAssemblyInsert(MainWindow& window) {
    window.applyAssemblyInsert();
  }
  static void cancelAssemblyInsert(MainWindow& window) {
    window.cancelAssemblyInsert();
  }
  static void undoAssemblyInsert(MainWindow& window) {
    window.undoLastAction();
  }
  static void redoAssemblyInsert(MainWindow& window) {
    window.redoLastAction();
  }
  static std::size_t assemblyOccurrenceCount(const MainWindow& window) {
    const auto* root = window.productDocument_.rootAssemblyDefinition();
    return root ? root->size() : 0;
  }
  static bool assemblyInsertPreviewActive(const MainWindow& window) {
    return window.viewport_->assemblyPlacementPreview().active();
  }
  static std::uint64_t assemblyInsertPreviewToken(const MainWindow& window) {
    return window.viewport_->assemblyPlacementPreview().token();
  }
  static const Document* assemblyInsertSourceAddress(const MainWindow& window) {
    const auto* source = window.assemblyInsertSession_.source();
    return source ? source->document.get() : nullptr;
  }
  static bool assemblyPlacementControlsIdentityAndEnabled(
      const MainWindow& window, bool enabled) {
    for (auto* spin : window.assemblyTranslationSpins_)
      if (!spin || spin->isEnabled() != enabled || spin->value() != 0.0)
        return false;
    for (auto* spin : window.assemblyRotationSpins_)
      if (!spin || spin->isEnabled() != enabled || spin->value() != 0.0)
        return false;
    return true;
  }
  static void setAssemblyPlacementControls(
      MainWindow& window, std::array<double, 3> translation,
      std::array<double, 3> rotation) {
    for (std::size_t i = 0; i < 3; ++i) {
      window.assemblyTranslationSpins_[i]->setValue(translation[i]);
      window.assemblyRotationSpins_[i]->setValue(rotation[i]);
    }
  }
  static void setAssemblyTeardownObserver(
      MainWindow& window,
      std::function<void(AssemblyInsertTeardownStep, bool)> observer) {
    window.assemblyInsertTeardownObserver_ = std::move(observer);
  }
  static AssemblyInsertToolState assemblyInsertState(const MainWindow& window) {
    return window.assemblyInsertSession_.state();
  }
  static std::size_t assemblyInsertHistorySize(const MainWindow& window) {
    return window.assemblyCommandHistory_.size();
  }
  static std::size_t assemblyInsertHistoryCursor(const MainWindow& window) {
    return window.assemblyCommandHistory_.cursor();
  }
  static bool assemblyRenameEnabled(const MainWindow& window) {
    return window.assemblyRenameAction_ &&
           window.assemblyRenameAction_->isEnabled() &&
           window.assemblyRenameButton_ &&
           window.assemblyRenameButton_->isEnabled() &&
           window.assemblyRenameShortcut_ &&
           window.assemblyRenameShortcut_->isEnabled();
  }
  static void triggerAssemblyRename(MainWindow& window) {
    window.assemblyRenameAction_->trigger();
  }
  static bool assemblyRenameEntrypointsConfigured(const MainWindow& window) {
    return window.assemblyRenameAction_ &&
           window.assemblyRenameAction_->text() ==
               QString::fromUtf8("Переименовать экземпляр…") &&
           window.assemblyRenameButton_ && window.assemblyRenameShortcut_ &&
           window.assemblyRenameShortcut_->key() == QKeySequence(Qt::Key_F2) &&
           window.assemblyTreeView_->contextMenuPolicy() ==
               Qt::CustomContextMenu;
  }
  static bool assemblyVisibilityEnabled(const MainWindow& window) {
    return window.assemblyVisibilityAction_ &&
           window.assemblyVisibilityAction_->isEnabled() &&
           window.assemblyVisibilityButton_ &&
           window.assemblyVisibilityButton_->isEnabled() &&
           window.assemblyVisibilityShortcut_ &&
           window.assemblyVisibilityShortcut_->isEnabled();
  }
  static QString assemblyVisibilityText(const MainWindow& window) {
    return window.assemblyVisibilityAction_
               ? window.assemblyVisibilityAction_->text()
               : QString{};
  }
  static void triggerAssemblyVisibility(MainWindow& window) {
    window.assemblyVisibilityAction_->trigger();
  }
  static bool assemblyGroundingEnabled(const MainWindow& window) {
    return window.assemblyGroundingAction_ &&
           window.assemblyGroundingAction_->isEnabled() &&
           window.assemblyGroundingButton_ &&
           window.assemblyGroundingButton_->isEnabled() &&
           window.assemblyGroundingShortcut_ &&
           window.assemblyGroundingShortcut_->isEnabled();
  }
  static QString assemblyGroundingText(const MainWindow& window) {
    return window.assemblyGroundingAction_
               ? window.assemblyGroundingAction_->text()
               : QString{};
  }
  static void triggerAssemblyGrounding(MainWindow& window) {
    window.assemblyGroundingAction_->trigger();
  }
  static void triggerAssemblyGroundingShortcut(MainWindow& window) {
    CHECK(QMetaObject::invokeMethod(window.assemblyGroundingShortcut_,
                                    "activated", Qt::DirectConnection));
  }
  static bool selectedOccurrenceGrounded(const MainWindow& window) {
    const auto& selected = window.viewport_->selectedAssemblyOccurrenceView();
    if (!selected) return false;
    const auto resolved = window.productDocument_.resolveRootOccurrence(
        selected->occurrence.path);
    return resolved && resolved.occurrence->flags().grounded;
  }
  static bool assemblySuppressionEnabled(const MainWindow& window) {
    return window.assemblySuppressionAction_ &&
           window.assemblySuppressionAction_->isEnabled() &&
           window.assemblySuppressionButton_ &&
           window.assemblySuppressionButton_->isEnabled() &&
           window.assemblySuppressionShortcut_ &&
           window.assemblySuppressionShortcut_->isEnabled();
  }
  static QString assemblySuppressionText(const MainWindow& window) {
    return window.assemblySuppressionAction_
               ? window.assemblySuppressionAction_->text()
               : QString{};
  }
  static bool assemblySuppressionEntrypointsConfigured(
      const MainWindow& window) {
    return window.assemblySuppressionAction_ &&
           window.assemblySuppressionButton_ &&
           window.assemblySuppressionShortcut_ &&
           window.assemblySuppressionShortcut_->key() ==
               QKeySequence(Qt::SHIFT | Qt::Key_Space) &&
           window.assemblyVisibilityShortcut_ &&
           window.assemblyVisibilityShortcut_->key() ==
               QKeySequence(Qt::Key_Space) &&
           window.assemblySuppressionShortcut_->key() !=
               window.assemblyVisibilityShortcut_->key();
  }
  static void triggerAssemblySuppression(MainWindow& window) {
    window.assemblySuppressionAction_->trigger();
  }
  static void triggerAssemblySuppressionShortcut(MainWindow& window) {
    CHECK(QMetaObject::invokeMethod(window.assemblySuppressionShortcut_,
                                    "activated", Qt::DirectConnection));
  }
  static bool selectedOccurrenceSuppressed(const MainWindow& window) {
    const auto& selected = window.viewport_->selectedAssemblyOccurrenceView();
    if (!selected) return false;
    const auto resolved = window.productDocument_.resolveRootOccurrence(
        selected->occurrence.path);
    return resolved && resolved.occurrence->flags().suppressed;
  }
  static bool assemblyDuplicateEnabled(const MainWindow& window) {
    return window.assemblyDuplicateAction_ &&
           window.assemblyDuplicateAction_->isEnabled() &&
           window.assemblyDuplicateButton_ &&
           window.assemblyDuplicateButton_->isEnabled() &&
           window.assemblyDuplicateShortcut_ &&
           window.assemblyDuplicateShortcut_->isEnabled();
  }
  static bool assemblyDuplicateEntrypointsConfigured(
      const MainWindow& window) {
    return window.assemblyDuplicateAction_ &&
           window.assemblyDuplicateAction_->text() ==
               QString::fromUtf8("Дублировать экземпляр") &&
           window.assemblyDuplicateButton_ &&
           window.assemblyDuplicateShortcut_ &&
           window.assemblyDuplicateShortcut_->key() ==
               QKeySequence(Qt::CTRL | Qt::Key_D) &&
           window.assemblyGroundingShortcut_ &&
           window.assemblyDuplicateShortcut_->key() !=
               window.assemblyGroundingShortcut_->key();
  }
  static void triggerAssemblyDuplicate(MainWindow& window) {
    window.assemblyDuplicateAction_->trigger();
  }
  static void triggerAssemblyDuplicateShortcut(MainWindow& window) {
    CHECK(QMetaObject::invokeMethod(window.assemblyDuplicateShortcut_,
                                    "activated", Qt::DirectConnection));
  }
  static bool assemblyRemoveEnabled(const MainWindow& window) {
    return window.assemblyRemoveAction_ &&
           window.assemblyRemoveAction_->isEnabled() &&
           window.assemblyRemoveButton_ &&
           window.assemblyRemoveButton_->isEnabled() &&
           window.assemblyRemoveShortcut_ &&
           window.assemblyRemoveShortcut_->isEnabled();
  }
  static bool assemblyRemoveEntrypointsConfigured(
      const MainWindow& window) {
    return window.assemblyRemoveAction_ &&
           window.assemblyRemoveAction_->text() ==
               QString::fromUtf8("Удалить экземпляр") &&
           window.assemblyRemoveButton_ && window.assemblyRemoveShortcut_ &&
           window.assemblyRemoveShortcut_->key() ==
               QKeySequence(Qt::Key_Delete) &&
           window.assemblyRemoveShortcut_->context() ==
               Qt::WidgetWithChildrenShortcut &&
           window.assemblyRemoveShortcut_->parent() ==
               window.assemblyTreeView_;
  }
  static void triggerAssemblyRemove(MainWindow& window) {
    window.assemblyRemoveAction_->trigger();
  }
  static void triggerAssemblyRemoveShortcut(MainWindow& window) {
    CHECK(QMetaObject::invokeMethod(window.assemblyRemoveShortcut_,
                                    "activated", Qt::DirectConnection));
  }
  static bool definitionIsOrphan(const MainWindow& window,
                                 DefinitionId definitionId) {
    if (!window.assemblyTreeModel_) return false;
    for (int row = 0; row < window.assemblyTreeModel_->rowCount(); ++row) {
      const QModelIndex index = window.assemblyTreeModel_->index(row, 0);
      const auto address = window.assemblyTreeModel_->addressForIndex(index);
      if (address &&
          address->get().kind == AssemblyTreeNodeKind::PartDefinition &&
          address->get().definitionId == definitionId)
        return window.assemblyTreeModel_
            ->data(index, kAssemblyTreeOrphanDefinitionRole)
            .toBool();
    }
    return false;
  }
  static std::optional<OccurrenceId> selectedOccurrenceId(
      const MainWindow& window) {
    const auto& selected = window.viewport_->selectedAssemblyOccurrenceView();
    if (!selected || selected->occurrence.path.depth() != 1)
      return std::nullopt;
    return selected->occurrence.path.entries().front();
  }
  static bool assemblyIsolationEnabled(const MainWindow& window) {
    return window.assemblyIsolationAction_ &&
           window.assemblyIsolationAction_->isEnabled() &&
           window.assemblyIsolationButton_ &&
           window.assemblyIsolationButton_->isEnabled();
  }
  static QString assemblyIsolationText(const MainWindow& window) {
    return window.assemblyIsolationAction_
               ? window.assemblyIsolationAction_->text()
               : QString{};
  }
  static void triggerAssemblyIsolation(MainWindow& window) {
    window.assemblyIsolationAction_->trigger();
  }
  static void triggerAssemblyEscape(MainWindow& window) {
    CHECK(QMetaObject::invokeMethod(window.assemblyInsertEscapeShortcut_,
                                    "activated", Qt::DirectConnection));
  }
  static bool assemblyIsolationActive(const MainWindow& window) {
    return window.viewport_ && window.viewport_->assemblySession() &&
           window.viewport_->assemblySession()->isolationActive();
  }
  static std::optional<OccurrencePath> isolatedOccurrencePath(
      const MainWindow& window) {
    const auto* session = window.viewport_->assemblySession();
    return session && session->isolatedOccurrence()
               ? std::optional<OccurrencePath>{
                     session->isolatedOccurrence()->path}
               : std::nullopt;
  }
  static bool selectedOccurrenceVisible(const MainWindow& window) {
    const auto& selected = window.viewport_->selectedAssemblyOccurrenceView();
    if (!selected) return false;
    const auto resolved = window.productDocument_.resolveRootOccurrence(
        selected->occurrence.path);
    return resolved && resolved.occurrence->flags().visible;
  }
  static std::size_t displayedAssemblyInstanceCount(const MainWindow& window) {
    const auto* session = window.viewport_->assemblySession();
    return session && session->scene() ? session->scene()->instances().size()
                                       : 0;
  }
  static void clickAssemblyRenameButton(MainWindow& window) {
    window.assemblyRenameButton_->click();
  }
  static void triggerAssemblyRenameShortcut(MainWindow& window) {
    CHECK(QMetaObject::invokeMethod(window.assemblyRenameShortcut_,
                                    "activated", Qt::DirectConnection));
  }
  static std::string selectedOccurrenceDisplayName(const MainWindow& window) {
    const auto& selected = window.viewport_->selectedAssemblyOccurrenceView();
    if (!selected) return {};
    const auto resolved = window.productDocument_.resolveRootOccurrence(
        selected->occurrence.path);
    return resolved ? resolved.occurrence->displayNameUtf8() : std::string{};
  }
  static QString assemblyTreeCurrentText(const MainWindow& window) {
    return window.assemblyTreeModel_
               ? window.assemblyTreeModel_
                     ->data(window.assemblyTreeView_->currentIndex())
                     .toString()
               : QString{};
  }
  static const AssemblyTreeModel* assemblyTreeModelAddress(
      const MainWindow& window) {
    return window.assemblyTreeModel_;
  }
  static bool readOnlyActionsDisabled(const MainWindow& window) {
    return window.createAction_ && !window.createAction_->isEnabled() &&
           window.undoAction_ && !window.undoAction_->isEnabled() &&
           window.redoAction_ && !window.redoAction_->isEnabled() &&
           window.saveAction_ && !window.saveAction_->isEnabled() &&
           window.saveProjectAsAction_ &&
           !window.saveProjectAsAction_->isEnabled() &&
           window.importStepAction_ && !window.importStepAction_->isEnabled() &&
           window.exportStepAction_ && !window.exportStepAction_->isEnabled() &&
           window.exportStlAction_ && !window.exportStlAction_->isEnabled() &&
           window.historyToggleAction_ &&
           !window.historyToggleAction_->isEnabled() &&
           window.modelRibbon_ && !window.modelRibbon_->isEnabled() &&
           window.sketchRibbon_ && !window.sketchRibbon_->isEnabled() &&
           window.sketchCanvas_ && !window.sketchCanvas_->isEnabled() &&
           window.featureTree_ && !window.featureTree_->isEnabled() &&
           window.historyTimeline_ && !window.historyTimeline_->isEnabled() &&
           window.assemblyRibbon_ && window.assemblyRibbon_->isEnabled() &&
           window.assemblyTreeView_ && window.assemblyTreeView_->isEnabled() &&
           window.findChild<QToolButton*>(
               QStringLiteral("assemblyDisplayModeCommand")) &&
           window.findChild<QToolButton*>(
               QStringLiteral("assemblyDisplayModeCommand"))->isEnabled();
  }
  static void exerciseReadOnlyQtCallbacks(MainWindow& window) {
    // Disabled actions are still programmatically addressable. Exercise both
    // QAction dispatch and the direct slot so the modal-producing New path is
    // proven to fail closed before it can open a dialog.
    window.createAction_->trigger();
    window.createProject();
    window.undoAction_->trigger();
    window.redoAction_->trigger();
    window.saveAction_->trigger();
    window.saveProjectAsAction_->trigger();
    window.importStepAction_->trigger();
    window.exportStepAction_->trigger();
    window.exportStlAction_->trigger();
    window.upgradeProjectFormatAction_->trigger();
    window.convertToProductDocumentAction_->trigger();
    window.relocateProductDocumentAction_->trigger();
    window.historyToggleAction_->setChecked(true);

    // Hidden part widgets remain alive after an assembly switch. Emit their
    // signals directly and also mutate representative controls whose value
    // signals previously reached the coordinator or viewport.
    emit window.modelRibbon_->createSketchRequested();
    emit window.modelRibbon_->referenceImageRequested();
    emit window.modelRibbon_->extrudeRequested();
    emit window.modelRibbon_->revolveRequested();
    emit window.modelRibbon_->pocketRequested();
    emit window.modelRibbon_->filletRequested();
    emit window.modelRibbon_->chamferRequested();
    emit window.modelRibbon_->joinBodiesRequested();
    emit window.modelRibbon_->moveRequested();
    emit window.modelRibbon_->shellRequested();
    emit window.modelRibbon_->draftRequested();
    emit window.modelRibbon_->mirrorRequested();
    emit window.modelRibbon_->linearPatternRequested();
    emit window.modelRibbon_->circularPatternRequested();
    emit window.modelRibbon_->rulerToggled(true);
    emit window.modelRibbon_->meshQualityRequested(
        static_cast<int>(ViewportMeshQuality::High));
    emit window.sketchRibbon_->finishRequested();

    emit window.toolParametersPanel_->parameterChanged(2.0);
    emit window.toolParametersPanel_->accepted();
    emit window.toolParametersPanel_->cancelled();
    emit window.toolParametersPanel_->selectionRequested();
    emit window.toolParametersPanel_->clearSelectionRequested();
    emit window.toolParametersPanel_->optionChanged(true);

    emit window.viewport_->revolveAxisPicked(1);
    emit window.viewport_->moveBodyPicked(kInvalidBodyId);
    emit window.viewport_->mirrorBodyPicked(kInvalidBodyId);
    emit window.viewport_->mirrorPlanePicked(0);
    emit window.viewport_->linearPatternBodyPicked(kInvalidBodyId);
    emit window.viewport_->linearPatternAxisPicked(0);
    emit window.viewport_->circularPatternBodyPicked(kInvalidBodyId);
    emit window.viewport_->circularPatternAxisPicked(0);
    emit window.viewport_->draftAxisPicked(0);
    emit window.viewport_->toolManipulatorValueChanged(2.0);
    emit window.viewport_->translationToolManipulatorValueChanged(0, 2.0);
    emit window.viewport_->angularToolManipulatorValueChanged(15.0);
    emit window.viewport_->bodyEdgeSelectionChanged();
    emit window.viewport_->bodyFaceSelectionChanged();
    emit window.viewport_->bodiesSelected({kInvalidBodyId});
    emit window.viewport_->toolManipulatorDragFinished();
    emit window.viewport_->extrusionManipulatorDragFinished();
    emit window.viewport_->toolParameterCommitted();
    emit window.viewport_->interactionCancelled(
        ViewportCancelReason::ActiveTool);

    emit window.sketchCanvas_->geometryChanged(1.0, 1.0);
    emit window.sketchCanvas_->selectionChanged(QStringLiteral("foreign"));
    emit window.workspaceStack_->currentChanged(
        window.workspaceStack_->indexOf(window.sketchCanvas_));
    if (auto* item = window.featureTree_->topLevelItem(0)) {
      emit window.featureTree_->itemChanged(item, 0);
      emit window.featureTree_->currentItemChanged(item, nullptr);
    }
    emit window.historyTimeline_->positionChanged(1);

    window.mirrorBodySelectButton_->click();
    window.moveBodySelectButton_->click();
    window.linearPatternBodySelectButton_->click();
    window.circularPatternBodySelectButton_->click();
    window.moveXSpin_->setValue(window.moveXSpin_->value() + 1.0);
    window.linearPatternSpacingSpin_->setValue(
        window.linearPatternSpacingSpin_->value() + 1.0);
    window.linearPatternCountSpin_->setValue(
        window.linearPatternCountSpin_->value() + 1);
    window.circularPatternAngleSpin_->setValue(
        window.circularPatternAngleSpin_->value() + 1.0);
    window.circularPatternCountSpin_->setValue(
        window.circularPatternCountSpin_->value() + 1);
    window.extrusionLengthSpin_->setValue(
        window.extrusionLengthSpin_->value() + 1.0);
  }
  static std::unique_ptr<AssemblyViewportSession>
  prepareForeignEquivalentAssemblySession(const MainWindow& window) {
    const auto profile = AssemblyMeshProfile{
        window.viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto prepared = AssemblyViewportSession::prepare(
        window.productDocument_,
        ProductPresentationEpoch{window.assemblyPresentationEpochClock_},
        profile);
    return prepared ? std::move(prepared.session) : nullptr;
  }
  static void emitAssemblyOccurrenceSelected(
      MainWindow& window, const AssemblyOccurrenceSelection& occurrence) {
    emit window.viewport_->assemblyOccurrenceSelected(occurrence);
  }
  static void emitAssemblyOccurrenceSelectionCleared(
      MainWindow& window,
      const AssemblyScenePublicationHandle& publication) {
    emit window.viewport_->assemblyOccurrenceSelectionCleared(publication);
  }
  static void primeSketchReferenceTransientForAssemblySwitch(
      MainWindow& window) {
    window.workspaceStack_->setCurrentWidget(window.sketchCanvas_);
    window.pendingReferenceImagePath_ = QStringLiteral("pending-image.png");
    window.pendingReferenceImagePixelSize_ = QSize(32, 24);
    window.selectedReferenceImageId_ = ReferenceImageId{777};
    window.referenceImageEditBefore_ = window.activePartDocument();
    window.referenceImageEditSelectionBefore_ = window.captureHistorySelection();
    ReferenceImage image;
    image.id = window.selectedReferenceImageId_;
    image.name = "pending";
    image.pixelWidth = 32;
    image.pixelHeight = 24;
    window.viewport_->setReferenceImageManipulator(image);
  }
  static bool assemblyUiHasNoPartTransients(const MainWindow& window) {
    return window.workspaceStack_->currentWidget() == window.viewport_ &&
           window.ribbonStack_->currentWidget() == window.assemblyRibbon_ &&
           window.pendingReferenceImagePath_.isEmpty() &&
           window.pendingReferenceImagePixelSize_.isEmpty() &&
           window.selectedReferenceImageId_ == kInvalidReferenceImageId &&
           !window.referenceImageEditBefore_ &&
           !window.referenceImageEditSelectionBefore_ &&
           window.historyDock_->isHidden() &&
           !window.historyToggleAction_->isChecked() &&
           !window.viewport_->referenceImageManipulatorActive() &&
           !window.viewport_->toolPreviewShape_;
  }
  static const std::optional<ProductDocumentIdentity>& identity(
      const MainWindow& window) {
    return window.productDocument_.identity();
  }
  static const Document* rootDefinitionDocumentAddress(
      const MainWindow& window) {
    return &window.productDocument_.rootDefinition().document();
  }
  static std::weak_ptr<const TopoDS_Shape> firstShapeWeak(
      const MainWindow& window) {
    if (window.activePartDocument().bodies().empty()) return {};
    const auto* feature = window.activePartDocument().bodies().front().activeFeature();
    return feature ? feature->lastValidShape()
                   : std::weak_ptr<const TopoDS_Shape>{};
  }
  static void commitStaged(MainWindow& window,
                           StagedApplicationDocument&& staged,
                           const QString& path) {
    window.commitDocumentReplacement(
        std::move(staged), MainWindow::DocumentReplacementOrigin::OpenProject,
        path);
  }
  static void saveProject(MainWindow& window) { window.saveProject(); }
  static bool saveProjectToPath(MainWindow& window, const QString& path,
                                QString* error) {
    return window.saveProjectToPath(path, error);
  }
  static bool pushBodyMarker(MainWindow& window, const std::string& name) {
    Document previous = window.activePartDocument();
    const auto previousSelection = window.captureHistorySelection();
    window.mutableStandaloneDocument().addBody(name);
    if (!window.mutableStandaloneDocument().recompute()) {
      window.mutableStandaloneDocument() = std::move(previous);
      return false;
    }
    return window.pushModelTransition(std::move(previous),
                                      previousSelection);
  }
  static std::size_t undoCount(const MainWindow& window) {
    return window.modelHistory_.undoCount();
  }
  static std::size_t retainedBytes(const MainWindow& window) {
    return window.modelHistory_.retainedBytes();
  }
  static void setHistoryByteBudget(MainWindow& window, std::size_t bytes) {
    window.modelHistory_.setByteBudget(bytes);
  }
  static bool applyingUndo(const MainWindow& window) {
    return window.modelHistory_.isApplying();
  }
  static void pushThrowingUndo(MainWindow& window) {
    Document invalid = window.activePartDocument();
    Body* body = invalid.activeBody();
    if (!body) body = &invalid.addBody("failure fixture");
    auto broken = std::make_unique<MoveFeature>(
        FeatureId{987654321}, Vector3d{1.0, 0.0, 0.0},
        "undo failure fixture");
    const FeatureId brokenId = broken->id();
    body->addFeature(std::move(broken));
    static_cast<void>(invalid.recompute());
    Document current = invalid;
    const auto index = current.findBody(body->id())->featureIndex(brokenId);
    if (!index || !current.applyFeatureSlice(body->id(), *index, nullptr) ||
        !current.recompute())
      return;
    window.mutableStandaloneDocument() = std::move(current);
    auto state = window.captureHistorySelection();
    static_cast<void>(window.modelHistory_.recordTransition(
        window.mutableStandaloneDocument(), std::move(invalid), state, state));
    window.updateUndoAvailability();
  }
  static std::pair<std::size_t, std::size_t> documentDeltaSize(
      const Document& before, const Document& after) {
    const auto metrics = ModelCommandHistory::inspectDelta(before, after);
    return {metrics.sliceCount, metrics.retainedBytes};
  }
  static Document documentCopy(const MainWindow& window) {
    return window.activePartDocument();
  }
  static bool importedHistoryIsColdAndRebuildable() {
    std::weak_ptr<const TopoDS_Shape> sourceWeak;
    std::weak_ptr<const TopologyIndex> topologyWeak;
    Document restored;
    ModelCommandHistory history;
    EditorCommittedState state;
    {
      Document before;
      auto& body = before.addBody("Imported heavy body");
      auto source = std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(30.0, 20.0, 10.0).Shape());
      sourceWeak = source;
      body.addFeature(std::make_unique<ImportedShapeFeature>(source, "Import"));
      source.reset();
      if (!before.recompute()) return false;
      const auto* imported = dynamic_cast<const ImportedShapeFeature*>(
          before.bodies().front().features().front().get());
      if (!imported || !imported->hasShape()) return false;
      std::string indexError;
      auto index = imported->lastValidTopologyIndex(&indexError);
      if (!index || !indexError.empty()) return false;
      topologyWeak = index;
      index.reset();
      const auto result = history.recordTransition(
          restored, std::move(before), state, state);
      if (result.status != HistoryCommitStatus::Accepted) return false;
    }
    if (!sourceWeak.expired() || !topologyWeak.expired()) return false;
    const auto undo = history.undo(restored, state);
    if (!undo.changed) return false;
    const auto* restoredImported = dynamic_cast<const ImportedShapeFeature*>(
        restored.bodies().front().features().front().get());
    return restoredImported && restoredImported->hasShape() &&
           restoredImported->importedShape() &&
           test::near(test::volumeOf(*restoredImported->shape()), 6000.0);
  }
  static void installDocument(MainWindow& window, Document document) {
    window.applyImportedDocument(std::move(document),
                                 QStringLiteral("history-matrix"));
    reset(window);
  }
  static void selectBody(MainWindow& window, BodyId id) {
    window.viewport_->setSelectedBodyEdges({});
    window.viewport_->setSelectedBodyFaces({});
    window.viewport_->setSelectedBodies({id});
    window.moveHistoryToEnd();
  }
  static bool selectSentinel(MainWindow& window, BodyId bodyId,
                             FeatureId featureId, int domain) {
    auto* feature = dynamic_cast<ShapeFeature*>(
        window.mutableStandaloneDocument().findFeature(featureId));
    if (!feature || !feature->lastValidShape()) return false;
    window.viewport_->setSelectedBodies({});
    window.viewport_->setSelectedBodyEdges({});
    window.viewport_->setSelectedBodyFaces({});
    if (domain == 0) {
      window.viewport_->setSelectedBodies({bodyId});
    } else if (domain == 1) {
      window.viewport_->setSelectedBodyEdges({makeEdgeReference(
          *feature->lastValidShape(), bodyId, featureId, 0)});
    } else {
      window.viewport_->setSelectedBodyFaces({makeFaceReference(
          *feature->lastValidShape(), bodyId, featureId, 0)});
    }
    window.moveHistoryToEnd();
    return true;
  }
  static void clearSelection(MainWindow& window) {
    window.viewport_->setSelectedBodyEdges({});
    window.viewport_->setSelectedBodyFaces({});
    window.viewport_->setSelectedBodies({});
    window.moveHistoryToEnd();
  }
  static bool commitBodyRemoval(MainWindow& window, BodyId id) {
    Document previous = window.activePartDocument();
    const auto selection = window.captureHistorySelection();
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i < window.activePartDocument().bodies().size(); ++i)
      if (window.activePartDocument().bodies()[i].id() == id) index = i;
    if (!index || !window.mutableStandaloneDocument().applyBodySlice(*index, std::nullopt) ||
        !window.mutableStandaloneDocument().recompute()) {
      window.mutableStandaloneDocument() = std::move(previous);
      return false;
    }
    window.refreshBodyViewFromDocument();
    window.rebuildFeatureTree();
    window.rebuildHistoryPanel();
    return window.pushModelTransition(std::move(previous), selection);
  }
  static bool acceptFixtureThroughProduction(MainWindow& window,
                                             const Document& reference,
                                             FeatureId featureId) {
    const auto* target = reference.findFeature(featureId);
    if (!target) return false;
    window.resetTransientModelingUi();
    const auto bodyForFeature = [&window](FeatureId id) -> Body* {
      for (const auto& candidate : window.activePartDocument().bodies())
        if (candidate.featureIndex(id))
          return window.mutableStandaloneDocument().findBody(candidate.id());
      return nullptr;
    };
    const auto source = [&window, &bodyForFeature](FeatureId id)
        -> std::pair<Body*, ShapeFeature*> {
      Body* body = bodyForFeature(id);
      return {body, body ? dynamic_cast<ShapeFeature*>(
                               window.mutableStandaloneDocument().findFeature(id))
                         : nullptr};
    };

    if (const auto* value = dynamic_cast<const ExtrudeFeature*>(target)) {
      if (value->isFaceSource()) {
        const auto face = value->faceReference();
        if (!face) return false;
        const auto [body, sourceFeature] = source(face->featureId());
        if (!body || !sourceFeature) return false;
        window.partDesignCoordinator_.beginExtrude(
            body->id(), sourceFeature->id(), sourceFeature->shape(), *face,
            value->lengthMm(), value->operation(), value->reversed(),
            std::nullopt, sourceFeature->topologyIndex());
      } else {
        const auto* profile = window.activePartDocument().findSketch(value->profileSketchId());
        if (!profile) return false;
        window.partDesignCoordinator_.beginSketchExtrude(
            *profile, profile->id, {}, value->lengthMm(), value->operation(),
            value->reversed(), std::nullopt, value->profileOverride());
      }
      window.toolParametersPanel_->setParameterValue(value->lengthMm());
      window.updateFaceExtrudeToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptFaceExtrudeTool();
    } else if (const auto* value = dynamic_cast<const RevolveFeature*>(target)) {
      window.partDesignCoordinator_.beginRevolve(window.activePartDocument(), kInvalidBodyId,
                                       kInvalidFeatureId, {});
      window.partDesignCoordinator_.setRevolveProfile(window.activePartDocument(), value->profileSketchId(),
                                            value->profileOverride());
      window.partDesignCoordinator_.setRevolveAxis(window.activePartDocument(), value->axis());
      window.partDesignCoordinator_.setRevolveAngleFromPanel(window.activePartDocument(), value->angleDeg());
      window.partDesignCoordinator_.setRevolveOperation(window.activePartDocument(), value->operation());
      window.partDesignCoordinator_.setRevolveReversed(window.activePartDocument(), value->reversed());
      window.updateRevolveToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, window.activePartDocument()).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptRevolveTool();
    } else if (const auto* value = dynamic_cast<const PocketFeature*>(target)) {
      QTimer::singleShot(0, [depth = value->depthMm()] {
        auto* dialog = qobject_cast<QInputDialog*>(
            QApplication::activeModalWidget());
        if (!dialog) return;
        const auto spins = dialog->findChildren<QDoubleSpinBox*>();
        if (!spins.empty()) spins.front()->setValue(depth);
        dialog->accept();
      });
      window.createPocket();
    } else if (const auto* value = dynamic_cast<const FilletFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->edge().featureId());
      if (!body || !sourceFeature) return false;
      const auto sourceSnapshot = body->topologySnapshot(sourceFeature->id());
      if (!sourceSnapshot) return false;
      window.partDesignCoordinator_.beginFillet(
          *sourceSnapshot, value->edges(), value->radiusMm());
      window.toolParametersPanel_->setParameterValue(value->radiusMm());
      window.updateFilletToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptFilletTool();
    } else if (const auto* value = dynamic_cast<const ChamferFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->edge().featureId());
      if (!body || !sourceFeature) return false;
      const auto sourceSnapshot = body->topologySnapshot(sourceFeature->id());
      if (!sourceSnapshot) return false;
      window.partDesignCoordinator_.beginChamfer(
          *sourceSnapshot, value->edges(), value->distanceMm());
      window.toolParametersPanel_->setParameterValue(value->distanceMm());
      window.updateChamferToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptChamferTool();
    } else if (const auto* value = dynamic_cast<const ShellFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginShell(body->id(), sourceFeature->id(),
          sourceFeature->shape(), value->removedFaces(), value->thicknessMm(),
          value->outside(), std::nullopt, sourceFeature->topologyIndex());
      window.toolParametersPanel_->setParameterValue(value->thicknessMm());
      window.updateShellToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptShellTool();
    } else if (const auto* value = dynamic_cast<const DraftFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      const auto sourceSnapshot = body->topologySnapshot(sourceFeature->id());
      if (!sourceSnapshot) return false;
      window.partDesignCoordinator_.beginDraft(window.activePartDocument(), *sourceSnapshot,
          value->draftedFaces(),
          value->neutralPlane(), value->pullDirection(), value->angleDeg(),
          value->reversed(), std::nullopt, value->rotationEdge());
      window.updateDraftToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, window.activePartDocument()).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptDraftTool();
    } else if (const auto* value = dynamic_cast<const JoinBodiesFeature*>(target)) {
      const auto [firstBody, firstFeature] = source(value->firstFeatureId());
      const auto [secondBody, secondFeature] = source(value->secondFeatureId());
      if (!firstBody || !secondBody || !firstFeature || !secondFeature)
        return false;
      window.partDesignCoordinator_.beginJoinBodies();
      window.partDesignCoordinator_.setJoinBodies({
          {firstBody->id(), firstFeature->id(), firstFeature->shape()},
          {secondBody->id(), secondFeature->id(), secondFeature->shape()}});
      window.updateJoinBodiesToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_ ||
          window.viewport_->toolPreviewReplacedBodyIds_.size() != 2)
        return false;
      window.acceptJoinBodiesTool();
    } else if (const auto* value = dynamic_cast<const MoveFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginMove(value->offsetMm());
      window.partDesignCoordinator_.setMoveBody(body->id(), sourceFeature->id(),
                                      sourceFeature->shape());
      window.updateMoveToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptMoveTool();
    } else if (const auto* value = dynamic_cast<const MirrorFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginMirror();
      window.partDesignCoordinator_.setMirrorBody(body->id(), sourceFeature->id(),
                                        sourceFeature->shape());
      window.partDesignCoordinator_.setMirrorPlane(value->plane());
      window.updateMirrorToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptMirrorTool();
    } else if (const auto* value =
                   dynamic_cast<const LinearPatternFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginLinearPattern(value->spacingMm(), value->count(),
                                             value->operation());
      window.partDesignCoordinator_.setLinearPatternBody(body->id(), sourceFeature->id(),
                                               sourceFeature->shape());
      window.partDesignCoordinator_.setLinearPatternDirection(value->direction());
      window.updateLinearPatternToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptLinearPatternTool();
    } else if (const auto* value =
                   dynamic_cast<const CircularPatternFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginCircularPattern(value->angleDeg(), value->count(),
                                               value->operation());
      window.partDesignCoordinator_.setCircularPatternBody(body->id(), sourceFeature->id(),
                                                 sourceFeature->shape());
      window.partDesignCoordinator_.setCircularPatternAxis(value->axis());
      window.updateCircularPatternToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptCircularPatternTool();
    } else {
      return false;
    }
    if (!window.modelHistory_.canUndo()) {
      if (window.modelHistory_.byteBudget() > 1U)
        std::cerr << "production accept failed for kind "
                  << static_cast<int>(target->kind())
                  << ": fillet=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).error
                  << " filletState="
                  << static_cast<int>(window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle)
                  << " chamfer=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).error
                  << " shell=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).error
                  << " draft=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, window.activePartDocument()).error
                  << '\n';
      return false;
    }
    return true;
  }
  static bool editFeatureThroughProduction(MainWindow& window, BodyId bodyId,
                                           FeatureId featureId) {
    const auto* feature = window.activePartDocument().findFeature(featureId);
    if (!feature) return false;
    const auto oldUndoCount = window.modelHistory_.undoCount();
    expectedEditUndoSelection_ = window.captureHistorySelection();
    const auto* extrudeFeature = dynamic_cast<const ExtrudeFeature*>(feature);
    if ((extrudeFeature && !extrudeFeature->isFaceSource()) ||
        dynamic_cast<const PocketFeature*>(feature)) {
      QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        const auto spins = dialog->findChildren<QDoubleSpinBox*>();
        if (!spins.empty()) spins.front()->setValue(
            dynamic_cast<QInputDialog*>(dialog) ? 7.0 : 25.0);
        dialog->accept();
      });
      window.editHistoryFeature(bodyId, featureId);
      return window.modelHistory_.undoCount() == oldUndoCount + 1;
    }

    window.editHistoryFeature(bodyId, featureId);
    if (extrudeFeature) {
      window.partDesignCoordinator_.setExtrudeLengthFromPanel(25.0);
      window.toolParametersPanel_->setParameterValue(25.0);
      window.acceptFaceExtrudeTool();
    } else if (dynamic_cast<const RevolveFeature*>(feature)) {
      window.partDesignCoordinator_.setRevolveAngleFromPanel(window.activePartDocument(), 180.0);
      window.acceptRevolveTool();
    } else if (dynamic_cast<const FilletFeature*>(feature)) {
      window.partDesignCoordinator_.setFilletRadiusFromPanel(1.0);
      window.toolParametersPanel_->setParameterValue(1.0);
      window.acceptFilletTool();
    } else if (dynamic_cast<const ChamferFeature*>(feature)) {
      window.partDesignCoordinator_.setChamferDistanceFromPanel(0.5);
      window.toolParametersPanel_->setParameterValue(0.5);
      window.acceptChamferTool();
    } else if (dynamic_cast<const ShellFeature*>(feature)) {
      window.partDesignCoordinator_.setShellThicknessFromPanel(1.0);
      window.toolParametersPanel_->setParameterValue(1.0);
      window.acceptShellTool();
    } else if (dynamic_cast<const DraftFeature*>(feature)) {
      window.partDesignCoordinator_.setDraftAngleFromPanel(window.activePartDocument(), 3.0);
      window.acceptDraftTool();
    } else if (const auto* join =
                   dynamic_cast<const JoinBodiesFeature*>(feature)) {
      const Body* replacementBody = nullptr;
      const ShapeFeature* replacementFeature = nullptr;
      for (const auto& body : window.activePartDocument().bodies()) {
        if (body.id() == bodyId || body.id() == join->firstBodyId() ||
            body.id() == join->secondBodyId() || !body.activeFeature())
          continue;
        replacementBody = &body;
        replacementFeature = body.activeFeature();
        break;
      }
      const Body* firstBody = window.activePartDocument().findBody(join->firstBodyId());
      const auto* firstFeature = dynamic_cast<const ShapeFeature*>(
          window.activePartDocument().findFeature(join->firstFeatureId()));
      if (!replacementBody || !replacementFeature || !firstBody ||
          !firstFeature)
        return false;
      window.partDesignCoordinator_.setJoinBodies({
          {firstBody->id(), firstFeature->id(), firstFeature->lastValidShape()},
          {replacementBody->id(), replacementFeature->id(),
           replacementFeature->lastValidShape()}});
      window.acceptJoinBodiesTool();
    } else if (dynamic_cast<const MoveFeature*>(feature)) {
      window.partDesignCoordinator_.setMoveOffset({7.0, 1.0, 0.0});
      window.acceptMoveTool();
    } else if (dynamic_cast<const MirrorFeature*>(feature)) {
      window.partDesignCoordinator_.setMirrorPlane(MirrorPlane::XZ);
      window.acceptMirrorTool();
    } else if (dynamic_cast<const LinearPatternFeature*>(feature)) {
      window.partDesignCoordinator_.setLinearPatternCount(2);
      window.partDesignCoordinator_.setLinearPatternSpacing(50.0);
      window.acceptLinearPatternTool();
    } else if (dynamic_cast<const CircularPatternFeature*>(feature)) {
      window.partDesignCoordinator_.setCircularPatternCount(3);
      window.partDesignCoordinator_.setCircularPatternAngle(270.0);
      window.acceptCircularPatternTool();
    } else {
      return false;
    }
    return window.modelHistory_.undoCount() == oldUndoCount + 1;
  }
  static bool acceptUnchangedFeatureEditThroughProduction(
      MainWindow& window, BodyId bodyId, FeatureId featureId) {
    const auto* feature = window.activePartDocument().findFeature(featureId);
    if (!feature) return false;
    const auto undoCount = window.modelHistory_.undoCount();
    const auto redoCount = window.modelHistory_.redoCount();
    const auto* extrude = dynamic_cast<const ExtrudeFeature*>(feature);
    if ((extrude && !extrude->isFaceSource()) ||
        dynamic_cast<const PocketFeature*>(feature)) {
      QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(
                QApplication::activeModalWidget()))
          dialog->accept();
      });
      window.editHistoryFeature(bodyId, featureId);
    } else {
      window.editHistoryFeature(bodyId, featureId);
      if (!window.editUiTransaction_) return false;
      if (extrude) window.acceptFaceExtrudeTool();
      else if (dynamic_cast<const RevolveFeature*>(feature))
        window.acceptRevolveTool();
      else if (dynamic_cast<const FilletFeature*>(feature))
        window.acceptFilletTool();
      else if (dynamic_cast<const ChamferFeature*>(feature))
        window.acceptChamferTool();
      else if (dynamic_cast<const ShellFeature*>(feature))
        window.acceptShellTool();
      else if (dynamic_cast<const DraftFeature*>(feature))
        window.acceptDraftTool();
      else if (dynamic_cast<const JoinBodiesFeature*>(feature))
        window.acceptJoinBodiesTool();
      else if (dynamic_cast<const MoveFeature*>(feature))
        window.acceptMoveTool();
      else if (dynamic_cast<const MirrorFeature*>(feature))
        window.acceptMirrorTool();
      else if (dynamic_cast<const LinearPatternFeature*>(feature))
        window.acceptLinearPatternTool();
      else if (dynamic_cast<const CircularPatternFeature*>(feature))
        window.acceptCircularPatternTool();
      else
        return false;
    }
    const bool sameUndo = window.modelHistory_.undoCount() == undoCount;
    const bool sameRedo = window.modelHistory_.redoCount() == redoCount;
    const bool noTransaction = !window.editUiTransaction_;
    const bool inactive = transientToolsInactive(window);
    if (!(sameUndo && sameRedo && noTransaction && inactive))
      std::cerr << "unchanged helper mismatch: undo " << sameUndo
                << " redo " << sameRedo << " tx " << noTransaction
                << " inactive " << inactive << '\n';
    return sameUndo && sameRedo && noTransaction && inactive;
  }
  static bool cancelFeatureEditThroughProduction(MainWindow& window,
                                                 BodyId bodyId,
                                                 FeatureId featureId) {
    const auto* feature = window.activePartDocument().findFeature(featureId);
    if (!feature) return false;
    const auto beforeSelection = window.captureHistorySelection();
    const auto undoCount = window.modelHistory_.undoCount();
    const auto* extrude = dynamic_cast<const ExtrudeFeature*>(feature);
    if ((extrude && !extrude->isFaceSource()) ||
        dynamic_cast<const PocketFeature*>(feature)) {
      QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(
                QApplication::activeModalWidget()))
          dialog->reject();
      });
      window.editHistoryFeature(bodyId, featureId);
    } else {
      window.editHistoryFeature(bodyId, featureId);
      if (extrude) window.cancelFaceExtrudeTool();
      else if (dynamic_cast<const RevolveFeature*>(feature))
        window.cancelRevolveTool();
      else if (dynamic_cast<const FilletFeature*>(feature))
        window.cancelFilletTool();
      else if (dynamic_cast<const ChamferFeature*>(feature))
        window.cancelChamferTool();
      else if (dynamic_cast<const ShellFeature*>(feature))
        window.cancelShellTool();
      else if (dynamic_cast<const DraftFeature*>(feature))
        window.cancelDraftTool();
      else if (dynamic_cast<const JoinBodiesFeature*>(feature))
        window.cancelJoinBodiesTool();
      else if (dynamic_cast<const MoveFeature*>(feature))
        window.cancelMoveTool();
      else if (dynamic_cast<const MirrorFeature*>(feature))
        window.cancelMirrorTool();
      else if (dynamic_cast<const LinearPatternFeature*>(feature))
        window.cancelLinearPatternTool();
      else if (dynamic_cast<const CircularPatternFeature*>(feature))
        window.cancelCircularPatternTool();
      else
        return false;
    }
    return window.modelHistory_.undoCount() == undoCount &&
           sameSelectionState(window.captureHistorySelection(),
                              beforeSelection) &&
           transientToolsInactive(window);
  }
  static bool editJoinOperands(MainWindow& window, BodyId ownerBodyId,
                               FeatureId joinId, BodyId firstBodyId,
                               BodyId secondBodyId) {
    const auto oldUndoCount = window.modelHistory_.undoCount();
    window.editHistoryFeature(ownerBodyId, joinId);
    const Body* first = window.activePartDocument().findBody(firstBodyId);
    const Body* second = window.activePartDocument().findBody(secondBodyId);
    if (!first || !second || !first->activeFeature() ||
        !second->activeFeature())
      return false;
    window.partDesignCoordinator_.setJoinBodies({
        {first->id(), first->activeFeature()->id(),
         first->activeFeature()->lastValidShape()},
        {second->id(), second->activeFeature()->id(),
         second->activeFeature()->lastValidShape()}});
    window.updateJoinBodiesToolPreview();
    if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle !=
        ToolLifecycle::PreviewValid)
      return false;
    window.acceptJoinBodiesTool();
    return window.modelHistory_.undoCount() == oldUndoCount + 1;
  }
  static bool cancelJoinEdit(MainWindow& window, BodyId ownerBodyId,
                             FeatureId joinId,
                             BodyId sharedOperand = kInvalidBodyId,
                             BodyId exclusiveOperand = kInvalidBodyId) {
    const auto payloadBefore = window.activePartDocument().findFeature(joinId);
    if (!payloadBefore) return false;
    const auto undoBefore = window.modelHistory_.undoCount();
    window.editHistoryFeature(ownerBodyId, joinId);
    if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle == ToolLifecycle::Inactive)
      return false;
    const auto presented = [&window](BodyId id) {
      return std::any_of(window.viewport_->bodyViewShapes_.begin(),
                         window.viewport_->bodyViewShapes_.end(),
                         [id](const BodyViewShape& shape) {
                           return shape.bodyId == id;
                         });
    };
    if (sharedOperand != kInvalidBodyId && presented(sharedOperand))
      return false;
    if (exclusiveOperand != kInvalidBodyId && !presented(exclusiveOperand))
      return false;
    window.cancelJoinBodiesTool();
    return window.modelHistory_.undoCount() == undoBefore &&
           window.activePartDocument().findFeature(joinId) != nullptr &&
           transientToolsInactive(window);
  }
  static bool editUndoSelectionRestored(const MainWindow& window) {
    if (!expectedEditUndoSelection_) return false;
    const auto current = window.captureHistorySelection();
    const auto& expected = *expectedEditUndoSelection_;
    return current.bodies == expected.bodies &&
           current.edges == expected.edges && current.faces == expected.faces &&
           current.historyPosition == expected.historyPosition &&
           current.atEnd == expected.atEnd &&
           current.historyBodyId == expected.historyBodyId &&
           current.historyFeatureId == expected.historyFeatureId &&
           current.historySketchId == expected.historySketchId;
  }
  static std::optional<MainWindow::HistorySelectionState>
  expectedEditUndoSelection() {
    return expectedEditUndoSelection_;
  }
  static bool removeFeatureThroughProduction(MainWindow& window,
                                             FeatureId featureId) {
    const auto found = std::find_if(
        window.historySteps_.begin(), window.historySteps_.end(),
        [featureId](const HistoryStep& step) {
          return step.featureId == featureId;
        });
    if (found == window.historySteps_.end()) return false;
    const HistoryStep step = *found;
    const auto oldUndoCount = window.modelHistory_.undoCount();
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->done(QMessageBox::Yes);
    });
    window.removeHistoryStep(step);
    return window.modelHistory_.undoCount() == oldUndoCount + 1 &&
           !window.activePartDocument().findFeature(featureId);
  }
  static const ShapeFeature* feature(const MainWindow& window, FeatureId id) {
    return window.activePartDocument().findFeature(id);
  }
  static std::vector<FeatureId> featuresOfKind(const MainWindow& window,
                                               FeatureKind kind) {
    std::vector<FeatureId> result;
    for (const auto& body : window.activePartDocument().bodies())
      for (const auto& feature : body.features())
        if (feature && feature->kind() == kind) result.push_back(feature->id());
    std::sort(result.begin(), result.end());
    return result;
  }
  static bool selectedBody(const MainWindow& window, BodyId id) {
    return window.viewport_->selectedBodies() == std::vector<BodyId>{id};
  }
  static bool bodyVisible(const MainWindow& window, BodyId id) {
    const auto* body = window.activePartDocument().findBody(id);
    return body && body->visible();
  }
  static bool transientToolsInactive(const MainWindow& window) {
    return window.partDesignCoordinator_.activeTool() == PartDesignToolKind::None &&
           !window.viewport_->toolPreviewShape_ &&
           !window.viewport_->toolCutPreviewShape_ &&
           window.viewport_->toolPreviewReplacedBodyIds_.empty() &&
           !window.viewport_->extrusionManipulatorVisible_ &&
           !window.viewport_->toolManipulator_ &&
           !window.viewport_->translationToolManipulator_ &&
           !window.viewport_->angularToolManipulator_ &&
           window.viewport_->pickMode_ == Viewport::PickMode::None &&
           window.viewport_->selectionFilter() == SelectionFilter::Any &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, window.activePartDocument()).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, window.activePartDocument()).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle == ToolLifecycle::Inactive &&
           window.previewUpdates_ && !window.previewUpdates_->hasPending() &&
           (!window.toolParametersDock_ || window.toolParametersDock_->isHidden()) &&
           (!window.revolveDock_ || window.revolveDock_->isHidden()) &&
           (!window.mirrorDock_ || window.mirrorDock_->isHidden()) &&
           (!window.linearPatternDock_ || window.linearPatternDock_->isHidden()) &&
           (!window.circularPatternDock_ || window.circularPatternDock_->isHidden());
  }
  static bool rejectedBeginLeavesNoToolUi(MainWindow& window,
                                           PartDesignToolKind kind) {
    window.resetTransientModelingUi();
    const auto preview = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(4.0, 4.0, 4.0).Shape());
    window.viewport_->setToolPreviewShape(1, 2, preview);
    window.viewport_->setEdgeMultiSelectionMode(true);
    window.viewport_->setFaceMultiSelectionMode(true);
    window.viewport_->setSelectionFilter(SelectionFilter::Edge);
    window.toolParametersDock_->show();

    PartDesignTransitionOutcome outcome;
    switch (kind) {
      case PartDesignToolKind::Fillet:
      case PartDesignToolKind::Chamfer:
      case PartDesignToolKind::Draft:
        outcome.effect = PartDesignTransitionEffect::Rejected;
        outcome.previousTool = window.partDesignCoordinator_.activeTool();
        outcome.activeTool = window.partDesignCoordinator_.activeTool();
        outcome.revision = window.partDesignCoordinator_.revisionToken();
        outcome.failure = {OperationFailureCode::InvalidInput,
                           "Rejected test begin"};
        break;
      case PartDesignToolKind::Shell:
        outcome = window.partDesignCoordinator_.beginShell(1, 2, {});
        break;
      default:
        return false;
    }
    const bool activated = window.applyPartDesignBeginResult(outcome, kind);
    return !activated && outcome.effect == PartDesignTransitionEffect::Rejected &&
           !window.viewport_->edgeMultiSelectionMode() &&
           !window.viewport_->faceMultiSelectionMode() &&
           window.viewport_->selectedBodies().empty() &&
           window.viewport_->selectedBodyEdges().empty() &&
           window.viewport_->selectedBodyFaces().empty() &&
           transientToolsInactive(window);
  }
  static BodyId featureOwner(const MainWindow& window, FeatureId id) {
    const Body* body = window.activePartDocument().findBodyForFeature(id);
    return body ? body->id() : kInvalidBodyId;
  }
  static MainWindow::HistorySelectionState selectionState(
      const MainWindow& window) {
    return window.captureHistorySelection();
  }
  static bool exactSelectionState(
      const MainWindow::HistorySelectionState& left,
      const MainWindow::HistorySelectionState& right) {
    const bool same = left.bodies == right.bodies && left.edges == right.edges &&
           left.faces == right.faces &&
           left.historyPosition == right.historyPosition &&
           left.atEnd == right.atEnd &&
           left.historicalLegacyExtrusionSourceSketchId ==
               right.historicalLegacyExtrusionSourceSketchId &&
           left.presentation == right.presentation &&
           left.historyBodyId == right.historyBodyId &&
           left.historyFeatureId == right.historyFeatureId &&
           left.historySketchId == right.historySketchId;
    if (!same)
      std::cerr << "exact selection mismatch: b/e/f " << left.bodies.size()
                << '/' << right.bodies.size() << ' ' << left.edges.size()
                << '/' << right.edges.size() << ' ' << left.faces.size()
                << '/' << right.faces.size() << " pos "
                << left.historyPosition << '/' << right.historyPosition
                << " end " << left.atEnd << '/' << right.atEnd
                << " keys " << left.historyBodyId << ','
                << left.historyFeatureId << ',' << left.historySketchId
                << '/' << right.historyBodyId << ','
                << right.historyFeatureId << ',' << right.historySketchId
                << " legacy "
                << left.historicalLegacyExtrusionSourceSketchId.value_or(0)
                << '/'
                << right.historicalLegacyExtrusionSourceSketchId.value_or(0)
                << '\n';
    return same;
  }
  struct RelocationStateSnapshot {
    QString path;
    std::optional<ProductDocumentIdentity> identity;
    project::ProjectPersistenceState persistence;
    DefinitionId rootDefinitionId{};
    std::size_t definitionCount{};
    const Document* document{};
    const Body* body{};
    const Feature* feature{};
    const TopoDS_Shape* shape{};
    const TopologyIndex* topology{};
    MainWindow::HistorySelectionState selection;
    std::size_t undoCount{};
    std::size_t redoCount{};
    std::uint64_t historyRevision{};
    std::uint64_t previewGeneration{};
    PartDesignRevisionToken partDesignRevision;
    SelectionFilter selectionFilter{SelectionFilter::Any};
    bool faceMultiSelectionMode{};
    bool edgeMultiSelectionMode{};
    int pickMode{};
    bool atSavepoint{};
    bool modified{};
    bool relocationActionEnabled{};
  };
  static RelocationStateSnapshot relocationState(const MainWindow& window) {
    return {window.windowFilePath(),
            window.productDocument_.identity(),
            window.projectPersistenceState_,
            window.productDocument_.rootDefinitionId(),
            window.productDocument_.definitions().size(),
            &window.activePartDocument(),
            firstBodyAddress(window),
            firstFeatureAddress(window),
            firstShapeAddress(window),
            firstTopologyAddress(window),
            window.captureHistorySelection(),
            window.modelHistory_.undoCount(),
            window.modelHistory_.redoCount(),
            window.modelHistory_.currentRevision(),
            window.previewDocumentGeneration_,
            window.partDesignCoordinator_.revisionToken(),
            window.viewport_->selectionFilter(),
            window.viewport_->faceMultiSelectionMode(),
            window.viewport_->edgeMultiSelectionMode(),
            static_cast<int>(window.viewport_->pickMode_),
            window.modelHistory_.isAtSavepoint(),
            window.isWindowModified(),
            window.relocateProductDocumentAction_ &&
                window.relocateProductDocumentAction_->isEnabled()};
  }
  static bool exactRelocationState(
      const MainWindow& window, const RelocationStateSnapshot& expected) {
    const auto actual = relocationState(window);
    return actual.path == expected.path &&
           actual.identity == expected.identity &&
           actual.persistence == expected.persistence &&
           actual.rootDefinitionId == expected.rootDefinitionId &&
           actual.definitionCount == expected.definitionCount &&
           actual.document == expected.document && actual.body == expected.body &&
           actual.feature == expected.feature && actual.shape == expected.shape &&
           actual.topology == expected.topology &&
           exactSelectionState(actual.selection, expected.selection) &&
           actual.undoCount == expected.undoCount &&
           actual.redoCount == expected.redoCount &&
           actual.historyRevision == expected.historyRevision &&
           actual.previewGeneration == expected.previewGeneration &&
           actual.partDesignRevision == expected.partDesignRevision &&
           actual.selectionFilter == expected.selectionFilter &&
           actual.faceMultiSelectionMode == expected.faceMultiSelectionMode &&
           actual.edgeMultiSelectionMode == expected.edgeMultiSelectionMode &&
           actual.pickMode == expected.pickMode &&
           actual.atSavepoint == expected.atSavepoint &&
           actual.modified == expected.modified &&
           actual.relocationActionEnabled ==
               expected.relocationActionEnabled;
  }
  static QStringList treeSnapshot(const MainWindow& window) {
    QStringList result;
    const auto append = [&result](const auto& self,
                                  const QTreeWidgetItem* item,
                                  int depth) -> void {
      if (!item) return;
      result.push_back(
          QStringLiteral("%1|%2|%3|%4")
              .arg(depth)
              .arg(item->text(0))
              .arg(item->data(0, Qt::UserRole).toString())
              .arg(static_cast<int>(item->checkState(0))));
      for (int index = 0; index < item->childCount(); ++index)
        self(self, item->child(index), depth + 1);
    };
    for (int index = 0; index < window.featureTree_->topLevelItemCount();
         ++index)
      append(append, window.featureTree_->topLevelItem(index), 0);
    return result;
  }
  static const QTreeWidgetItem* currentTreeItem(const MainWindow& window) {
    return window.featureTree_->currentItem();
  }
  static bool selectFirstTreeItem(MainWindow& window) {
    auto* item = window.featureTree_->topLevelItemCount() > 0
                     ? window.featureTree_->topLevelItem(0)
                     : nullptr;
    if (!item) return false;
    window.featureTree_->setCurrentItem(item);
    return window.featureTree_->currentItem() == item;
  }
  static const QWidget* currentWorkspaceWidget(const MainWindow& window) {
    return window.workspaceStack_->currentWidget();
  }
  static QStringList checkedRibbonButtons(const MainWindow& window) {
    QStringList result;
    const auto buttons = window.modelRibbon_->findChildren<QToolButton*>();
    for (const auto* button : buttons) {
      if (button && button->isCheckable() && button->isChecked())
        result.push_back(button->objectName() + QLatin1Char('|') +
                         button->text());
    }
    result.sort();
    return result;
  }
  static bool setRibbonToolChecked(MainWindow& window,
                                   const QString& objectName) {
    auto* button = window.modelRibbon_->findChild<QToolButton*>(objectName);
    if (!button || !button->isCheckable()) return false;
    button->setChecked(true);
    return button->isChecked();
  }
  static void editHistoryFeature(MainWindow& window, BodyId bodyId,
                                 FeatureId featureId) {
    window.editHistoryFeature(bodyId, featureId);
  }
  static bool editTransactionActive(const MainWindow& window) {
    return window.editUiTransaction_.has_value();
  }
  static ToolLifecycle faceExtrudeLifecycle(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle;
  }
  static std::optional<FeatureId> faceExtrudeEditingFeature(
      const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).editingFeatureId;
  }
  static ToolLifecycle moveLifecycle(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle;
  }
  static std::optional<FeatureId> moveEditingFeature(
      const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).editingFeatureId;
  }
  static void cancelFaceExtrude(MainWindow& window) {
    window.cancelFaceExtrudeTool();
  }
  static void cancelMove(MainWindow& window) { window.cancelMoveTool(); }
  static int historyPosition(const MainWindow& window) {
    return window.historyPosition_;
  }
  static bool sameSelectionState(
      const MainWindow::HistorySelectionState& left,
      const MainWindow::HistorySelectionState& right) {
    const bool sameHistoryLocation = left.atEnd == right.atEnd &&
        (left.atEnd ||
         (left.historyBodyId == right.historyBodyId &&
          left.historyFeatureId == right.historyFeatureId &&
          left.historySketchId == right.historySketchId));
    const bool same = left.bodies == right.bodies && left.edges == right.edges &&
           left.faces == right.faces &&
           sameHistoryLocation;
    if (!same) {
      std::cerr << "selection mismatch: bodies " << left.bodies.size() << '/'
                << right.bodies.size() << " edges " << left.edges.size()
                << '/' << right.edges.size() << " faces " << left.faces.size()
                << '/' << right.faces.size() << " atEnd " << left.atEnd << '/'
                << right.atEnd << " pos " << left.historyPosition << '/'
                << right.historyPosition << " keys " << left.historyBodyId
                << ',' << left.historyFeatureId << ',' << left.historySketchId
                << '/' << right.historyBodyId << ',' << right.historyFeatureId
                << ',' << right.historySketchId << '\n';
    }
    return same;
  }
  static bool finishSketchEdit(MainWindow& window, SketchId sketchId,
                               const sketch::Sketch& geometry,
                               SketchPlacement placement) {
    const auto before = window.modelHistory_.undoCount();
    window.editSketchById(sketchId);
    if (!window.editingSketchIndex_) return false;
    window.sketchCanvas_->loadSketch(geometry);
    window.currentSketchPlacement_ = placement;
    window.finishSketch();
    return window.workspaceStack_->currentWidget() == window.viewport_ &&
           window.modelHistory_.undoCount() == before + 1;
  }
  static bool finishSketchEditNoCommand(MainWindow& window,
                                        SketchId sketchId,
                                        const sketch::Sketch& geometry,
                                        SketchPlacement placement) {
    const auto beforeUndo = window.modelHistory_.undoCount();
    const auto beforeRedo = window.modelHistory_.redoCount();
    window.editSketchById(sketchId);
    if (!window.editingSketchIndex_) return false;
    window.sketchCanvas_->loadSketch(geometry);
    window.currentSketchPlacement_ = placement;
    window.finishSketch();
    return window.workspaceStack_->currentWidget() == window.viewport_ &&
           window.modelHistory_.undoCount() == beforeUndo &&
           window.modelHistory_.redoCount() == beforeRedo;
  }
  static std::size_t redoCount(const MainWindow& window) {
    return window.modelHistory_.redoCount();
  }
  static QString statusMessage(const MainWindow& window) {
    return window.statusBar()->currentMessage();
  }
  static bool hasCommittedModernSolid(const MainWindow& window) {
    return window.hasCommittedModernSolid();
  }
  static bool hasDisplayableModernSolid(const MainWindow& window) {
    return window.hasDisplayableModernSolid();
  }
  static bool foreignShapeIsDisplayable(const MainWindow& window) {
    if (window.activePartDocument().bodies().empty()) return false;
    auto foreign = std::make_shared<const TopoDS_Shape>(
        BRepPrimAPI_MakeBox(3.0, 4.0, 5.0).Shape());
    return MainWindow::resolveModernSolidPresentation(
               window.activePartDocument().bodies().front(), foreign, true)
        .has_value();
  }
  static bool viewportHasBodyPresentations(const MainWindow& window) {
    return !window.viewport_->bodyViewShapes_.empty();
  }
  static bool hasExportableModernSolid(const MainWindow& window,
                                       QString* error = nullptr) {
    return window.hasExportableModernSolid(error);
  }
  static std::optional<SketchId> historicalLegacySourceSketchId(
      const MainWindow& window) {
    return window.historicalLegacyExtrusionSourceSketchId_;
  }
  static bool sketchConsumedByModernFeature(const MainWindow& window,
                                            SketchId sketchId) {
    return isSketchConsumedByPartDesign(window.activePartDocument(), sketchId);
  }
  static QAction* undoAction(MainWindow& window) { return window.undoAction_; }
  static QAction* redoAction(MainWindow& window) { return window.redoAction_; }
  static QStackedWidget* workspace(MainWindow& window) {
    return window.workspaceStack_;
  }
  static SketchCanvas* sketch(MainWindow& window) { return window.sketchCanvas_; }
  static Viewport* viewport(MainWindow& window) { return window.viewport_; }
  static ViewportCameraState assemblyCameraState(const MainWindow& window) {
    return window.viewport_->assemblyCamera();
  }
  static std::optional<AssemblyOccurrenceIdentity> selectedHighlightIdentity(
      const MainWindow& window) {
    const auto* session = window.viewport_->assemblySession();
    return session && session->selectedHighlight()
               ? std::optional<AssemblyOccurrenceIdentity>(
                     session->selectedHighlight()->occurrence())
               : std::nullopt;
  }
  static std::optional<AssemblyOccurrenceIdentity> hoveredHighlightIdentity(
      const MainWindow& window) {
    const auto* session = window.viewport_->assemblySession();
    return session && session->hoveredHighlight()
               ? std::optional<AssemblyOccurrenceIdentity>(
                     session->hoveredHighlight()->occurrence())
               : std::nullopt;
  }
  static void finishViewAnimation(Viewport& viewport) {
    if (auto* animation = viewport.findChild<QVariantAnimation*>(
            QStringLiteral("viewOrientationTransition"));
        animation && animation->state() == QAbstractAnimation::Running)
      animation->setCurrentTime(animation->duration());
    QApplication::processEvents();
  }
  static bool openSketchOnBodyFaceAt(MainWindow& window, Point3d world) {
    auto* viewport = window.viewport_;
    viewport->resize(800, 600);
    const ViewportCameraState camera{
        viewport->yaw_, viewport->pitch_, viewport->zoom_,
        viewport->cameraPan_, viewport->size(), 1.0F,
        viewport->displayedBodyCenter_,
        std::max(1.0, viewport->displayedBodyDiagonal_ * 3.0)};
    const QPointF position = camera.worldToScreen(world);
    viewport->beginSketchPlaneSelection();
    QMouseEvent move(QEvent::MouseMove, position, position, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    viewport->flushPendingHover(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, position,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    const auto* animation = viewport->findChild<QVariantAnimation*>(
        QStringLiteral("viewOrientationTransition"));
    if (window.workspaceStack_->currentWidget() != viewport || !animation ||
        animation->state() != QAbstractAnimation::Running)
      return false;
    finishViewAnimation(*viewport);
    return window.workspaceStack_->currentWidget() == window.sketchCanvas_;
  }
  static QPointF viewportWorldScreen(const MainWindow& window,
                                     Point3d world) {
    const auto* viewport = window.viewport_;
    const ViewportCameraState camera{
        viewport->yaw_, viewport->pitch_, viewport->zoom_,
        viewport->cameraPan_, viewport->size(), 1.0F,
        viewport->displayedBodyCenter_,
        std::max(1.0, viewport->displayedBodyDiagonal_ * 3.0)};
    return camera.worldToScreen(world);
  }
  static double viewportPixelsPerMm(const MainWindow& window) {
    return std::max(1, std::min(window.viewport_->width(),
                                window.viewport_->height())) *
           0.008 * window.viewport_->zoom_;
  }
  static float viewportZoom(const MainWindow& window) {
    return window.viewport_->zoom_;
  }
  static SketchPlacement currentSketchPlacement(const MainWindow& window) {
    return window.currentSketchPlacement_;
  }
  static bool selectedExtrusionIsLegacyFace(const MainWindow& window) {
    return window.selectedExtrusionSource_ &&
           std::holds_alternative<LegacySolidFacePick>(
               window.selectedExtrusionSource_->source);
  }
  static bool historicalLegacyExtrusionActive(const MainWindow& window) {
    return window.hasHistoricalLegacyExtrusion();
  }
  static bool legacySolidSketchEquals(const MainWindow& window,
                                      const sketch::Sketch& expected) {
    return window.viewport_->solidSketch_.semanticFingerprint() ==
               expected.semanticFingerprint() &&
           window.viewport_->solidSketch_.semanticallyEqual(expected);
  }
  static SketchId firstSketchId(const MainWindow& window) {
    return window.activePartDocument().sketches().empty()
               ? kInvalidSketchId
               : window.activePartDocument().sketches().front().id;
  }
  static bool commitLegacyExtrusionAsNewBody(MainWindow& window) {
    window.extrusionOperationCombo_->setCurrentIndex(
        static_cast<int>(ExtrudeOperation::NewBody));
    window.extrusionLengthSpin_->setValue(10.0);
    window.extrudeSketch();
    return !window.activePartDocument().bodies().empty();
  }
  static void removeBody(MainWindow& window, BodyId id) {
    window.removeBody(id);
  }
  static void removeHistoryFeature(MainWindow& window, BodyId bodyId,
                                   FeatureId featureId) {
    HistoryStep step;
    step.bodyId = bodyId;
    step.featureId = featureId;
    step.title = QStringLiteral("Stage 2 removal");
    window.removeHistoryStep(step);
  }
  static void removeHistorySketch(MainWindow& window, SketchId sketchId) {
    HistoryStep step;
    step.sketchId = sketchId;
    step.title = QStringLiteral("Historical source sketch");
    window.removeHistoryStep(step);
  }
  static std::size_t bodyCount(const MainWindow& window) {
    return window.activePartDocument().bodies().size();
  }
  static BodyId firstBodyId(const MainWindow& window) {
    return window.activePartDocument().bodies().empty()
        ? kInvalidBodyId : window.activePartDocument().bodies().front().id();
  }
  static std::size_t bodyFeatureCount(const MainWindow& window,
                                      BodyId bodyId) {
    const Body* body = window.activePartDocument().findBody(bodyId);
    return body ? body->features().size() : 0;
  }
  static bool hasSketch(const MainWindow& window, SketchId sketchId) {
    return window.activePartDocument().findSketch(sketchId) != nullptr;
  }
  static std::size_t sketchViewCount(const MainWindow& window) {
    return window.sketchViews_.size();
  }
  static std::size_t documentSketchCount(const MainWindow& window) {
    return window.activePartDocument().sketches().size();
  }
  static bool displayedSketchesMatchDocument(const MainWindow& window) {
    if (window.viewport_->displaySketches_.size() !=
        window.sketchViews_.size())
      return false;
    const auto samePoint = [](const auto& left, const auto& right) {
      return left.x == right.x && left.y == right.y && left.z == right.z;
    };
    for (std::size_t index = 0; index < window.sketchViews_.size(); ++index) {
      const auto& view = window.sketchViews_[index];
      const auto* model = window.activePartDocument().findSketch(view.documentSketchId);
      if (!model) return false;
      const auto& displayed = window.viewport_->displaySketches_[index];
      if (displayed.sketchId != model->id ||
          displayed.geometry.semanticFingerprint() !=
              model->geometry.semanticFingerprint() ||
          !displayed.geometry.semanticallyEqual(model->geometry) ||
          !samePoint(displayed.placement.origin, model->placement.origin) ||
          !samePoint(displayed.placement.xDirection,
                     model->placement.xDirection) ||
          !samePoint(displayed.placement.yDirection,
                     model->placement.yDirection))
        return false;
    }
    return true;
  }
  static void injectMissingSketchViewAndRefresh(MainWindow& window) {
    if (window.sketchViews_.empty()) return;
    const auto* model = window.activePartDocument().findSketch(
        window.sketchViews_.front().documentSketchId);
    if (!model) return;
    window.sketchCanvas_->loadSketch(model->geometry);
    window.sketchViews_.front().documentSketchId =
        std::numeric_limits<SketchId>::max();
    window.refreshBodyViewFromDocument();
    window.editingSketchIndex_ = 0;
    window.workspaceStack_->setCurrentWidget(window.sketchCanvas_);
    window.finishSketch();
  }
  static bool sketchEditActive(const MainWindow& window) {
    return window.editingSketchIndex_.has_value();
  }
  static std::size_t displayedSketchCount(const MainWindow& window) {
    return window.viewport_->displaySketches_.size();
  }
  static QString displayedSketchLabel(const MainWindow& window,
                                      std::size_t index) {
    return index < window.viewport_->displaySketches_.size()
               ? window.viewport_->displaySketches_[index].supportName
               : QString{};
  }
  static bool noSketchPickState(const MainWindow& window) {
    const auto invalid = static_cast<std::size_t>(-1);
    return window.viewport_->selectedExtrusionSketchIndex_ == invalid &&
           window.viewport_->hoveredExtrusionSketchIndex_ == invalid &&
           window.viewport_->revolveAxisSketchIndex_ == invalid &&
           window.viewport_->selectedExtrusionSketch_.lines().empty() &&
           window.viewport_->selectedExtrusionSketch_.circles().empty() &&
           window.viewport_->selectedExtrusionSketch_.arcs().empty() &&
           window.viewport_->hoveredExtrusionSketch_.lines().empty() &&
           window.viewport_->hoveredExtrusionSketch_.circles().empty() &&
           window.viewport_->hoveredExtrusionSketch_.arcs().empty() &&
           window.viewport_->selectedExtrusionRegionSketches_.empty();
  }
  static void setRevolveAxisSketchIndex(MainWindow& window,
                                        std::size_t index) {
    window.viewport_->revolveAxisSketchIndex_ = index;
  }
  static std::size_t revolveAxisSketchIndex(const MainWindow& window) {
    return window.viewport_->revolveAxisSketchIndex_;
  }
  static QPointF extrusionDirectionForLabel(
      MainWindow& window, std::size_t index, const QString& label,
      const SketchPlacement& placement) {
    if (index >= window.viewport_->displaySketches_.size()) return {};
    auto& displayed = window.viewport_->displaySketches_[index];
    displayed.supportName = label;
    displayed.placement = placement;
    window.viewport_->selectedExtrusionSketchIndex_ = index;
    window.viewport_->selectedExtrusionSupport_ = label;
    window.viewport_->selectedExtrusionPlacement_ = placement;
    window.viewport_->selectedLegacySolidFace_.reset();
    window.viewport_->selectedExtrusionReverse_ = false;
    return window.viewport_->extrusionScreenOffset(25.0);
  }
  static void finishNewSketch(MainWindow& window,
                              const sketch::Sketch& geometry,
                              const SketchPlacement& placement,
                              const QString& presentationLabel) {
    window.editingSketchIndex_.reset();
    window.currentSketchPlacement_ = placement;
    window.currentSketchFaceReference_.reset();
    window.currentSketchSupport_ = presentationLabel;
    window.sketchCanvas_->loadSketch(geometry);
    window.workspaceStack_->setCurrentWidget(window.sketchCanvas_);
    window.finishSketch();
  }
  static CameraOrientation finishNewSketchFromOrbit(
      MainWindow& window, const sketch::Sketch& geometry,
      const SketchPlacement& placement, double yawDeg, double pitchDeg,
      const QString& presentationLabel) {
    window.editingSketchIndex_.reset();
    window.currentSketchPlacement_ = placement;
    window.currentSketchFaceReference_.reset();
    window.currentSketchSupport_ = presentationLabel;
    window.sketchCanvas_->setSceneReferences(placement, {}, {});
    window.sketchCanvas_->loadSketch(geometry);
    window.sketchCanvas_->setViewOrientation(yawDeg, pitchDeg);
    window.workspaceStack_->setCurrentWidget(window.sketchCanvas_);
    const auto camera = window.sketchCanvas_->viewCubeCamera();
    window.finishSketch();
    return camera;
  }
  static std::optional<FaceReference> currentSketchFaceReference(
      const MainWindow& window) {
    return window.currentSketchFaceReference_;
  }
  static void reverseSketchPresentationOrder(MainWindow& window) {
    std::reverse(window.sketchViews_.begin(), window.sketchViews_.end());
    window.syncSketchPresentationFromDocument();
    window.refreshBodyViewFromDocument();
  }
  static bool modified(const MainWindow& window) {
    return window.isWindowModified();
  }
  static void save(MainWindow& window) { window.saveProject(); }
  static void queueInvalidSourcePreview(MainWindow& window, int* rebuilds,
                                        int* publications) {
    window.schedulePreviewUpdate(
        kInvalidBodyId, kInvalidFeatureId,
        [rebuilds] { ++*rebuilds; },
        [publications] { ++*publications; });
  }
  static void dispatchPreviewCadence(MainWindow& window) {
    window.previewUpdates_->dispatchCadenceForTests();
  }
  static bool previewHasPending(const MainWindow& window) {
    return window.previewUpdates_ && window.previewUpdates_->hasPending();
  }
  static PreviewUpdateCoordinator::Counters previewCounters(
      const MainWindow& window) {
    return window.previewUpdates_ ? window.previewUpdates_->counters()
                                  : PreviewUpdateCoordinator::Counters{};
  }
  static std::uint64_t automaticExtrudeDetectionCount(
      const MainWindow& window) {
    return window.automaticExtrudeDetectionCount_;
  }
  static void scheduleAutomaticExtrudeDetection(MainWindow& window) {
    window.extrudeOperationManuallyChanged_ = false;
    window.scheduleAutomaticExtrudeOperation();
  }
  static void cancelLegacyExtrusion(MainWindow& window) {
    window.cancelLegacyExtrusion();
  }
  static bool legacyExtrusionDockVisible(const MainWindow& window) {
    // The parent MainWindow is deliberately never shown in this offscreen
    // test; isVisible() would therefore be false even when the dock itself is
    // in its shown state.
    return window.extrusionDock_ && !window.extrusionDock_->isHidden();
  }
  static void exposeLegacyExtrusion(MainWindow& window) {
    window.extrusionDock_->show();
    window.selectedExtrusionSource_ = ExtrusionSourcePick{
        LegacySolidFacePick{LegacySolidFace::EndCap, SketchPlacement::xy(),
                            {0.0, 0.0, 1.0}, {}},
        QStringLiteral("pending surface")};
    window.viewport_->beginExtrusionSurfaceSelection();
  }
  static bool legacyExtrusionSelectionActive(const MainWindow& window) {
    return window.viewport_->pickMode_ == Viewport::PickMode::ExtrusionSurface;
  }
  static void applyImportedDocument(MainWindow& window, Document staged) {
    window.applyImportedDocument(std::move(staged),
                                 QStringLiteral("pending-preview-test.step"));
  }
  static void startSketchExtrude(MainWindow& window, std::size_t index) {
    if (index >= window.sketchViews_.size()) return;
    const auto& entry = window.sketchViews_[index];
    const auto* sketch = window.activePartDocument().findSketch(entry.documentSketchId);
    if (!sketch) return;
    window.createSketchExtrude(
        ExtrusionSourcePick{SketchRegionPick{sketch->id, sketch->placement,
                                             sketch->geometry},
                            QStringLiteral("test profile")});
  }
  static bool startLegacySketchExtrude(MainWindow& window, std::size_t index) {
    if (index >= window.sketchViews_.size()) return false;
    const auto& entry = window.sketchViews_[index];
    const auto* sketch = window.activePartDocument().findSketch(entry.documentSketchId);
    if (!sketch) return false;
    window.viewport_->selectedExtrusionSketch_ = sketch->geometry;
    window.viewport_->selectedExtrusionPlacement_ = sketch->placement;
    window.viewport_->selectedExtrusionSketchIndex_ = index;
    emit window.viewport_->extrusionSourcePicked(ExtrusionSourcePick{
        SketchRegionPick{sketch->id, sketch->placement, sketch->geometry},
        QStringLiteral("legacy test profile")});
    window.extrusionLengthSpin_->setValue(10.0);
    return window.extrusionDock_ && !window.extrusionDock_->isHidden();
  }
  static bool startTopFaceExtrude(MainWindow& window, double zMm) {
    Body* body = window.mutableStandaloneDocument().activeBody();
    if (!body || !body->activeFeature() || !body->resultShape()) return false;
    const auto faceIndex = test::topPlanarFace(*body->resultShape(), zMm);
    if (!faceIndex) return false;
    window.createFaceExtrude(makeFaceReference(
        *body->resultShape(), body->id(), body->activeFeature()->id(),
        *faceIndex));
    return true;
  }
  static void dragDirectExtrude(MainWindow& window, double length) {
    emit window.viewport_->toolManipulatorValueChanged(length);
    window.previewUpdates_->dispatchCadenceForTests();
  }
  static bool pressDirectExtrudeKey(MainWindow& window, Qt::Key key) {
    window.toolParametersPanel_->focusParameterInput();
    QApplication::processEvents();
    QWidget* focus = QApplication::focusWidget();
    if (!focus) return false;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(focus, &press);
    QApplication::sendEvent(focus, &release);
    QApplication::processEvents();
    return true;
  }
  static void commitDirectExtrudeFromViewport(MainWindow& window) {
    emit window.viewport_->toolParameterCommitted();
    QApplication::processEvents();
  }
  static ToolLifecycle directExtrudeLifecycle(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle;
  }
  static ExtrudeOperation directExtrudeOperation(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation;
  }
  static bool directExtrudeReversed(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed;
  }
  static ShapeFeature::ShapePtr directExtrudePreview(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).previewShape;
  }
  static PartDesignToolKind activeTool(const MainWindow& window) {
    return window.partDesignCoordinator_.activeTool();
  }
  static std::size_t bodyFeatureCount(const MainWindow& window) {
    const Body* body = window.activePartDocument().activeBody();
    return body ? body->features().size() : 0;
  }
  static ShapeFeature::ShapePtr bodyShape(const MainWindow& window) {
    const Body* body = window.activePartDocument().activeBody();
    return body ? body->resultShape() : ShapeFeature::ShapePtr{};
  }
  static ShapeFeature::ShapePtr displayedBodyShape(const MainWindow& window) {
    return window.viewport_->bodyShape_;
  }
  static bool viewportSolidVisible(const MainWindow& window) {
    return window.viewport_->solidVisible_;
  }
  static bool setFirstBodyVisible(MainWindow& window, bool visible) {
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 3) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static bool setOriginVisible(MainWindow& window, bool visible) {
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 1) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static bool setBasePlaneVisible(MainWindow& window, int plane,
                                  bool visible) {
    if (plane < 0 || plane > 2) return false;
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 10 + plane) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static bool setSketchVisible(MainWindow& window, std::size_t index,
                               bool visible) {
    const int kind = 20 + static_cast<int>(index);
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != kind) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static std::optional<bool> treeVisibility(const MainWindow& window,
                                            int kind) {
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items)
      if (item->data(0, Qt::UserRole).toInt() == kind)
        return item->checkState(0) == Qt::Checked;
    return std::nullopt;
  }
  static bool presentationMatchesTree(const MainWindow& window) {
    if (treeVisibility(window, 1) !=
        std::optional<bool>{window.viewport_->originVisible()}) {
      std::cerr << "presentation/tree origin mismatch\n";
      return false;
    }
    for (int plane = 0; plane < 3; ++plane)
      if (treeVisibility(window, 10 + plane) !=
          std::optional<bool>{window.viewport_->basePlaneVisible(plane)}) {
        std::cerr << "presentation/tree plane mismatch " << plane << '\n';
        return false;
      }
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      const int kind = item->data(0, Qt::UserRole).toInt();
      if (kind < 20) continue;
      const auto index = static_cast<std::size_t>(kind - 20);
      if (item->checkState(0) !=
          (window.viewport_->sketchVisible(index) ? Qt::Checked
                                                   : Qt::Unchecked)) {
        std::cerr << "presentation/tree sketch mismatch " << index << '\n';
        return false;
      }
    }
    return true;
  }
  static bool originVisible(const MainWindow& window) {
    return window.viewport_->originVisible();
  }
  static bool basePlaneVisible(const MainWindow& window, int plane) {
    return window.viewport_->basePlaneVisible(plane);
  }
  static bool sketchVisible(const MainWindow& window, std::size_t index) {
    return window.viewport_->sketchVisible(index);
  }
  static void commitBodyPosition(MainWindow& window, QPointF position) {
    const QPointF previous = window.viewport_->bodyPosition();
    window.viewport_->setBodyPosition(position);
    window.viewport_->bodyMoveCommitted(previous, position);
  }
  static QPointF bodyPosition(const MainWindow& window) {
    return window.viewport_->bodyPosition();
  }
  static bool startMoveForActiveBody(MainWindow& window) {
    window.createMove();
    Body* body = window.mutableStandaloneDocument().activeBody();
    if (!body || !body->activeFeature() || !body->resultShape()) return false;
    window.partDesignCoordinator_.setMoveBody(body->id(), body->activeFeature()->id(),
                                    body->resultShape());
    window.updateMoveToolPreview();
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::PreviewValid;
  }
  static bool enterMoveXAndAccept(MainWindow& window, const QString& text) {
    if (!window.moveDock_ || !window.moveXSpin_) return false;
    auto* editor = window.moveXSpin_->findChild<QLineEdit*>();
    if (!editor) return false;
    editor->setText(text);
    for (QShortcut* shortcut : window.moveDock_->findChildren<QShortcut*>()) {
      if (shortcut->key() != QKeySequence(Qt::Key_Return)) continue;
      return QMetaObject::invokeMethod(shortcut, "activated",
                                       Qt::DirectConnection);
    }
    return false;
  }
  static std::optional<Vector3d> lastMoveOffset(const MainWindow& window) {
    const Body* body = window.activePartDocument().activeBody();
    if (!body || body->features().empty()) return std::nullopt;
    const auto* move =
        dynamic_cast<const MoveFeature*>(body->features().back().get());
    return move ? std::optional<Vector3d>(move->offsetMm()) : std::nullopt;
  }
  static bool bodyRowsHaveNoFeatureChildren(const MainWindow& window) {
    bool foundBody = false;
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 3) continue;
      foundBody = true;
      if (item->childCount() != 0) return false;
    }
    return foundBody;
  }
  static bool viewportHasToolPreview(const MainWindow& window) {
    return (window.viewport_->toolPreviewShape_ &&
            !window.viewport_->toolPreviewShape_->IsNull()) ||
           (window.viewport_->toolCutPreviewShape_ &&
            !window.viewport_->toolCutPreviewShape_->IsNull());
  }
  static ShapeFeature::ShapePtr viewportToolPreviewShape(
      const MainWindow& window) {
    return window.viewport_->toolPreviewShape_;
  }
  static std::size_t historyStepCount(const MainWindow& window) {
    return window.historySteps_.size();
  }
  static bool historyContainsFeature(const MainWindow& window,
                                     FeatureId id) {
    return std::any_of(window.historySteps_.begin(), window.historySteps_.end(),
                       [id](const HistoryStep& step) {
                         return step.featureId == id;
                       });
  }
  static void applyHistoryPosition(MainWindow& window, int position) {
    window.applyHistoryPosition(position);
  }
  static void moveHistoryToEnd(MainWindow& window) {
    window.moveHistoryToEnd();
    window.applyHistoryPosition(window.historyPosition_);
  }
  static bool historyAtEnd(const MainWindow& window) {
    return window.isHistoryAtEnd();
  }
  static void editSketch(MainWindow& window, SketchId sketchId) {
    window.editSketchById(sketchId);
  }
  static QAction* upgradeProjectFormatAction(MainWindow& window) {
    return window.upgradeProjectFormatAction_;
  }
  static QAction* saveProjectAsAction(MainWindow& window) {
    return window.saveProjectAsAction_;
  }
  static QAction* convertToProductDocumentAction(MainWindow& window) {
    return window.convertToProductDocumentAction_;
  }
  static QAction* relocateProductDocumentAction(MainWindow& window) {
    return window.relocateProductDocumentAction_;
  }
  static void refreshPersistenceActions(MainWindow& window) {
    window.updateUpgradeProjectFormatAction();
  }
  static void upgradeProjectFormat(MainWindow& window) {
    window.upgradeProjectFormat();
  }
  static bool projectUpgradeIsQuiescent(const MainWindow& window) {
    return window.projectPersistenceTransitionIsQuiescent();
  }
  static bool productConversionIsEligible(const MainWindow& window) {
    return window.standaloneProductConversionIsEligible();
  }
  static bool saveProjectAsIsEligible(const MainWindow& window) {
    return window.saveProjectAsIsEligible();
  }
  static bool productRelocationIsEligible(const MainWindow& window) {
    return window.productRelocationIsEligible();
  }
  static ProjectApplicationStatus saveProductCloneToPath(
      MainWindow& window, const QString& targetPath,
      const ProductCloneRequest& request) {
    return window.saveProductCloneToPath(targetPath, request);
  }
  static ProjectApplicationStatus runSaveProjectAsCommand(
      MainWindow& window, const MainWindow::SaveProjectAsTargetChooser& chooser,
      const MainWindow::ProductCloneRequestFactory& factory,
      bool* attempted = nullptr) {
    return window.runSaveProjectAsCommand(chooser, factory, attempted);
  }
  static ProjectApplicationStatus runAssemblySaveAsCommand(
      MainWindow& window,
      const MainWindow::SaveProjectAsTargetChooser& chooser,
      bool* attempted = nullptr) {
    return window.runAssemblySaveAsCommand(chooser, attempted);
  }
  enum class OpenProjectTestOutcome {
    Opened,
    Cancelled,
    Rejected,
    ReceiverDestroyed,
  };
  enum class ConfirmationTestOutcome {
    Proceed,
    Cancelled,
    ReceiverDestroyed,
  };
  static ConfirmationTestOutcome confirmProjectReplacement(
      MainWindow& window) {
    switch (MainWindow::confirmProjectReplacement(window)) {
      case MainWindow::ProjectReplacementConfirmation::Proceed:
        return ConfirmationTestOutcome::Proceed;
      case MainWindow::ProjectReplacementConfirmation::Cancelled:
        return ConfirmationTestOutcome::Cancelled;
      case MainWindow::ProjectReplacementConfirmation::ReceiverDestroyed:
        return ConfirmationTestOutcome::ReceiverDestroyed;
    }
    return ConfirmationTestOutcome::Cancelled;
  }
  using OpenProjectConfirmation = std::function<bool(MainWindow&)>;
  static OpenProjectTestOutcome runOpenProjectCommand(
      MainWindow& window, const QString& path,
      const OpenProjectConfirmation& confirmation, QString* error = nullptr) {
    const auto outcome = window.runOpenProjectCommand(
        path,
        [&confirmation](MainWindow& owner) {
          return confirmation(owner)
                     ? MainWindow::ProjectReplacementConfirmation::Proceed
                     : MainWindow::ProjectReplacementConfirmation::Cancelled;
        },
        error);
    switch (outcome) {
      case MainWindow::OpenProjectCommandOutcome::Opened:
        return OpenProjectTestOutcome::Opened;
      case MainWindow::OpenProjectCommandOutcome::Cancelled:
        return OpenProjectTestOutcome::Cancelled;
      case MainWindow::OpenProjectCommandOutcome::Rejected:
        return OpenProjectTestOutcome::Rejected;
      case MainWindow::OpenProjectCommandOutcome::ReceiverDestroyed:
        return OpenProjectTestOutcome::ReceiverDestroyed;
    }
    return OpenProjectTestOutcome::Rejected;
  }
  static ProjectApplicationStatus convertProjectToPath(
      MainWindow& window, const QString& targetPath,
      const StandaloneProductConversionRequest& request) {
    return window.convertProjectToPath(targetPath, request);
  }
  static ProjectApplicationStatus runProductConversionCommand(
      MainWindow& window,
      const MainWindow::ProductConversionTargetChooser& chooser,
      const MainWindow::ProductConversionRequestFactory& factory,
      bool* attempted = nullptr) {
    return window.runProductConversionCommand(chooser, factory, attempted);
  }
  static ProjectApplicationStatus runProductRelocationCommand(
      MainWindow& window,
      const MainWindow::ProductRelocationTargetChooser& chooser,
      const MainWindow::ProductRelocationExecutor& relocate,
      bool* attempted = nullptr) {
    return window.runProductRelocationCommand(chooser, relocate, attempted);
  }
  static std::uint64_t previewDocumentGeneration(const MainWindow& window) {
    return window.previewDocumentGeneration_;
  }
  static std::uint64_t currentHistoryRevision(const MainWindow& window) {
    return window.modelHistory_.currentRevision();
  }
  static bool historyAtSavepoint(const MainWindow& window) {
    return window.modelHistory_.isAtSavepoint();
  }
  static const Body* firstBodyAddress(const MainWindow& window) {
    return window.activePartDocument().bodies().empty()
               ? nullptr
               : &window.activePartDocument().bodies().front();
  }
  static const Feature* firstFeatureAddress(const MainWindow& window) {
    const Body* body = firstBodyAddress(window);
    return !body || body->features().empty()
               ? nullptr
               : body->features().front().get();
  }
  static const TopoDS_Shape* firstShapeAddress(const MainWindow& window) {
    const auto* feature =
        dynamic_cast<const ShapeFeature*>(firstFeatureAddress(window));
    return feature ? feature->lastValidShape().get() : nullptr;
  }
  static ShapeRevision firstShapeRevision(const MainWindow& window) {
    const auto* feature =
        dynamic_cast<const ShapeFeature*>(firstFeatureAddress(window));
    return feature ? feature->shapeRevision() : kInvalidShapeRevision;
  }
  static const TopologyIndex* firstTopologyAddress(const MainWindow& window) {
    const auto* feature =
        dynamic_cast<const ShapeFeature*>(firstFeatureAddress(window));
    return feature ? feature->lastValidTopologyIndex().get() : nullptr;
  }
  static bool insertAdditionalDefinition(MainWindow& window,
                                         DefinitionId id) {
    const auto result = window.productDocument_.insert(
        PartDefinition(id, std::make_unique<Document>()));
    window.updateUpgradeProjectFormatAction();
    return result == DefinitionInsertStatus::Inserted;
  }
  static bool startV2FaceExtrude(MainWindow& window) {
    Body* body = window.mutableStandaloneDocument().activeBody();
    auto* source = body ? dynamic_cast<ShapeFeature*>(body->activeFeature())
                        : nullptr;
    const auto snapshot =
        body && source ? body->topologySnapshot(source->id()) : std::nullopt;
    if (!snapshot || !snapshot->topologyIndex()) return false;
    std::optional<FaceReference> reference;
    for (std::size_t raw = 0;
         raw < snapshot->topologyIndex()->rawFaceCount(); ++raw) {
      const auto created = snapshot->topologyIndex()->createFaceReferenceV2(
          body->id(), source->id(), raw);
      if (created) {
        reference = created.reference;
        break;
      }
    }
    if (!reference) return false;
    window.partDesignCoordinator_.beginExtrude(
        body->id(), source->id(), source->shape(), *reference, 2.0,
        ExtrudeOperation::Join, false, std::nullopt,
        snapshot->topologyIndex());
    window.updateFaceExtrudeToolPreview();
    return window.partDesignCoordinator_
               .snapshot(PartDesignToolKind::Extrude, window.activePartDocument())
               .lifecycle == ToolLifecycle::PreviewValid;
  }
  static bool startV2Fillet(MainWindow& window) {
    Body* body = window.mutableStandaloneDocument().activeBody();
    auto* source = body ? dynamic_cast<ShapeFeature*>(body->activeFeature())
                        : nullptr;
    const auto snapshot =
        body && source ? body->topologySnapshot(source->id()) : std::nullopt;
    if (!snapshot || !snapshot->topologyIndex()) return false;
    std::optional<EdgeReference> reference;
    for (std::size_t raw = 0;
         raw < snapshot->topologyIndex()->rawEdgeCount(); ++raw) {
      const auto created = snapshot->topologyIndex()->createEdgeReferenceV2(
          body->id(), source->id(), raw);
      if (created) {
        reference = created.reference;
        break;
      }
    }
    if (!reference) return false;
    window.partDesignCoordinator_.beginFillet(*snapshot, {*reference}, 1.0);
    window.updateFilletToolPreview();
    return window.partDesignCoordinator_
               .snapshot(PartDesignToolKind::Fillet, window.activePartDocument())
               .lifecycle == ToolLifecycle::PreviewValid;
  }
  static void acceptActiveFaceExtrude(MainWindow& window) {
    window.acceptFaceExtrudeTool();
  }
  static void acceptActiveFillet(MainWindow& window) {
    window.acceptFilletTool();
  }
  static bool finishV2FaceSketch(MainWindow& window) {
    Body* body = window.mutableStandaloneDocument().activeBody();
    auto* source = body ? dynamic_cast<ShapeFeature*>(body->activeFeature())
                        : nullptr;
    const auto snapshot =
        body && source ? body->topologySnapshot(source->id()) : std::nullopt;
    if (!snapshot || !snapshot->topologyIndex()) return false;
    for (std::size_t raw = 0;
         raw < snapshot->topologyIndex()->rawFaceCount(); ++raw) {
      const auto created = snapshot->topologyIndex()->createFaceReferenceV2(
          body->id(), source->id(), raw);
      if (!created) continue;
      sketch::Sketch geometry;
      geometry.addRectangle({0.0, 0.0}, {2.0, 2.0});
      window.editingSketchIndex_.reset();
      window.currentSketchFaceReference_ = created.reference;
      window.sketchCanvas_->loadSketch(geometry);
      window.workspaceStack_->setCurrentWidget(window.sketchCanvas_);
      window.finishSketch();
      return true;
    }
    return false;
  }
  static bool sketchWorkspaceActive(const MainWindow& window) {
    return window.workspaceStack_->currentWidget() == window.sketchCanvas_;
  }
  static bool lastFeatureUsesV2Face(const MainWindow& window) {
    const Body* body = window.activePartDocument().activeBody();
    if (!body || body->features().empty()) return false;
    const auto* extrude =
        dynamic_cast<const ExtrudeFeature*>(body->features().back().get());
    const auto face = extrude ? extrude->faceReference() : std::nullopt;
    return face && face->isV2();
  }
  static bool lastFeatureUsesV1Face(const MainWindow& window) {
    const Body* body = window.activePartDocument().activeBody();
    if (!body || body->features().empty()) return false;
    const auto* extrude =
        dynamic_cast<const ExtrudeFeature*>(body->features().back().get());
    const auto face = extrude ? extrude->faceReference() : std::nullopt;
    return face && face->isV1();
  }
  static bool editLastFaceExtrudeParameter(MainWindow& window,
                                           double lengthMm) {
    Body* body = window.mutableStandaloneDocument().activeBody();
    if (!body || body->features().empty()) return false;
    auto* extrude =
        dynamic_cast<ExtrudeFeature*>(body->features().back().get());
    if (!extrude || !extrude->faceReference()) return false;
    window.editFaceExtrudeStep(body, extrude, body->features().size() - 1);
    if (window.partDesignCoordinator_.activeTool() !=
        PartDesignToolKind::Extrude)
      return false;
    window.partDesignCoordinator_.setExtrudeLengthFromPanel(lengthMm);
    window.updateFaceExtrudeToolPreview();
    window.acceptFaceExtrudeTool();
    return window.partDesignCoordinator_.activeTool() ==
           PartDesignToolKind::None;
  }
  static bool reselectLastFaceExtrudeV2(MainWindow& window) {
    Body* body = window.mutableStandaloneDocument().activeBody();
    if (!body || body->features().size() < 2) return false;
    auto* extrude =
        dynamic_cast<ExtrudeFeature*>(body->features().back().get());
    auto* source = dynamic_cast<ShapeFeature*>(
        body->features()[body->features().size() - 2].get());
    if (!extrude || !source || !extrude->faceReference()) return false;
    window.editFaceExtrudeStep(body, extrude, body->features().size() - 1);
    if (window.partDesignCoordinator_.activeTool() !=
        PartDesignToolKind::Extrude)
      return false;
    const auto topology = source->topologyIndex();
    if (!topology) return false;
    for (std::size_t raw = 0; raw < topology->rawFaceCount(); ++raw) {
      const auto created = topology->createFaceReferenceV2(
          body->id(), source->id(), raw);
      if (!created) continue;
      window.partDesignCoordinator_.setExtrudeFace(created.reference);
      window.updateFaceExtrudeToolPreview();
      window.acceptFaceExtrudeTool();
      return window.partDesignCoordinator_.activeTool() ==
             PartDesignToolKind::None;
    }
    return false;
  }
};

}  // namespace solidar

// Every critical check uses CHECK (not assert) so it remains active in the
// Release CI build where NDEBUG is defined.
namespace {

bool sameRendererResources(
    const solidar::RendererResourceDiagnostics& left,
    const solidar::RendererResourceDiagnostics& right) {
  if (left.sourceUploadAttempts != right.sourceUploadAttempts ||
      left.sourceUploadSuccesses != right.sourceUploadSuccesses ||
      left.sourceUploadFailures != right.sourceUploadFailures ||
      left.sourceDestroyCount != right.sourceDestroyCount ||
      left.sourcePruneCount != right.sourcePruneCount ||
      left.sources.size() != right.sources.size())
    return false;
  for (std::size_t index = 0; index < left.sources.size(); ++index) {
    if (left.sources[index].key != right.sources[index].key ||
        left.sources[index].uploadGeneration !=
            right.sources[index].uploadGeneration ||
        left.sources[index].contentFingerprint !=
            right.sources[index].contentFingerprint)
      return false;
  }
  return true;
}

bool sameCameraState(const solidar::ViewportCameraState& left,
                     const solidar::ViewportCameraState& right) {
  return left.yawDeg == right.yawDeg && left.pitchDeg == right.pitchDeg &&
         left.zoom == right.zoom && left.pan == right.pan &&
         left.logicalSize == right.logicalSize &&
         left.devicePixelRatio == right.devicePixelRatio &&
         left.center.x == right.center.x && left.center.y == right.center.y &&
         left.center.z == right.center.z &&
         left.depthExtent == right.depthExtent;
}

constexpr solidar::ProjectId kUiV5ProjectId{
    {0x8d, 0xe4, 0x33, 0x71, 0x26, 0x9a, 0x4f, 0x5c,
     0xa1, 0x06, 0x52, 0x89, 0x34, 0x7b, 0xcd, 0xef}};
constexpr solidar::ProjectId kUiConversionProjectId{
    {0x10, 0x27, 0x34, 0x49, 0x58, 0x6a, 0x4b, 0x7c,
     0x8d, 0x9e, 0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5}};
constexpr solidar::ProjectId kUiCloneProjectId{
    {0x21, 0x38, 0x45, 0x5a, 0x69, 0x7b, 0x4c, 0x8d,
     0x9e, 0xaf, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5, 0x06}};
constexpr solidar::ProjectId kUiSecondCloneProjectId{
    {0x32, 0x49, 0x56, 0x6b, 0x7a, 0x8c, 0x4d, 0x9e,
     0xaf, 0xb0, 0xc2, 0xd3, 0xe4, 0xf5, 0x06, 0x17}};
constexpr solidar::DefinitionId kUiV5RootDefinition{424242};

solidar::StandaloneProductConversionRequest uiConversionRequest(
    solidar::DefinitionId root,
    QString name = QString::fromUtf8("Изделие Ω/雪"),
    QString createdAt = QStringLiteral("2026-10-23T20:15:30Z")) {
  return {kUiConversionProjectId, root, std::move(name),
          std::move(createdAt)};
}

solidar::ProductCloneRequest uiCloneRequest(
    solidar::ProjectId projectId = kUiCloneProjectId,
    QString name = QString::fromUtf8("Копия Ω/雪"),
    QString createdAt = QStringLiteral("2026-10-23T22:15:30Z")) {
  return {projectId, std::move(name), std::move(createdAt)};
}

solidar::ProductDocumentIdentity uiV5Identity() {
  return {kUiV5ProjectId,
          {"UI product \xCE\xA9/\xE9\x9B\xAA",
           "2026-10-23T18:45:00+03:00/\xD1\x82\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE"}};
}

std::unique_ptr<solidar::ProductDocument> uiV5Product(
    solidar::Document document) {
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            kUiV5RootDefinition,
            std::make_unique<solidar::Document>(std::move(document)))) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), kUiV5RootDefinition, uiV5Identity());
  CHECK(created);
  return std::move(created.product);
}

constexpr solidar::ProjectId kUiLinkedSourceProjectId{
    {0x71, 0x18, 0x25, 0x3a, 0x49, 0x5b, 0x4c, 0x8d,
     0x9e, 0xaf, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5, 0x06}};
constexpr solidar::ProjectId kUiLinkedAssemblyProjectId{
    {0x72, 0x29, 0x36, 0x4b, 0x5a, 0x6c, 0x4d, 0x9e,
     0xaf, 0xb0, 0xc2, 0xd3, 0xe4, 0xf5, 0x06, 0x17}};
constexpr solidar::DefinitionId kUiLinkedSourceDefinition{71};
constexpr solidar::DefinitionId kUiLinkedDefinition{72};
constexpr solidar::DefinitionId kUiLinkedAssemblyRoot{73};
constexpr solidar::DefinitionId kUiLateFailureDefinition{74};
constexpr solidar::DefinitionId kUiLateFailureRoot{75};

solidar::Document uiLinkedDocument(double widthMm) {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  solidar::Document document;
  document.setBox({widthMm, 20.0, 10.0});
  auto& sketch = document.addSketch(solidar::SketchId{7100},
                                    "linked profile");
  sketch.geometry.addRectangle({0.0, 0.0}, {widthMm, 20.0});
  auto& body = document.addBody(7101, "linked body");
  body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      solidar::FeatureId{7102}, solidar::SketchId{7100}, 10.0,
      "linked solid"));
  CHECK(document.recompute());
  return document;
}

solidar::Document uiInvalidRecomputeDocument() {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  solidar::Document document;
  auto& first = document.addBody(7201, "first");
  first.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      7202,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      "first archive"));
  auto& second = document.addBody(7203, "second");
  second.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      7204,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0),
                             20.0, 10.0, 5.0).Shape()),
      "second archive"));
  document.findBody(7201)->addFeature(
      std::make_unique<solidar::JoinBodiesFeature>(
          7205, 7201, 7202, 7203, 7204, "disconnected join"));
  return document;
}

std::unique_ptr<solidar::ProductDocument> uiLateFailureAssembly() {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            kUiLateFailureDefinition,
            std::make_unique<solidar::Document>(
                uiInvalidRecomputeDocument()))) ==
        solidar::DefinitionInsertStatus::Inserted);
  solidar::AssemblyDefinition root(kUiLateFailureRoot);
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{703}, kUiLateFailureDefinition,
            solidar::RigidTransform3d::identity(), {}, "late failure")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), kUiLateFailureRoot,
      solidar::ProductDocumentIdentity{
          kUiLinkedAssemblyProjectId,
          {"Late failure assembly", "2026-10-27T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<solidar::ProductDocument> uiLinkedSource(
    double widthMm) {
  solidar::Document document = uiLinkedDocument(widthMm);
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            kUiLinkedSourceDefinition,
            std::make_unique<solidar::Document>(std::move(document)))) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), kUiLinkedSourceDefinition,
      solidar::ProductDocumentIdentity{
          kUiLinkedSourceProjectId,
          {"Linked source", "2026-10-27T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<solidar::ProductDocument> uiLinkedAssembly(
    const solidar::LinkedPartBinding& binding) {
  solidar::Document cache = uiLinkedDocument(60.0);
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            kUiLinkedDefinition,
            std::make_unique<solidar::Document>(std::move(cache)),
            binding)) == solidar::DefinitionInsertStatus::Inserted);
  solidar::AssemblyDefinition root(kUiLinkedAssemblyRoot);
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{701}, kUiLinkedDefinition,
            solidar::RigidTransform3d::identity(), {}, "left")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{702}, kUiLinkedDefinition,
            solidar::RigidTransform3d::identity(),
            {true, false, true}, "right")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), kUiLinkedAssemblyRoot,
      solidar::ProductDocumentIdentity{
          kUiLinkedAssemblyProjectId,
          {"Linked assembly", "2026-10-27T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<solidar::ProductDocument> uiNestedAssembly() {
  std::vector<solidar::PartDefinition> parts;
  parts.emplace_back(solidar::DefinitionId{10},
                     std::make_unique<solidar::Document>(
                         uiLinkedDocument(25.0)));
  std::vector<solidar::AssemblyDefinition> assemblies;
  solidar::AssemblyDefinition child(solidar::DefinitionId{200});
  CHECK(child.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{7}, solidar::DefinitionId{10},
            solidar::RigidTransform3d::identity(), {}, "nested leaf")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  solidar::AssemblyDefinition orphan(solidar::DefinitionId{300});
  solidar::AssemblyDefinition root(solidar::DefinitionId{100});
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{1}, solidar::DefinitionId{200},
            solidar::RigidTransform3d::identity(), {}, "left branch")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{2}, solidar::DefinitionId{200},
            solidar::RigidTransform3d::identity(), {}, "right branch")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(child));
  assemblies.push_back(std::move(orphan));
  assemblies.push_back(std::move(root));
  auto created = solidar::ProductDocument::createNested(
      std::move(parts), std::move(assemblies), solidar::DefinitionId{100});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<solidar::ProductDocument> uiNestedLinkedAssembly(
    const solidar::LinkedPartBinding& binding) {
  std::vector<solidar::PartDefinition> parts;
  parts.emplace_back(kUiLinkedDefinition,
                     std::make_unique<solidar::Document>(
                         uiLinkedDocument(60.0)),
                     binding);
  std::vector<solidar::AssemblyDefinition> assemblies;
  solidar::AssemblyDefinition child(solidar::DefinitionId{200});
  CHECK(child.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{7}, kUiLinkedDefinition,
            solidar::RigidTransform3d::identity(), {}, "linked leaf")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  solidar::AssemblyDefinition root(kUiLinkedAssemblyRoot);
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{701}, solidar::DefinitionId{200},
            solidar::RigidTransform3d::identity(), {}, "left nested")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{702}, solidar::DefinitionId{200},
            solidar::RigidTransform3d::identity(), {}, "right nested")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(child));
  assemblies.push_back(std::move(root));
  auto created = solidar::ProductDocument::createNested(
      std::move(parts), std::move(assemblies), kUiLinkedAssemblyRoot,
      solidar::ProductDocumentIdentity{
          kUiLinkedAssemblyProjectId,
          {"Nested linked assembly", "2026-10-29T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

bool writeTestBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readTestBytes(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}

solidar::BodyId consumeBodyId(const char* name) {
  solidar::Document document;
  return document.addBody(name).id();
}

struct ModelIdCursorSample {
  solidar::BodyId body{};
  solidar::SketchId sketch{};
  solidar::FeatureId feature{};
  solidar::ProductIncarnationId product;
};

ModelIdCursorSample consumeModelIdCursors(const char* name) {
  solidar::Document document;
  const auto sketch = document.addSketch(std::string(name) + " sketch").id;
  auto& body = document.addBody(std::string(name) + " body");
  auto feature = std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape()),
      std::string(name) + " feature");
  const auto featureId = feature->id();
  body.addFeature(std::move(feature));
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            solidar::DefinitionId{1},
            std::make_unique<solidar::Document>())) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto product = solidar::ProductDocument::create(
      std::move(definitions), solidar::DefinitionId{1});
  CHECK(product);
  return {body.id(), sketch, featureId, product.product->incarnation()};
}

solidar::Document makeHeavyImportedDocument(int bodyCount) {
  solidar::Document document;
  for (int bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (int shapeIndex = 0; shapeIndex < 120; ++shapeIndex) {
      const double x = static_cast<double>(shapeIndex % 12) * 3.0;
      const double y = static_cast<double>(shapeIndex / 12) * 3.0;
      builder.Add(compound,
                  BRepPrimAPI_MakeBox(
                      gp_Pnt(x, y, static_cast<double>(bodyIndex) * 4.0),
                      2.0, 2.0, 2.0)
                      .Shape());
    }
    auto& body = document.addBody("CAD history body");
    body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(compound), "CAD compound"));
  }
  if (!document.recompute()) return {};
  return document;
}

struct HistoryMatrixFixture {
  std::string label;
  solidar::Document before;
  solidar::Document after;
  solidar::BodyId bodyId{solidar::kInvalidBodyId};
  solidar::BodyId createSelectionBodyId{solidar::kInvalidBodyId};
  solidar::FeatureId featureId{solidar::kInvalidFeatureId};
  solidar::BodyId sentinelBodyId{solidar::kInvalidBodyId};
  solidar::FeatureId sentinelFeatureId{solidar::kInvalidFeatureId};
  solidar::FeatureKind featureKind{solidar::FeatureKind::Unknown};
};

struct BoxSeed {
  solidar::BodyId bodyId{};
  solidar::SketchId sketchId{};
  solidar::FeatureId featureId{};
};

std::optional<BoxSeed> addBox(solidar::Document& document,
                              double x0 = 0.0, double x1 = 20.0,
                              double height = 10.0,
                              const std::string& name = "Box") {
  auto& sketch = document.addSketch(name + " profile");
  sketch.geometry.addRectangle({x0, 0.0}, {x1, 20.0});
  auto& body = document.addBody(name);
  auto feature = std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, height, name + " Extrude");
  const auto featureId = feature->id();
  body.addFeature(std::move(feature));
  if (!document.recompute()) return std::nullopt;
  return BoxSeed{body.id(), sketch.id, featureId};
}

std::optional<HistoryMatrixFixture> makeHistoryMatrixFixture(
    const std::string& kind) {
  solidar::Document document;
  solidar::Document before;
  solidar::BodyId targetBody = solidar::kInvalidBodyId;
  solidar::BodyId createSelectionBody = solidar::kInvalidBodyId;
  solidar::FeatureId targetFeature = solidar::kInvalidFeatureId;
  const auto sentinel = addBox(document, 5.0, 15.0, 5.0,
                               "Selection sentinel");
  if (!sentinel) return std::nullopt;

  if (kind == "Extrude") {
    auto& sketch = document.addSketch("Extrude profile");
    sketch.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    before = document;
    auto& body = document.addBody("Extrude");
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Extrude", solidar::ExtrudeOperation::NewBody,
        false);
    targetBody = body.id(); targetFeature = feature->id();
    body.addFeature(std::move(feature));
  } else if (kind == "Revolve") {
    auto& sketch = document.addSketch("Revolve profile");
    sketch.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    before = document;
    auto& body = document.addBody("Revolve body");
    auto feature = std::make_unique<solidar::RevolveFeature>(
        sketch.id,
        solidar::AxisReference{
            solidar::AxisReferenceType::SketchHorizontalAxis, sketch.id,
            solidar::sketch::kInvalidGeometryId},
        270.0, "Revolve", solidar::ExtrudeOperation::NewBody);
    targetBody = body.id(); targetFeature = feature->id();
    body.addFeature(std::move(feature));
  } else if (kind == "Join") {
    const auto first = addBox(document, 0.0, 20.0, 10.0, "Join A");
    const auto second = addBox(document, 10.0, 30.0, 10.0, "Join B");
    const auto replacement = addBox(document, 5.0, 25.0, 10.0, "Join C");
    if (!first || !second || !replacement) return std::nullopt;
    before = document;
    createSelectionBody = first->bodyId;
    auto& body = document.addBody("Join result");
    auto feature = std::make_unique<solidar::JoinBodiesFeature>(
        first->bodyId, first->featureId, second->bodyId, second->featureId,
        "Join");
    targetBody = body.id(); targetFeature = feature->id();
    body.addFeature(std::move(feature));
    document.findBody(first->bodyId)->setVisible(false);
    document.findBody(second->bodyId)->setVisible(false);
  } else {
    const double x0 = (kind == "Mirror" || kind == "CircularPattern")
                          ? 10.0 : 0.0;
    const auto base = addBox(document, x0, x0 + 20.0, 20.0, kind);
    if (!base) return std::nullopt;
    auto* body = document.findBody(base->bodyId);
    if (!body) return std::nullopt;
    targetBody = body->id();
    createSelectionBody = body->id();
    if (kind == "FaceExtrude") {
      const auto shape = body->resultShape();
      const auto face = shape ? solidar::test::topPlanarFace(*shape, 20.0)
                              : std::nullopt;
      if (!shape || !face) return std::nullopt;
      auto feature = std::make_unique<solidar::ExtrudeFeature>(
          solidar::makeFaceReference(*shape, body->id(), base->featureId,
                                     *face),
          5.0, "Face Extrude", solidar::ExtrudeOperation::Join, false);
      before = document;
      targetFeature = feature->id();
      body->addFeature(std::move(feature));
    } else if (kind == "Move") {
      auto feature = std::make_unique<solidar::MoveFeature>(
          base->featureId, solidar::Vector3d{5.0, 0.0, 0.0}, "Move");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Mirror") {
      auto feature = std::make_unique<solidar::MirrorFeature>(
          base->featureId, solidar::MirrorPlane::YZ, "Mirror");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "LinearPattern") {
      auto feature = std::make_unique<solidar::LinearPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::X, 3, 40.0,
          solidar::PatternOperation::Join, "Linear Pattern");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "CircularPattern") {
      auto feature = std::make_unique<solidar::CircularPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::Z, 4, 360.0,
          solidar::PatternOperation::Join, "Circular Pattern");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "LinearPatternNewBody") {
      before = document;
      auto& result = document.addBody("Linear Pattern NewBody");
      auto feature = std::make_unique<solidar::LinearPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::X, 3, 40.0,
          solidar::PatternOperation::NewBody, "Linear Pattern NewBody");
      targetBody = result.id(); targetFeature = feature->id();
      result.addFeature(std::move(feature));
    } else if (kind == "CircularPatternNewBody") {
      before = document;
      auto& result = document.addBody("Circular Pattern NewBody");
      auto feature = std::make_unique<solidar::CircularPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::Z, 4, 360.0,
          solidar::PatternOperation::NewBody, "Circular Pattern NewBody");
      targetBody = result.id(); targetFeature = feature->id();
      result.addFeature(std::move(feature));
    } else if (kind == "Fillet") {
      const auto shape = body->resultShape();
      if (!shape) return std::nullopt;
      auto feature = std::make_unique<solidar::FilletFeature>(
          solidar::makeEdgeReference(*shape, body->id(), base->featureId, 0),
          2.0, "Fillet");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Chamfer") {
      const auto shape = body->resultShape();
      if (!shape) return std::nullopt;
      auto feature = std::make_unique<solidar::ChamferFeature>(
          solidar::makeEdgeReference(*shape, body->id(), base->featureId, 0),
          1.0, "Chamfer");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Shell") {
      const auto shape = body->resultShape();
      const auto face = shape ? solidar::test::topPlanarFace(*shape, 20.0)
                              : std::nullopt;
      if (!face) return std::nullopt;
      auto feature = std::make_unique<solidar::ShellFeature>(
          base->featureId,
          std::vector{solidar::makeFaceReference(
              *shape, body->id(), base->featureId, *face)},
          2.0, false, "Shell");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Draft") {
      const auto shape = body->resultShape();
      if (!shape) return std::nullopt;
      std::optional<std::size_t> side;
      std::size_t index = 0;
      for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
           faces.Next(), ++index) {
        const auto placement = solidar::resolveFacePlacement(*shape, index);
        if (placement.planar &&
            std::abs(placement.placement.normal().z) < 0.1) {
          side = index; break;
        }
      }
      if (!side) return std::nullopt;
      auto feature = std::make_unique<solidar::DraftFeature>(
          base->featureId,
          std::vector{solidar::makeFaceReference(
              *shape, body->id(), base->featureId, *side)},
          solidar::PlaneReference{solidar::NeutralPlaneType::GlobalXY},
          solidar::AxisReference{solidar::AxisReferenceType::GlobalZ,
                                 solidar::kInvalidSketchId,
                                 solidar::sketch::kInvalidGeometryId},
          5.0, false, "Draft");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Pocket") {
      const auto shape = body->resultShape();
      const auto face = shape ? solidar::test::topPlanarFace(*shape, 20.0)
                              : std::nullopt;
      if (!face) return std::nullopt;
      auto& profile = document.addSketch("Pocket profile");
      if (!document.attachSketchToFace(
              profile.id, solidar::makeFaceReference(
                  *shape, body->id(), base->featureId, *face)))
        return std::nullopt;
      const auto first = profile.placement.toLocal({5.0, 5.0, 20.0});
      const auto second = profile.placement.toLocal({10.0, 10.0, 20.0});
      profile.geometry.addRectangle({first.x, first.y},
                                    {second.x, second.y});
      if (!document.recompute()) return std::nullopt;
      auto feature = std::make_unique<solidar::PocketFeature>(
          profile.id, 5.0, "Pocket");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else {
      return std::nullopt;
    }
  }

  if (!document.recompute()) return std::nullopt;
  const auto* feature = document.findFeature(targetFeature);
  if (!feature || !feature->isValid()) return std::nullopt;
  const auto featureKind = feature->kind();
  return HistoryMatrixFixture{kind, std::move(before), std::move(document),
                              targetBody, createSelectionBody, targetFeature,
                              sentinel->bodyId, sentinel->featureId,
                               featureKind};
}

std::optional<solidar::Document> makeExpectedEditedDocument(
    const HistoryMatrixFixture& fixture) {
  solidar::Document expected = fixture.after;
  auto* body = expected.findBody(fixture.bodyId);
  auto* feature = expected.findFeature(fixture.featureId);
  if (!body || !feature) return std::nullopt;
  const auto index = body->featureIndex(fixture.featureId);
  if (!index) return std::nullopt;

  if (auto* item = dynamic_cast<solidar::ExtrudeFeature*>(feature)) {
    item->setLengthMm(25.0);
  } else if (auto* item = dynamic_cast<solidar::RevolveFeature*>(feature)) {
    item->setAngleDeg(180.0);
  } else if (auto* item = dynamic_cast<solidar::PocketFeature*>(feature)) {
    item->setDepthMm(7.0);
  } else if (auto* item = dynamic_cast<solidar::FilletFeature*>(feature)) {
    item->setRadiusMm(1.0);
  } else if (auto* item = dynamic_cast<solidar::ChamferFeature*>(feature)) {
    item->setDistanceMm(0.5);
  } else if (auto* item = dynamic_cast<solidar::ShellFeature*>(feature)) {
    item->setThicknessMm(1.0);
  } else if (auto* item = dynamic_cast<solidar::DraftFeature*>(feature)) {
    item->setAngleDeg(3.0);
  } else if (auto* item = dynamic_cast<solidar::MoveFeature*>(feature)) {
    item->setOffsetMm({7.0, 1.0, 0.0});
  } else if (auto* item = dynamic_cast<solidar::MirrorFeature*>(feature)) {
    item->setPlane(solidar::MirrorPlane::XZ);
  } else if (auto* item =
                 dynamic_cast<solidar::LinearPatternFeature*>(feature)) {
    item->setCount(2);
    item->setSpacingMm(50.0);
  } else if (auto* item =
                 dynamic_cast<solidar::CircularPatternFeature*>(feature)) {
    item->setCount(3);
    item->setAngleDeg(270.0);
  } else if (auto* item =
                 dynamic_cast<solidar::JoinBodiesFeature*>(feature)) {
    const solidar::Body* replacement = nullptr;
    for (const auto& candidate : expected.bodies()) {
      if (candidate.id() == fixture.bodyId ||
          candidate.id() == item->firstBodyId() ||
          candidate.id() == item->secondBodyId() || !candidate.activeFeature())
        continue;
      replacement = &candidate;
      break;
    }
    if (!replacement || !replacement->activeFeature()) return std::nullopt;
    const auto oldSecond = item->secondBodyId();
    item->setInputs(item->firstBodyId(), item->firstFeatureId(),
                    replacement->id(), replacement->activeFeature()->id());
    if (auto* detached = expected.findBody(oldSecond)) detached->setVisible(true);
    if (auto* attached = expected.findBody(replacement->id()))
      attached->setVisible(false);
  } else {
    return std::nullopt;
  }
  body->markDirtyFrom(*index);
  if (!expected.recompute()) return std::nullopt;
  return expected;
}

template <typename Reference>
void appendTopologyReference(std::ostringstream& out,
                             const Reference& reference) {
  if constexpr (std::is_same_v<Reference, solidar::FaceReference>) {
    out << reference.bodyId() << ',' << reference.featureId() << ',';
    out << reference.faceIndex() << ',' << reference.persistentTag() << ',';
    if (reference.signature()) {
      const auto& signature = *reference.signature();
      out << static_cast<int>(signature.surface) << ',' << signature.area << ','
          << signature.centroid.x << ',' << signature.centroid.y << ','
          << signature.centroid.z << ',' << signature.normal.x << ','
          << signature.normal.y << ',' << signature.normal.z << ','
          << signature.bounds.minimum.x << ',' << signature.bounds.minimum.y
          << ',' << signature.bounds.minimum.z << ','
          << signature.bounds.maximum.x << ',' << signature.bounds.maximum.y
          << ',' << signature.bounds.maximum.z << ',' << signature.radius
          << ',' << signature.axis.x << ',' << signature.axis.y << ','
          << signature.axis.z;
    } else {
      out << "none";
    }
  } else {
    out << reference.bodyId() << ',' << reference.featureId() << ',';
    out << reference.edgeIndex() << ',' << reference.persistentTag() << ',';
    if (reference.signature()) {
      const auto& signature = *reference.signature();
      out << static_cast<int>(signature.curve) << ',' << signature.length << ','
          << signature.midpoint.x << ',' << signature.midpoint.y << ','
          << signature.midpoint.z << ',' << signature.tangent.x << ','
          << signature.tangent.y << ',' << signature.tangent.z << ','
          << signature.bounds.minimum.x << ',' << signature.bounds.minimum.y
          << ',' << signature.bounds.minimum.z << ','
          << signature.bounds.maximum.x << ',' << signature.bounds.maximum.y
          << ',' << signature.bounds.maximum.z << ',' << signature.radius
          << ',' << signature.center.x << ',' << signature.center.y << ','
          << signature.center.z;
    } else {
      out << "none";
    }
  }
}

std::string historyMatrixPayload(const solidar::ShapeFeature& feature,
                                 bool includeIdentity = true) {
  std::ostringstream out;
  out.precision(17);
  out << static_cast<int>(feature.kind());
  if (includeIdentity) out << ':' << feature.id() << ':' << feature.name();
  if (const auto* item = dynamic_cast<const solidar::ExtrudeFeature*>(&feature)) {
    out << ":face=" << item->isFaceSource();
    if (item->isFaceSource()) {
      if (item->faceReference()) appendTopologyReference(out, *item->faceReference());
    } else {
      out << ":sketch=" << item->profileSketchId();
    }
    out << ":override=" << item->profileOverride().has_value();
    if (item->profileOverride())
      out << ':' << item->profileOverride()->semanticFingerprint();
    out << ':' << item->lengthMm() << ':' << static_cast<int>(item->operation())
        << ':' << item->reversed();
  } else if (const auto* item = dynamic_cast<const solidar::RevolveFeature*>(&feature)) {
    out << ':' << item->profileSketchId() << ':'
        << static_cast<int>(item->axis().type) << ':' << item->axis().sketchId
        << ':' << item->axis().lineId << ':' << item->angleDeg() << ':'
        << static_cast<int>(item->operation()) << ':' << item->reversed()
        << ':' << item->profileOverride().has_value();
    if (item->profileOverride())
      out << ':' << item->profileOverride()->semanticFingerprint();
  } else if (const auto* item = dynamic_cast<const solidar::PocketFeature*>(&feature)) {
    out << ':' << item->profileSketchId() << ':' << item->depthMm();
  } else if (const auto* item = dynamic_cast<const solidar::FilletFeature*>(&feature)) {
    out << ':' << item->radiusMm() << ':' << item->edges().size();
    for (const auto& edge : item->edges()) appendTopologyReference(out, edge);
  } else if (const auto* item = dynamic_cast<const solidar::ChamferFeature*>(&feature)) {
    out << ':' << item->distanceMm() << ':' << item->edges().size();
    for (const auto& edge : item->edges()) appendTopologyReference(out, edge);
  } else if (const auto* item = dynamic_cast<const solidar::ShellFeature*>(&feature)) {
    out << ':' << item->sourceFeatureId() << ':' << item->thicknessMm() << ':'
        << item->outside() << ':' << item->removedFaces().size();
    for (const auto& face : item->removedFaces()) appendTopologyReference(out, face);
  } else if (const auto* item = dynamic_cast<const solidar::DraftFeature*>(&feature)) {
    out << ':' << item->sourceFeatureId() << ':' << item->angleDeg() << ':'
        << item->reversed() << ':' << static_cast<int>(item->neutralPlane().type)
        << ':' << static_cast<int>(item->pullDirection().type) << ':'
        << item->pullDirection().sketchId << ':' << item->pullDirection().lineId
        << ':' << item->draftedFaces().size();
    if (item->neutralPlane().face)
      appendTopologyReference(out, *item->neutralPlane().face);
    if (item->rotationEdge()) appendTopologyReference(out, *item->rotationEdge());
    for (const auto& face : item->draftedFaces()) appendTopologyReference(out, face);
  } else if (const auto* item = dynamic_cast<const solidar::MirrorFeature*>(&feature)) {
    out << ':' << item->sourceFeatureId() << ':' << static_cast<int>(item->plane());
  } else if (const auto* item = dynamic_cast<const solidar::MoveFeature*>(&feature)) {
    const auto offset = item->offsetMm();
    out << ':' << item->sourceFeatureId() << ':' << offset.x << ':' << offset.y
        << ':' << offset.z;
  } else if (const auto* item =
                 dynamic_cast<const solidar::LinearPatternFeature*>(&feature)) {
    out << ':' << item->sourceBodyId() << ':' << item->sourceFeatureId() << ':'
        << static_cast<int>(item->direction()) << ':' << item->count() << ':'
        << item->spacingMm() << ':' << static_cast<int>(item->operation());
  } else if (const auto* item =
                 dynamic_cast<const solidar::CircularPatternFeature*>(&feature)) {
    out << ':' << item->sourceBodyId() << ':' << item->sourceFeatureId() << ':'
        << static_cast<int>(item->axis()) << ':' << item->count() << ':'
        << item->angleDeg() << ':' << static_cast<int>(item->operation());
  } else if (const auto* item = dynamic_cast<const solidar::JoinBodiesFeature*>(&feature)) {
    out << ':' << item->firstBodyId() << ':' << item->firstFeatureId() << ':'
        << item->secondBodyId() << ':' << item->secondFeatureId();
  }
  return out.str();
}

struct MatrixShapeSignature {
  std::size_t solids{};
  std::size_t faces{};
  std::size_t edges{};
  std::size_t rawFaces{};
  std::size_t rawEdges{};
  double volume{};
  solidar::test::Bounds bounds;
};

std::optional<MatrixShapeSignature> matrixShapeSignature(
    const solidar::ShapeFeature* feature) {
  if (!feature || !feature->isValid() || !feature->error().empty() ||
      !feature->lastValidShape() || feature->lastValidShape()->IsNull() ||
      !BRepCheck_Analyzer(*feature->lastValidShape()).IsValid())
    return std::nullopt;
  std::string error;
  const auto topology = feature->lastValidTopologyIndex(&error);
  if (!topology || !error.empty()) return std::nullopt;
  return MatrixShapeSignature{
      solidar::test::solidCount(*feature->lastValidShape()),
      topology->faceCount(), topology->edgeCount(), topology->rawFaceCount(),
      topology->rawEdgeCount(),
      solidar::test::volumeOf(*feature->lastValidShape()),
      solidar::test::boundsOf(*feature->lastValidShape())};
}

bool sameMatrixShape(const MatrixShapeSignature& left,
                     const MatrixShapeSignature& right) {
  const auto near = [](double a, double b) {
    return solidar::test::near(a, b, 1e-5);
  };
  return left.solids == right.solids && left.faces == right.faces &&
         left.edges == right.edges && left.rawFaces == right.rawFaces &&
         left.rawEdges == right.rawEdges && near(left.volume, right.volume) &&
         near(left.bounds.minX, right.bounds.minX) &&
         near(left.bounds.minY, right.bounds.minY) &&
         near(left.bounds.minZ, right.bounds.minZ) &&
         near(left.bounds.maxX, right.bounds.maxX) &&
         near(left.bounds.maxY, right.bounds.maxY) &&
         near(left.bounds.maxZ, right.bounds.maxZ);
}

bool featureBelongsToBody(const solidar::Document& document,
                          solidar::BodyId bodyId,
                          solidar::FeatureId featureId) {
  const auto* body = document.findBody(bodyId);
  return body && std::any_of(
      body->features().begin(), body->features().end(),
      [featureId](const auto& feature) {
        return feature && feature->id() == featureId;
      });
}

bool faceReferenceResolves(const solidar::Document& document,
                           const solidar::FaceReference& reference) {
  if (!featureBelongsToBody(document, reference.bodyId(),
                            reference.featureId()))
    return false;
  const auto* feature = dynamic_cast<const solidar::ShapeFeature*>(
      document.findFeature(reference.featureId()));
  const auto topology = feature ? feature->lastValidTopologyIndex() : nullptr;
  return topology && topology->resolveFace(reference);
}

bool edgeReferenceResolves(const solidar::Document& document,
                           const solidar::EdgeReference& reference) {
  if (!featureBelongsToBody(document, reference.bodyId(),
                            reference.featureId()))
    return false;
  const auto* feature = dynamic_cast<const solidar::ShapeFeature*>(
      document.findFeature(reference.featureId()));
  const auto topology = feature ? feature->lastValidTopologyIndex() : nullptr;
  return topology && topology->resolveEdge(reference);
}

bool axisReferenceResolves(const solidar::Document& document,
                           const solidar::AxisReference& axis) {
  if (!solidar::isKnownAxisReferenceType(axis.type)) return false;
  if (!solidar::isSketchAxisReferenceType(axis.type)) return true;
  const auto* sketch = document.findSketch(axis.sketchId);
  if (!sketch) return false;
  return axis.type != solidar::AxisReferenceType::SketchLine ||
         sketch->geometry.lineIndex(axis.lineId).has_value();
}

bool matrixReferencesResolve(const solidar::Document& document,
                             const solidar::ShapeFeature& feature) {
  if (const auto* item =
          dynamic_cast<const solidar::ExtrudeFeature*>(&feature)) {
    return item->isFaceSource()
        ? item->faceReference() &&
              faceReferenceResolves(document, *item->faceReference())
        : document.findSketch(item->profileSketchId()) != nullptr;
  }
  if (const auto* item =
          dynamic_cast<const solidar::RevolveFeature*>(&feature))
    return document.findSketch(item->profileSketchId()) &&
           axisReferenceResolves(document, item->axis());
  if (const auto* item = dynamic_cast<const solidar::PocketFeature*>(&feature))
    return document.findSketch(item->profileSketchId()) != nullptr;
  if (const auto* item = dynamic_cast<const solidar::FilletFeature*>(&feature))
    return !item->edges().empty() &&
           std::all_of(item->edges().begin(), item->edges().end(),
                       [&document](const auto& edge) {
                         return edgeReferenceResolves(document, edge);
                       });
  if (const auto* item = dynamic_cast<const solidar::ChamferFeature*>(&feature))
    return !item->edges().empty() &&
           std::all_of(item->edges().begin(), item->edges().end(),
                       [&document](const auto& edge) {
                         return edgeReferenceResolves(document, edge);
                       });
  if (const auto* item = dynamic_cast<const solidar::ShellFeature*>(&feature))
    return document.findFeature(item->sourceFeatureId()) &&
           !item->removedFaces().empty() &&
           std::all_of(item->removedFaces().begin(), item->removedFaces().end(),
                       [&document](const auto& face) {
                         return faceReferenceResolves(document, face);
                       });
  if (const auto* item = dynamic_cast<const solidar::DraftFeature*>(&feature)) {
    if (!document.findFeature(item->sourceFeatureId()) ||
        !axisReferenceResolves(document, item->pullDirection()) ||
        item->draftedFaces().empty() ||
        !std::all_of(item->draftedFaces().begin(), item->draftedFaces().end(),
                     [&document](const auto& face) {
                       return faceReferenceResolves(document, face);
                     }))
      return false;
    if (item->neutralPlane().face &&
        !faceReferenceResolves(document, *item->neutralPlane().face))
      return false;
    return !item->rotationEdge() ||
           edgeReferenceResolves(document, *item->rotationEdge());
  }
  if (const auto* item = dynamic_cast<const solidar::JoinBodiesFeature*>(&feature))
    return featureBelongsToBody(document, item->firstBodyId(),
                                item->firstFeatureId()) &&
           featureBelongsToBody(document, item->secondBodyId(),
                                item->secondFeatureId());
  if (const auto* item = dynamic_cast<const solidar::MoveFeature*>(&feature))
    return document.findFeature(item->sourceFeatureId()) != nullptr;
  if (const auto* item = dynamic_cast<const solidar::MirrorFeature*>(&feature))
    return document.findFeature(item->sourceFeatureId()) != nullptr;
  if (const auto* item =
          dynamic_cast<const solidar::LinearPatternFeature*>(&feature))
    return featureBelongsToBody(document, item->sourceBodyId(),
                                item->sourceFeatureId());
  if (const auto* item =
          dynamic_cast<const solidar::CircularPatternFeature*>(&feature))
    return featureBelongsToBody(document, item->sourceBodyId(),
                                item->sourceFeatureId());
  return false;
}

std::optional<QByteArray> serializedDocumentDefinition(
    const solidar::Document& document) {
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("unchanged-edit-document-XXXXXX")));
  if (!directory.isValid()) return std::nullopt;
  const QString path = directory.filePath(QStringLiteral("state.solidar"));
  QString error;
  if (!solidar::project::ProjectFile::saveDocument(path, document, &error))
    return std::nullopt;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
  QJsonParseError parseError;
  auto serialized = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !serialized.isObject())
    return std::nullopt;
  // createdAt is export metadata generated by saveDocument(), not persisted
  // Document state.  Canonicalize only that clock-dependent field; equality
  // below remains byte-exact for the complete model definition.
  auto root = serialized.object();
  root.remove(QStringLiteral("createdAt"));
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool writeDefinitionMoveAssemblyFixture(const QString& path) {
  constexpr solidar::ProjectId kProject{
      {0x2a, 0x45, 0x64, 0x83, 0x19, 0x3e, 0x48, 0x72,
       0x8c, 0x61, 0x7d, 0x55, 0x10, 0xa9, 0x3b, 0xc4}};
  constexpr solidar::DefinitionId kPart{1};
  constexpr solidar::DefinitionId kOrphan{2};
  constexpr solidar::DefinitionId kRoot{100};
  solidar::Document part;
  auto& sketch = part.addSketch("Definition move profile");
  sketch.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
  auto& body = part.addBody("Definition move body");
  body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, 5.0, "Definition move source"));
  if (!part.recompute()) return false;
  solidar::Document orphan = part;

  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  solidar::DefinitionRegistry definitions;
  if (definitions.insert(solidar::PartDefinition(
          kPart, std::make_unique<solidar::Document>(std::move(part)))) !=
          solidar::DefinitionInsertStatus::Inserted ||
      definitions.insert(solidar::PartDefinition(
          kOrphan, std::make_unique<solidar::Document>(std::move(orphan)))) !=
          solidar::DefinitionInsertStatus::Inserted)
    return false;
  const auto secondPlacement = solidar::RigidTransform3d::create(
      {40.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  if (!secondPlacement) return false;
  solidar::AssemblyDefinition root(kRoot);
  if (root.insert(solidar::ComponentOccurrence(
          solidar::OccurrenceId{11}, kPart,
          solidar::RigidTransform3d::identity())) !=
          solidar::AssemblyOccurrenceInsertStatus::Inserted ||
      root.insert(solidar::ComponentOccurrence(
          solidar::OccurrenceId{12}, kPart, *secondPlacement.transform)) !=
          solidar::AssemblyOccurrenceInsertStatus::Inserted ||
      definitions.insert(std::move(root)) !=
          solidar::DefinitionInsertStatus::Inserted)
    return false;
  auto product = solidar::ProductDocument::create(std::move(definitions),
                                                   kRoot);
  if (!product) return false;
  const auto encoded = solidar::project::encodeProductEnvelopeV6Root(
      *product.product, kProject,
      {QStringLiteral("definition-move-assembly"),
       QStringLiteral("2026-10-26T00:00:00Z")});
  if (!encoded.succeeded()) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(encoded.payload->bytes) == encoded.payload->bytes.size();
}

std::optional<QString> firstJsonDifference(const QJsonValue& left,
                                           const QJsonValue& right,
                                           const QString& path) {
  if (left.type() != right.type())
    return QStringLiteral("%1: type %2 != %3")
        .arg(path).arg(static_cast<int>(left.type()))
        .arg(static_cast<int>(right.type()));
  if (left.isObject()) {
    const auto leftObject = left.toObject();
    const auto rightObject = right.toObject();
    QStringList keys = leftObject.keys();
    for (const auto& key : rightObject.keys())
      if (!keys.contains(key)) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
      if (!leftObject.contains(key) || !rightObject.contains(key))
        return QStringLiteral("%1.%2: key presence %3 != %4")
            .arg(path, key)
            .arg(leftObject.contains(key)).arg(rightObject.contains(key));
      if (auto difference = firstJsonDifference(
              leftObject.value(key), rightObject.value(key),
              path + QLatin1Char('.') + key))
        return difference;
    }
    return std::nullopt;
  }
  if (left.isArray()) {
    const auto leftArray = left.toArray();
    const auto rightArray = right.toArray();
    if (leftArray.size() != rightArray.size())
      return QStringLiteral("%1: array size %2 != %3")
          .arg(path).arg(leftArray.size()).arg(rightArray.size());
    for (qsizetype index = 0; index < leftArray.size(); ++index) {
      if (auto difference = firstJsonDifference(
              leftArray.at(index), rightArray.at(index),
              QStringLiteral("%1[%2]").arg(path).arg(index)))
        return difference;
    }
    return std::nullopt;
  }
  if (left != right)
    return QStringLiteral("%1: '%2' != '%3'")
        .arg(path, left.toVariant().toString(), right.toVariant().toString());
  return std::nullopt;
}

QString firstSerializedDocumentDifference(const QByteArray& left,
                                          const QByteArray& right) {
  QJsonParseError leftError;
  QJsonParseError rightError;
  const auto leftDocument = QJsonDocument::fromJson(left, &leftError);
  const auto rightDocument = QJsonDocument::fromJson(right, &rightError);
  if (leftError.error != QJsonParseError::NoError ||
      rightError.error != QJsonParseError::NoError)
    return QStringLiteral("JSON parse errors: %1 / %2")
        .arg(leftError.errorString(), rightError.errorString());
  return firstJsonDifference(QJsonValue(leftDocument.object()),
                             QJsonValue(rightDocument.object()),
                             QStringLiteral("root"))
      .value_or(QStringLiteral("byte-only difference"));
}

void runInitialPersistenceAndConversionScenarios(
    const QTemporaryDir& directory, solidar::AppSettings& settings,
    const QString& pathA, QString& error) {
  using Access = solidar::MainWindowUndoTestAccess;

  // MainWindow starts with one valid transient root. A successful replacement
  // moves the staged carrier (and its exact Document/B-Rep graph) at one hard
  // boundary. The synchronous resetScene notification must still observe the
  // old root; only after it returns may the carrier swap destroy that graph.
  {
    solidar::MainWindow ownershipEditor(settings);
    CHECK(Access::definitionCount(ownershipEditor) == 1);
    CHECK(Access::rootDefinitionId(ownershipEditor).isValid());
    CHECK(Access::documentAddress(ownershipEditor) ==
          Access::rootDefinitionDocumentAddress(ownershipEditor));
    CHECK(ownershipEditor.loadProject(pathA, &error));

    const auto* const outgoingDocument =
        Access::documentAddress(ownershipEditor);
    const auto outgoingShape = Access::firstShapeWeak(ownershipEditor);
    CHECK(!outgoingShape.expired());

    auto staged = solidar::ProjectApplicationService{}.stageOpen(pathA);
    CHECK(staged.succeeded());
    const auto* const stagedDocument = &staged.staged->rootDocument();
    const auto* const stagedBody = &stagedDocument->bodies().front();
    const auto* const stagedFeature = stagedBody->features().front().get();
    const auto* const stagedShapeFeature =
        dynamic_cast<const solidar::ShapeFeature*>(stagedFeature);
    CHECK(stagedShapeFeature);
    const auto* const stagedShape = stagedShapeFeature->lastValidShape().get();
    std::string topologyError;
    const auto stagedTopology =
        stagedShapeFeature->lastValidTopologyIndex(&topologyError);
    CHECK(stagedTopology);
    CHECK(topologyError.empty());

    const solidar::Document* observedDuringReset = nullptr;
    bool outgoingShapeAliveDuringReset = false;
    Access::viewport(ownershipEditor)->beginRulerMeasurement();
    const auto resetConnection = QObject::connect(
        Access::viewport(ownershipEditor),
        &solidar::Viewport::rulerActiveChanged, &ownershipEditor,
        [&](bool active) {
          if (active) return;
          observedDuringReset = Access::documentAddress(ownershipEditor);
          outgoingShapeAliveDuringReset = !outgoingShape.expired();
        },
        Qt::DirectConnection);
    Access::commitStaged(ownershipEditor, std::move(*staged.staged), pathA);
    QObject::disconnect(resetConnection);

    CHECK(observedDuringReset == outgoingDocument);
    CHECK(outgoingShapeAliveDuringReset);
    CHECK(Access::documentAddress(ownershipEditor) == stagedDocument);
    CHECK(Access::rootDefinitionDocumentAddress(ownershipEditor) ==
          stagedDocument);
    CHECK(&Access::documentAddress(ownershipEditor)->bodies().front() ==
          stagedBody);
    CHECK(Access::documentAddress(ownershipEditor)
              ->bodies()
              .front()
              .features()
              .front()
              .get() == stagedFeature);
    const auto* const committedShapeFeature =
        dynamic_cast<const solidar::ShapeFeature*>(stagedFeature);
    CHECK(committedShapeFeature);
    CHECK(committedShapeFeature->lastValidShape().get() == stagedShape);
    CHECK(committedShapeFeature->lastValidTopologyIndex().get() ==
          stagedTopology.get());
    CHECK(outgoingShape.expired());
  }

  // Persistence format is committed together with the Document. Rejected v4
  // staging preserves every live editor surface, while a successful v4 open
  // clears the old session and routes subsequent Save through the v4 writer.
  {
    const solidar::project::ProjectPersistenceState v3{
        solidar::project::ProjectPersistenceFormat::V3};
    const solidar::project::ProjectPersistenceState v4{
        solidar::project::ProjectPersistenceFormat::V4};
    solidar::MainWindow persistenceEditor(settings);
    CHECK(persistenceEditor.loadProject(pathA, &error));
    CHECK(Access::persistenceState(persistenceEditor) == v3);

    const QString validV4Path =
        directory.filePath(QStringLiteral("application-v4.solidar"));
    const auto v4Document = Access::documentCopy(persistenceEditor);
    CHECK(solidar::project::ProjectFile::saveDocument(
        validV4Path, v4Document, v4, &error));

    solidar::Document hard = v4Document;
    auto& hardBody = hard.addBody(910001, "hard v4 body");
    constexpr solidar::FeatureId hardSourceId = 910002;
    hardBody.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
        hardSourceId,
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
        "hard v4 source"));
    hardBody.addFeature(std::make_unique<solidar::FilletFeature>(
        910003, solidar::EdgeReference(hardBody.id(), hardSourceId, 0),
        1000.0, "hard v4 fillet"));
    const QString hardV4Path =
        directory.filePath(QStringLiteral("application-hard-v4.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        hardV4Path, hard, v4, &error));

    CHECK(Access::startTopFaceExtrude(persistenceEditor, 15.0));
    persistenceEditor.setWindowModified(true);
    const QString beforePath = persistenceEditor.windowFilePath();
    const auto beforeSelection = Access::selectionState(persistenceEditor);
    const auto beforeHistory = Access::historyStepCount(persistenceEditor);
    const auto beforeBodies = Access::bodyCount(persistenceEditor);
    CHECK(!persistenceEditor.loadProject(hardV4Path, &error));
    CHECK(!error.isEmpty());
    CHECK(persistenceEditor.windowFilePath() == beforePath);
    CHECK(Access::modified(persistenceEditor));
    CHECK(Access::persistenceState(persistenceEditor) == v3);
    CHECK(Access::historyStepCount(persistenceEditor) == beforeHistory);
    CHECK(Access::bodyCount(persistenceEditor) == beforeBodies);
    CHECK(Access::activeTool(persistenceEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::exactSelectionState(Access::selectionState(persistenceEditor),
                                      beforeSelection));

    CHECK(persistenceEditor.loadProject(validV4Path, &error));
    CHECK(Access::persistenceState(persistenceEditor) == v4);
    CHECK(Access::activeTool(persistenceEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(!Access::modified(persistenceEditor));
    CHECK(persistenceEditor.windowFilePath() == validV4Path);
    Access::saveProject(persistenceEditor);
    QFile savedV4(validV4Path);
    CHECK(savedV4.open(QIODevice::ReadOnly));
    CHECK(QJsonDocument::fromJson(savedV4.readAll())
              .object()
              .value(QStringLiteral("version"))
              .toInteger() == 4);

    solidar::Document imported = Access::documentCopy(persistenceEditor);
    imported.addBody("STEP-format-preservation sentinel");
    Access::applyImportedDocument(persistenceEditor, std::move(imported));
    CHECK(Access::persistenceState(persistenceEditor) == v4);
    CHECK(persistenceEditor.loadProject(pathA, &error));
    CHECK(Access::persistenceState(persistenceEditor) == v3);
  }

  // Standalone V3/V4 projects expose an explicit product conversion. The
  // application transaction retains the live model graph and every editor
  // surface while switching only persistence identity/path/savepoint state.
  {
    const solidar::project::ProjectPersistenceState v3{
        solidar::project::ProjectPersistenceFormat::V3};
    const solidar::project::ProjectPersistenceState v4{
        solidar::project::ProjectPersistenceFormat::V4};
    const solidar::project::ProjectPersistenceState v5{
        solidar::project::ProjectPersistenceFormat::V5};

    // A pathless standalone root is eligible, and the action is placed
    // immediately after Upgrade with its stable automation identity.
    solidar::MainWindow pathless(settings);
    auto* pathlessUpgrade = Access::upgradeProjectFormatAction(pathless);
    auto* pathlessConvert = Access::convertToProductDocumentAction(pathless);
    CHECK(pathlessUpgrade);
    CHECK(pathlessConvert);
    CHECK(pathlessConvert->objectName() ==
          QStringLiteral("convertToProductDocumentAction"));
    CHECK(pathlessConvert->text() ==
          QString::fromUtf8("Преобразовать в документ изделия…"));
    CHECK(pathlessConvert->isEnabled());
    CHECK(Access::productConversionIsEligible(pathless));
    auto* fileMenu = qobject_cast<QMenu*>(pathlessUpgrade->parent());
    CHECK(fileMenu);
    const auto fileActions = fileMenu->actions();
    CHECK(fileActions.indexOf(pathlessConvert) ==
          fileActions.indexOf(pathlessUpgrade) + 1);

    bool cancelledFactoryCalled = false;
    bool cancelledAttempted = true;
    QString cancelledSuggestion;
    const auto cancelled = Access::runProductConversionCommand(
        pathless,
        [&](const QString& suggestion) -> std::optional<QString> {
          cancelledSuggestion = suggestion;
          return std::nullopt;
        },
        [&](const QString&, solidar::DefinitionId) {
          cancelledFactoryCalled = true;
          return uiConversionRequest(Access::rootDefinitionId(pathless));
        },
        &cancelledAttempted);
    CHECK(cancelled.succeeded());
    CHECK(!cancelledAttempted);
    CHECK(!cancelledFactoryCalled);
    CHECK(cancelledSuggestion ==
          QString::fromUtf8("Документ изделия.solidar"));
    CHECK(Access::identity(pathless) == std::nullopt);
    CHECK(pathless.windowFilePath().isEmpty());

    // A modal chooser may synchronously close and delete its editor. The
    // command must return without touching the dead receiver or invoking the
    // request factory.
    auto* closingEditor = new solidar::MainWindow(settings);
    CHECK(closingEditor->loadProject(pathA, &error));
    QPointer<solidar::MainWindow> closingGuard(closingEditor);
    const QString closedWindowTarget =
        directory.filePath(QStringLiteral("closed-window-product.solidar"));
    bool closedWindowFactoryCalled = false;
    const auto closedWindowResult = Access::runProductConversionCommand(
        *closingEditor,
        [&](const QString&) -> std::optional<QString> {
          delete closingEditor;
          closingEditor = nullptr;
          CHECK(closingGuard.isNull());
          return closedWindowTarget;
        },
        [&](const QString&, solidar::DefinitionId root) {
          closedWindowFactoryCalled = true;
          return uiConversionRequest(root);
        });
    CHECK(closingGuard.isNull());
    CHECK(!closedWindowResult.succeeded());
    CHECK(closedWindowResult.code ==
          solidar::ProjectApplicationErrorCode::InvalidProject);
    CHECK(!closedWindowFactoryCalled);
    CHECK(!QFile::exists(closedWindowTarget));

    solidar::MainWindow multiRoot(settings);
    CHECK(Access::insertAdditionalDefinition(
        multiRoot,
        solidar::DefinitionId{
            Access::rootDefinitionId(multiRoot).value() + 1000}));
    CHECK(Access::definitionCount(multiRoot) == 2);
    CHECK(!Access::productConversionIsEligible(multiRoot));
    CHECK(!Access::convertToProductDocumentAction(multiRoot)->isEnabled());
    CHECK(!Access::saveProjectAsAction(multiRoot)->isEnabled());

    QFile sourceFile(pathA);
    CHECK(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray sourceBytesBefore = sourceFile.readAll();
    sourceFile.close();

    solidar::MainWindow editor(settings);
    CHECK(editor.loadProject(pathA, &error));
    auto* convertAction = Access::convertToProductDocumentAction(editor);
    CHECK(convertAction);
    CHECK(convertAction->isEnabled());
    CHECK(Access::persistenceState(editor) == v3);
    CHECK(Access::pushBodyMarker(editor, "conversion history one"));
    CHECK(Access::pushBodyMarker(editor, "conversion history two"));
    Access::undoAction(editor)->trigger();
    CHECK(Access::undoCount(editor) > 0);
    CHECK(Access::redoCount(editor) > 0);
    Access::selectBody(editor, Access::firstBodyId(editor));

    const auto* const documentBefore = Access::documentAddress(editor);
    const auto* const bodyBefore = Access::firstBodyAddress(editor);
    const auto* const featureBefore = Access::firstFeatureAddress(editor);
    const auto* const shapeBefore = Access::firstShapeAddress(editor);
    const auto shapeRevisionBefore = Access::firstShapeRevision(editor);
    const auto selectionBefore = Access::selectionState(editor);
    const auto undoBefore = Access::undoCount(editor);
    const auto redoBefore = Access::redoCount(editor);
    const auto revisionBefore = Access::currentHistoryRevision(editor);
    const auto previewGenerationBefore =
        Access::previewDocumentGeneration(editor);
    const bool dirtyBefore = Access::modified(editor);
    const auto cursorBefore = consumeModelIdCursors("before UI conversion");
    CHECK(documentBefore);
    CHECK(bodyBefore);
    CHECK(featureBefore);
    CHECK(shapeBefore);
    CHECK(shapeRevisionBefore != solidar::kInvalidShapeRevision);
    CHECK(dirtyBefore);

    const QString distinctWithoutExtension =
        directory.filePath(QString::fromUtf8("богатое-изделие-Ω"));
    const QString distinctTarget =
        distinctWithoutExtension + QStringLiteral(".solidar");
    QString suggestionSeen;
    bool factoryCalled = false;
    bool attempted = false;
    const auto converted = Access::runProductConversionCommand(
        editor,
        [&](const QString& suggestion) -> std::optional<QString> {
          suggestionSeen = suggestion;
          return distinctWithoutExtension;
        },
        [&](const QString& target, solidar::DefinitionId root) {
          factoryCalled = true;
          CHECK(target == distinctTarget);
          CHECK(root == Access::rootDefinitionId(editor));
          return uiConversionRequest(root);
        },
        &attempted);
    CHECK(converted.succeeded());
    CHECK(attempted);
    CHECK(factoryCalled);
    CHECK(suggestionSeen ==
          directory.filePath(QString::fromUtf8("a-изделие.solidar")));
    CHECK(QFile::exists(distinctTarget));

    const auto cursorAfter = consumeModelIdCursors("after UI conversion");
    CHECK(cursorAfter.body == cursorBefore.body + 1);
    CHECK(cursorAfter.sketch == cursorBefore.sketch + 1);
    CHECK(cursorAfter.feature == cursorBefore.feature + 1);
    CHECK(Access::documentAddress(editor) == documentBefore);
    CHECK(Access::rootDefinitionDocumentAddress(editor) == documentBefore);
    CHECK(Access::firstBodyAddress(editor) == bodyBefore);
    CHECK(Access::firstFeatureAddress(editor) == featureBefore);
    CHECK(Access::firstShapeAddress(editor) == shapeBefore);
    CHECK(Access::firstShapeRevision(editor) == shapeRevisionBefore);
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      selectionBefore));
    CHECK(Access::undoCount(editor) == undoBefore);
    CHECK(Access::redoCount(editor) == redoBefore);
    CHECK(Access::currentHistoryRevision(editor) == revisionBefore);
    CHECK(Access::previewDocumentGeneration(editor) ==
          previewGenerationBefore);
    CHECK(Access::persistenceState(editor) == v5);
    CHECK(editor.windowFilePath() == distinctTarget);
    CHECK(!Access::modified(editor));
    CHECK(Access::historyAtSavepoint(editor));
    CHECK(!convertAction->isEnabled());
    CHECK(!Access::upgradeProjectFormatAction(editor)->isEnabled());
    CHECK(Access::identity(editor).has_value());
    CHECK(Access::identity(editor)->projectId == kUiConversionProjectId);
    CHECK(Access::identity(editor)->metadata.nameUtf8 ==
          std::string("Изделие \xCE\xA9/\xE9\x9B\xAA"));
    CHECK(Access::identity(editor)->metadata.createdAtUtf8 ==
          "2026-10-23T20:15:30Z");

    CHECK(sourceFile.open(QIODevice::ReadOnly));
    CHECK(sourceFile.readAll() == sourceBytesBefore);
    sourceFile.close();
    auto convertedStage = solidar::ProjectApplicationService{}.stageOpen(
        distinctTarget, solidar::ProductV5OpenPolicy::Allow);
    CHECK(convertedStage.succeeded());
    CHECK(convertedStage.staged->productDocument().identity() ==
          Access::identity(editor));
    CHECK(convertedStage.staged->productDocument().rootDefinitionId() ==
          Access::rootDefinitionId(editor));

    // Repeating through the dialog-free seam is a strict no-op once the
    // product identity has been committed.
    const auto repeatIdentity = *Access::identity(editor);
    const auto repeatPath = editor.windowFilePath();
    const auto repeatRevision = Access::currentHistoryRevision(editor);
    const auto repeat = Access::convertProjectToPath(
        editor, directory.filePath(QStringLiteral("repeat-product.solidar")),
        uiConversionRequest(Access::rootDefinitionId(editor)));
    CHECK(!repeat.succeeded());
    CHECK(repeat.code == solidar::ProjectApplicationErrorCode::InvalidProject);
    CHECK(Access::identity(editor) == repeatIdentity);
    CHECK(editor.windowFilePath() == repeatPath);
    CHECK(Access::currentHistoryRevision(editor) == repeatRevision);
    CHECK(!QFile::exists(
        directory.filePath(QStringLiteral("repeat-product.solidar"))));

    // An actual V4 carrier is eligible too. Returning its current path from
    // the injected chooser models an explicit same-target confirmation.
    const QString sameTargetV4 =
        directory.filePath(QStringLiteral("same-target-v4.solidar"));
    auto v4Source = solidar::ProjectApplicationService{}.stageOpen(pathA);
    CHECK(v4Source.succeeded());
    CHECK(solidar::project::ProjectFile::saveDocument(
        sameTargetV4, v4Source.staged->rootDocument(), v4, &error));
    solidar::MainWindow v4Editor(settings);
    CHECK(v4Editor.loadProject(sameTargetV4, &error));
    CHECK(Access::persistenceState(v4Editor) == v4);
    CHECK(Access::convertToProductDocumentAction(v4Editor)->isEnabled());
    bool sameFactoryCalled = false;
    const auto sameTargetConverted = Access::runProductConversionCommand(
        v4Editor,
        [&](const QString&) -> std::optional<QString> {
          return sameTargetV4;
        },
        [&](const QString& target, solidar::DefinitionId root) {
          CHECK(target == sameTargetV4);
          sameFactoryCalled = true;
          return uiConversionRequest(
              root, QFileInfo(target).completeBaseName(),
              QStringLiteral("2026-10-23T21:00:00Z"));
        });
    CHECK(sameTargetConverted.succeeded());
    CHECK(sameFactoryCalled);
    CHECK(Access::persistenceState(v4Editor) == v5);
    CHECK(v4Editor.windowFilePath() == sameTargetV4);
    auto sameTargetStage = solidar::ProjectApplicationService{}.stageOpen(
        sameTargetV4, solidar::ProductV5OpenPolicy::Allow);
    CHECK(sameTargetStage.succeeded());
    CHECK(sameTargetStage.staged->productDocument()
              .identity()
              ->metadata.nameUtf8 == "same-target-v4");

    // A failed publish is state-exact and retryable.
    solidar::MainWindow retryEditor(settings);
    CHECK(retryEditor.loadProject(pathA, &error));
    retryEditor.setWindowModified(true);
    Access::selectBody(retryEditor, Access::firstBodyId(retryEditor));
    const auto* const retryDocument = Access::documentAddress(retryEditor);
    const auto* const retryShape = Access::firstShapeAddress(retryEditor);
    const auto retrySelection = Access::selectionState(retryEditor);
    const auto retryRevision = Access::currentHistoryRevision(retryEditor);
    const auto retryPreview = Access::previewDocumentGeneration(retryEditor);
    const auto retryUndo = Access::undoCount(retryEditor);
    const auto retryRedo = Access::redoCount(retryEditor);
    const QString missingTarget =
        directory.filePath(QStringLiteral("missing/product.solidar"));
    const auto failed = Access::convertProjectToPath(
        retryEditor, missingTarget,
        uiConversionRequest(Access::rootDefinitionId(retryEditor)));
    CHECK(!failed.succeeded());
    CHECK(failed.code == solidar::ProjectApplicationErrorCode::SaveFailed);
    CHECK(!QFile::exists(missingTarget));
    CHECK(Access::documentAddress(retryEditor) == retryDocument);
    CHECK(Access::firstShapeAddress(retryEditor) == retryShape);
    CHECK(Access::identity(retryEditor) == std::nullopt);
    CHECK(Access::persistenceState(retryEditor) == v3);
    CHECK(retryEditor.windowFilePath() == pathA);
    CHECK(Access::modified(retryEditor));
    CHECK(Access::undoCount(retryEditor) == retryUndo);
    CHECK(Access::redoCount(retryEditor) == retryRedo);
    CHECK(Access::currentHistoryRevision(retryEditor) == retryRevision);
    CHECK(Access::previewDocumentGeneration(retryEditor) == retryPreview);
    CHECK(Access::exactSelectionState(Access::selectionState(retryEditor),
                                      retrySelection));
    const QString retryTarget =
        directory.filePath(QStringLiteral("retry-product.solidar"));
    CHECK(Access::convertProjectToPath(
              retryEditor, retryTarget,
              uiConversionRequest(Access::rootDefinitionId(retryEditor)))
              .succeeded());
    CHECK(Access::persistenceState(retryEditor) == v5);
    CHECK(QFile::exists(retryTarget));

    // Mutating the model from the chooser invalidates the outer snapshot and
    // prevents request generation and writes.
    solidar::MainWindow reentrantEditor(settings);
    CHECK(reentrantEditor.loadProject(pathA, &error));
    const QString staleTarget =
        directory.filePath(QStringLiteral("stale-outer-product.solidar"));
    bool staleFactoryCalled = false;
    const auto staleResult = Access::runProductConversionCommand(
        reentrantEditor,
        [&](const QString&) -> std::optional<QString> {
          CHECK(Access::pushBodyMarker(reentrantEditor,
                                       "reentrant chooser mutation"));
          return staleTarget;
        },
        [&](const QString&, solidar::DefinitionId root) {
          staleFactoryCalled = true;
          return uiConversionRequest(root);
        });
    CHECK(!staleResult.succeeded());
    CHECK(staleResult.code ==
          solidar::ProjectApplicationErrorCode::InvalidProject);
    CHECK(!staleFactoryCalled);
    CHECK(!QFile::exists(staleTarget));
    CHECK(Access::identity(reentrantEditor) == std::nullopt);
    CHECK(Access::persistenceState(reentrantEditor) == v3);
    CHECK(Access::statusMessage(reentrantEditor).contains(
        QString::fromUtf8("изменилось")));

    // A nested successful conversion wins. The stale outer command neither
    // allocates a request nor publishes its selected target.
    solidar::MainWindow nestedEditor(settings);
    CHECK(nestedEditor.loadProject(pathA, &error));
    const QString nestedTarget =
        directory.filePath(QStringLiteral("nested-product.solidar"));
    const QString outerTarget =
        directory.filePath(QStringLiteral("outer-product.solidar"));
    bool outerFactoryCalled = false;
    const auto outerResult = Access::runProductConversionCommand(
        nestedEditor,
        [&](const QString&) -> std::optional<QString> {
          CHECK(Access::convertProjectToPath(
                    nestedEditor, nestedTarget,
                    uiConversionRequest(
                        Access::rootDefinitionId(nestedEditor),
                        QStringLiteral("nested winner")))
                    .succeeded());
          return outerTarget;
        },
        [&](const QString&, solidar::DefinitionId root) {
          outerFactoryCalled = true;
          return uiConversionRequest(root, QStringLiteral("outer loser"));
        });
    CHECK(!outerResult.succeeded());
    CHECK(!outerFactoryCalled);
    CHECK(QFile::exists(nestedTarget));
    CHECK(!QFile::exists(outerTarget));
    CHECK(nestedEditor.windowFilePath() == nestedTarget);
    CHECK(Access::persistenceState(nestedEditor) == v5);
    CHECK(Access::identity(nestedEditor)->metadata.nameUtf8 ==
          "nested winner");

    // Active transient work is rejected before the chooser and remains
    // active; conversion never auto-applies or cancels it.
    solidar::MainWindow activeToolEditor(settings);
    CHECK(activeToolEditor.loadProject(pathA, &error));
    CHECK(Access::startTopFaceExtrude(activeToolEditor, 15.0));
    bool activeChooserCalled = false;
    bool activeFactoryCalled = false;
    const QString activeTarget =
        directory.filePath(QStringLiteral("active-tool-product.solidar"));
    const auto activeRejected = Access::runProductConversionCommand(
        activeToolEditor,
        [&](const QString&) -> std::optional<QString> {
          activeChooserCalled = true;
          return activeTarget;
        },
        [&](const QString&, solidar::DefinitionId root) {
          activeFactoryCalled = true;
          return uiConversionRequest(root);
        });
    CHECK(!activeRejected.succeeded());
    CHECK(!activeChooserCalled);
    CHECK(!activeFactoryCalled);
    CHECK(!QFile::exists(activeTarget));
    CHECK(Access::activeTool(activeToolEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::viewportHasToolPreview(activeToolEditor));
    CHECK(Access::convertToProductDocumentAction(activeToolEditor)
              ->isEnabled());
    Access::cancelFaceExtrude(activeToolEditor);
  }
}

void runA6b2EditorPersistenceScenario(
    const QTemporaryDir& directory, solidar::AppSettings& settings,
    const QString& pathA) {
  using Access = solidar::MainWindowUndoTestAccess;

  // A6b2 opens V7 into its embedded last-valid cache, owns link diagnostics
  // in the application layer, and exposes explicit linked lifecycle commands.
  // Reload updates one shared definition while both occurrences and the
  // occurrence selection survive; Undo/Redo remain cold after source removal.
  {
    const QString linkedRoot =
        directory.filePath(QStringLiteral("a6b2-ui"));
    CHECK(QDir().mkpath(linkedRoot + QStringLiteral("/parts")));
    solidar::MainWindow firstSaveEditor(settings);
    CHECK(Access::createNewAssembly(firstSaveEditor));
    CHECK(!Access::productDocumentAddress(firstSaveEditor)->identity());
    const QString firstAssemblyPath =
        linkedRoot + QStringLiteral("/first-assembly.solidar");
    bool firstSaveAttempted = false;
    const auto firstAssemblySave = Access::runAssemblySaveAsCommand(
        firstSaveEditor,
        [&](const QString&) -> std::optional<QString> {
          return firstAssemblyPath;
        },
        &firstSaveAttempted);
    CHECK(firstAssemblySave.succeeded());
    CHECK(firstSaveAttempted);
    CHECK(firstSaveEditor.windowFilePath() == firstAssemblyPath);
    CHECK(Access::productDocumentAddress(firstSaveEditor)->identity());
    CHECK(!firstSaveEditor.isWindowModified());
    const auto stagedFirstAssembly =
        solidar::ProjectApplicationService{}.stageOpenProduct(
            firstAssemblyPath);
    CHECK(stagedFirstAssembly.succeeded());
    CHECK(stagedFirstAssembly.staged->persistenceState->format ==
          solidar::project::ProjectPersistenceFormat::V7);

    const QString linkedSourcePath =
        linkedRoot + QStringLiteral("/parts/source.solidar");
    auto linkedSource = uiLinkedSource(77.0);
    const auto linkedSourceEncoded =
        solidar::project::encodeProductEnvelopeV5Root(
            *linkedSource, kUiLinkedSourceProjectId,
            {QStringLiteral("Linked source"),
             QStringLiteral("2026-10-27T00:00:00Z")});
    CHECK(linkedSourceEncoded.succeeded());
    CHECK(writeTestBytes(linkedSourcePath,
                         linkedSourceEncoded.payload->bytes));
    const solidar::LinkedPartBinding linkedBinding{
        {kUiLinkedSourceProjectId, kUiLinkedSourceDefinition},
        "parts/source.solidar",
        solidar::project::linkedSourceSha256(
            QByteArrayView(linkedSourceEncoded.payload->bytes))};
    auto linkedAssembly = uiLinkedAssembly(linkedBinding);
    const auto linkedAssemblyEncoded =
        solidar::project::encodeProductEnvelopeV7Root(
            *linkedAssembly, kUiLinkedAssemblyProjectId,
            {QStringLiteral("Linked assembly"),
             QStringLiteral("2026-10-27T00:00:00Z")});
    CHECK(linkedAssemblyEncoded.succeeded());
    const QString linkedAssemblyPath =
        linkedRoot + QStringLiteral("/assembly.solidar");
    CHECK(writeTestBytes(linkedAssemblyPath,
                         linkedAssemblyEncoded.payload->bytes));

    solidar::MainWindow linkedEditor(settings);
    QString linkedError;
    CHECK(linkedEditor.loadProject(linkedAssemblyPath, &linkedError));
    CHECK(linkedError.isEmpty());
    CHECK(Access::persistenceState(linkedEditor).format ==
          solidar::project::ProjectPersistenceFormat::V7);
    CHECK(Access::linkedRuntimeStatus(linkedEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Exact);
    CHECK(Access::linkedBox(linkedEditor, kUiLinkedDefinition).widthMm ==
          60.0);
    CHECK(!linkedEditor.isWindowModified());
    CHECK(Access::saveAction(linkedEditor)->isEnabled());
    CHECK(Access::saveProjectAsAction(linkedEditor)->isEnabled());
    CHECK(!Access::exportStepAction(linkedEditor)->isEnabled());
    CHECK(!Access::exportStlAction(linkedEditor)->isEnabled());
    CHECK(Access::exportStepAction(linkedEditor)->toolTip().contains(
        QStringLiteral("A9")));
    CHECK(Access::linkedReloadAction(linkedEditor)->text() ==
          QString::fromUtf8("Обновить связанную деталь"));
    CHECK(Access::linkedRepairAction(linkedEditor)->text() ==
          QString::fromUtf8("Исправить ссылку…"));

    // The injectable modal boundary proves cancel, success, alias failure and
    // a re-entrant project switch before any publication or identity commit.
    solidar::MainWindow saveAsEditor(settings);
    CHECK(saveAsEditor.loadProject(linkedAssemblyPath, &linkedError));
    const auto saveAsIdentityBefore =
        Access::productDocumentAddress(saveAsEditor)->identity();
    const QByteArray sourceBeforeSaveAs = readTestBytes(linkedSourcePath);
    const QDateTime sourceMtimeBeforeSaveAs =
        QFileInfo(linkedSourcePath).lastModified();
    bool saveAsAttempted = true;
    const auto cancelledSaveAs = Access::runAssemblySaveAsCommand(
        saveAsEditor,
        [](const QString&) -> std::optional<QString> { return std::nullopt; },
        &saveAsAttempted);
    CHECK(cancelledSaveAs.succeeded());
    CHECK(!saveAsAttempted);
    CHECK(saveAsEditor.windowFilePath() == linkedAssemblyPath);
    CHECK(Access::productDocumentAddress(saveAsEditor)->identity() ==
          saveAsIdentityBefore);

    const QString linkedCopyPath =
        linkedRoot + QStringLiteral("/assembly-copy.solidar");
    const auto successfulSaveAs = Access::runAssemblySaveAsCommand(
        saveAsEditor,
        [&](const QString&) -> std::optional<QString> {
          return linkedCopyPath;
        },
        &saveAsAttempted);
    CHECK(successfulSaveAs.succeeded());
    CHECK(saveAsAttempted);
    CHECK(saveAsEditor.windowFilePath() == linkedCopyPath);
    CHECK(Access::productDocumentAddress(saveAsEditor)->identity());
    CHECK(Access::productDocumentAddress(saveAsEditor)
              ->identity()
              ->projectId != saveAsIdentityBefore->projectId);
    CHECK(!saveAsEditor.isWindowModified());
    CHECK(readTestBytes(linkedSourcePath) == sourceBeforeSaveAs);
    CHECK(QFileInfo(linkedSourcePath).lastModified() ==
          sourceMtimeBeforeSaveAs);

    const auto aliasPathBefore = saveAsEditor.windowFilePath();
    const auto aliasIdentityBefore =
        Access::productDocumentAddress(saveAsEditor)->identity();
    const auto failedAliasSaveAs = Access::runAssemblySaveAsCommand(
        saveAsEditor,
        [&](const QString&) -> std::optional<QString> {
          return linkedSourcePath;
        },
        &saveAsAttempted);
    CHECK(!failedAliasSaveAs.succeeded());
    CHECK(saveAsAttempted);
    CHECK(saveAsEditor.windowFilePath() == aliasPathBefore);
    CHECK(Access::productDocumentAddress(saveAsEditor)->identity() ==
          aliasIdentityBefore);
    CHECK(readTestBytes(linkedSourcePath) == sourceBeforeSaveAs);

    solidar::MainWindow reentrantSaveAsEditor(settings);
    CHECK(reentrantSaveAsEditor.loadProject(linkedAssemblyPath,
                                            &linkedError));
    const QString rejectedReentrantTarget =
        linkedRoot + QStringLiteral("/reentrant-copy.solidar");
    const auto rejectedReentrantSaveAs =
        Access::runAssemblySaveAsCommand(
            reentrantSaveAsEditor,
            [&](const QString&) -> std::optional<QString> {
              QString switchError;
              CHECK(reentrantSaveAsEditor.loadProject(pathA, &switchError));
              CHECK(switchError.isEmpty());
              return rejectedReentrantTarget;
            },
            &saveAsAttempted);
    CHECK(!rejectedReentrantSaveAs.succeeded());
    CHECK(!saveAsAttempted);
    CHECK(reentrantSaveAsEditor.windowFilePath() == pathA);
    CHECK(Access::projectKind(reentrantSaveAsEditor) ==
          solidar::EditorProjectKind::Part);
    CHECK(!QFile::exists(rejectedReentrantTarget));

    auto revisedLinkedSource = uiLinkedSource(88.0);
    const auto revisedLinkedEncoded =
        solidar::project::encodeProductEnvelopeV5Root(
            *revisedLinkedSource, kUiLinkedSourceProjectId,
            {QStringLiteral("Linked source"),
             QStringLiteral("2026-10-27T00:00:00Z")});
    CHECK(revisedLinkedEncoded.succeeded());

    const QString repairedExactPath =
        linkedRoot + QStringLiteral("/parts/relocated.solidar");
    const QString repairedOutdatedPath =
        linkedRoot + QStringLiteral("/parts/newer.solidar");
    const QString repairedWrongPath =
        linkedRoot + QStringLiteral("/parts/wrong.solidar");
    CHECK(writeTestBytes(repairedExactPath,
                         linkedSourceEncoded.payload->bytes));
    CHECK(writeTestBytes(repairedOutdatedPath,
                         revisedLinkedEncoded.payload->bytes));
    QJsonObject wrongIdentityRoot =
        QJsonDocument::fromJson(linkedSourceEncoded.payload->bytes).object();
    wrongIdentityRoot[QStringLiteral("projectId")] =
        QStringLiteral("223e4567-e89b-42d3-a456-426614174099");
    CHECK(writeTestBytes(
        repairedWrongPath,
        QJsonDocument(wrongIdentityRoot).toJson(QJsonDocument::Compact)));

    // A6c1a end-to-end nested linked lifecycle: the terminal leaf is selected
    // by its full prefix while Reload/Repair remain definition-wide. Failure
    // seams are exact no-ops and cold Undo/Redo do not reopen removed files.
    const QString nestedAssemblyPath =
        linkedRoot + QStringLiteral("/nested-assembly.solidar");
    const auto nestedAuthored = uiNestedLinkedAssembly(linkedBinding);
    const auto nestedV8Encoded =
        solidar::project::encodeProductEnvelopeV8Root(
            *nestedAuthored, kUiLinkedAssemblyProjectId,
            {QStringLiteral("Nested linked assembly"),
             QStringLiteral("2026-10-29T00:00:00Z")});
    CHECK(nestedV8Encoded.succeeded());
    CHECK(writeTestBytes(nestedAssemblyPath,
                         nestedV8Encoded.payload->bytes));
    const auto nestedPath = solidar::OccurrencePath::create(
        {solidar::OccurrenceId{701}, solidar::OccurrenceId{7}});
    CHECK(nestedPath);
    CHECK(writeTestBytes(linkedSourcePath,
                         linkedSourceEncoded.payload->bytes));
    {
      solidar::MainWindow nestedReloadEditor(settings);
      QString nestedOpenError;
      CHECK(nestedReloadEditor.loadProject(nestedAssemblyPath,
                                           &nestedOpenError));
      CHECK(nestedOpenError.isEmpty());
      CHECK(Access::persistenceState(nestedReloadEditor).format ==
            solidar::project::ProjectPersistenceFormat::V8);
      CHECK(Access::linkedRuntimeStatus(nestedReloadEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Exact);
      const QModelIndex nestedLeaf = Access::assemblyOccurrenceIndexForPath(
          nestedReloadEditor, *nestedPath.path);
      CHECK(nestedLeaf.isValid());
      Access::selectAssemblyTreeIndex(nestedReloadEditor, nestedLeaf);
      CHECK(Access::viewportAssemblySelection(nestedReloadEditor)
                ->occurrence.path == *nestedPath.path);
      CHECK(Access::linkedReloadAction(nestedReloadEditor)->isEnabled());
      CHECK(writeTestBytes(linkedSourcePath,
                           revisedLinkedEncoded.payload->bytes));

      const auto* reloadFailureSession =
          Access::assemblySessionAddress(nestedReloadEditor);
      const auto reloadFailurePublication =
          reloadFailureSession->publicationHandle();
      const auto reloadFailureBinding =
          Access::linkedBinding(nestedReloadEditor, kUiLinkedDefinition);
      const auto reloadFailureRevision =
          Access::assemblyHistoryRevision(nestedReloadEditor);
      Access::injectAssemblyPreparationFailure(
          nestedReloadEditor,
          solidar::AssemblyViewportSessionFailurePoint::Resource);
      Access::reloadSelectedLinkedPart(nestedReloadEditor);
      Access::injectAssemblyPreparationFailure(
          nestedReloadEditor,
          solidar::AssemblyViewportSessionFailurePoint::None);
      CHECK(Access::assemblyHistorySize(nestedReloadEditor) == 0);
      CHECK(Access::assemblyHistoryCursor(nestedReloadEditor) == 0);
      CHECK(Access::assemblyHistoryRevision(nestedReloadEditor) ==
            reloadFailureRevision);
      CHECK(Access::linkedBinding(nestedReloadEditor,
                                  kUiLinkedDefinition) ==
            reloadFailureBinding);
      CHECK(Access::linkedBox(nestedReloadEditor,
                              kUiLinkedDefinition).widthMm == 60.0);
      CHECK(Access::linkedRuntimeStatus(nestedReloadEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Exact);
      CHECK(Access::assemblySessionAddress(nestedReloadEditor) ==
            reloadFailureSession);
      CHECK(Access::assemblySessionAddress(nestedReloadEditor)
                ->publicationHandle()
                .isSameCurrentPublication(reloadFailurePublication));
      CHECK(Access::viewportAssemblySelection(nestedReloadEditor)
                ->occurrence.path == *nestedPath.path);

      Access::reloadSelectedLinkedPart(nestedReloadEditor);
      CHECK(Access::assemblyHistorySize(nestedReloadEditor) == 1);
      CHECK(Access::assemblyHistoryCursor(nestedReloadEditor) == 1);
      CHECK(Access::linkedBox(nestedReloadEditor,
                              kUiLinkedDefinition).widthMm == 88.0);
      CHECK(Access::linkedRuntimeStatus(nestedReloadEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Exact);
      CHECK(Access::viewportAssemblySelection(nestedReloadEditor)
                ->occurrence.path == *nestedPath.path);
      CHECK(QFile::remove(linkedSourcePath));
      Access::undoAssembly(nestedReloadEditor);
      CHECK(Access::linkedBox(nestedReloadEditor,
                              kUiLinkedDefinition).widthMm == 60.0);
      CHECK(Access::linkedRuntimeStatus(nestedReloadEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Unchecked);
      CHECK(Access::viewportAssemblySelection(nestedReloadEditor)
                ->occurrence.path == *nestedPath.path);
      Access::redoAssembly(nestedReloadEditor);
      CHECK(Access::linkedBox(nestedReloadEditor,
                              kUiLinkedDefinition).widthMm == 88.0);
      CHECK(Access::linkedRuntimeStatus(nestedReloadEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Unchecked);
      CHECK(Access::viewportAssemblySelection(nestedReloadEditor)
                ->occurrence.path == *nestedPath.path);
    }
    CHECK(writeTestBytes(linkedSourcePath,
                         linkedSourceEncoded.payload->bytes));
    {
      solidar::MainWindow nestedRepairEditor(settings);
      QString nestedOpenError;
      CHECK(nestedRepairEditor.loadProject(nestedAssemblyPath,
                                           &nestedOpenError));
      CHECK(nestedOpenError.isEmpty());
      CHECK(Access::persistenceState(nestedRepairEditor).format ==
            solidar::project::ProjectPersistenceFormat::V8);
      CHECK(Access::linkedRuntimeStatus(nestedRepairEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Exact);
      const QModelIndex nestedLeaf = Access::assemblyOccurrenceIndexForPath(
          nestedRepairEditor, *nestedPath.path);
      CHECK(nestedLeaf.isValid());
      Access::selectAssemblyTreeIndex(nestedRepairEditor, nestedLeaf);
      CHECK(Access::linkedRepairAction(nestedRepairEditor)->isEnabled());
      const auto* repairFailureSession =
          Access::assemblySessionAddress(nestedRepairEditor);
      const auto repairFailurePublication =
          repairFailureSession->publicationHandle();
      const auto repairFailureBinding =
          Access::linkedBinding(nestedRepairEditor, kUiLinkedDefinition);
      const auto repairFailureRevision =
          Access::assemblyHistoryRevision(nestedRepairEditor);
      bool nestedRepairAttempted = false;
      Access::injectAssemblyTreeResourceFailure(nestedRepairEditor, true);
      CHECK(!Access::runLinkedPartRepairCommand(
          nestedRepairEditor,
          [&](const QString&) -> std::optional<QString> {
            return repairedOutdatedPath;
          },
          &nestedRepairAttempted));
      Access::injectAssemblyTreeResourceFailure(nestedRepairEditor, false);
      CHECK(nestedRepairAttempted);
      CHECK(Access::assemblyHistorySize(nestedRepairEditor) == 0);
      CHECK(Access::assemblyHistoryCursor(nestedRepairEditor) == 0);
      CHECK(Access::assemblyHistoryRevision(nestedRepairEditor) ==
            repairFailureRevision);
      CHECK(Access::linkedBinding(nestedRepairEditor,
                                  kUiLinkedDefinition) ==
            repairFailureBinding);
      CHECK(Access::linkedRuntimeStatus(nestedRepairEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Exact);
      CHECK(Access::assemblySessionAddress(nestedRepairEditor) ==
            repairFailureSession);
      CHECK(Access::assemblySessionAddress(nestedRepairEditor)
                ->publicationHandle()
                .isSameCurrentPublication(repairFailurePublication));
      CHECK(Access::viewportAssemblySelection(nestedRepairEditor)
                ->occurrence.path == *nestedPath.path);

      CHECK(Access::runLinkedPartRepairCommand(
          nestedRepairEditor,
          [&](const QString&) -> std::optional<QString> {
            bool nestedSaveAttempted = true;
            const auto nestedSave = Access::runAssemblySaveAsCommand(
                nestedRepairEditor,
                [&](const QString&) -> std::optional<QString> {
                  return linkedRoot +
                         QStringLiteral("/repair-reentrant-copy.solidar");
                },
                &nestedSaveAttempted);
            CHECK(!nestedSave.succeeded());
            CHECK(!nestedSaveAttempted);
            return repairedOutdatedPath;
          },
          &nestedRepairAttempted));
      CHECK(Access::assemblyHistorySize(nestedRepairEditor) == 1);
      CHECK(Access::linkedBinding(nestedRepairEditor, kUiLinkedDefinition)
                ->relativePathHintUtf8 == "parts/newer.solidar");
      CHECK(Access::linkedRuntimeStatus(nestedRepairEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Outdated);
      CHECK(Access::viewportAssemblySelection(nestedRepairEditor)
                ->occurrence.path == *nestedPath.path);
      CHECK(QFile::remove(repairedOutdatedPath));
      Access::undoAssembly(nestedRepairEditor);
      CHECK(Access::linkedBinding(nestedRepairEditor, kUiLinkedDefinition)
                ->relativePathHintUtf8 == "parts/source.solidar");
      CHECK(Access::linkedRuntimeStatus(nestedRepairEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Unchecked);
      CHECK(Access::viewportAssemblySelection(nestedRepairEditor)
                ->occurrence.path == *nestedPath.path);
      Access::redoAssembly(nestedRepairEditor);
      CHECK(Access::linkedBinding(nestedRepairEditor, kUiLinkedDefinition)
                ->relativePathHintUtf8 == "parts/newer.solidar");
      CHECK(Access::linkedRuntimeStatus(nestedRepairEditor,
                                        kUiLinkedDefinition) ==
            solidar::LinkedPartRuntimeStatus::Unchecked);
      CHECK(Access::viewportAssemblySelection(nestedRepairEditor)
                ->occurrence.path == *nestedPath.path);
    }
    CHECK(writeTestBytes(repairedOutdatedPath,
                         revisedLinkedEncoded.payload->bytes));

    // Actual editor-routed V8 Save As preserves V8 even with a redo tail and
    // does not touch any linked source. The cold Redo remains usable after the
    // identity fork and retains the full nested selection path.
    {
      CHECK(writeTestBytes(linkedSourcePath,
                           linkedSourceEncoded.payload->bytes));
      solidar::MainWindow v8SaveAsEditor(settings);
      QString v8OpenError;
      CHECK(v8SaveAsEditor.loadProject(nestedAssemblyPath, &v8OpenError));
      CHECK(v8OpenError.isEmpty());
      const QModelIndex nestedLeaf = Access::assemblyOccurrenceIndexForPath(
          v8SaveAsEditor, *nestedPath.path);
      CHECK(nestedLeaf.isValid());
      Access::selectAssemblyTreeIndex(v8SaveAsEditor, nestedLeaf);
      CHECK(Access::assemblyIsolationEnabled(v8SaveAsEditor));
      Access::triggerAssemblyIsolation(v8SaveAsEditor);
      CHECK(Access::assemblyIsolationActive(v8SaveAsEditor));
      bool isolatedSaveAttempted = true;
      const auto isolatedSave = Access::runAssemblySaveAsCommand(
          v8SaveAsEditor,
          [&](const QString&) -> std::optional<QString> {
            return linkedRoot +
                   QStringLiteral("/isolated-copy-v8.solidar");
          },
          &isolatedSaveAttempted);
      CHECK(!isolatedSave.succeeded());
      CHECK(!isolatedSaveAttempted);
      Access::triggerAssemblyIsolation(v8SaveAsEditor);
      CHECK(!Access::assemblyIsolationActive(v8SaveAsEditor));
      bool repairAttempted = false;
      CHECK(Access::runLinkedPartRepairCommand(
          v8SaveAsEditor,
          [&](const QString&) -> std::optional<QString> {
            return repairedExactPath;
          },
          &repairAttempted));
      CHECK(repairAttempted);
      Access::undoAssembly(v8SaveAsEditor);
      CHECK(Access::assemblyHistorySize(v8SaveAsEditor) == 1);
      CHECK(Access::assemblyHistoryCursor(v8SaveAsEditor) == 0);
      const QByteArray linkedBytesBefore = readTestBytes(linkedSourcePath);
      const QDateTime linkedMtimeBefore =
          QFileInfo(linkedSourcePath).lastModified();
      const QString v8CopyPath =
          linkedRoot + QStringLiteral("/nested-copy-v8.solidar");
      bool v8SaveAttempted = false;
      const auto v8Saved = Access::runAssemblySaveAsCommand(
          v8SaveAsEditor,
          [&](const QString&) -> std::optional<QString> {
            bool reentrantRepairAttempted = true;
            CHECK(!Access::runLinkedPartRepairCommand(
                v8SaveAsEditor,
                [&](const QString&) -> std::optional<QString> {
                  return repairedExactPath;
                },
                &reentrantRepairAttempted));
            CHECK(!reentrantRepairAttempted);
            return v8CopyPath;
          },
          &v8SaveAttempted);
      CHECK(v8Saved.succeeded());
      CHECK(v8SaveAttempted);
      CHECK(v8SaveAsEditor.windowFilePath() == v8CopyPath);
      CHECK(Access::persistenceState(v8SaveAsEditor).format ==
            solidar::project::ProjectPersistenceFormat::V8);
      CHECK(Access::assemblyHistorySize(v8SaveAsEditor) == 1);
      CHECK(Access::assemblyHistoryCursor(v8SaveAsEditor) == 0);
      CHECK(readTestBytes(linkedSourcePath) == linkedBytesBefore);
      CHECK(QFileInfo(linkedSourcePath).lastModified() == linkedMtimeBefore);
      const auto stagedV8Copy =
          solidar::ProjectApplicationService{}.stageOpenProduct(v8CopyPath);
      CHECK(stagedV8Copy.succeeded());
      CHECK(stagedV8Copy.staged->persistenceState->format ==
            solidar::project::ProjectPersistenceFormat::V8);
      Access::redoAssembly(v8SaveAsEditor);
      CHECK(Access::linkedBinding(v8SaveAsEditor, kUiLinkedDefinition)
                ->relativePathHintUtf8 == "parts/relocated.solidar");
      CHECK(Access::viewportAssemblySelection(v8SaveAsEditor)
                ->occurrence.path == *nestedPath.path);
    }

    solidar::MainWindow repairEditor(settings);
    CHECK(repairEditor.loadProject(linkedAssemblyPath, &linkedError));
    Access::selectAssemblyTreeIndex(
        repairEditor, Access::firstAssemblyOccurrenceIndex(repairEditor));
    const auto repairBindingBefore =
        Access::linkedBinding(repairEditor, kUiLinkedDefinition);
    const auto repairStateBefore =
        Access::definitionStateId(repairEditor, kUiLinkedDefinition);
    const auto repairHistoryBefore =
        Access::assemblyHistoryRevision(repairEditor);
    bool repairAttempted = true;
    CHECK(!Access::runLinkedPartRepairCommand(
        repairEditor,
        [](const QString&) -> std::optional<QString> {
          return std::nullopt;
        },
        &repairAttempted));
    CHECK(!repairAttempted);
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition) ==
          repairBindingBefore);
    CHECK(Access::definitionStateId(repairEditor, kUiLinkedDefinition) ==
          repairStateBefore);
    CHECK(Access::assemblyHistoryRevision(repairEditor) ==
          repairHistoryBefore);

    const QString outsideRepairPath =
        directory.filePath(QStringLiteral("outside-repair.solidar"));
    CHECK(!Access::runLinkedPartRepairCommand(
        repairEditor,
        [&](const QString&) -> std::optional<QString> {
          return outsideRepairPath;
        },
        &repairAttempted));
    CHECK(!repairAttempted);
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition) ==
          repairBindingBefore);
    CHECK(!Access::runLinkedPartRepairCommand(
        repairEditor,
        [&](const QString&) -> std::optional<QString> {
          return repairedWrongPath;
        },
        &repairAttempted));
    CHECK(repairAttempted);
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition) ==
          repairBindingBefore);
    CHECK(Access::assemblyHistoryRevision(repairEditor) ==
          repairHistoryBefore);

    CHECK(Access::runLinkedPartRepairCommand(
        repairEditor,
        [&](const QString&) -> std::optional<QString> {
          return repairedExactPath;
        },
        &repairAttempted));
    CHECK(repairAttempted);
    CHECK(Access::linkedRuntimeStatus(repairEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Exact);
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition)
              ->relativePathHintUtf8 == "parts/relocated.solidar");
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition)
              ->acceptedHash == repairBindingBefore->acceptedHash);
    CHECK(Access::linkedBox(repairEditor, kUiLinkedDefinition).widthMm ==
          60.0);
    CHECK(Access::runLinkedPartRepairCommand(
        repairEditor,
        [&](const QString&) -> std::optional<QString> {
          return repairedOutdatedPath;
        },
        &repairAttempted));
    CHECK(repairAttempted);
    CHECK(Access::linkedRuntimeStatus(repairEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Outdated);
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition)
              ->relativePathHintUtf8 == "parts/newer.solidar");
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition)
              ->acceptedHash == repairBindingBefore->acceptedHash);
    CHECK(Access::linkedBox(repairEditor, kUiLinkedDefinition).widthMm ==
          60.0);
    CHECK(Access::assemblyHistorySize(repairEditor) == 2);
    CHECK(Access::assemblyDefinitionIndex(repairEditor,
                                          kUiLinkedDefinition)
              .data()
              .toString()
              .contains(QString::fromUtf8("источник изменён")));
    const auto repairSelectionBeforeColdHistory =
        Access::viewportAssemblySelection(repairEditor);
    CHECK(repairSelectionBeforeColdHistory);
    CHECK(QFile::remove(repairedExactPath));
    CHECK(QFile::remove(repairedOutdatedPath));
    Access::undoAssembly(repairEditor);
    CHECK(Access::assemblyHistoryCursor(repairEditor) == 1);
    CHECK(Access::linkedRuntimeStatus(repairEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Unchecked);
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition)
              ->relativePathHintUtf8 == "parts/relocated.solidar");
    CHECK(Access::viewportAssemblySelection(repairEditor)
              ->occurrence.path ==
          repairSelectionBeforeColdHistory->occurrence.path);
    Access::redoAssembly(repairEditor);
    CHECK(Access::assemblyHistoryCursor(repairEditor) == 2);
    CHECK(Access::linkedRuntimeStatus(repairEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Unchecked);
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition)
              ->relativePathHintUtf8 == "parts/newer.solidar");
    CHECK(Access::linkedBinding(repairEditor, kUiLinkedDefinition)
              ->acceptedHash == repairBindingBefore->acceptedHash);
    CHECK(Access::viewportAssemblySelection(repairEditor)
              ->occurrence.path ==
          repairSelectionBeforeColdHistory->occurrence.path);

    solidar::MainWindow reentrantRepairEditor(settings);
    CHECK(reentrantRepairEditor.loadProject(linkedAssemblyPath,
                                             &linkedError));
    Access::selectAssemblyTreeIndex(
        reentrantRepairEditor,
        Access::firstAssemblyOccurrenceIndex(reentrantRepairEditor));
    CHECK(!Access::runLinkedPartRepairCommand(
        reentrantRepairEditor,
        [&](const QString&) -> std::optional<QString> {
          QString switchError;
          CHECK(reentrantRepairEditor.loadProject(pathA, &switchError));
          CHECK(switchError.isEmpty());
          return repairedExactPath;
        },
        &repairAttempted));
    CHECK(!repairAttempted);
    CHECK(reentrantRepairEditor.windowFilePath() == pathA);
    CHECK(Access::projectKind(reentrantRepairEditor) ==
          solidar::EditorProjectKind::Part);

    solidar::MainWindow isolationRepairEditor(settings);
    CHECK(isolationRepairEditor.loadProject(linkedAssemblyPath,
                                             &linkedError));
    Access::selectAssemblyTreeIndex(
        isolationRepairEditor,
        Access::firstAssemblyOccurrenceIndex(isolationRepairEditor));
    const auto isolationBindingBefore =
        Access::linkedBinding(isolationRepairEditor, kUiLinkedDefinition);
    const auto isolationStateBefore =
        Access::definitionStateId(isolationRepairEditor,
                                  kUiLinkedDefinition);
    const auto isolationHistoryBefore =
        Access::assemblyHistoryRevision(isolationRepairEditor);
    const auto* isolationSessionBefore =
        Access::assemblySessionAddress(isolationRepairEditor);
    CHECK(Access::assemblyIsolationEnabled(isolationRepairEditor));
    CHECK(!Access::runLinkedPartRepairCommand(
        isolationRepairEditor,
        [&](const QString&) -> std::optional<QString> {
          Access::triggerAssemblyIsolation(isolationRepairEditor);
          CHECK(Access::assemblyIsolationActive(isolationRepairEditor));
          return repairedExactPath;
        },
        &repairAttempted));
    CHECK(!repairAttempted);
    CHECK(Access::assemblyIsolationActive(isolationRepairEditor));
    CHECK(Access::assemblySessionAddress(isolationRepairEditor) ==
          isolationSessionBefore);
    CHECK(Access::linkedBinding(isolationRepairEditor,
                                kUiLinkedDefinition) ==
          isolationBindingBefore);
    CHECK(Access::definitionStateId(isolationRepairEditor,
                                    kUiLinkedDefinition) ==
          isolationStateBefore);
    CHECK(Access::assemblyHistoryRevision(isolationRepairEditor) ==
          isolationHistoryBefore);
    CHECK(!isolationRepairEditor.isWindowModified());

    const QModelIndex linkedOccurrenceIndex =
        Access::firstAssemblyOccurrenceIndex(linkedEditor);
    CHECK(linkedOccurrenceIndex.isValid());
    Access::selectAssemblyTreeIndex(linkedEditor, linkedOccurrenceIndex);
    CHECK(Access::linkedReloadAction(linkedEditor)->isEnabled());
    CHECK(Access::linkedRepairAction(linkedEditor)->isEnabled());
    CHECK(!Access::definitionEditEnabled(linkedEditor));
    const auto selectedBeforeReload =
        Access::viewportAssemblySelection(linkedEditor);
    CHECK(selectedBeforeReload);
    CHECK(selectedBeforeReload->occurrence.path.entries() ==
          std::vector<solidar::OccurrenceId>{solidar::OccurrenceId{701}});
    CHECK(Access::setHoveredAssemblyOccurrence(linkedEditor,
                                                selectedBeforeReload));
    const auto* sessionBeforeReload =
        Access::assemblySessionAddress(linkedEditor);
    const auto* treeBeforeReload =
        Access::assemblyTreeModelAddress(linkedEditor);
    CHECK(sessionBeforeReload && sessionBeforeReload->scene());
    const auto publicationBeforeReload =
        sessionBeforeReload->publicationHandle();
    const auto* sceneBeforeReload = sessionBeforeReload->scene().get();
    CHECK(sceneBeforeReload->snapshot()->sources().size() == 1);
    CHECK(sceneBeforeReload->snapshot()->instances().size() == 2);
    CHECK(sceneBeforeReload->snapshot()->instances()[0].sourceKey() ==
          sceneBeforeReload->snapshot()->instances()[1].sourceKey());
    const auto* shapeBeforeReload =
        sceneBeforeReload->snapshot()->sources().front().shape().get();

    CHECK(writeTestBytes(linkedSourcePath,
                         revisedLinkedEncoded.payload->bytes));

    // Merely opening diagnoses Outdated but keeps the serialized cache and
    // the project clean. It never accepts the new revision implicitly.
    solidar::MainWindow outdatedEditor(settings);
    CHECK(outdatedEditor.loadProject(linkedAssemblyPath, &linkedError));
    CHECK(Access::linkedRuntimeStatus(outdatedEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Outdated);
    CHECK(Access::linkedBox(outdatedEditor, kUiLinkedDefinition).widthMm ==
          60.0);
    CHECK(!outdatedEditor.isWindowModified());
    const QModelIndex outdatedDefinition =
        Access::assemblyDefinitionIndex(outdatedEditor,
                                        kUiLinkedDefinition);
    CHECK(outdatedDefinition.data().toString().contains(
        QString::fromUtf8("источник изменён")));

    const QByteArray sourceBytesBeforeReload =
        readTestBytes(linkedSourcePath);
    const QDateTime sourceMtimeBeforeReload =
        QFileInfo(linkedSourcePath).lastModified();
    const QStringList sourceDirectoryBeforeReload =
        QDir(linkedRoot + QStringLiteral("/parts"))
            .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                       QDir::Name);
    Access::reloadSelectedLinkedPart(linkedEditor);
    CHECK(readTestBytes(linkedSourcePath) == sourceBytesBeforeReload);
    CHECK(QFileInfo(linkedSourcePath).lastModified() ==
          sourceMtimeBeforeReload);
    CHECK(QDir(linkedRoot + QStringLiteral("/parts"))
              .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                         QDir::Name) == sourceDirectoryBeforeReload);
    CHECK(Access::assemblyHistorySize(linkedEditor) == 1);
    CHECK(Access::assemblyHistoryCursor(linkedEditor) == 1);
    CHECK(linkedEditor.isWindowModified());
    CHECK(Access::linkedRuntimeStatus(linkedEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Exact);
    CHECK(Access::linkedBox(linkedEditor, kUiLinkedDefinition).widthMm ==
          88.0);
    const auto* sessionAfterReload =
        Access::assemblySessionAddress(linkedEditor);
    const auto* treeAfterReload =
        Access::assemblyTreeModelAddress(linkedEditor);
    CHECK(sessionAfterReload && sessionAfterReload != sessionBeforeReload);
    CHECK(treeAfterReload && treeAfterReload != treeBeforeReload);
    CHECK(!sessionAfterReload->publicationHandle().isSameCurrentPublication(
        publicationBeforeReload));
    CHECK(sessionAfterReload->scene().get() != sceneBeforeReload);
    CHECK(sessionAfterReload->scene()->snapshot()->sources().size() == 1);
    CHECK(sessionAfterReload->scene()->snapshot()->instances().size() == 2);
    CHECK(sessionAfterReload->scene()->snapshot()->instances()[0].sourceKey() ==
          sessionAfterReload->scene()->snapshot()->instances()[1].sourceKey());
    CHECK(sessionAfterReload->scene()
              ->snapshot()
              ->sources()
              .front()
              .shape()
              .get() != shapeBeforeReload);
    CHECK(Access::linkedBinding(linkedEditor, kUiLinkedDefinition)
              ->acceptedHash ==
          solidar::project::linkedSourceSha256(
              QByteArrayView(revisedLinkedEncoded.payload->bytes)));
    const auto* sharedRoot =
        Access::productDocumentAddress(linkedEditor)
            ->rootAssemblyDefinition();
    CHECK(sharedRoot && sharedRoot->occurrences().size() == 2);
    CHECK(sharedRoot->find(solidar::OccurrenceId{701})->displayNameUtf8() ==
          "left");
    CHECK(sharedRoot->find(solidar::OccurrenceId{702})->displayNameUtf8() ==
          "right");
    CHECK(sharedRoot->find(solidar::OccurrenceId{702})->flags() ==
          solidar::OccurrenceFlags({true, false, true}));
    const auto selectedAfterReload =
        Access::viewportAssemblySelection(linkedEditor);
    CHECK(selectedAfterReload);
    CHECK(selectedAfterReload->occurrence.path.entries() ==
          selectedBeforeReload->occurrence.path.entries());
    CHECK(Access::hoveredHighlightIdentity(linkedEditor));
    CHECK(Access::hoveredHighlightIdentity(linkedEditor)->path ==
          selectedBeforeReload->occurrence.path);

    CHECK(QFile::remove(linkedSourcePath));
    const auto publicationAfterReload =
        sessionAfterReload->publicationHandle();
    const auto* shapeAfterReload = sessionAfterReload->scene()
                                       ->snapshot()
                                       ->sources()
                                       .front()
                                       .shape()
                                       .get();
    Access::undoAssembly(linkedEditor);
    CHECK(Access::assemblyHistoryCursor(linkedEditor) == 0);
    CHECK(Access::linkedBox(linkedEditor, kUiLinkedDefinition).widthMm ==
          60.0);
    CHECK(Access::linkedBinding(linkedEditor, kUiLinkedDefinition) ==
          linkedBinding);
    CHECK(Access::linkedRuntimeStatus(linkedEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Unchecked);
    const auto* sessionAfterUndo =
        Access::assemblySessionAddress(linkedEditor);
    CHECK(sessionAfterUndo && sessionAfterUndo != sessionAfterReload);
    CHECK(!sessionAfterUndo->publicationHandle().isSameCurrentPublication(
        publicationAfterReload));
    CHECK(sessionAfterUndo->scene()->snapshot()->sources().size() == 1);
    CHECK(sessionAfterUndo->scene()->snapshot()->instances().size() == 2);
    CHECK(sessionAfterUndo->scene()->snapshot()->instances()[0].sourceKey() ==
          sessionAfterUndo->scene()->snapshot()->instances()[1].sourceKey());
    CHECK(sessionAfterUndo->scene()
              ->snapshot()
              ->sources()
              .front()
              .shape()
              .get() != shapeAfterReload);
    const auto publicationAfterUndo = sessionAfterUndo->publicationHandle();
    const auto* shapeAfterUndo = sessionAfterUndo->scene()
                                     ->snapshot()
                                     ->sources()
                                     .front()
                                     .shape()
                                     .get();
    Access::redoAssembly(linkedEditor);
    CHECK(Access::assemblyHistoryCursor(linkedEditor) == 1);
    CHECK(Access::linkedBox(linkedEditor, kUiLinkedDefinition).widthMm ==
          88.0);
    CHECK(Access::linkedRuntimeStatus(linkedEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Unchecked);
    const auto* sessionAfterRedo =
        Access::assemblySessionAddress(linkedEditor);
    CHECK(sessionAfterRedo && sessionAfterRedo != sessionAfterUndo);
    CHECK(!sessionAfterRedo->publicationHandle().isSameCurrentPublication(
        publicationAfterUndo));
    CHECK(sessionAfterRedo->scene()->snapshot()->sources().size() == 1);
    CHECK(sessionAfterRedo->scene()->snapshot()->instances().size() == 2);
    CHECK(sessionAfterRedo->scene()->snapshot()->instances()[0].sourceKey() ==
          sessionAfterRedo->scene()->snapshot()->instances()[1].sourceKey());
    CHECK(sessionAfterRedo->scene()
              ->snapshot()
              ->sources()
              .front()
              .shape()
              .get() != shapeAfterUndo);
    CHECK(Access::viewportAssemblySelection(linkedEditor));
    CHECK(Access::viewportAssemblySelection(linkedEditor)
              ->occurrence.path == selectedBeforeReload->occurrence.path);
    CHECK(Access::hoveredHighlightIdentity(linkedEditor));

    // Save at an undone cursor marks that exact revision clean without
    // truncating the cold redo tail. Moving away from and back to the saved
    // revision updates dirtiness symmetrically without touching the source.
    QString linkedSaveError;
    Access::undoAssembly(linkedEditor);
    CHECK(Access::assemblyHistoryCursor(linkedEditor) == 0);
    CHECK(Access::assemblyHistorySize(linkedEditor) == 1);
    CHECK(Access::redoAction(linkedEditor)->isEnabled());
    CHECK(Access::saveProjectToPath(linkedEditor, linkedAssemblyPath,
                                    &linkedSaveError));
    CHECK(linkedSaveError.isEmpty());
    CHECK(!linkedEditor.isWindowModified());
    CHECK(Access::assemblyHistoryCursor(linkedEditor) == 0);
    CHECK(Access::assemblyHistorySize(linkedEditor) == 1);
    CHECK(Access::redoAction(linkedEditor)->isEnabled());
    CHECK(Access::assemblyHistorySavepoint(linkedEditor) ==
          Access::assemblyHistoryRevision(linkedEditor));
    Access::redoAssembly(linkedEditor);
    CHECK(linkedEditor.isWindowModified());
    Access::undoAssembly(linkedEditor);
    CHECK(!linkedEditor.isWindowModified());
    Access::redoAssembly(linkedEditor);
    CHECK(linkedEditor.isWindowModified());

    // Persist the redone linked revision for the subsequent Missing-open
    // diagnostic, then retain the ordinary end-save Undo/Redo symmetry.
    CHECK(Access::saveProjectToPath(linkedEditor, linkedAssemblyPath,
                                    &linkedSaveError));
    CHECK(linkedSaveError.isEmpty());
    CHECK(!linkedEditor.isWindowModified());
    Access::undoAssembly(linkedEditor);
    CHECK(linkedEditor.isWindowModified());
    Access::redoAssembly(linkedEditor);
    CHECK(!linkedEditor.isWindowModified());

    // Missing is diagnostic too and materializes the most recently saved
    // cache. A malformed V7 replacement cannot disturb the live workspace.
    solidar::MainWindow missingEditor(settings);
    CHECK(missingEditor.loadProject(linkedAssemblyPath, &linkedError));
    CHECK(Access::linkedRuntimeStatus(missingEditor, kUiLinkedDefinition) ==
          solidar::LinkedPartRuntimeStatus::Missing);
    CHECK(Access::linkedBox(missingEditor, kUiLinkedDefinition).widthMm ==
          88.0);
    CHECK(!missingEditor.isWindowModified());
    const auto* liveProduct = Access::productDocumentAddress(missingEditor);
    const auto* liveSession = Access::assemblySessionAddress(missingEditor);
    const auto* liveTree = Access::assemblyTreeModelAddress(missingEditor);
    CHECK(liveSession && liveSession->scene());
    const auto livePublication = liveSession->publicationHandle();
    const auto* liveScene = liveSession->scene().get();
    const auto* liveShape =
        liveScene->snapshot()->sources().front().shape().get();
    const auto livePersistence = Access::persistenceState(missingEditor);
    const auto liveBinding =
        Access::linkedBinding(missingEditor, kUiLinkedDefinition);
    const auto liveDefinitionState =
        Access::definitionStateId(missingEditor, kUiLinkedDefinition);
    const auto liveHistorySize = Access::assemblyHistorySize(missingEditor);
    const auto liveHistoryCursor = Access::assemblyHistoryCursor(missingEditor);
    const auto liveHistory = Access::assemblyHistoryRevision(missingEditor);
    const auto liveSavepoint =
        Access::assemblyHistorySavepoint(missingEditor);
    const auto liveTreeSelection = Access::occurrenceForIndex(
        missingEditor, Access::assemblyTreeCurrentIndex(missingEditor));
    const auto liveViewportSelection =
        Access::viewportAssemblySelection(missingEditor);
    const auto liveHover = Access::hoveredHighlightIdentity(missingEditor);
    const QString livePath = missingEditor.windowFilePath();
    const QString malformedV7Path =
        linkedRoot + QStringLiteral("/malformed-v7.solidar");
    CHECK(writeTestBytes(
        malformedV7Path,
        QByteArrayLiteral("{\"format\":\"solidar3d\",\"version\":7}")));
    CHECK(!missingEditor.loadProject(malformedV7Path, &linkedError));
    CHECK(!linkedError.isEmpty());
    CHECK(Access::productDocumentAddress(missingEditor) == liveProduct);
    CHECK(Access::assemblySessionAddress(missingEditor) == liveSession);
    CHECK(Access::assemblyHistoryRevision(missingEditor) == liveHistory);
    CHECK(missingEditor.windowFilePath() == livePath);
    CHECK(Access::linkedBox(missingEditor, kUiLinkedDefinition).widthMm ==
          88.0);

    // A syntactically valid V7 that fails only while recomputing its staged
    // lastValid payload is equally atomic at the live UI boundary. In
    // particular, speculative explicit IDs do not advance global cursors.
    auto lateFailureAssembly = uiLateFailureAssembly();
    const auto lateFailureEncoded =
        solidar::project::encodeProductEnvelopeV7Root(
            *lateFailureAssembly, kUiLinkedAssemblyProjectId,
            {QStringLiteral("Late failure assembly"),
             QStringLiteral("2026-10-27T00:00:00Z")});
    CHECK(lateFailureEncoded.succeeded());
    const QString lateFailureV7Path =
        linkedRoot + QStringLiteral("/late-failure-v7.solidar");
    CHECK(writeTestBytes(lateFailureV7Path,
                         lateFailureEncoded.payload->bytes));
    const auto cursorsBeforeLateFailure =
        consumeModelIdCursors("before UI V7 late failure");
    CHECK(!missingEditor.loadProject(lateFailureV7Path, &linkedError));
    const auto cursorsAfterLateFailure =
        consumeModelIdCursors("after UI V7 late failure");
    CHECK(cursorsAfterLateFailure.body == cursorsBeforeLateFailure.body + 1);
    CHECK(cursorsAfterLateFailure.sketch ==
          cursorsBeforeLateFailure.sketch + 1);
    CHECK(cursorsAfterLateFailure.feature ==
          cursorsBeforeLateFailure.feature + 1);
    CHECK(!linkedError.isEmpty());
    CHECK(Access::productDocumentAddress(missingEditor) == liveProduct);
    CHECK(Access::assemblySessionAddress(missingEditor) == liveSession);
    CHECK(Access::assemblyTreeModelAddress(missingEditor) == liveTree);
    CHECK(liveSession->scene().get() == liveScene);
    CHECK(liveSession->scene()->snapshot()->sources().front().shape().get() ==
          liveShape);
    CHECK(liveSession->publicationHandle().isSameCurrentPublication(
        livePublication));
    CHECK(Access::persistenceState(missingEditor) == livePersistence);
    CHECK(Access::linkedBinding(missingEditor, kUiLinkedDefinition) ==
          liveBinding);
    CHECK(Access::definitionStateId(missingEditor, kUiLinkedDefinition) ==
          liveDefinitionState);
    CHECK(Access::assemblyHistorySize(missingEditor) == liveHistorySize);
    CHECK(Access::assemblyHistoryCursor(missingEditor) == liveHistoryCursor);
    CHECK(Access::assemblyHistoryRevision(missingEditor) == liveHistory);
    CHECK(Access::assemblyHistorySavepoint(missingEditor) == liveSavepoint);
    CHECK(Access::occurrenceForIndex(
              missingEditor,
              Access::assemblyTreeCurrentIndex(missingEditor)) ==
          liveTreeSelection);
    CHECK(Access::viewportAssemblySelection(missingEditor) ==
          liveViewportSelection);
    CHECK(Access::hoveredHighlightIdentity(missingEditor) == liveHover);
    CHECK(missingEditor.windowFilePath() == livePath);
    CHECK(!missingEditor.isWindowModified());
  }
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-switch-ui-tests-XXXXXX")));
  CHECK(directory.isValid());

  // Project A carries a real body so the replacement tears down actual B-Rep
  // state, not an empty shell.
  {
    solidar::Document document;
    auto& baseSketch = document.addSketch("Base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    auto& body = document.addBody("Body");
    body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 15.0, "Extrude"));
    CHECK(document.recompute());
    QString error;
    CHECK(solidar::project::ProjectFile::saveDocument(
        directory.filePath(QStringLiteral("a.solidar")), document, &error));
  }

  QString error;
  const QString pathA = directory.filePath(QStringLiteral("a.solidar"));
  const QString pathB = directory.filePath(QStringLiteral("b.solidar"));
  const QString definitionMoveAssemblyPath =
      directory.filePath(QStringLiteral("definition-move-assembly.solidar"));
  CHECK(solidar::project::ProjectFile::create(pathB, &error));
  CHECK(writeDefinitionMoveAssemblyFixture(definitionMoveAssemblyPath));
  solidar::AppSettings settings(
      directory.filePath(QStringLiteral("settings.ini")));
  using Access = solidar::MainWindowUndoTestAccess;

  runInitialPersistenceAndConversionScenarios(directory, settings, pathA,
                                              error);


  // Save As is one guarded command for standalone V3/V4 and product V5. It
  // preserves the carrier format for standalone files and forks identity only
  // for products, after the modal boundary has been fully revalidated.
  {
    const solidar::project::ProjectPersistenceState v3{
        solidar::project::ProjectPersistenceFormat::V3};
    const solidar::project::ProjectPersistenceState v4{
        solidar::project::ProjectPersistenceFormat::V4};
    const solidar::project::ProjectPersistenceState v5{
        solidar::project::ProjectPersistenceFormat::V5};

    solidar::MainWindow pathless(settings);
    auto* saveAsAction = Access::saveProjectAsAction(pathless);
    auto* upgradeAction = Access::upgradeProjectFormatAction(pathless);
    CHECK(saveAsAction);
    CHECK(saveAsAction->objectName() == QStringLiteral("saveProjectAsAction"));
    CHECK(saveAsAction->text() == QString::fromUtf8("Сохранить как…"));
    CHECK(saveAsAction->shortcut() == QKeySequence::SaveAs);
    CHECK(saveAsAction->isEnabled());
    CHECK(Access::saveProjectAsIsEligible(pathless));
    auto* fileMenu = qobject_cast<QMenu*>(saveAsAction->parent());
    CHECK(fileMenu);
    const auto fileActions = fileMenu->actions();
    const int saveAsIndex = fileActions.indexOf(saveAsAction);
    CHECK(saveAsIndex > 0);
    CHECK(fileActions.at(saveAsIndex - 1)->text() ==
          QString::fromUtf8("Сохранить"));
    CHECK(fileActions.indexOf(upgradeAction) == saveAsIndex + 1);

    bool cancelFactoryCalled = false;
    bool cancelAttempted = true;
    const auto cancelled = Access::runSaveProjectAsCommand(
        pathless,
        [](const QString&) -> std::optional<QString> { return std::nullopt; },
        [&](const QString&) {
          cancelFactoryCalled = true;
          return uiCloneRequest();
        },
        &cancelAttempted);
    CHECK(cancelled.succeeded());
    CHECK(!cancelAttempted);
    CHECK(!cancelFactoryCalled);
    const auto empty = Access::runSaveProjectAsCommand(
        pathless,
        [](const QString&) -> std::optional<QString> { return QString(); },
        [&](const QString&) {
          cancelFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(empty.succeeded());
    CHECK(!cancelFactoryCalled);

    QFile v3Source(pathA);
    CHECK(v3Source.open(QIODevice::ReadOnly));
    const QByteArray v3SourceBytes = v3Source.readAll();
    v3Source.close();
    solidar::MainWindow v3Editor(settings);
    CHECK(v3Editor.loadProject(pathA, &error));
    CHECK(Access::persistenceState(v3Editor) == v3);
    CHECK(Access::saveProjectAsAction(v3Editor)->isEnabled());
    const QString v3CopyBase =
        directory.filePath(QStringLiteral("save-as-v3-copy"));
    const QString v3Copy = v3CopyBase + QStringLiteral(".solidar");
    bool standaloneFactoryCalled = false;
    bool v3Attempted = false;
    const auto savedV3 = Access::runSaveProjectAsCommand(
        v3Editor,
        [&](const QString&) -> std::optional<QString> { return v3CopyBase; },
        [&](const QString&) {
          standaloneFactoryCalled = true;
          return uiCloneRequest();
        },
        &v3Attempted);
    CHECK(savedV3.succeeded());
    CHECK(v3Attempted);
    CHECK(!standaloneFactoryCalled);
    CHECK(v3Editor.windowFilePath() == v3Copy);
    CHECK(Access::persistenceState(v3Editor) == v3);
    CHECK(Access::identity(v3Editor) == std::nullopt);
    CHECK(!Access::modified(v3Editor));
    QFile v3Output(v3Copy);
    CHECK(v3Output.open(QIODevice::ReadOnly));
    CHECK(QJsonDocument::fromJson(v3Output.readAll())
              .object()
              .value(QStringLiteral("version"))
              .toInteger() == 3);
    auto stagedV3Copy = solidar::ProjectApplicationService{}.stageOpen(v3Copy);
    CHECK(stagedV3Copy.succeeded());
    CHECK(stagedV3Copy.staged->productDocument().identity() == std::nullopt);
    CHECK(v3Source.open(QIODevice::ReadOnly));
    CHECK(v3Source.readAll() == v3SourceBytes);
    v3Source.close();

    const QString v4SourcePath =
        directory.filePath(QStringLiteral("save-as-v4-source.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        v4SourcePath, Access::documentCopy(v3Editor), v4, &error));
    QFile v4Source(v4SourcePath);
    CHECK(v4Source.open(QIODevice::ReadOnly));
    const QByteArray v4SourceBytes = v4Source.readAll();
    v4Source.close();
    solidar::MainWindow v4Editor(settings);
    CHECK(v4Editor.loadProject(v4SourcePath, &error));
    CHECK(Access::saveProjectAsAction(v4Editor)->isEnabled());
    const QString v4Copy =
        directory.filePath(QStringLiteral("save-as-v4-copy.solidar"));
    const auto savedV4 = Access::runSaveProjectAsCommand(
        v4Editor,
        [&](const QString&) -> std::optional<QString> { return v4Copy; },
        [&](const QString&) {
          standaloneFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(savedV4.succeeded());
    CHECK(!standaloneFactoryCalled);
    CHECK(Access::persistenceState(v4Editor) == v4);
    CHECK(Access::identity(v4Editor) == std::nullopt);
    QFile v4Output(v4Copy);
    CHECK(v4Output.open(QIODevice::ReadOnly));
    CHECK(QJsonDocument::fromJson(v4Output.readAll())
              .object()
              .value(QStringLiteral("version"))
              .toInteger() == 4);
    auto stagedV4Copy = solidar::ProjectApplicationService{}.stageOpen(v4Copy);
    CHECK(stagedV4Copy.succeeded());
    CHECK(stagedV4Copy.staged->productDocument().identity() == std::nullopt);
    CHECK(v4Source.open(QIODevice::ReadOnly));
    CHECK(v4Source.readAll() == v4SourceBytes);
    v4Source.close();

    // Existing source aliases are rejected before either request allocation
    // or I/O. The absolute normalization also covers a relative alias.
    bool aliasFactoryCalled = false;
    const QString v4CurrentPath = v4Editor.windowFilePath();
    const QString relativeAlias =
        QDir::current().relativeFilePath(v4CurrentPath);
    const auto aliasRejected = Access::runSaveProjectAsCommand(
        v4Editor,
        [&](const QString&) -> std::optional<QString> { return relativeAlias; },
        [&](const QString&) {
          aliasFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(!aliasRejected.succeeded());
    CHECK(!aliasFactoryCalled);
    CHECK(Access::statusMessage(v4Editor).contains(
        QString::fromUtf8("Сохранить")));
#ifdef Q_OS_WIN
    const auto caseAliasRejected = Access::runSaveProjectAsCommand(
        v4Editor,
        [&](const QString&) -> std::optional<QString> {
          return v4CurrentPath.toUpper();
        },
        [&](const QString&) {
          aliasFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(!caseAliasRejected.succeeded());
    CHECK(!aliasFactoryCalled);
#endif

    // Missing-parent failure is state exact and retryable for standalone
    // files as well.
    const QString standaloneBeforeFailure = v4Editor.windowFilePath();
    const QString missingStandalone =
        directory.filePath(
            QStringLiteral("missing-standalone/standalone-copy.solidar"));
    const auto failedStandalone = Access::runSaveProjectAsCommand(
        v4Editor,
        [&](const QString&) -> std::optional<QString> {
          return missingStandalone;
        },
        [&](const QString&) {
          standaloneFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(!failedStandalone.succeeded());
    CHECK(failedStandalone.code ==
          solidar::ProjectApplicationErrorCode::SaveFailed);
    CHECK(v4Editor.windowFilePath() == standaloneBeforeFailure);
    CHECK(!QFile::exists(missingStandalone));
    CHECK(QDir().mkpath(QFileInfo(missingStandalone).absolutePath()));
    const auto retriedStandalone = Access::runSaveProjectAsCommand(
        v4Editor,
        [&](const QString&) -> std::optional<QString> {
          return missingStandalone;
        },
        [&](const QString&) {
          standaloneFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(retriedStandalone.succeeded());
    CHECK(!standaloneFactoryCalled);
    CHECK(v4Editor.windowFilePath() == missingStandalone);

    // Build a rich V5 carrier A through the already-covered conversion path.
    const QString productAPath =
        directory.filePath(QStringLiteral("save-as-product-a.solidar"));
    solidar::MainWindow productEditor(settings);
    CHECK(productEditor.loadProject(pathA, &error));
    CHECK(Access::convertProjectToPath(
              productEditor, productAPath,
              uiConversionRequest(Access::rootDefinitionId(productEditor),
                                  QStringLiteral("product A")))
              .succeeded());
    CHECK(Access::persistenceState(productEditor) == v5);
    CHECK(Access::saveProjectAsAction(productEditor)->isEnabled());
    CHECK(Access::saveProjectAsIsEligible(productEditor));
    CHECK(Access::pushBodyMarker(productEditor, "clone history one"));
    CHECK(Access::pushBodyMarker(productEditor, "clone history two"));
    Access::undoAction(productEditor)->trigger();
    Access::selectBody(productEditor, Access::firstBodyId(productEditor));

    QString ordinarySaveError;
    const auto identityABeforeOrdinary = *Access::identity(productEditor);
    CHECK(Access::saveProjectToPath(productEditor, productAPath,
                                    &ordinarySaveError));
    CHECK(ordinarySaveError.isEmpty());
    CHECK(Access::identity(productEditor) == identityABeforeOrdinary);
    QFile productAFile(productAPath);
    CHECK(productAFile.open(QIODevice::ReadOnly));
    const QByteArray productABytes = productAFile.readAll();
    productAFile.close();

    // V5 cancellation and same-target checks never construct a clone request.
    bool productFactoryCalled = false;
    const auto productCancelled = Access::runSaveProjectAsCommand(
        productEditor,
        [](const QString&) -> std::optional<QString> { return std::nullopt; },
        [&](const QString&) {
          productFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(productCancelled.succeeded());
    CHECK(!productFactoryCalled);
    const auto sameProductRejected = Access::runSaveProjectAsCommand(
        productEditor,
        [&](const QString&) -> std::optional<QString> { return productAPath; },
        [&](const QString&) {
          productFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(!sameProductRejected.succeeded());
    CHECK(!productFactoryCalled);

    // Invalid fork identities fail before publication and preserve A.
    const QString invalidClonePath =
        directory.filePath(QStringLiteral("invalid-clone.solidar"));
    const auto nilRejected = Access::saveProductCloneToPath(
        productEditor, invalidClonePath,
        uiCloneRequest(solidar::ProjectId{}));
    CHECK(!nilRejected.succeeded());
    CHECK(!QFile::exists(invalidClonePath));
    CHECK(Access::identity(productEditor) == identityABeforeOrdinary);
    const auto sameIdRejected = Access::saveProductCloneToPath(
        productEditor, invalidClonePath,
        uiCloneRequest(identityABeforeOrdinary.projectId));
    CHECK(!sameIdRejected.succeeded());
    CHECK(!QFile::exists(invalidClonePath));
    CHECK(Access::identity(productEditor) == identityABeforeOrdinary);

    const auto* const documentBefore = Access::documentAddress(productEditor);
    const auto* const bodyBefore = Access::firstBodyAddress(productEditor);
    const auto* const featureBefore = Access::firstFeatureAddress(productEditor);
    const auto* const shapeBefore = Access::firstShapeAddress(productEditor);
    const auto* const topologyBefore =
        Access::firstTopologyAddress(productEditor);
    CHECK(bodyBefore);
    CHECK(featureBefore);
    CHECK(shapeBefore);
    CHECK(topologyBefore);
    const auto rootBefore = Access::rootDefinitionId(productEditor);
    const auto bodyIdBefore = bodyBefore->id();
    const auto featureIdBefore = featureBefore->id();
    const auto shapeRevisionBefore = Access::firstShapeRevision(productEditor);
    const auto selectionBefore = Access::selectionState(productEditor);
    const auto undoBefore = Access::undoCount(productEditor);
    const auto redoBefore = Access::redoCount(productEditor);
    const auto historyRevisionBefore =
        Access::currentHistoryRevision(productEditor);
    const auto previewBefore = Access::previewDocumentGeneration(productEditor);
    const auto cursorsBefore = consumeModelIdCursors("before UI Save As");

    const QString missingClone =
        directory.filePath(
            QStringLiteral("missing-product/product-clone.solidar"));
    bool failedCloneFactoryCalled = false;
    const auto failedClone = Access::runSaveProjectAsCommand(
        productEditor,
        [&](const QString&) -> std::optional<QString> { return missingClone; },
        [&](const QString& target) {
          CHECK(target == missingClone);
          failedCloneFactoryCalled = true;
          return uiCloneRequest();
        });
    CHECK(!failedClone.succeeded());
    CHECK(failedClone.code == solidar::ProjectApplicationErrorCode::SaveFailed);
    CHECK(failedCloneFactoryCalled);
    CHECK(!QFile::exists(missingClone));
    CHECK(Access::identity(productEditor) == identityABeforeOrdinary);
    CHECK(productEditor.windowFilePath() == productAPath);

    const QString productBBase =
        directory.filePath(QString::fromUtf8("save-as-product-b-Ω"));
    const QString productBPath = productBBase + QStringLiteral(".solidar");
    QString productSuggestion;
    bool successfulFactoryCalled = false;
    const auto cloned = Access::runSaveProjectAsCommand(
        productEditor,
        [&](const QString& suggestion) -> std::optional<QString> {
          productSuggestion = suggestion;
          return productBBase;
        },
        [&](const QString& target) {
          successfulFactoryCalled = true;
          CHECK(target == productBPath);
          return uiCloneRequest();
        });
    CHECK(cloned.succeeded());
    CHECK(successfulFactoryCalled);
    CHECK(productSuggestion ==
          directory.filePath(QStringLiteral("save-as-product-a-копия.solidar")));
    CHECK(Access::persistenceState(productEditor) == v5);
    CHECK(productEditor.windowFilePath() == productBPath);
    CHECK(!Access::modified(productEditor));
    CHECK(Access::historyAtSavepoint(productEditor));
    CHECK(Access::saveProjectAsAction(productEditor)->isEnabled());
    CHECK(!Access::convertToProductDocumentAction(productEditor)->isEnabled());
    CHECK(!Access::upgradeProjectFormatAction(productEditor)->isEnabled());
    CHECK(Access::identity(productEditor)->projectId == kUiCloneProjectId);
    CHECK(Access::identity(productEditor)->metadata.nameUtf8 ==
          std::string("Копия \xCE\xA9/\xE9\x9B\xAA"));
    CHECK(Access::identity(productEditor)->metadata.createdAtUtf8 ==
          "2026-10-23T22:15:30Z");
    CHECK(Access::documentAddress(productEditor) == documentBefore);
    CHECK(Access::firstBodyAddress(productEditor) == bodyBefore);
    CHECK(Access::firstFeatureAddress(productEditor) == featureBefore);
    CHECK(Access::rootDefinitionId(productEditor) == rootBefore);
    CHECK(Access::firstBodyId(productEditor) == bodyIdBefore);
    CHECK(Access::firstFeatureAddress(productEditor)->id() ==
          featureIdBefore);
    CHECK(Access::firstShapeAddress(productEditor) == shapeBefore);
    CHECK(Access::firstTopologyAddress(productEditor) == topologyBefore);
    CHECK(Access::firstShapeRevision(productEditor) == shapeRevisionBefore);
    CHECK(Access::exactSelectionState(Access::selectionState(productEditor),
                                      selectionBefore));
    CHECK(Access::undoCount(productEditor) == undoBefore);
    CHECK(Access::redoCount(productEditor) == redoBefore);
    CHECK(Access::currentHistoryRevision(productEditor) ==
          historyRevisionBefore);
    CHECK(Access::previewDocumentGeneration(productEditor) == previewBefore);
    const auto cursorsAfter = consumeModelIdCursors("after UI Save As");
    CHECK(cursorsAfter.body == cursorsBefore.body + 1);
    CHECK(cursorsAfter.sketch == cursorsBefore.sketch + 1);
    CHECK(cursorsAfter.feature == cursorsBefore.feature + 1);
    CHECK(productAFile.open(QIODevice::ReadOnly));
    CHECK(productAFile.readAll() == productABytes);
    productAFile.close();
    auto productBStage = solidar::ProjectApplicationService{}.stageOpen(
        productBPath, solidar::ProductV5OpenPolicy::Allow);
    CHECK(productBStage.succeeded());
    CHECK(productBStage.staged->productDocument().identity() ==
          Access::identity(productEditor));
    CHECK(productBStage.staged->productDocument().rootDefinitionId() ==
          Access::rootDefinitionId(productEditor));

    // Ordinary Save and Undo/Redo retain the clone B identity/path. Savepoint
    // dirtiness continues to follow the unchanged history revision.
    const auto identityB = *Access::identity(productEditor);
    CHECK(Access::saveProjectToPath(productEditor, productBPath,
                                    &ordinarySaveError));
    CHECK(Access::identity(productEditor) == identityB);
    Access::undoAction(productEditor)->trigger();
    CHECK(Access::modified(productEditor));
    CHECK(Access::identity(productEditor) == identityB);
    CHECK(productEditor.windowFilePath() == productBPath);
    Access::redoAction(productEditor)->trigger();
    CHECK(!Access::modified(productEditor));
    CHECK(Access::identity(productEditor) == identityB);
    CHECK(productEditor.windowFilePath() == productBPath);

    // A factory exception is surfaced without writing or changing B.
    const QString throwingTarget =
        directory.filePath(QStringLiteral("throwing-clone.solidar"));
    bool throwingAttempted = false;
    const auto throwingResult = Access::runSaveProjectAsCommand(
        productEditor,
        [&](const QString&) -> std::optional<QString> { return throwingTarget; },
        [](const QString&) -> solidar::ProductCloneRequest {
          throw std::runtime_error("clone factory failure");
        },
        &throwingAttempted);
    CHECK(!throwingResult.succeeded());
    CHECK(throwingAttempted);
    CHECK(!QFile::exists(throwingTarget));
    CHECK(Access::identity(productEditor) == identityB);

    // Re-entrant history mutation invalidates the snapshot before factory/I/O.
    const QString staleClone =
        directory.filePath(QStringLiteral("stale-save-as.solidar"));
    bool staleFactoryCalled = false;
    const auto staleCloneResult = Access::runSaveProjectAsCommand(
        productEditor,
        [&](const QString&) -> std::optional<QString> {
          CHECK(Access::pushBodyMarker(productEditor,
                                       "Save As chooser mutation"));
          return staleClone;
        },
        [&](const QString&) {
          staleFactoryCalled = true;
          return uiCloneRequest(kUiSecondCloneProjectId);
        });
    CHECK(!staleCloneResult.succeeded());
    CHECK(!staleFactoryCalled);
    CHECK(!QFile::exists(staleClone));
    CHECK(Access::identity(productEditor) == identityB);

    // A nested successful Save As wins; the stale outer command cannot
    // allocate or publish its request.
    const QString nestedSource =
        directory.filePath(QStringLiteral("nested-source.solidar"));
    solidar::MainWindow nestedEditor(settings);
    CHECK(nestedEditor.loadProject(pathA, &error));
    CHECK(Access::convertProjectToPath(
              nestedEditor, nestedSource,
              uiConversionRequest(Access::rootDefinitionId(nestedEditor),
                                  QStringLiteral("nested A")))
              .succeeded());
    const QString nestedWinner =
        directory.filePath(QStringLiteral("nested-winner.solidar"));
    const QString staleOuter =
        directory.filePath(QStringLiteral("stale-outer-save-as.solidar"));
    bool outerFactoryCalled = false;
    const auto outerSave = Access::runSaveProjectAsCommand(
        nestedEditor,
        [&](const QString&) -> std::optional<QString> {
          CHECK(Access::saveProductCloneToPath(
                    nestedEditor, nestedWinner, uiCloneRequest())
                    .succeeded());
          return staleOuter;
        },
        [&](const QString&) {
          outerFactoryCalled = true;
          return uiCloneRequest(kUiSecondCloneProjectId);
        });
    CHECK(!outerSave.succeeded());
    CHECK(!outerFactoryCalled);
    CHECK(QFile::exists(nestedWinner));
    CHECK(!QFile::exists(staleOuter));
    CHECK(nestedEditor.windowFilePath() == nestedWinner);
    CHECK(Access::identity(nestedEditor)->projectId == kUiCloneProjectId);

    // Loading a replacement from the chooser also invalidates the outer
    // command before request generation.
    const QString replacementOuter =
        directory.filePath(QStringLiteral("replacement-outer.solidar"));
    bool replacementFactoryCalled = false;
    const auto replacementResult = Access::runSaveProjectAsCommand(
        nestedEditor,
        [&](const QString&) -> std::optional<QString> {
          CHECK(nestedEditor.loadProject(pathA, &error));
          return replacementOuter;
        },
        [&](const QString&) {
          replacementFactoryCalled = true;
          return uiCloneRequest(kUiSecondCloneProjectId);
        });
    CHECK(!replacementResult.succeeded());
    CHECK(!replacementFactoryCalled);
    CHECK(!QFile::exists(replacementOuter));
    CHECK(Access::persistenceState(nestedEditor) == v3);

    // Active tools are rejected before the chooser and remain untouched.
    solidar::MainWindow activeSaveAsEditor(settings);
    CHECK(activeSaveAsEditor.loadProject(nestedWinner, &error));
    CHECK(Access::startTopFaceExtrude(activeSaveAsEditor, 15.0));
    bool activeChooserCalled = false;
    const auto activeSaveRejected = Access::runSaveProjectAsCommand(
        activeSaveAsEditor,
        [&](const QString&) -> std::optional<QString> {
          activeChooserCalled = true;
          return directory.filePath(QStringLiteral("active-save-as.solidar"));
        },
        [](const QString&) { return uiCloneRequest(kUiSecondCloneProjectId); });
    CHECK(!activeSaveRejected.succeeded());
    CHECK(!activeChooserCalled);
    CHECK(Access::activeTool(activeSaveAsEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::viewportHasToolPreview(activeSaveAsEditor));
    Access::cancelFaceExtrude(activeSaveAsEditor);

    // Destruction inside the chooser must not access the dead receiver or
    // invoke the factory.
    auto* closingSaveAsEditor = new solidar::MainWindow(settings);
    CHECK(closingSaveAsEditor->loadProject(productBPath, &error));
    QPointer<solidar::MainWindow> closingSaveAsGuard(closingSaveAsEditor);
    bool closingFactoryCalled = false;
    const QString closingTarget =
        directory.filePath(QStringLiteral("closed-save-as.solidar"));
    const auto closingResult = Access::runSaveProjectAsCommand(
        *closingSaveAsEditor,
        [&](const QString&) -> std::optional<QString> {
          delete closingSaveAsEditor;
          closingSaveAsEditor = nullptr;
          return closingTarget;
        },
        [&](const QString&) {
          closingFactoryCalled = true;
          return uiCloneRequest(kUiSecondCloneProjectId);
        });
    CHECK(closingSaveAsGuard.isNull());
    CHECK(!closingResult.succeeded());
    CHECK(!closingFactoryCalled);
    CHECK(!QFile::exists(closingTarget));

    // Relocate is deliberately narrower than Save As: only a clean V5
    // carrier can move, and success changes only its locator.
    solidar::MainWindow relocationEditor(settings);
    CHECK(relocationEditor.loadProject(productBPath, &error));
    auto* relocationAction =
        Access::relocateProductDocumentAction(relocationEditor);
    CHECK(relocationAction);
    CHECK(relocationAction->objectName() ==
          QStringLiteral("relocateProductDocumentAction"));
    CHECK(relocationAction->text() ==
          QString::fromUtf8("Переместить документ изделия…"));
    CHECK(relocationAction->isEnabled());
    CHECK(Access::productRelocationIsEligible(relocationEditor));
    Access::selectBody(relocationEditor,
                       Access::firstBodyId(relocationEditor));
    const auto cleanRelocationState = Access::relocationState(relocationEditor);

    bool relocationExecutorCalled = false;
    bool relocationAttempted = true;
    const QString relocationPathBefore = relocationEditor.windowFilePath();
    const auto relocationIdentityBefore = Access::identity(relocationEditor);
    const auto relocationRevisionBefore =
        Access::currentHistoryRevision(relocationEditor);
    const auto relocationCancelled = Access::runProductRelocationCommand(
        relocationEditor,
        [](const QString&) -> std::optional<QString> { return std::nullopt; },
        [&](const QString&, const QString&,
            const solidar::ProductRelocationPrecondition&) {
          relocationExecutorCalled = true;
          return solidar::ProjectApplicationStatus{};
        },
        &relocationAttempted);
    CHECK(relocationCancelled.succeeded());
    CHECK(!relocationAttempted);
    CHECK(!relocationExecutorCalled);
    CHECK(relocationEditor.windowFilePath() == relocationPathBefore);
    CHECK(Access::identity(relocationEditor) == relocationIdentityBefore);
    CHECK(Access::currentHistoryRevision(relocationEditor) ==
          relocationRevisionBefore);
    CHECK(Access::exactRelocationState(relocationEditor,
                                       cleanRelocationState));

    relocationEditor.setWindowModified(true);
    Access::refreshPersistenceActions(relocationEditor);
    CHECK(!relocationAction->isEnabled());
    bool dirtyChooserCalled = false;
    const auto dirtyRejected = Access::runProductRelocationCommand(
        relocationEditor,
        [&](const QString&) -> std::optional<QString> {
          dirtyChooserCalled = true;
          return directory.filePath(QStringLiteral("dirty-relocation.solidar"));
        },
        [&](const QString&, const QString&,
            const solidar::ProductRelocationPrecondition&) {
          relocationExecutorCalled = true;
          return solidar::ProjectApplicationStatus{};
        });
    CHECK(!dirtyRejected.succeeded());
    CHECK(!dirtyChooserCalled);
    relocationEditor.setWindowModified(false);
    Access::refreshPersistenceActions(relocationEditor);
    CHECK(relocationAction->isEnabled());

    bool sameAttempted = false;
    const auto sameRelocation = Access::runProductRelocationCommand(
        relocationEditor,
        [&](const QString&) -> std::optional<QString> { return productBPath; },
        [&](const QString&, const QString&,
            const solidar::ProductRelocationPrecondition&) {
          relocationExecutorCalled = true;
          return solidar::ProjectApplicationStatus{};
        },
        &sameAttempted);
    CHECK(sameRelocation.code ==
          solidar::ProjectApplicationErrorCode::RelocateSameFile);
    CHECK(sameAttempted);
    CHECK(!relocationExecutorCalled);
    CHECK(relocationEditor.windowFilePath() == productBPath);
    CHECK(Access::exactRelocationState(relocationEditor,
                                       cleanRelocationState));

    const QString failedRelocationPath =
        directory.filePath(QStringLiteral("failed-relocation.solidar"));
    const auto failedRelocation = Access::runProductRelocationCommand(
        relocationEditor,
        [&](const QString&) -> std::optional<QString> {
          return failedRelocationPath;
        },
        [&](const QString& source, const QString& target,
            const solidar::ProductRelocationPrecondition& precondition) {
          relocationExecutorCalled = true;
          CHECK(source == productBPath);
          CHECK(target == failedRelocationPath);
          CHECK(precondition.sourceIsClean);
          CHECK(!precondition.historicalLegacyExtrusionSourceSketchId);
          return solidar::ProjectApplicationStatus{
              solidar::ProjectApplicationErrorCode::RelocateFailed,
              QStringLiteral("injected relocation failure")};
        });
    CHECK(!failedRelocation.succeeded());
    CHECK(relocationExecutorCalled);
    CHECK(relocationEditor.windowFilePath() == productBPath);
    CHECK(Access::exactRelocationState(relocationEditor,
                                       cleanRelocationState));

    bool throwingChooserAttempted = false;
    const auto throwingChooser = Access::runProductRelocationCommand(
        relocationEditor,
        [](const QString&) -> std::optional<QString> {
          throw std::runtime_error("relocation chooser failure");
        },
        [](const QString&, const QString&,
           const solidar::ProductRelocationPrecondition&) {
          return solidar::ProjectApplicationStatus{};
        },
        &throwingChooserAttempted);
    CHECK(throwingChooser.code ==
          solidar::ProjectApplicationErrorCode::UnexpectedFailure);
    CHECK(throwingChooserAttempted);
    CHECK(Access::exactRelocationState(relocationEditor,
                                       cleanRelocationState));

    bool throwingExecutorAttempted = false;
    const auto throwingExecutor = Access::runProductRelocationCommand(
        relocationEditor,
        [&](const QString&) -> std::optional<QString> {
          return directory.filePath(
              QStringLiteral("throwing-relocation.solidar"));
        },
        [](const QString&, const QString&,
           const solidar::ProductRelocationPrecondition&)
            -> solidar::ProjectApplicationStatus {
          throw std::runtime_error("relocation executor failure");
        },
        &throwingExecutorAttempted);
    CHECK(throwingExecutor.code ==
          solidar::ProjectApplicationErrorCode::UnexpectedFailure);
    CHECK(throwingExecutorAttempted);
    CHECK(Access::exactRelocationState(relocationEditor,
                                       cleanRelocationState));

    solidar::MainWindow historyDivergedRelocation(settings);
    CHECK(historyDivergedRelocation.loadProject(productBPath, &error));
    CHECK(Access::pushBodyMarker(historyDivergedRelocation,
                                 "relocation savepoint divergence"));
    historyDivergedRelocation.setWindowModified(false);
    Access::refreshPersistenceActions(historyDivergedRelocation);
    CHECK(!Access::productRelocationIsEligible(historyDivergedRelocation));
    CHECK(!Access::relocateProductDocumentAction(historyDivergedRelocation)
               ->isEnabled());

    solidar::MainWindow staleRelocationEditor(settings);
    CHECK(staleRelocationEditor.loadProject(productBPath, &error));
    bool staleRelocationExecutorCalled = false;
    const auto staleRelocation = Access::runProductRelocationCommand(
        staleRelocationEditor,
        [&](const QString&) -> std::optional<QString> {
          CHECK(Access::pushBodyMarker(staleRelocationEditor,
                                       "relocation chooser mutation"));
          return directory.filePath(QStringLiteral("stale-relocation.solidar"));
        },
        [&](const QString&, const QString&,
            const solidar::ProductRelocationPrecondition&) {
          staleRelocationExecutorCalled = true;
          return solidar::ProjectApplicationStatus{};
        });
    CHECK(staleRelocation.code ==
          solidar::ProjectApplicationErrorCode::RelocateNotEligible);
    CHECK(!staleRelocationExecutorCalled);
    CHECK(staleRelocationEditor.windowFilePath() == productBPath);

    // A document replacement from the chooser wins. The stale outer command
    // performs no I/O and leaves the newly loaded carrier exactly current.
    solidar::MainWindow replacementRelocationEditor(settings);
    CHECK(replacementRelocationEditor.loadProject(productBPath, &error));
    bool replacementRelocationExecutorCalled = false;
    std::optional<Access::RelocationStateSnapshot> replacementState;
    const auto replacementRelocation = Access::runProductRelocationCommand(
        replacementRelocationEditor,
        [&](const QString&) -> std::optional<QString> {
          CHECK(replacementRelocationEditor.loadProject(pathA, &error));
          replacementState =
              Access::relocationState(replacementRelocationEditor);
          return directory.filePath(
              QStringLiteral("stale-after-replacement.solidar"));
        },
        [&](const QString&, const QString&,
            const solidar::ProductRelocationPrecondition&) {
          replacementRelocationExecutorCalled = true;
          return solidar::ProjectApplicationStatus{};
        });
    CHECK(replacementRelocation.code ==
          solidar::ProjectApplicationErrorCode::RelocateNotEligible);
    CHECK(!replacementRelocationExecutorCalled);
    CHECK(replacementState.has_value());
    CHECK(Access::exactRelocationState(replacementRelocationEditor,
                                       *replacementState));

    // A nested valid command may commit once while the outer chooser is open;
    // the outer snapshot then becomes stale and cannot invoke a second
    // filesystem executor.
    solidar::MainWindow nestedRelocationEditor(settings);
    CHECK(nestedRelocationEditor.loadProject(productBPath, &error));
    const QString nestedRelocationTarget =
        directory.filePath(QStringLiteral("nested-relocation.solidar"));
    const QString staleOuterRelocationTarget =
        directory.filePath(QStringLiteral("stale-outer-relocation.solidar"));
    int nestedRelocationExecutorCalls = 0;
    bool outerRelocationExecutorCalled = false;
    const auto staleOuterRelocation = Access::runProductRelocationCommand(
        nestedRelocationEditor,
        [&](const QString&) -> std::optional<QString> {
          const auto nestedRelocation = Access::runProductRelocationCommand(
              nestedRelocationEditor,
              [&](const QString&) -> std::optional<QString> {
                return nestedRelocationTarget;
              },
              [&](const QString&, const QString&,
                  const solidar::ProductRelocationPrecondition&) {
                ++nestedRelocationExecutorCalls;
                return solidar::ProjectApplicationStatus{};
              });
          CHECK(nestedRelocation.succeeded());
          return staleOuterRelocationTarget;
        },
        [&](const QString&, const QString&,
            const solidar::ProductRelocationPrecondition&) {
          outerRelocationExecutorCalled = true;
          return solidar::ProjectApplicationStatus{};
        });
    CHECK(staleOuterRelocation.code ==
          solidar::ProjectApplicationErrorCode::RelocateNotEligible);
    CHECK(nestedRelocationExecutorCalls == 1);
    CHECK(!outerRelocationExecutorCalled);
    CHECK(nestedRelocationEditor.windowFilePath() == nestedRelocationTarget);

    const auto* const relocationDocument =
        Access::documentAddress(relocationEditor);
    const auto* const relocationBody =
        Access::firstBodyAddress(relocationEditor);
    const auto* const relocationFeature =
        Access::firstFeatureAddress(relocationEditor);
    const auto* const relocationShape =
        Access::firstShapeAddress(relocationEditor);
    const auto* const relocationTopology =
        Access::firstTopologyAddress(relocationEditor);
    const auto relocationSelection =
        Access::selectionState(relocationEditor);
    const auto relocationUndo = Access::undoCount(relocationEditor);
    const auto relocationRedo = Access::redoCount(relocationEditor);
    const auto relocationHistory =
        Access::currentHistoryRevision(relocationEditor);
    const auto relocationPreview =
        Access::previewDocumentGeneration(relocationEditor);
    const bool relocationSavepoint =
        Access::historyAtSavepoint(relocationEditor);
    const QString relocatedBase =
        directory.filePath(QString::fromUtf8("relocated-product-Ω"));
    const QString relocatedPath = relocatedBase + QStringLiteral(".solidar");
    QString relocationSuggestion;
    relocationExecutorCalled = false;
    const auto relocationSucceeded = Access::runProductRelocationCommand(
        relocationEditor,
        [&](const QString& suggestion) -> std::optional<QString> {
          relocationSuggestion = suggestion;
          return relocatedBase;
        },
        [&](const QString& source, const QString& target,
            const solidar::ProductRelocationPrecondition& precondition) {
          relocationExecutorCalled = true;
          CHECK(source == productBPath);
          CHECK(target == relocatedPath);
          CHECK(precondition.sourceIsClean);
          CHECK(!precondition.historicalLegacyExtrusionSourceSketchId);
          return solidar::ProjectApplicationStatus{};
        });
    CHECK(relocationSucceeded.succeeded());
    CHECK(relocationExecutorCalled);
    CHECK(relocationSuggestion.endsWith(
        QString::fromUtf8("save-as-product-b-Ω-перемещено.solidar")));
    CHECK(relocationEditor.windowFilePath() == relocatedPath);
    CHECK(Access::identity(relocationEditor) == relocationIdentityBefore);
    CHECK(Access::documentAddress(relocationEditor) == relocationDocument);
    CHECK(Access::firstBodyAddress(relocationEditor) == relocationBody);
    CHECK(Access::firstFeatureAddress(relocationEditor) == relocationFeature);
    CHECK(Access::firstShapeAddress(relocationEditor) == relocationShape);
    CHECK(Access::firstTopologyAddress(relocationEditor) == relocationTopology);
    CHECK(Access::exactSelectionState(
        Access::selectionState(relocationEditor), relocationSelection));
    CHECK(Access::undoCount(relocationEditor) == relocationUndo);
    CHECK(Access::redoCount(relocationEditor) == relocationRedo);
    CHECK(Access::currentHistoryRevision(relocationEditor) ==
          relocationHistory);
    CHECK(Access::previewDocumentGeneration(relocationEditor) ==
          relocationPreview);
    CHECK(Access::historyAtSavepoint(relocationEditor) == relocationSavepoint);
    CHECK(!Access::modified(relocationEditor));
    CHECK(relocationAction->isEnabled());

    solidar::MainWindow activeRelocationEditor(settings);
    CHECK(activeRelocationEditor.loadProject(nestedWinner, &error));
    CHECK(Access::startTopFaceExtrude(activeRelocationEditor, 15.0));
    CHECK(!Access::relocateProductDocumentAction(activeRelocationEditor)
               ->isEnabled());
    bool activeRelocationChooserCalled = false;
    const auto activeRelocationRejected =
        Access::runProductRelocationCommand(
            activeRelocationEditor,
            [&](const QString&) -> std::optional<QString> {
              activeRelocationChooserCalled = true;
              return directory.filePath(
                  QStringLiteral("active-relocation.solidar"));
            },
            [](const QString&, const QString&,
               const solidar::ProductRelocationPrecondition&) {
              return solidar::ProjectApplicationStatus{};
            });
    CHECK(!activeRelocationRejected.succeeded());
    CHECK(!activeRelocationChooserCalled);
    CHECK(Access::activeTool(activeRelocationEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::viewportHasToolPreview(activeRelocationEditor));
    Access::cancelFaceExtrude(activeRelocationEditor);
    CHECK(Access::relocateProductDocumentAction(activeRelocationEditor)
              ->isEnabled());

    auto* closingRelocationEditor = new solidar::MainWindow(settings);
    CHECK(closingRelocationEditor->loadProject(productBPath, &error));
    QPointer<solidar::MainWindow> closingRelocationGuard(
        closingRelocationEditor);
    bool closingRelocationExecutorCalled = false;
    const auto closingRelocationResult =
        Access::runProductRelocationCommand(
            *closingRelocationEditor,
            [&](const QString&) -> std::optional<QString> {
              delete closingRelocationEditor;
              closingRelocationEditor = nullptr;
              return directory.filePath(
                  QStringLiteral("closed-relocation.solidar"));
            },
            [&](const QString&, const QString&,
                const solidar::ProductRelocationPrecondition&) {
              closingRelocationExecutorCalled = true;
              return solidar::ProjectApplicationStatus{};
            });
    CHECK(closingRelocationGuard.isNull());
    CHECK(!closingRelocationResult.succeeded());
    CHECK(!closingRelocationExecutorCalled);

    auto* executorClosingEditor = new solidar::MainWindow(settings);
    CHECK(executorClosingEditor->loadProject(productBPath, &error));
    QPointer<solidar::MainWindow> executorClosingGuard(executorClosingEditor);
    bool executorClosingCalled = false;
    const auto executorClosingResult = Access::runProductRelocationCommand(
        *executorClosingEditor,
        [&](const QString&) -> std::optional<QString> {
          return directory.filePath(
              QStringLiteral("executor-closed-relocation.solidar"));
        },
        [&](const QString&, const QString&,
            const solidar::ProductRelocationPrecondition&) {
          executorClosingCalled = true;
          delete executorClosingEditor;
          executorClosingEditor = nullptr;
          return solidar::ProjectApplicationStatus{};
        });
    CHECK(executorClosingCalled);
    CHECK(executorClosingGuard.isNull());
    CHECK(executorClosingResult.code ==
          solidar::ProjectApplicationErrorCode::RelocateNotEligible);
  }

  // Product v5 is activated only by the editor boundary. Service callers keep
  // the conservative default, staging is allocation-neutral, and the single
  // document-replacement commit reserves persisted IDs for later editing.
  [&] {
    const auto cursorBefore = consumeBodyId("before-v5-ui-stage");
    const auto persistedBodyId = cursorBefore + 1000;
    const auto persistedFeatureId =
        static_cast<solidar::FeatureId>(persistedBodyId + 1);

    solidar::Document sourceDocument;
    {
      solidar::detail::ScopedExplicitIdReservationPause pause;
      auto& body = sourceDocument.addBody(persistedBodyId, "v5 UI root");
      body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
          persistedFeatureId,
          std::make_shared<const TopoDS_Shape>(
              BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
          "v5 UI source"));
    }
    CHECK(sourceDocument.recompute());
    auto sourceProduct = uiV5Product(std::move(sourceDocument));
    const auto expectedIdentity = uiV5Identity();
    const QString v5Path =
        directory.filePath(QStringLiteral("editor-product-v5.solidar"));
    solidar::ProjectApplicationService service;
    CHECK(service.save(v5Path, *sourceProduct).succeeded());

    const auto defaultStage = service.stageOpen(v5Path);
    CHECK(!defaultStage.succeeded());
    CHECK(defaultStage.status.code ==
          solidar::ProjectApplicationErrorCode::UnsupportedProject);
    CHECK(!defaultStage.staged);

    auto staged = service.stageOpen(
        v5Path, solidar::ProductV5OpenPolicy::Allow);
    CHECK(staged.succeeded());
    CHECK(staged.staged->productDocument().identity() == expectedIdentity);
    CHECK(staged.staged->productDocument().rootDefinitionId() ==
          kUiV5RootDefinition);
    const auto* const stagedDocument = &staged.staged->rootDocument();
    const auto cursorAfterStage = consumeBodyId("after-v5-ui-stage");
    CHECK(cursorAfterStage == cursorBefore + 1);

    solidar::MainWindow stagedCommitEditor(settings);
    Access::commitStaged(stagedCommitEditor, std::move(*staged.staged),
                         v5Path);
    CHECK(Access::documentAddress(stagedCommitEditor) == stagedDocument);
    CHECK(Access::identity(stagedCommitEditor) == expectedIdentity);
    CHECK(Access::rootDefinitionId(stagedCommitEditor) ==
          kUiV5RootDefinition);
    const auto cursorAfterCommit = consumeBodyId("after-v5-ui-commit");
    CHECK(cursorAfterCommit == persistedBodyId + 1);

    // EditorFactory exercises the public Home-to-editor route: MainWindow is
    // the component that explicitly opts into v5 and commits a clean product.
    QString factoryError;
    std::unique_ptr<QMainWindow> editorShell(
        solidar::createEditorWindow(v5Path, settings, &factoryError));
    CHECK(editorShell);
    auto* productEditor = dynamic_cast<solidar::MainWindow*>(editorShell.get());
    CHECK(productEditor);
    CHECK(factoryError.isEmpty());
    CHECK(productEditor->windowFilePath() == v5Path);
    CHECK(Access::persistenceState(*productEditor) ==
          solidar::project::ProjectPersistenceState{
              solidar::project::ProjectPersistenceFormat::V5});
    CHECK(Access::identity(*productEditor) == expectedIdentity);
    CHECK(Access::rootDefinitionId(*productEditor) == kUiV5RootDefinition);
    CHECK(Access::definitionCount(*productEditor) == 1);
    CHECK(Access::bodyCount(*productEditor) == 1);
    CHECK(Access::firstBodyId(*productEditor) == persistedBodyId);
    CHECK(!Access::modified(*productEditor));

    auto* upgradeAction = Access::upgradeProjectFormatAction(*productEditor);
    auto* loadedProductConvertAction =
        Access::convertToProductDocumentAction(*productEditor);
    CHECK(upgradeAction);
    CHECK(loadedProductConvertAction);
    CHECK(!upgradeAction->isEnabled());
    CHECK(!loadedProductConvertAction->isEnabled());
    const auto upgradeIdentityBefore = *Access::identity(*productEditor);
    const auto upgradePathBefore = productEditor->windowFilePath();
    const auto upgradeUndoBefore = Access::undoCount(*productEditor);
    const auto upgradeHistoryBefore = Access::historyStepCount(*productEditor);
    const bool upgradeDirtyBefore = Access::modified(*productEditor);
    Access::upgradeProjectFormat(*productEditor);
    CHECK(Access::persistenceState(*productEditor) ==
          solidar::project::ProjectPersistenceState{
              solidar::project::ProjectPersistenceFormat::V5});
    CHECK(Access::identity(*productEditor) == upgradeIdentityBefore);
    CHECK(productEditor->windowFilePath() == upgradePathBefore);
    CHECK(Access::undoCount(*productEditor) == upgradeUndoBefore);
    CHECK(Access::historyStepCount(*productEditor) == upgradeHistoryBefore);
    CHECK(Access::modified(*productEditor) == upgradeDirtyBefore);
    CHECK(!upgradeAction->isEnabled());

    QFile authored(v5Path);
    CHECK(authored.open(QIODevice::ReadOnly));
    const auto authoredBytes = authored.readAll();
    authored.close();
    auto invalidRoot = QJsonDocument::fromJson(authoredBytes).object();
    invalidRoot.insert(QStringLiteral("rootDefinitionId"),
                       static_cast<qint64>(kUiV5RootDefinition.value() + 1));
    const QString invalidPath =
        directory.filePath(QStringLiteral("editor-invalid-v5.solidar"));
    CHECK(writeTestBytes(
        invalidPath,
        QJsonDocument(invalidRoot).toJson(QJsonDocument::Compact)));

    auto unsupportedRoot = QJsonDocument::fromJson(authoredBytes).object();
    unsupportedRoot.insert(
        QStringLiteral("capabilities"),
        QJsonArray{QStringLiteral("product-envelope-v1"),
                   QStringLiteral("zzz-future-required-ui-capability")});
    const QString unsupportedPath =
        directory.filePath(QStringLiteral("editor-unsupported-v5.solidar"));
    CHECK(writeTestBytes(
        unsupportedPath,
        QJsonDocument(unsupportedRoot).toJson(QJsonDocument::Compact)));

    CHECK(Access::startTopFaceExtrude(*productEditor, 5.0));
    productEditor->setWindowModified(true);
    const auto* const failureDocument = Access::documentAddress(*productEditor);
    const auto failureIdentity = *Access::identity(*productEditor);
    const auto failurePath = productEditor->windowFilePath();
    const auto failureUndo = Access::undoCount(*productEditor);
    const auto failureHistory = Access::historyStepCount(*productEditor);
    const auto failureSelection = Access::selectionState(*productEditor);
    const auto failureTool = Access::activeTool(*productEditor);
    const bool failurePreview = Access::viewportHasToolPreview(*productEditor);
    CHECK(failureTool == solidar::PartDesignToolKind::Extrude);
    CHECK(failurePreview);

    QString invalidError;
    CHECK(!productEditor->loadProject(invalidPath, &invalidError));
    CHECK(invalidError.contains(QString::fromUtf8("повреждён")));
    CHECK(Access::documentAddress(*productEditor) == failureDocument);
    CHECK(Access::identity(*productEditor) == failureIdentity);
    CHECK(productEditor->windowFilePath() == failurePath);
    CHECK(Access::modified(*productEditor));
    CHECK(Access::undoCount(*productEditor) == failureUndo);
    CHECK(Access::historyStepCount(*productEditor) == failureHistory);
    CHECK(Access::activeTool(*productEditor) == failureTool);
    CHECK(Access::viewportHasToolPreview(*productEditor) == failurePreview);
    CHECK(Access::exactSelectionState(Access::selectionState(*productEditor),
                                      failureSelection));

    QString unsupportedError;
    CHECK(!productEditor->loadProject(unsupportedPath, &unsupportedError));
    CHECK(unsupportedError.contains(QString::fromUtf8("не поддерж")));
    CHECK(invalidError != unsupportedError);
    CHECK(Access::documentAddress(*productEditor) == failureDocument);
    CHECK(Access::identity(*productEditor) == failureIdentity);
    CHECK(productEditor->windowFilePath() == failurePath);
    CHECK(Access::modified(*productEditor));
    CHECK(Access::undoCount(*productEditor) == failureUndo);
    CHECK(Access::historyStepCount(*productEditor) == failureHistory);
    CHECK(Access::activeTool(*productEditor) == failureTool);
    CHECK(Access::viewportHasToolPreview(*productEditor) == failurePreview);
    CHECK(Access::exactSelectionState(Access::selectionState(*productEditor),
                                      failureSelection));
    Access::cancelFaceExtrude(*productEditor);

    // Save failure leaves the authoritative v5 carrier and editor state
    // untouched; successful Save then round-trips an actual model change.
    CHECK(Access::pushBodyMarker(*productEditor, "v5 UI saved edit"));
    CHECK(Access::modified(*productEditor));
    const auto saveIdentityBefore = *Access::identity(*productEditor);
    const auto saveUndoBefore = Access::undoCount(*productEditor);
    const auto saveHistoryBefore = Access::historyStepCount(*productEditor);
    const auto saveBodiesBefore = Access::bodyCount(*productEditor);
    const QString failedSavePath =
        directory.filePath(QStringLiteral("missing/v5-ui-save.solidar"));
    const QString savePathBefore = productEditor->windowFilePath();
    QString saveError;
    CHECK(!Access::saveProjectToPath(*productEditor, failedSavePath,
                                     &saveError));
    CHECK(!saveError.isEmpty());
    CHECK(productEditor->windowFilePath() == savePathBefore);
    CHECK(Access::identity(*productEditor) == saveIdentityBefore);
    CHECK(Access::undoCount(*productEditor) == saveUndoBefore);
    CHECK(Access::historyStepCount(*productEditor) == saveHistoryBefore);
    CHECK(Access::bodyCount(*productEditor) == saveBodiesBefore);
    CHECK(Access::modified(*productEditor));
    CHECK(!QFile::exists(failedSavePath));

    CHECK(Access::saveProjectToPath(*productEditor, v5Path, &saveError));
    CHECK(saveError.isEmpty());
    CHECK(!Access::modified(*productEditor));
    CHECK(Access::identity(*productEditor) == expectedIdentity);
    auto savedStage = service.stageOpen(
        v5Path, solidar::ProductV5OpenPolicy::Allow);
    CHECK(savedStage.succeeded());
    CHECK(savedStage.staged->productDocument().identity() == expectedIdentity);
    CHECK(savedStage.staged->productDocument().rootDefinitionId() ==
          kUiV5RootDefinition);
    CHECK(savedStage.staged->rootDocument().bodies().size() ==
          saveBodiesBefore);
  }();

  // Keep this large scenario in a separate MSVC Debug stack frame. Folding
  // all MainWindow fixtures into main's RTC frame exhausts the 1 MiB reserve.
  [&] {
  // A5a commits a fully prepared V6 read-only workspace only after the modal
  // seam. Active part tools/history are torn down while their old model lives;
  // failed preparation remains an exact no-op and does not consume an epoch.
  {
    const QString assemblyPath =
        QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH);
    solidar::MainWindow assemblyEditor(settings);
    CHECK(assemblyEditor.loadProject(pathA, &error));
    CHECK(Access::setFirstBodyVisible(assemblyEditor, false));
    CHECK(Access::setFirstBodyVisible(assemblyEditor, true));
    CHECK(Access::startTopFaceExtrude(assemblyEditor, 15.0));
    Access::dragDirectExtrude(assemblyEditor, 3.0);
    CHECK(Access::directExtrudeLifecycle(assemblyEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(assemblyEditor));
    // Keep the active PartDesign session while exposing the other dangerous
    // replacement seam: SketchCanvas is current and a reference-image edit,
    // HUD manipulator and pending import are all live when assembly commit
    // changes the authoritative product root.
    Access::primeSketchReferenceTransientForAssemblySwitch(assemblyEditor);
    CHECK(Access::currentWorkspaceWidget(assemblyEditor) ==
          Access::sketch(assemblyEditor));
    CHECK(Access::viewport(assemblyEditor)->referenceImageManipulatorActive());
    assemblyEditor.setWindowModified(true);

    int assemblyConfirmationCount = 0;
    QString assemblyCommandError;
    const auto assemblyCommand = Access::runOpenProjectCommand(
        assemblyEditor, assemblyPath,
        [&](solidar::MainWindow&) {
          ++assemblyConfirmationCount;
          return true;
        },
        &assemblyCommandError);
    CHECK(assemblyCommand == Access::OpenProjectTestOutcome::Opened);
    CHECK(assemblyConfirmationCount == 1);
    CHECK(assemblyCommandError.isEmpty());
    CHECK(Access::projectKind(assemblyEditor) ==
          solidar::EditorProjectKind::AssemblyReadOnly);
    CHECK(Access::rootDefinitionId(assemblyEditor).isValid());
    CHECK(Access::hasAssemblySession(assemblyEditor));
    CHECK(Access::assemblyTreeRootCount(assemblyEditor) >= 1);
    CHECK(Access::historyStepCount(assemblyEditor) == 0);
    CHECK(Access::undoCount(assemblyEditor) == 0);
    CHECK(Access::redoCount(assemblyEditor) == 0);
    CHECK(Access::activeTool(assemblyEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(!Access::viewportHasToolPreview(assemblyEditor));
    CHECK(Access::assemblyUiHasNoPartTransients(assemblyEditor));
    CHECK(Access::readOnlyActionsDisabled(assemblyEditor));
    CHECK(!assemblyEditor.isWindowModified());
    const auto firstAssemblyEpoch = Access::assemblyEpochClock(assemblyEditor);
    CHECK(firstAssemblyEpoch != 0);

    // Typed tree -> viewport and viewport -> tree selection uses the exact
    // publication-scoped occurrence identity. Definition rows clear the
    // occurrence selection instead of retaining stale highlight state.
    const QModelIndex firstOccurrenceIndex =
        Access::firstAssemblyOccurrenceIndex(assemblyEditor);
    CHECK(firstOccurrenceIndex.isValid());
    const auto firstOccurrence =
        Access::occurrenceForIndex(assemblyEditor, firstOccurrenceIndex);
    CHECK(firstOccurrence);
    Access::selectAssemblyTreeIndex(assemblyEditor, firstOccurrenceIndex);
    CHECK(Access::viewportAssemblySelection(assemblyEditor) == firstOccurrence);
    Access::selectAssemblyTreeIndex(
        assemblyEditor,
        Access::assemblyTreeCurrentIndex(assemblyEditor).parent());
    CHECK(!Access::viewportAssemblySelection(assemblyEditor));
    CHECK(Access::selectAssemblyOccurrenceFromViewport(assemblyEditor,
                                                        *firstOccurrence));
    CHECK(Access::assemblyTreeCurrentIndex(assemblyEditor) ==
          firstOccurrenceIndex);
    CHECK(Access::clearAssemblyOccurrenceFromViewport(assemblyEditor));
    CHECK(!Access::viewportAssemblySelection(assemblyEditor));
    CHECK(!Access::assemblyTreeCurrentIndex(assemblyEditor).isValid());
    CHECK(Access::selectAssemblyOccurrenceFromViewport(assemblyEditor,
                                                        *firstOccurrence));

    // Assembly A -> assembly B never aliases the old publication, even when
    // local occurrence/definition IDs are identical.
    QString assemblyAgainError;
    CHECK(assemblyEditor.loadProject(assemblyPath, &assemblyAgainError));
    CHECK(assemblyAgainError.isEmpty());
    CHECK(Access::projectKind(assemblyEditor) ==
          solidar::EditorProjectKind::AssemblyReadOnly);
    CHECK(Access::assemblyEpochClock(assemblyEditor) > firstAssemblyEpoch);
    CHECK(!Access::viewportAssemblySelection(assemblyEditor));
    CHECK(!Access::assemblyTreeCurrentIndex(assemblyEditor).isValid());
    const auto secondAssemblyEpoch = Access::assemblyEpochClock(assemblyEditor);
    const QModelIndex secondOccurrenceIndex =
        Access::firstAssemblyOccurrenceIndex(assemblyEditor);
    const auto secondOccurrence =
        Access::occurrenceForIndex(assemblyEditor, secondOccurrenceIndex);
    CHECK(secondOccurrence);
    CHECK(Access::selectAssemblyOccurrenceFromViewport(assemblyEditor,
                                                        *secondOccurrence));
    auto foreignOccurrence = *firstOccurrence;
    CHECK(foreignOccurrence.occurrence.epoch.value() != secondAssemblyEpoch);
    CHECK(!Access::selectAssemblyOccurrenceFromViewport(assemblyEditor,
                                                         foreignOccurrence));
    CHECK(Access::viewportAssemblySelection(assemblyEditor) == secondOccurrence);
    CHECK(Access::assemblyTreeCurrentIndex(assemblyEditor) ==
          secondOccurrenceIndex);

    // A second coordinator deliberately reuses the same numeric epoch,
    // generation and local IDs. Its exact publication token is nevertheless
    // foreign and cannot replace or clear the newer current selection through
    // either the Viewport API or queued signal-shaped MainWindow callbacks.
    {
      auto foreignSession =
          Access::prepareForeignEquivalentAssemblySession(assemblyEditor);
      CHECK(foreignSession);
      CHECK(foreignSession->epoch().value() == secondAssemblyEpoch);
      CHECK(foreignSession->publicationHandle().generation() ==
            Access::assemblySessionAddress(assemblyEditor)
                ->publicationHandle()
                .generation());
      solidar::AssemblyOccurrenceSelection foreignSameNumeric{
          foreignSession->publicationHandle(),
          secondOccurrence->occurrence};
      CHECK(!Access::selectAssemblyOccurrenceFromViewport(
          assemblyEditor, foreignSameNumeric));
      Access::emitAssemblyOccurrenceSelected(assemblyEditor,
                                             foreignSameNumeric);
      Access::emitAssemblyOccurrenceSelectionCleared(
          assemblyEditor, foreignSession->publicationHandle());
      CHECK(Access::viewportAssemblySelection(assemblyEditor) ==
            secondOccurrence);
      CHECK(Access::assemblyTreeCurrentIndex(assemblyEditor) ==
            secondOccurrenceIndex);
    }

    const auto* exactAssemblySession =
        Access::assemblySessionAddress(assemblyEditor);
    const QString exactAssemblyPath = assemblyEditor.windowFilePath();
    Access::injectAssemblyPreparationFailure(
        assemblyEditor,
        solidar::AssemblyViewportSessionFailurePoint::FinalPreparation);
    QString assemblyFailureError;
    CHECK(!assemblyEditor.loadProject(assemblyPath, &assemblyFailureError));
    CHECK(!assemblyFailureError.isEmpty());
    CHECK(Access::assemblySessionAddress(assemblyEditor) ==
          exactAssemblySession);
    CHECK(assemblyEditor.windowFilePath() == exactAssemblyPath);
    CHECK(Access::assemblyEpochClock(assemblyEditor) == secondAssemblyEpoch);
    CHECK(Access::viewportAssemblySelection(assemblyEditor) == secondOccurrence);
    Access::injectAssemblyPreparationFailure(
        assemblyEditor, solidar::AssemblyViewportSessionFailurePoint::None);

    // Disabled commands and their direct handlers fail closed.
    const auto guardedSelection =
        Access::viewportAssemblySelection(assemblyEditor);
    const auto* guardedProduct =
        Access::productDocumentAddress(assemblyEditor);
    const auto guardedRoot = Access::rootDefinitionId(assemblyEditor);
    const auto guardedDefinitionCount =
        Access::definitionCount(assemblyEditor);
    const auto guardedUndoCount = Access::undoCount(assemblyEditor);
    const auto guardedRedoCount = Access::redoCount(assemblyEditor);
    const auto guardedHistoryCount =
        Access::historyStepCount(assemblyEditor);
    Access::exerciseReadOnlyMutationGuards(assemblyEditor);
    Access::exerciseReadOnlyQtCallbacks(assemblyEditor);
    Access::saveProject(assemblyEditor);
    CHECK(Access::assemblyEpochClock(assemblyEditor) == secondAssemblyEpoch);
    CHECK(Access::projectKind(assemblyEditor) ==
          solidar::EditorProjectKind::AssemblyReadOnly);
    CHECK(Access::assemblySessionAddress(assemblyEditor) ==
          exactAssemblySession);
    CHECK(Access::productDocumentAddress(assemblyEditor) == guardedProduct);
    CHECK(Access::rootDefinitionId(assemblyEditor) == guardedRoot);
    CHECK(Access::definitionCount(assemblyEditor) ==
          guardedDefinitionCount);
    CHECK(Access::undoCount(assemblyEditor) == guardedUndoCount);
    CHECK(Access::redoCount(assemblyEditor) == guardedRedoCount);
    CHECK(Access::historyStepCount(assemblyEditor) == guardedHistoryCount);
    CHECK(Access::viewportAssemblySelection(assemblyEditor) ==
          guardedSelection);
    CHECK(Access::assemblyTreeCurrentIndex(assemblyEditor) ==
          secondOccurrenceIndex);
    CHECK(Access::assemblyUiHasNoPartTransients(assemblyEditor));
    CHECK(Access::readOnlyActionsDisabled(assemblyEditor));
    CHECK(assemblyEditor.windowFilePath() == exactAssemblyPath);
    CHECK(!assemblyEditor.isWindowModified());

    // Assembly -> part restores the complete part workspace.
    QString partAgainError;
    CHECK(assemblyEditor.loadProject(pathB, &partAgainError));
    CHECK(partAgainError.isEmpty());
    CHECK(Access::projectKind(assemblyEditor) ==
          solidar::EditorProjectKind::Part);
    CHECK(!Access::hasAssemblySession(assemblyEditor));
    CHECK(Access::rootDefinitionDocumentAddress(assemblyEditor));

    // Every deterministic prepare failure preserves the exact active part and
    // does not advance the product presentation epoch.
    const auto partBeforeFailure = Access::relocationState(assemblyEditor);
    const auto epochBeforeFailure = Access::assemblyEpochClock(assemblyEditor);
    Access::injectAssemblyPreparationFailure(
        assemblyEditor,
        solidar::AssemblyViewportSessionFailurePoint::Picking);
    QString failureError;
    CHECK(!assemblyEditor.loadProject(assemblyPath, &failureError));
    CHECK(!failureError.isEmpty());
    CHECK(Access::exactRelocationState(assemblyEditor, partBeforeFailure));
    CHECK(Access::assemblyEpochClock(assemblyEditor) == epochBeforeFailure);
    Access::injectAssemblyPreparationFailure(
        assemblyEditor,
        solidar::AssemblyViewportSessionFailurePoint::None);

    QString factoryAssemblyError;
    std::unique_ptr<QMainWindow> assemblyFactoryEditor(
        solidar::createEditorWindow(assemblyPath, settings,
                                    &factoryAssemblyError));
    CHECK(assemblyFactoryEditor);
    CHECK(factoryAssemblyError.isEmpty());
  }

  runA6b2EditorPersistenceScenario(directory, settings, pathA);


  // A5d edits an embedded PartDefinition without transferring ownership away
  // from ProductDocument.  Navigation is presentation-only; authored changes
  // use the single assembly history and preserve the suspended assembly camera
  // and authoritative Document address across Apply/Undo/Redo and Back.
  {
    const QString assemblyPath = definitionMoveAssemblyPath;
    auto definitionEditor =
        std::make_unique<solidar::MainWindow>(settings);
    QString definitionError;
    CHECK(definitionEditor->loadProject(assemblyPath, &definitionError));
    CHECK(definitionError.isEmpty());
    const auto target = Access::firstPartDefinitionId(*definitionEditor);
    CHECK(target.has_value());
    const QModelIndex definitionIndex =
        Access::assemblyDefinitionIndex(*definitionEditor, *target);
    CHECK(definitionIndex.isValid());
    Access::selectAssemblyTreeIndex(*definitionEditor, definitionIndex);
    CHECK(Access::definitionEditEnabled(*definitionEditor));

    const auto* const embeddedAddress =
        Access::embeddedDocumentAddress(*definitionEditor, *target);
    CHECK(embeddedAddress != nullptr);
    const auto* const assemblySession =
        Access::assemblySessionAddress(*definitionEditor);
    CHECK(assemblySession != nullptr);
    const auto epoch = assemblySession->epoch();
    const auto publication = assemblySession->publicationHandle();
    const solidar::ViewportNavigationState assemblyNavigation{
        17.0F, -23.0F, 1.75F, QPointF(31.0, -14.0)};
    Access::setViewportNavigation(*definitionEditor, assemblyNavigation);
    const auto historySizeBefore =
        Access::assemblyHistorySize(*definitionEditor);
    const auto historyRevisionBefore =
        Access::assemblyHistoryRevision(*definitionEditor);

    Access::enterDefinitionEdit(*definitionEditor);
    CHECK(Access::definitionEditActive(*definitionEditor));
    CHECK(!Access::hasAssemblySession(*definitionEditor));
    CHECK(Access::suspendedAssemblySessionAddress(*definitionEditor) ==
          assemblySession);
    CHECK(Access::documentAddress(*definitionEditor) == embeddedAddress);
    CHECK(Access::definitionEditNavigationVisible(*definitionEditor, *target));
    CHECK(Access::definitionEditPersistenceDisabled(*definitionEditor));
    CHECK(Access::definitionEditMoveOnlyAvailable(*definitionEditor));
    CHECK(Access::undoCount(*definitionEditor) == 0);
    CHECK(Access::redoCount(*definitionEditor) == 0);

    // Back is navigation only and must refuse a real UI-started Move session.
    auto* moveCommand = definitionEditor->findChild<QToolButton*>(
        QStringLiteral("moveCommand"));
    auto* moveX = definitionEditor->findChild<QDoubleSpinBox*>(
        QStringLiteral("moveXSpin"));
    auto* moveY = definitionEditor->findChild<QDoubleSpinBox*>(
        QStringLiteral("moveYSpin"));
    auto* moveZ = definitionEditor->findChild<QDoubleSpinBox*>(
        QStringLiteral("moveZSpin"));
    auto* moveApply = definitionEditor->findChild<QPushButton*>(
        QStringLiteral("moveAcceptButton"));
    auto* editDefinitionButton = definitionEditor->findChild<QPushButton*>(
        QStringLiteral("assemblyEditDefinitionCommand"));
    auto* definitionBackButton = definitionEditor->findChild<QPushButton*>(
        QStringLiteral("assemblyDefinitionEditBackCommand"));
    auto* createSketchCommand = definitionEditor->findChild<QToolButton*>(
        QStringLiteral("createSketchCommand"));
    CHECK(moveCommand && moveX && moveY && moveZ && moveApply &&
          editDefinitionButton && definitionBackButton &&
          createSketchCommand);
    // Sketch authoring is intentionally unported at A5d. Prove the real UI
    // command cannot create a working copy instead of synthesizing one in a
    // lifecycle test.
    CHECK(!createSketchCommand->isEnabled());
    CHECK(!Access::sketchWorkingCopyActive(*definitionEditor));
    createSketchCommand->click();
    application.processEvents();
    CHECK(!Access::sketchWorkingCopyActive(*definitionEditor));
    moveCommand->click();
    application.processEvents();
    CHECK(Access::activeTool(*definitionEditor) ==
          solidar::PartDesignToolKind::Move);
    Access::leaveDefinitionEdit(*definitionEditor);
    CHECK(Access::definitionEditActive(*definitionEditor));
    Access::cancelMove(*definitionEditor);

    const auto stateBefore = Access::definitionStateId(*definitionEditor, *target);
    const auto bodyId = embeddedAddress->bodies().front().id();
    const std::size_t featureCountBefore =
        embeddedAddress->bodies().front().features().size();
    moveCommand->click();
    application.processEvents();
    Access::viewport(*definitionEditor)->moveBodyPicked(bodyId);
    moveX->setValue(7.0);
    moveY->setValue(-3.0);
    moveZ->setValue(2.0);
    CHECK(moveApply->isEnabled());
    moveApply->click();
    application.processEvents();
    CHECK(Access::embeddedDocumentAddress(*definitionEditor, *target) ==
          embeddedAddress);
    CHECK(Access::definitionStateId(*definitionEditor, *target) != stateBefore);
    CHECK(embeddedAddress->bodies().front().features().size() ==
          featureCountBefore + 1);
    const solidar::Vector3d expectedMoveOffset{7.0, -3.0, 2.0};
    const auto* committedMove = dynamic_cast<const solidar::MoveFeature*>(
        embeddedAddress->bodies().front().features().back().get());
    CHECK(committedMove &&
          solidar::test::near(committedMove->offsetMm().x,
                              expectedMoveOffset.x, 1e-9) &&
          solidar::test::near(committedMove->offsetMm().y,
                              expectedMoveOffset.y, 1e-9) &&
          solidar::test::near(committedMove->offsetMm().z,
                              expectedMoveOffset.z, 1e-9));
    CHECK(Access::assemblyHistorySize(*definitionEditor) ==
          historySizeBefore + 1);
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          historySizeBefore + 1);
    CHECK(Access::assemblyHistoryRevision(*definitionEditor) !=
          historyRevisionBefore);
    CHECK(Access::undoCount(*definitionEditor) == 0);
    CHECK(definitionEditor->isWindowModified());

    // The same real UI path must reject a candidate recompute failure before
    // history/successor publication and preserve the authoritative B-Rep.
    const auto exactBeforeRejectedMove =
        std::make_unique<solidar::Document>(*embeddedAddress);
    const auto* shapeOwnerBeforeRejectedMove =
        embeddedAddress->bodies().front().activeFeature();
    CHECK(shapeOwnerBeforeRejectedMove);
    const auto shapeBeforeRejectedMove =
        shapeOwnerBeforeRejectedMove->lastValidShape();
    const auto stateBeforeRejectedMove =
        Access::definitionStateId(*definitionEditor, *target);
    const auto sizeBeforeRejectedMove =
        Access::assemblyHistorySize(*definitionEditor);
    const auto cursorBeforeRejectedMove =
        Access::assemblyHistoryCursor(*definitionEditor);
    const auto revisionBeforeRejectedMove =
        Access::assemblyHistoryRevision(*definitionEditor);
    const auto savepointBeforeRejectedMove =
        Access::assemblyHistorySavepoint(*definitionEditor);
    const bool dirtyBeforeRejectedMove = definitionEditor->isWindowModified();
    const auto suspendedPublicationBeforeRejectedMove =
        Access::suspendedAssemblySessionAddress(*definitionEditor)
            ->publicationHandle();
    Access::injectDefinitionMoveRecomputeFailure(*definitionEditor, true);
    moveCommand->click();
    application.processEvents();
    Access::viewport(*definitionEditor)->moveBodyPicked(bodyId);
    moveX->setValue(9.0);
    moveY->setValue(4.0);
    moveZ->setValue(-1.0);
    CHECK(moveApply->isEnabled());
    moveApply->click();
    application.processEvents();
    CHECK(solidar::DocumentDelta::create(*exactBeforeRejectedMove,
                                         *embeddedAddress).empty());
    CHECK(embeddedAddress->bodies().front()
              .activeFeature()->lastValidShape().get() ==
          shapeBeforeRejectedMove.get());
    CHECK(Access::definitionStateId(*definitionEditor, *target) ==
          stateBeforeRejectedMove);
    CHECK(Access::assemblyHistorySize(*definitionEditor) ==
          sizeBeforeRejectedMove);
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          cursorBeforeRejectedMove);
    CHECK(Access::assemblyHistoryRevision(*definitionEditor) ==
          revisionBeforeRejectedMove);
    CHECK(Access::assemblyHistorySavepoint(*definitionEditor) ==
          savepointBeforeRejectedMove);
    CHECK(definitionEditor->isWindowModified() == dirtyBeforeRejectedMove);
    CHECK(Access::suspendedAssemblySessionAddress(*definitionEditor)
              ->publicationHandle()
              .isSameCurrentPublication(
                  suspendedPublicationBeforeRejectedMove));
    CHECK(Access::activeTool(*definitionEditor) ==
          solidar::PartDesignToolKind::Move);
    Access::injectDefinitionMoveRecomputeFailure(*definitionEditor, false);
    Access::cancelMove(*definitionEditor);

    Access::undoAction(*definitionEditor)->trigger();
    CHECK(Access::documentAddress(*definitionEditor) == embeddedAddress);
    CHECK(embeddedAddress->bodies().front().features().size() ==
          featureCountBefore);
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          historySizeBefore);
    CHECK(Access::definitionEditActive(*definitionEditor));
    Access::redoAction(*definitionEditor)->trigger();
    CHECK(Access::documentAddress(*definitionEditor) == embeddedAddress);
    CHECK(embeddedAddress->bodies().front().features().size() ==
          featureCountBefore + 1);
    const auto* redoneMove = dynamic_cast<const solidar::MoveFeature*>(
        embeddedAddress->bodies().front().features().back().get());
    CHECK(redoneMove &&
          solidar::test::near(redoneMove->offsetMm().x,
                              expectedMoveOffset.x, 1e-9) &&
          solidar::test::near(redoneMove->offsetMm().y,
                              expectedMoveOffset.y, 1e-9) &&
          solidar::test::near(redoneMove->offsetMm().z,
                              expectedMoveOffset.z, 1e-9));
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          historySizeBefore + 1);
    CHECK(Access::undoCount(*definitionEditor) == 0);

    Access::leaveDefinitionEdit(*definitionEditor);
    CHECK(!Access::definitionEditActive(*definitionEditor));
    CHECK(Access::assemblySessionAddress(*definitionEditor) != nullptr);
    CHECK(Access::assemblySessionAddress(*definitionEditor)->epoch() == epoch);
    CHECK(!Access::assemblySessionAddress(*definitionEditor)
               ->publicationHandle()
               .isSameCurrentPublication(publication));
    CHECK(Access::assemblyTreeCurrentDefinition(*definitionEditor) == target);
    const auto restoredNavigation =
        Access::viewportNavigation(*definitionEditor);
    CHECK(restoredNavigation.yawDeg == assemblyNavigation.yawDeg);
    CHECK(restoredNavigation.pitchDeg == assemblyNavigation.pitchDeg);
    CHECK(restoredNavigation.zoom == assemblyNavigation.zoom);
    CHECK(restoredNavigation.pan == assemblyNavigation.pan);
    CHECK(Access::embeddedDocumentAddress(*definitionEditor, *target) ==
          embeddedAddress);

    // Undo remains the same assembly command after leaving the edit context;
    // no parallel standalone ModelCommandHistory is created.
    Access::undoAction(*definitionEditor)->trigger();
    CHECK(Access::embeddedDocumentAddress(*definitionEditor, *target) ==
          embeddedAddress);
    CHECK(embeddedAddress->bodies().front().features().size() ==
          featureCountBefore);
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          historySizeBefore);

    // An orphan definition uses the same production entry and Move command.
    // Editing it must not invent an occurrence; Undo/Redo stays in the one
    // assembly history and restores/reapplies its cold document delta.
    constexpr solidar::DefinitionId orphanId{2};
    const auto occurrenceCountBeforeOrphan =
        Access::assemblyOccurrenceCount(*definitionEditor);
    const auto orphanIndex =
        Access::assemblyDefinitionIndex(*definitionEditor, orphanId);
    CHECK(orphanIndex.isValid());
    CHECK(Access::definitionIsOrphan(*definitionEditor, orphanId));
    Access::selectAssemblyTreeIndex(*definitionEditor, orphanIndex);
    CHECK(editDefinitionButton->isEnabled());
    editDefinitionButton->click();
    application.processEvents();
    CHECK(Access::definitionEditActive(*definitionEditor));
    const auto* const orphanDocument =
        Access::embeddedDocumentAddress(*definitionEditor, orphanId);
    CHECK(orphanDocument && !orphanDocument->bodies().empty());
    const auto orphanBefore =
        std::make_unique<solidar::Document>(*orphanDocument);
    const auto orphanBodyId = orphanDocument->bodies().front().id();
    const auto orphanFeaturesBefore =
        orphanDocument->bodies().front().features().size();
    const auto orphanStateBefore =
        Access::definitionStateId(*definitionEditor, orphanId);
    const auto orphanHistoryCursorBefore =
        Access::assemblyHistoryCursor(*definitionEditor);
    const auto orphanShapeBefore =
        orphanDocument->bodies().front().activeFeature()->lastValidShape();
    moveCommand->click();
    application.processEvents();
    Access::viewport(*definitionEditor)->moveBodyPicked(orphanBodyId);
    moveX->setValue(3.0);
    moveY->setValue(1.0);
    moveZ->setValue(-2.0);
    CHECK(moveApply->isEnabled());
    moveApply->click();
    application.processEvents();
    CHECK(Access::assemblyOccurrenceCount(*definitionEditor) ==
          occurrenceCountBeforeOrphan);
    CHECK(Access::definitionIsOrphan(*definitionEditor, orphanId));
    CHECK(Access::embeddedDocumentAddress(*definitionEditor, orphanId) ==
          orphanDocument);
    CHECK(orphanDocument->bodies().front().features().size() ==
          orphanFeaturesBefore + 1);
    CHECK(orphanDocument->bodies().front()
              .activeFeature()->lastValidShape().get() !=
          orphanShapeBefore.get());
    CHECK(Access::definitionStateId(*definitionEditor, orphanId) !=
          orphanStateBefore);
    CHECK(Access::assemblyHistorySize(*definitionEditor) ==
          orphanHistoryCursorBefore + 1);
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          orphanHistoryCursorBefore + 1);
    Access::undoAction(*definitionEditor)->trigger();
    CHECK(solidar::DocumentDelta::create(*orphanBefore, *orphanDocument)
              .empty());
    CHECK(Access::assemblyOccurrenceCount(*definitionEditor) ==
          occurrenceCountBeforeOrphan);
    CHECK(Access::definitionIsOrphan(*definitionEditor, orphanId));
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          orphanHistoryCursorBefore);
    Access::redoAction(*definitionEditor)->trigger();
    CHECK(orphanDocument->bodies().front().features().size() ==
          orphanFeaturesBefore + 1);
    CHECK(Access::assemblyOccurrenceCount(*definitionEditor) ==
          occurrenceCountBeforeOrphan);
    CHECK(Access::definitionIsOrphan(*definitionEditor, orphanId));
    CHECK(Access::assemblyHistoryCursor(*definitionEditor) ==
          orphanHistoryCursorBefore + 1);
    definitionBackButton->click();
    application.processEvents();
    CHECK(!Access::definitionEditActive(*definitionEditor));

    // A successful replacement clears the suspended edit context before the
    // ProductDocument swap.  The replacement stays a Part project.
    Access::selectAssemblyTreeIndex(
        *definitionEditor,
        Access::assemblyDefinitionIndex(*definitionEditor, *target));
    CHECK(editDefinitionButton->isEnabled());
    editDefinitionButton->click();
    application.processEvents();
    CHECK(Access::definitionEditActive(*definitionEditor));
    moveCommand->click();
    application.processEvents();
    Access::viewport(*definitionEditor)->moveBodyPicked(bodyId);
    moveX->setValue(4.0);
    moveY->setValue(-1.0);
    moveZ->setValue(0.5);
    CHECK(Access::activeTool(*definitionEditor) ==
          solidar::PartDesignToolKind::Move);
    CHECK(Access::previewHasPending(*definitionEditor));
    const auto previewBeforeReplacement =
        Access::previewCounters(*definitionEditor);
    using ReplacementStep = solidar::AssemblyInsertTeardownStep;
    auto definitionReplacementTrace = std::make_shared<std::vector<
        std::pair<ReplacementStep, bool>>>();
    Access::setAssemblyTeardownObserver(
        *definitionEditor,
        [definitionReplacementTrace](ReplacementStep step, bool sourceAlive) {
          definitionReplacementTrace->emplace_back(step, sourceAlive);
        });
    CHECK(definitionEditor->loadProject(pathA, &definitionError));
    CHECK(!Access::definitionEditActive(*definitionEditor));
    CHECK(Access::projectKind(*definitionEditor) ==
          solidar::EditorProjectKind::Part);
    Access::dispatchPreviewCadence(*definitionEditor);
    const auto previewAfterReplacement =
        Access::previewCounters(*definitionEditor);
    CHECK(!Access::previewHasPending(*definitionEditor));
    CHECK(previewAfterReplacement.executions ==
          previewBeforeReplacement.executions);
    CHECK(previewAfterReplacement.publications ==
          previewBeforeReplacement.publications);
    CHECK(previewAfterReplacement.invalidations >
          previewBeforeReplacement.invalidations);
    CHECK(previewAfterReplacement.cancellations >
          previewBeforeReplacement.cancellations);
    CHECK(Access::transientToolsInactive(*definitionEditor));
    const std::vector<std::pair<ReplacementStep, bool>>
        expectedDefinitionReplacementTrace{
            {ReplacementStep::InsertCallbacksInvalidated, false},
            {ReplacementStep::InsertControllerCancelled, false},
            {ReplacementStep::InsertSessionCancelled, false},
            {ReplacementStep::PartDesignPrepared, true},
            {ReplacementStep::PartDesignSessionsCancelled, true},
            {ReplacementStep::GhostUiDetached, false},
            {ReplacementStep::AssemblyHistoryCleared, false},
            {ReplacementStep::OldWorkspaceDetached, false},
            {ReplacementStep::ProductReplaced, false},
            {ReplacementStep::WorkspaceInstalled, false}};
    CHECK(*definitionReplacementTrace == expectedDefinitionReplacementTrace);
    Access::setAssemblyTeardownObserver(*definitionEditor, {});
  }

  // A5b Insert prepares its complete successor before publication. A failed
  // projection restores the exact authoritative graph and keeps the staged
  // source/ghost alive; Apply, Undo and Redo then publish fresh epochs while
  // retaining the stable occurrence ID and canonical placement.
  {
    auto teardownTrace = std::make_shared<
        std::vector<std::pair<solidar::AssemblyInsertTeardownStep, bool>>>();
    solidar::MainWindow insertEditor(settings);
    CHECK(Access::createNewAssembly(insertEditor));
    CHECK(insertEditor.windowFilePath().isEmpty());
    CHECK(!insertEditor.isWindowModified());
    CHECK(Access::definitionCount(insertEditor) == 1);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 0);
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    CHECK(Access::assemblyPlacementControlsIdentityAndEnabled(insertEditor,
                                                               true));
    Access::setAssemblyPlacementControls(
        insertEditor, {11.0, -7.0, 3.0}, {15.0, 25.0, 35.0});
    CHECK(Access::assemblyInsertPreviewActive(insertEditor));

    // The real modal Insert route may fail staging while another source is
    // already being placed. The old source allocation and preview token stay
    // exact; only the temporary nested checkpoint is closed.
    const QString corruptInsertPath =
        directory.filePath(QStringLiteral("corrupt-live-insert.solidar"));
    CHECK(writeTestBytes(corruptInsertPath, QByteArray("not-json")));
    const auto* sourceBeforeFailedStage =
        Access::assemblyInsertSourceAddress(insertEditor);
    const auto tokenBeforeFailedStage =
        Access::assemblyInsertPreviewToken(insertEditor);
    QTimer::singleShot(0, [&] {
      auto* dialog = qobject_cast<QFileDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->selectFile(corruptInsertPath);
      CHECK(QMetaObject::invokeMethod(dialog, "accept",
                                      Qt::DirectConnection));
    });
    Access::beginAssemblyInsertUi(insertEditor);
    CHECK(Access::assemblyInsertSourceAddress(insertEditor) ==
          sourceBeforeFailedStage);
    CHECK(Access::assemblyInsertPreviewToken(insertEditor) ==
          tokenBeforeFailedStage);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Placing);
    const auto initialEpoch = Access::assemblyEpochClock(insertEditor);

    Access::injectAssemblyPreparationFailure(
        insertEditor,
        solidar::AssemblyViewportSessionFailurePoint::FinalPreparation);
    Access::applyAssemblyInsert(insertEditor);
    CHECK(Access::definitionCount(insertEditor) == 1);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 0);
    CHECK(Access::assemblyEpochClock(insertEditor) == initialEpoch);
    CHECK(Access::assemblyInsertHistorySize(insertEditor) == 0);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 0);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Placing);
    CHECK(Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(!insertEditor.isWindowModified());

    Access::injectAssemblyPreparationFailure(
        insertEditor, solidar::AssemblyViewportSessionFailurePoint::None);
    Access::applyAssemblyInsert(insertEditor);
    CHECK(Access::definitionCount(insertEditor) == 2);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 1);
    CHECK(Access::assemblyEpochClock(insertEditor) > initialEpoch);
    CHECK(Access::assemblyInsertHistorySize(insertEditor) == 1);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 1);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Inactive);
    CHECK(!Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(Access::assemblyPlacementControlsIdentityAndEnabled(insertEditor,
                                                               false));
    CHECK(insertEditor.isWindowModified());
    const auto appliedEpoch = Access::assemblyEpochClock(insertEditor);
    const auto appliedSelection =
        Access::viewportAssemblySelection(insertEditor);
    CHECK(appliedSelection);
    CHECK(appliedSelection->isCurrentFor(
        Access::assemblySessionAddress(insertEditor)->publicationHandle()));
    const auto appliedPath = appliedSelection->occurrence.path;
    std::weak_ptr<const solidar::AssemblyViewportScene> appliedScene =
        Access::assemblySessionAddress(insertEditor)->scene();
    const auto appliedTransform =
        Access::assemblySessionAddress(insertEditor)
            ->scene()->instances().front().localToWorld;

    Access::undoAssemblyInsert(insertEditor);
    CHECK(Access::definitionCount(insertEditor) == 1);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 0);
    CHECK(Access::assemblyInsertHistorySize(insertEditor) == 1);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 0);
    CHECK(Access::assemblyEpochClock(insertEditor) > appliedEpoch);
    CHECK(!Access::viewportAssemblySelection(insertEditor));
    CHECK(!insertEditor.isWindowModified());
    CHECK(appliedScene.expired());
    const auto undoEpoch = Access::assemblyEpochClock(insertEditor);
    std::weak_ptr<const solidar::AssemblyViewportScene> undoScene =
        Access::assemblySessionAddress(insertEditor)->scene();

    Access::redoAssemblyInsert(insertEditor);
    CHECK(Access::definitionCount(insertEditor) == 2);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 1);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 1);
    CHECK(Access::assemblyEpochClock(insertEditor) > undoEpoch);
    CHECK(undoScene.expired());
    const auto redoneSelection =
        Access::viewportAssemblySelection(insertEditor);
    CHECK(redoneSelection);
    CHECK(redoneSelection->occurrence.path == appliedPath);
    CHECK(redoneSelection->isCurrentFor(
        Access::assemblySessionAddress(insertEditor)->publicationHandle()));
    CHECK(Access::assemblySessionAddress(insertEditor)
              ->scene()->instances().front().localToWorld ==
          appliedTransform);
    CHECK(insertEditor.isWindowModified());

    // Undo/Redo first tear down a live ghost. A failed successor preparation
    // keeps the committed model/history/selection/dirty state exact but never
    // leaves a stale preview attached to an inactive controller.
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    Access::injectAssemblyPreparationFailure(
        insertEditor,
        solidar::AssemblyViewportSessionFailurePoint::FinalPreparation);
    const auto epochBeforeFailedUndo = Access::assemblyEpochClock(insertEditor);
    const auto selectionBeforeFailedUndo =
        Access::viewportAssemblySelection(insertEditor);
    Access::undoAssemblyInsert(insertEditor);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Inactive);
    CHECK(!Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(Access::assemblyPlacementControlsIdentityAndEnabled(insertEditor,
                                                               false));
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 1);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 1);
    CHECK(Access::assemblyEpochClock(insertEditor) == epochBeforeFailedUndo);
    CHECK(Access::viewportAssemblySelection(insertEditor) ==
          selectionBeforeFailedUndo);
    CHECK(insertEditor.isWindowModified());
    Access::injectAssemblyPreparationFailure(
        insertEditor, solidar::AssemblyViewportSessionFailurePoint::None);

    // A new source always resets stale placement fields. Immediate Apply
    // commits exactly the displayed identity, and Undo restores selection of
    // the first occurrence through the fresh publication.
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    CHECK(Access::assemblyPlacementControlsIdentityAndEnabled(insertEditor,
                                                               true));
    const auto firstSelection = Access::viewportAssemblySelection(insertEditor);
    CHECK(firstSelection);
    Access::applyAssemblyInsert(insertEditor);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 2);
    CHECK(Access::assemblySessionAddress(insertEditor)
              ->scene()->instances().back().localToWorld ==
          solidar::RigidTransform3d::identity());
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    Access::undoAssemblyInsert(insertEditor);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 1);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Inactive);
    CHECK(!Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(insertEditor.isWindowModified());
    const auto restoredFirst = Access::viewportAssemblySelection(insertEditor);
    CHECK(restoredFirst);
    CHECK(restoredFirst->occurrence.path == firstSelection->occurrence.path);
    CHECK(restoredFirst->isCurrentFor(
        Access::assemblySessionAddress(insertEditor)->publicationHandle()));
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    Access::injectAssemblyPreparationFailure(
        insertEditor,
        solidar::AssemblyViewportSessionFailurePoint::FinalPreparation);
    const auto epochBeforeFailedRedo = Access::assemblyEpochClock(insertEditor);
    Access::redoAssemblyInsert(insertEditor);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 1);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 1);
    CHECK(Access::assemblyEpochClock(insertEditor) == epochBeforeFailedRedo);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Inactive);
    CHECK(!Access::assemblyInsertPreviewActive(insertEditor));
    const auto selectionAfterFailedRedo =
        Access::viewportAssemblySelection(insertEditor);
    CHECK(selectionAfterFailedRedo);
    CHECK(selectionAfterFailedRedo->occurrence.path ==
          firstSelection->occurrence.path);
    CHECK(insertEditor.isWindowModified());
    Access::injectAssemblyPreparationFailure(
        insertEditor, solidar::AssemblyViewportSessionFailurePoint::None);
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    Access::redoAssemblyInsert(insertEditor);
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 2);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Inactive);
    CHECK(!Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(insertEditor.isWindowModified());

    // A failed Open is staged before any teardown and therefore preserves an
    // active tool and its already-built preview exactly.
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    const auto failedOpenEpoch = Access::assemblyEpochClock(insertEditor);
    QString failedOpenError;
    CHECK(!insertEditor.loadProject(
        directory.filePath(QStringLiteral("missing-a5b-open.solidar")),
        &failedOpenError));
    CHECK(!failedOpenError.isEmpty());
    CHECK(Access::assemblyEpochClock(insertEditor) == failedOpenEpoch);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Placing);
    CHECK(Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 2);

    Access::cancelAssemblyInsert(insertEditor);
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Inactive);
    CHECK(!Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(Access::assemblyOccurrenceCount(insertEditor) == 2);

    // Successful replacement cancels the live Insert source before replacing
    // the ProductDocument and clears the narrow history at the hard boundary.
    CHECK(Access::stageAssemblyInsert(insertEditor, pathA));
    Access::setAssemblyTeardownObserver(
        insertEditor, [teardownTrace](solidar::AssemblyInsertTeardownStep step,
                                     bool sourceAlive) {
          teardownTrace->emplace_back(step, sourceAlive);
        });
    const QString assemblyPath =
        QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH);
    QString replacementError;
    CHECK(insertEditor.loadProject(assemblyPath, &replacementError));
    CHECK(replacementError.isEmpty());
    CHECK(Access::assemblyInsertState(insertEditor) ==
          solidar::AssemblyInsertToolState::Inactive);
    CHECK(!Access::assemblyInsertPreviewActive(insertEditor));
    CHECK(Access::assemblyInsertHistorySize(insertEditor) == 0);
    CHECK(Access::assemblyInsertHistoryCursor(insertEditor) == 0);
    const std::vector<std::pair<solidar::AssemblyInsertTeardownStep, bool>>
        expectedTrace{
            {solidar::AssemblyInsertTeardownStep::InsertCallbacksInvalidated,
             false},
            {solidar::AssemblyInsertTeardownStep::InsertControllerCancelled,
             true},
            {solidar::AssemblyInsertTeardownStep::InsertSessionCancelled,
             true},
            {solidar::AssemblyInsertTeardownStep::PartDesignPrepared, true},
            {solidar::AssemblyInsertTeardownStep::PartDesignSessionsCancelled,
             true},
            {solidar::AssemblyInsertTeardownStep::GhostUiDetached, false},
            {solidar::AssemblyInsertTeardownStep::AssemblyHistoryCleared,
             false},
            {solidar::AssemblyInsertTeardownStep::OldWorkspaceDetached,
             false},
            {solidar::AssemblyInsertTeardownStep::ProductReplaced, false},
            {solidar::AssemblyInsertTeardownStep::WorkspaceInstalled, false}};
    CHECK(*teardownTrace == expectedTrace);
    Access::setAssemblyTeardownObserver(insertEditor, {});
  }

  // Full-projection semantic preparation may allocate after the candidate
  // workspace exists. Its dedicated resource seam must roll the paired model
  // transaction back without publishing any part of that candidate.
  {
    solidar::MainWindow projectionFailureEditor(settings);
    CHECK(Access::createNewAssembly(projectionFailureEditor));
    CHECK(Access::stageAssemblyInsert(projectionFailureEditor, pathA));
    const auto* projectionFailureProduct =
        Access::productDocumentAddress(projectionFailureEditor);
    const auto* projectionFailureSession =
        Access::assemblySessionAddress(projectionFailureEditor);
    const auto projectionFailurePublication =
        projectionFailureSession->publicationHandle();
    const auto* projectionFailureTree =
        Access::assemblyTreeModelAddress(projectionFailureEditor);
    const auto projectionFailureRevision =
        Access::assemblyHistoryRevision(projectionFailureEditor);
    const auto projectionFailureSelection =
        Access::viewportAssemblySelection(projectionFailureEditor);
    Access::injectFullProjectionSelectionResourceFailure(
        projectionFailureEditor, true);
    Access::applyAssemblyInsert(projectionFailureEditor);
    Access::injectFullProjectionSelectionResourceFailure(
        projectionFailureEditor, false);
    CHECK(Access::productDocumentAddress(projectionFailureEditor) ==
          projectionFailureProduct);
    CHECK(Access::definitionCount(projectionFailureEditor) == 1);
    CHECK(Access::assemblyOccurrenceCount(projectionFailureEditor) == 0);
    CHECK(Access::assemblyInsertHistorySize(projectionFailureEditor) == 0);
    CHECK(Access::assemblyInsertHistoryCursor(projectionFailureEditor) == 0);
    CHECK(Access::assemblyHistoryRevision(projectionFailureEditor) ==
          projectionFailureRevision);
    CHECK(Access::assemblySessionAddress(projectionFailureEditor) ==
          projectionFailureSession);
    CHECK(Access::assemblySessionAddress(projectionFailureEditor)
              ->publicationHandle()
              .isSameCurrentPublication(projectionFailurePublication));
    CHECK(Access::assemblyTreeModelAddress(projectionFailureEditor) ==
          projectionFailureTree);
    CHECK(Access::viewportAssemblySelection(projectionFailureEditor) ==
          projectionFailureSelection);
    CHECK(Access::assemblyInsertState(projectionFailureEditor) ==
          solidar::AssemblyInsertToolState::Placing);
    CHECK(Access::assemblyInsertPreviewActive(projectionFailureEditor));
    Access::cancelAssemblyInsert(projectionFailureEditor);
  }

  // A6c1a full projection prepares the exact recursive occurrence/definition
  // selection before publication. Assembly-definition rows survive a command
  // and its cold Undo/Redo without being truncated to a root occurrence.
  {
    solidar::MainWindow nestedSelectionEditor(settings);
    CHECK(Access::installAssemblyProduct(nestedSelectionEditor,
                                         uiNestedAssembly()));
    const auto nestedLeafPath =
        solidar::OccurrencePath::create(
            {solidar::OccurrenceId{1}, solidar::OccurrenceId{7}});
    CHECK(nestedLeafPath);
    CHECK(Access::installFullProjection(
              nestedSelectionEditor,
              solidar::AssemblyCommandSemanticState{*nestedLeafPath.path,
                                                     {}}) ==
          solidar::ProductDocumentSuccessorStatus::Prepared);
    const auto nestedLeafSelection =
        Access::viewportAssemblySelection(nestedSelectionEditor);
    CHECK(nestedLeafSelection);
    CHECK(nestedLeafSelection->occurrence.path == *nestedLeafPath.path);
    CHECK(Access::occurrenceForIndex(
              nestedSelectionEditor,
              Access::assemblyTreeCurrentIndex(nestedSelectionEditor)) ==
          nestedLeafSelection);

    const auto* fullSessionBeforeFailure =
        Access::assemblySessionAddress(nestedSelectionEditor);
    const auto fullPublicationBeforeFailure =
        fullSessionBeforeFailure->publicationHandle();
    const QModelIndex fullIndexBeforeFailure =
        Access::assemblyTreeCurrentIndex(nestedSelectionEditor);
    Access::injectAssemblyPreparationFailure(
        nestedSelectionEditor,
        solidar::AssemblyViewportSessionFailurePoint::Resource);
    CHECK(Access::installFullProjection(
              nestedSelectionEditor,
              solidar::AssemblyCommandSemanticState{*nestedLeafPath.path,
                                                     {}}) ==
          solidar::ProductDocumentSuccessorStatus::ResourceFailure);
    Access::injectAssemblyPreparationFailure(
        nestedSelectionEditor,
        solidar::AssemblyViewportSessionFailurePoint::None);
    CHECK(Access::assemblySessionAddress(nestedSelectionEditor) ==
          fullSessionBeforeFailure);
    CHECK(Access::assemblySessionAddress(nestedSelectionEditor)
              ->publicationHandle()
              .isSameCurrentPublication(fullPublicationBeforeFailure));
    CHECK(Access::assemblyTreeCurrentIndex(nestedSelectionEditor) ==
          fullIndexBeforeFailure);
    CHECK(Access::viewportAssemblySelection(nestedSelectionEditor) ==
          nestedLeafSelection);

    const auto fullHistorySizeBeforeSelectionFailure =
        Access::assemblyInsertHistorySize(nestedSelectionEditor);
    const auto fullHistoryCursorBeforeSelectionFailure =
        Access::assemblyInsertHistoryCursor(nestedSelectionEditor);
    const auto fullHistoryRevisionBeforeSelectionFailure =
        Access::assemblyHistoryRevision(nestedSelectionEditor);
    const auto* fullProductBeforeSelectionFailure =
        Access::productDocumentAddress(nestedSelectionEditor);
    const auto* fullTreeBeforeSelectionFailure =
        Access::assemblyTreeModelAddress(nestedSelectionEditor);
    Access::injectFullProjectionSelectionResourceFailure(
        nestedSelectionEditor, true);
    CHECK(Access::installFullProjection(
              nestedSelectionEditor,
              solidar::AssemblyCommandSemanticState{*nestedLeafPath.path,
                                                     {}}) ==
          solidar::ProductDocumentSuccessorStatus::ResourceFailure);
    Access::injectFullProjectionSelectionResourceFailure(
        nestedSelectionEditor, false);
    CHECK(Access::productDocumentAddress(nestedSelectionEditor) ==
          fullProductBeforeSelectionFailure);
    CHECK(Access::assemblyTreeModelAddress(nestedSelectionEditor) ==
          fullTreeBeforeSelectionFailure);
    CHECK(Access::assemblySessionAddress(nestedSelectionEditor) ==
          fullSessionBeforeFailure);
    CHECK(Access::assemblySessionAddress(nestedSelectionEditor)
              ->publicationHandle()
              .isSameCurrentPublication(fullPublicationBeforeFailure));
    CHECK(Access::assemblyTreeCurrentIndex(nestedSelectionEditor) ==
          fullIndexBeforeFailure);
    CHECK(Access::viewportAssemblySelection(nestedSelectionEditor) ==
          nestedLeafSelection);
    CHECK(Access::assemblyInsertHistorySize(nestedSelectionEditor) ==
          fullHistorySizeBeforeSelectionFailure);
    CHECK(Access::assemblyInsertHistoryCursor(nestedSelectionEditor) ==
          fullHistoryCursorBeforeSelectionFailure);
    CHECK(Access::assemblyHistoryRevision(nestedSelectionEditor) ==
          fullHistoryRevisionBeforeSelectionFailure);

    const QModelIndex childDefinition = Access::assemblyDefinitionIndex(
        nestedSelectionEditor, solidar::DefinitionId{200});
    const QModelIndex orphanAssemblyDefinition =
        Access::assemblyDefinitionIndex(nestedSelectionEditor,
                                        solidar::DefinitionId{300});
    CHECK(childDefinition.isValid());
    CHECK(orphanAssemblyDefinition.isValid());
    const QModelIndex rootDefinition = Access::assemblyDefinitionIndex(
        nestedSelectionEditor, solidar::DefinitionId{100});
    CHECK(rootDefinition.isValid());
    CHECK(Access::installFullProjection(
              nestedSelectionEditor,
              {{}, solidar::DefinitionId{100}}) ==
          solidar::ProductDocumentSuccessorStatus::Prepared);
    CHECK(Access::assemblyCommandSemanticState(nestedSelectionEditor)
              .selectedDefinition == solidar::DefinitionId{100});
    CHECK(Access::assemblyTreeCurrentDefinition(nestedSelectionEditor) ==
          solidar::DefinitionId{100});
    CHECK(Access::executeAssemblyRenameForSemanticState(
              nestedSelectionEditor,
              {{}, solidar::DefinitionId{100}},
              {{}, solidar::DefinitionId{300}}) ==
          solidar::AssemblyCommandHistoryStatus::Committed);
    CHECK(Access::assemblyTreeCurrentDefinition(nestedSelectionEditor) ==
          solidar::DefinitionId{300});
    Access::undoAssembly(nestedSelectionEditor);
    CHECK(Access::assemblyTreeCurrentDefinition(nestedSelectionEditor) ==
          solidar::DefinitionId{100});
    Access::redoAssembly(nestedSelectionEditor);
    CHECK(Access::assemblyTreeCurrentDefinition(nestedSelectionEditor) ==
          solidar::DefinitionId{300});
  }

  // A5c1 rename is a tree-only successor: Apply/Undo/Redo replace only the
  // immutable tree and keep the exact viewport publication/session/scene.
  {
    solidar::MainWindow renameEditor(settings);
    CHECK(Access::createNewAssembly(renameEditor));
    CHECK(!Access::assemblyRenameEnabled(renameEditor));
    CHECK(Access::stageAssemblyInsert(renameEditor, pathA));
    CHECK(!Access::assemblyRenameEnabled(renameEditor));
    Access::applyAssemblyInsert(renameEditor);
    CHECK(Access::assemblyRenameEnabled(renameEditor));
    CHECK(Access::assemblyRenameEntrypointsConfigured(renameEditor));
    QTimer::singleShot(0, [] {
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      CHECK(menu);
      CHECK(menu->actions().size() == 7);
      CHECK(menu->actions().front()->text() ==
            QString::fromUtf8("Переименовать экземпляр…"));
      CHECK(menu->actions()[1]->text() ==
            QString::fromUtf8("Скрыть экземпляр"));
      CHECK(menu->actions()[2]->text() ==
            QString::fromUtf8("Зафиксировать экземпляр"));
      CHECK(menu->actions()[3]->text() ==
            QString::fromUtf8("Подавить экземпляр"));
      CHECK(menu->actions()[4]->text() ==
            QString::fromUtf8("Дублировать экземпляр"));
      CHECK(menu->actions()[5]->text() ==
            QString::fromUtf8("\xD0\xA3\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C\x20\xD1\x8D\xD0\xBA\xD0\xB7\xD0\xB5\xD0\xBC\xD0\xBF\xD0\xBB\xD1\x8F\xD1\x80"));
      CHECK(menu->actions()[6]->text() ==
            QString::fromUtf8("Изолировать экземпляр"));
      QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QInputDialog*>(
            QApplication::activeModalWidget());
        CHECK(dialog);
        dialog->reject();
      });
      menu->actions().front()->trigger();
      menu->close();
    });
    const QModelIndex contextOccurrence =
        Access::assemblyTreeCurrentIndex(renameEditor);
    CHECK(contextOccurrence.isValid());
    const QPoint contextPosition =
        renameEditor.findChild<QTreeView*>(QStringLiteral("assemblyTreeView"))
            ->visualRect(contextOccurrence)
            .center();
    CHECK(QMetaObject::invokeMethod(
        renameEditor.findChild<QTreeView*>(
            QStringLiteral("assemblyTreeView")),
        "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, contextPosition)));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 1);
    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->reject();
    });
    Access::clickAssemblyRenameButton(renameEditor);
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 1);
    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->reject();
    });
    Access::triggerAssemblyRenameShortcut(renameEditor);
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 1);
    const auto* renameSession = Access::assemblySessionAddress(renameEditor);
    const auto renameEpoch = Access::assemblyEpochClock(renameEditor);
    const auto renamePublication =
        renameSession->publicationHandle().generation();
    const auto* renameScene = renameSession->scene().get();
    const auto renameSelection =
        Access::viewportAssemblySelection(renameEditor);
    const auto renameUploads =
        Access::viewport(renameEditor)->rendererResourceDiagnostics();
    const auto renameCamera = Access::assemblyCameraState(renameEditor);
    const auto renameSelectedHighlight =
        Access::selectedHighlightIdentity(renameEditor);
    const auto renameHoveredHighlight =
        Access::hoveredHighlightIdentity(renameEditor);
    const auto selectionCallbacksBeforeApply =
        Access::assemblyTreeSelectionCallbackCount(renameEditor);
    CHECK(Access::viewportAssemblySelectionViewAliasesSessionStorage(
        renameEditor));
    CHECK(renameSelection);
    CHECK(renameSelection->occurrence.path.depth() == 1);
    CHECK(Access::assemblyCommandSemanticState(renameEditor)
              .selectedOccurrence == renameSelection->occurrence.path);
    Access::injectAssemblyTreeSelectionCallbackResourceFailure(renameEditor,
                                                               true);
    // These postcommit helpers are noexcept and read the exact session-owned
    // optional through Viewport's immutable view.  Even with the old
    // allocation-capable callback seam armed they neither copy a path nor
    // enter the selection callback.
    Access::updateAssemblyRenameAvailability(renameEditor);
    CHECK(Access::assemblyCommandSemanticState(renameEditor)
              .selectedOccurrence == renameSelection->occurrence.path);
    CHECK(Access::assemblyTreeSelectionCallbackCount(renameEditor) ==
          selectionCallbacksBeforeApply);
    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      CHECK(dialog->labelText() == QString::fromUtf8("Новое имя:"));
      dialog->setTextValue(QString::fromUtf8("Опора 🔩"));
      dialog->accept();
    });
    Access::triggerAssemblyRename(renameEditor);
    Access::injectAssemblyTreeSelectionCallbackResourceFailure(renameEditor,
                                                               false);
    CHECK(Access::assemblyTreeSelectionCallbackCount(renameEditor) ==
          selectionCallbacksBeforeApply);
    CHECK(Access::viewportAssemblySelectionViewAliasesSessionStorage(
        renameEditor));
    CHECK(Access::occurrenceForIndex(
              renameEditor, Access::assemblyTreeCurrentIndex(renameEditor)) ==
          renameSelection);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyTreeCurrentText(renameEditor).contains(
        QString::fromUtf8("Опора 🔩")));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::assemblySessionAddress(renameEditor)
              ->publicationHandle()
              .generation() == renamePublication);
    CHECK(Access::assemblySessionAddress(renameEditor)->scene().get() ==
          renameScene);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);
    CHECK(sameRendererResources(
        renameUploads,
        Access::viewport(renameEditor)->rendererResourceDiagnostics()));
    CHECK(sameCameraState(renameCamera,
                          Access::assemblyCameraState(renameEditor)));
    CHECK(Access::selectedHighlightIdentity(renameEditor) ==
          renameSelectedHighlight);
    CHECK(Access::hoveredHighlightIdentity(renameEditor) ==
          renameHoveredHighlight);

    const auto selectionCallbacksBeforeUndo =
        Access::assemblyTreeSelectionCallbackCount(renameEditor);
    Access::injectAssemblyTreeSelectionCallbackResourceFailure(renameEditor,
                                                               true);
    Access::undoAssemblyInsert(renameEditor);
    Access::injectAssemblyTreeSelectionCallbackResourceFailure(renameEditor,
                                                               false);
    CHECK(Access::assemblyTreeSelectionCallbackCount(renameEditor) ==
          selectionCallbacksBeforeUndo);
    CHECK(Access::viewportAssemblySelectionViewAliasesSessionStorage(
        renameEditor));
    CHECK(Access::occurrenceForIndex(
              renameEditor, Access::assemblyTreeCurrentIndex(renameEditor)) ==
          renameSelection);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor).empty());
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 1);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::assemblySessionAddress(renameEditor)->scene().get() ==
          renameScene);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);
    CHECK(sameRendererResources(
        renameUploads,
        Access::viewport(renameEditor)->rendererResourceDiagnostics()));
    CHECK(sameCameraState(renameCamera,
                          Access::assemblyCameraState(renameEditor)));
    CHECK(Access::selectedHighlightIdentity(renameEditor) ==
          renameSelectedHighlight);
    CHECK(Access::hoveredHighlightIdentity(renameEditor) ==
          renameHoveredHighlight);
    Access::redoAssemblyInsert(renameEditor);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::assemblySessionAddress(renameEditor)->scene().get() ==
          renameScene);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);
    CHECK(sameRendererResources(
        renameUploads,
        Access::viewport(renameEditor)->rendererResourceDiagnostics()));
    CHECK(sameCameraState(renameCamera,
                          Access::assemblyCameraState(renameEditor)));
    CHECK(Access::selectedHighlightIdentity(renameEditor) ==
          renameSelectedHighlight);
    CHECK(Access::hoveredHighlightIdentity(renameEditor) ==
          renameHoveredHighlight);

    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->setTextValue(QString::fromUtf8("Опора 🔩"));
      dialog->accept();
    });
    Access::triggerAssemblyRename(renameEditor);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);

    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->setTextValue(QStringLiteral("   "));
      dialog->accept();
    });
    Access::triggerAssemblyRename(renameEditor);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);

    const QModelIndex occurrenceBeforeSelectionChange =
        Access::assemblyTreeCurrentIndex(renameEditor);
    CHECK(occurrenceBeforeSelectionChange.isValid());
    QTimer::singleShot(0, [&renameEditor, occurrenceBeforeSelectionChange] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      Access::selectAssemblyTreeIndex(renameEditor,
                                      occurrenceBeforeSelectionChange.parent());
      dialog->setTextValue(QString::fromUtf8("Не должно примениться"));
      dialog->accept();
    });
    Access::triggerAssemblyRename(renameEditor);
    Access::selectAssemblyTreeIndex(renameEditor,
                                    occurrenceBeforeSelectionChange);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);

    QTimer::singleShot(0, [&renameEditor] {
      CHECK(qobject_cast<QInputDialog*>(QApplication::activeModalWidget()));
      Access::undoAssemblyInsert(renameEditor);
    });
    Access::triggerAssemblyRename(renameEditor);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 1);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor).empty());
    Access::redoAssemblyInsert(renameEditor);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);

    Access::injectAssemblyPreparationFailure(
        renameEditor,
        solidar::AssemblyViewportSessionFailurePoint::FinalPreparation);
    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->setTextValue(QString::fromUtf8("Отклонённое имя"));
      dialog->accept();
    });
    Access::triggerAssemblyRename(renameEditor);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);
    Access::injectAssemblyPreparationFailure(
        renameEditor, solidar::AssemblyViewportSessionFailurePoint::None);

    const auto* treeBeforeResourceFailure =
        Access::assemblyTreeModelAddress(renameEditor);
    Access::injectAssemblyTreeResourceFailure(renameEditor, true);
    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->setTextValue(QString::fromUtf8("Недостаточно памяти"));
      dialog->accept();
    });
    Access::triggerAssemblyRename(renameEditor);
    Access::injectAssemblyTreeResourceFailure(renameEditor, false);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::assemblyTreeModelAddress(renameEditor) ==
          treeBeforeResourceFailure);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);
    CHECK(sameRendererResources(
        renameUploads,
        Access::viewport(renameEditor)->rendererResourceDiagnostics()));
    CHECK(sameCameraState(renameCamera,
                          Access::assemblyCameraState(renameEditor)));
    CHECK(Access::selectedHighlightIdentity(renameEditor) ==
          renameSelectedHighlight);
    CHECK(Access::hoveredHighlightIdentity(renameEditor) ==
          renameHoveredHighlight);

    Access::injectAssemblyTreeResourceFailure(renameEditor, true);
    Access::undoAssemblyInsert(renameEditor);
    Access::injectAssemblyTreeResourceFailure(renameEditor, false);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);
    CHECK(Access::assemblyTreeModelAddress(renameEditor) ==
          treeBeforeResourceFailure);
    CHECK(Access::assemblySessionAddress(renameEditor) == renameSession);
    CHECK(Access::assemblyEpochClock(renameEditor) == renameEpoch);
    CHECK(Access::viewportAssemblySelection(renameEditor) == renameSelection);
    CHECK(sameRendererResources(
        renameUploads,
        Access::viewport(renameEditor)->rendererResourceDiagnostics()));
    CHECK(sameCameraState(renameCamera,
                          Access::assemblyCameraState(renameEditor)));
    CHECK(Access::selectedHighlightIdentity(renameEditor) ==
          renameSelectedHighlight);
    CHECK(Access::hoveredHighlightIdentity(renameEditor) ==
          renameHoveredHighlight);

    QTimer::singleShot(0, [] {
      auto* dialog = qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget());
      CHECK(dialog);
      dialog->setTextValue(QString::fromUtf8("Не применять"));
      dialog->reject();
    });
    Access::triggerAssemblyRename(renameEditor);
    CHECK(Access::selectedOccurrenceDisplayName(renameEditor) ==
          std::string("Опора 🔩"));
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 2);
    CHECK(Access::assemblyInsertHistoryCursor(renameEditor) == 2);

    bool replacedWhileDialogOpen = false;
    QTimer::singleShot(0, [&] {
      CHECK(qobject_cast<QInputDialog*>(
          QApplication::activeModalWidget()));
      replacedWhileDialogOpen = Access::createNewAssembly(renameEditor);
    });
    Access::triggerAssemblyRename(renameEditor);
    CHECK(replacedWhileDialogOpen);
    CHECK(Access::assemblyOccurrenceCount(renameEditor) == 0);
    CHECK(Access::assemblyInsertHistorySize(renameEditor) == 0);
    CHECK(!Access::assemblyRenameEnabled(renameEditor));
  }

  // The rename dialog is a guarded heap child: DeferredDelete of MainWindow
  // during nested exec destroys both objects exactly once and the suspended
  // callback observes null QPointers before touching either one again.
  {
    QPointer<solidar::MainWindow> destroyedDuringRename =
        new solidar::MainWindow(settings);
    CHECK(Access::createNewAssembly(*destroyedDuringRename));
    CHECK(Access::stageAssemblyInsert(*destroyedDuringRename, pathA));
    Access::applyAssemblyInsert(*destroyedDuringRename);
    CHECK(Access::assemblyRenameEnabled(*destroyedDuringRename));
    auto* rawEditor = destroyedDuringRename.data();
    QTimer::singleShot(0, [rawEditor] {
      CHECK(qobject_cast<QInputDialog*>(QApplication::activeModalWidget()));
      rawEditor->deleteLater();
      QCoreApplication::sendPostedEvents(rawEditor, QEvent::DeferredDelete);
    });
    Access::triggerAssemblyRename(*rawEditor);
    CHECK(destroyedDuringRename.isNull());
  }

  // A5c2 persistent visibility publishes a same-epoch scene without fitting
  // the camera; isolate is presentation-only and keeps the exact session,
  // publication, history and dirty state.
  {
    // This scenario deliberately keeps many exact projection snapshots alive.
    // Keep the large editor object off main's already extensive regression
    // stack so Debug exercises the assertions rather than exhausting Win32's
    // default stack reserve.
    auto visibilityEditorOwner =
        std::make_unique<solidar::MainWindow>(settings);
    solidar::MainWindow& visibilityEditor = *visibilityEditorOwner;
    CHECK(Access::createNewAssembly(visibilityEditor));
    CHECK(Access::stageAssemblyInsert(visibilityEditor, pathA));
    Access::applyAssemblyInsert(visibilityEditor);
    CHECK(Access::stageAssemblyInsert(visibilityEditor, pathA));
    Access::applyAssemblyInsert(visibilityEditor);
    const QModelIndex first =
        Access::assemblyOccurrenceIndex(visibilityEditor, 0);
    CHECK(first.isValid());
    Access::selectAssemblyTreeIndex(visibilityEditor, first);
    CHECK(Access::assemblyGroundingEnabled(visibilityEditor));
    CHECK(Access::assemblyGroundingText(visibilityEditor) ==
          QString::fromUtf8("Зафиксировать экземпляр"));
    QTimer::singleShot(0, [] {
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      CHECK(menu);
      QAction* groundingAction = nullptr;
      for (QAction* action : menu->actions())
        if (action->text() ==
            QString::fromUtf8("Зафиксировать экземпляр"))
          groundingAction = action;
      CHECK(groundingAction);
      groundingAction->trigger();
      menu->close();
    });
    const QPoint groundingContextPosition =
        visibilityEditor
            .findChild<QTreeView*>(QStringLiteral("assemblyTreeView"))
            ->visualRect(first)
            .center();
    CHECK(QMetaObject::invokeMethod(
        visibilityEditor.findChild<QTreeView*>(
            QStringLiteral("assemblyTreeView")),
        "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, groundingContextPosition)));
    CHECK(Access::selectedOccurrenceGrounded(visibilityEditor));
    CHECK(Access::assemblyGroundingText(visibilityEditor) ==
          QString::fromUtf8("Снять фиксацию экземпляра"));
    Access::triggerAssemblyGroundingShortcut(visibilityEditor);
    CHECK(!Access::selectedOccurrenceGrounded(visibilityEditor));

    const auto* beforeGroundingSession =
        Access::assemblySessionAddress(visibilityEditor);
    const auto beforeGroundingPublication =
        beforeGroundingSession->publicationHandle();
    const auto beforeGroundingEpoch =
        Access::assemblyEpochClock(visibilityEditor);
    const auto beforeGroundingCamera =
        Access::assemblyCameraState(visibilityEditor);
    const auto beforeGroundingResources =
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics();
    const auto beforeGroundingInstances =
        beforeGroundingSession->scene()->instances();
    const auto historyBeforeGrounding =
        Access::assemblyInsertHistorySize(visibilityEditor);
    Access::triggerAssemblyGrounding(visibilityEditor);
    const auto* groundedSession =
        Access::assemblySessionAddress(visibilityEditor);
    CHECK(Access::selectedOccurrenceGrounded(visibilityEditor));
    CHECK(Access::assemblyInsertHistorySize(visibilityEditor) ==
          historyBeforeGrounding + 1);
    CHECK(Access::assemblyEpochClock(visibilityEditor) ==
          beforeGroundingEpoch);
    CHECK(groundedSession != beforeGroundingSession);
    CHECK(!groundedSession->publicationHandle().isSameCurrentPublication(
        beforeGroundingPublication));
    CHECK(groundedSession->scene()->instances().size() ==
          beforeGroundingInstances.size());
    for (std::size_t index = 0; index < beforeGroundingInstances.size();
         ++index) {
      const auto& before = beforeGroundingInstances[index];
      const auto& after = groundedSession->scene()->instances()[index];
      CHECK(after.sourceKey == before.sourceKey);
      CHECK(after.mesh == before.mesh);
      CHECK(after.localToWorld == before.localToWorld);
      CHECK(after.occurrenceIdentity == before.occurrenceIdentity);
      CHECK(after.worldBounds.minimum().x == before.worldBounds.minimum().x);
      CHECK(after.worldBounds.minimum().y == before.worldBounds.minimum().y);
      CHECK(after.worldBounds.minimum().z == before.worldBounds.minimum().z);
      CHECK(after.worldBounds.maximum().x == before.worldBounds.maximum().x);
      CHECK(after.worldBounds.maximum().y == before.worldBounds.maximum().y);
      CHECK(after.worldBounds.maximum().z == before.worldBounds.maximum().z);
      CHECK(after.grounded == (index == 0));
    }
    CHECK(groundedSession->sceneDiagnostics().meshBuildAttempts == 0);
    CHECK(sameCameraState(beforeGroundingCamera,
                          Access::assemblyCameraState(visibilityEditor)));
    CHECK(sameRendererResources(
        beforeGroundingResources,
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics()));
    CHECK(Access::selectedHighlightIdentity(visibilityEditor));
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(!Access::selectedOccurrenceGrounded(visibilityEditor));
    Access::redoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceGrounded(visibilityEditor));
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(!Access::selectedOccurrenceGrounded(visibilityEditor));

    // A5c4 suppression is independent from visibility/grounding.  Context,
    // ribbon and Shift+Space share one action, while Space remains visibility.
    CHECK(Access::assemblySuppressionEntrypointsConfigured(visibilityEditor));
    CHECK(Access::assemblySuppressionEnabled(visibilityEditor));
    CHECK(Access::assemblySuppressionText(visibilityEditor) ==
          QString::fromUtf8("Подавить экземпляр"));
    QTimer::singleShot(0, [] {
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      CHECK(menu);
      QAction* suppressionAction = nullptr;
      for (QAction* action : menu->actions())
        if (action->text() == QString::fromUtf8("Подавить экземпляр"))
          suppressionAction = action;
      CHECK(suppressionAction);
      suppressionAction->trigger();
      menu->close();
    });
    const QModelIndex suppressionIndex =
        Access::assemblyTreeCurrentIndex(visibilityEditor);
    CHECK(suppressionIndex.isValid());
    const QPoint suppressionContextPosition =
        visibilityEditor
            .findChild<QTreeView*>(QStringLiteral("assemblyTreeView"))
            ->visualRect(suppressionIndex)
            .center();
    CHECK(QMetaObject::invokeMethod(
        visibilityEditor.findChild<QTreeView*>(
            QStringLiteral("assemblyTreeView")),
        "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, suppressionContextPosition)));
    CHECK(Access::selectedOccurrenceSuppressed(visibilityEditor));
    CHECK(Access::assemblySuppressionText(visibilityEditor) ==
          QString::fromUtf8("Снять подавление экземпляра"));
    Access::triggerAssemblySuppressionShortcut(visibilityEditor);
    CHECK(!Access::selectedOccurrenceSuppressed(visibilityEditor));

    const auto* beforeSuppressionSession =
        Access::assemblySessionAddress(visibilityEditor);
    const auto beforeSuppressionPublication =
        beforeSuppressionSession->publicationHandle();
    const auto beforeSuppressionEpoch =
        Access::assemblyEpochClock(visibilityEditor);
    const auto beforeSuppressionCamera =
        Access::assemblyCameraState(visibilityEditor);
    const auto beforeSuppressionResources =
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics();
    const auto historyBeforeSuppression =
        Access::assemblyInsertHistorySize(visibilityEditor);
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 2);
    Access::triggerAssemblySuppression(visibilityEditor);
    CHECK(Access::selectedOccurrenceSuppressed(visibilityEditor));
    CHECK(Access::assemblyInsertHistorySize(visibilityEditor) ==
          historyBeforeSuppression + 1);
    CHECK(Access::assemblyEpochClock(visibilityEditor) ==
          beforeSuppressionEpoch);
    CHECK(Access::assemblySessionAddress(visibilityEditor) !=
          beforeSuppressionSession);
    CHECK(!Access::assemblySessionAddress(visibilityEditor)
               ->publicationHandle()
               .isSameCurrentPublication(beforeSuppressionPublication));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 1);
    CHECK(!Access::selectedHighlightIdentity(visibilityEditor));
    CHECK(sameCameraState(beforeSuppressionCamera,
                          Access::assemblyCameraState(visibilityEditor)));
    CHECK(sameRendererResources(
        beforeSuppressionResources,
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics()));
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(!Access::selectedOccurrenceSuppressed(visibilityEditor));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 2);
    CHECK(Access::selectedHighlightIdentity(visibilityEditor));
    Access::redoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceSuppressed(visibilityEditor));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 1);
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(!Access::selectedOccurrenceSuppressed(visibilityEditor));

    // A5c5 Duplicate is immediate and structural: context/shortcut share one
    // command, the lowest free ID is stable through Undo/Redo, and the exact
    // Part definition/Document/mesh resources remain shared.
    [&] {
    CHECK(Access::assemblyDuplicateEntrypointsConfigured(visibilityEditor));
    CHECK(Access::assemblyDuplicateEnabled(visibilityEditor));
    const auto duplicateSourceId =
        Access::selectedOccurrenceId(visibilityEditor);
    CHECK(duplicateSourceId);
    const auto* duplicateProduct =
        Access::productDocumentAddress(visibilityEditor);
    const auto* duplicateSource = duplicateProduct->rootAssemblyDefinition()
                                      ->find(*duplicateSourceId);
    CHECK(duplicateSource);
    const solidar::DefinitionId duplicateTarget =
        duplicateSource->targetDefinitionId();
    const solidar::RigidTransform3d duplicatePlacement =
        duplicateSource->placement();
    const solidar::OccurrenceFlags duplicateFlags = duplicateSource->flags();
    const std::string duplicateName = duplicateSource->displayNameUtf8();
    const auto* duplicateDefinition =
        duplicateProduct->definitions().findPart(duplicateTarget);
    CHECK(duplicateDefinition);
    const solidar::Document* duplicateDocument =
        &duplicateDefinition->document();
    const auto duplicateDefinitionCount =
        Access::definitionCount(visibilityEditor);
    const auto duplicateBeforeCount =
        Access::assemblyOccurrenceCount(visibilityEditor);
    const auto duplicateBeforeInstances =
        Access::displayedAssemblyInstanceCount(visibilityEditor);
    const auto* duplicateBeforeSession =
        Access::assemblySessionAddress(visibilityEditor);
    const auto duplicateBeforePublication =
        duplicateBeforeSession->publicationHandle();
    const auto duplicateBeforeEpoch =
        Access::assemblyEpochClock(visibilityEditor);
    const auto duplicateBeforeCamera =
        Access::assemblyCameraState(visibilityEditor);
    const auto duplicateBeforeResources =
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics();
    const auto duplicateBeforeHistory =
        Access::assemblyInsertHistorySize(visibilityEditor);
    const auto duplicateBeforeCursor =
        Access::assemblyInsertHistoryCursor(visibilityEditor);
    QTimer::singleShot(0, [] {
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      CHECK(menu);
      QAction* duplicateAction = nullptr;
      for (QAction* action : menu->actions())
        if (action->text() == QString::fromUtf8("Дублировать экземпляр"))
          duplicateAction = action;
      CHECK(duplicateAction);
      duplicateAction->trigger();
      menu->close();
    });
    const QModelIndex duplicateContextIndex =
        Access::assemblyTreeCurrentIndex(visibilityEditor);
    CHECK(duplicateContextIndex.isValid());
    const QPoint duplicateContextPosition =
        visibilityEditor
            .findChild<QTreeView*>(QStringLiteral("assemblyTreeView"))
            ->visualRect(duplicateContextIndex)
            .center();
    CHECK(QMetaObject::invokeMethod(
        visibilityEditor.findChild<QTreeView*>(
            QStringLiteral("assemblyTreeView")),
        "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, duplicateContextPosition)));
    const auto duplicateId = Access::selectedOccurrenceId(visibilityEditor);
    CHECK(duplicateId && *duplicateId != *duplicateSourceId);
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          duplicateBeforeCount + 1);
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) ==
          duplicateBeforeInstances + 1);
    CHECK(duplicateBeforeHistory > duplicateBeforeCursor);
    CHECK(Access::assemblyInsertHistorySize(visibilityEditor) ==
          duplicateBeforeCursor + 1);
    CHECK(Access::assemblyInsertHistoryCursor(visibilityEditor) ==
          duplicateBeforeCursor + 1);
    CHECK(Access::assemblyEpochClock(visibilityEditor) ==
          duplicateBeforeEpoch);
    CHECK(Access::assemblySessionAddress(visibilityEditor) !=
          duplicateBeforeSession);
    CHECK(!Access::assemblySessionAddress(visibilityEditor)
               ->publicationHandle()
               .isSameCurrentPublication(duplicateBeforePublication));
    CHECK(Access::selectedHighlightIdentity(visibilityEditor));
    const auto* duplicateOccurrence =
        Access::productDocumentAddress(visibilityEditor)
            ->rootAssemblyDefinition()
            ->find(*duplicateId);
    CHECK(duplicateOccurrence);
    CHECK(duplicateOccurrence->targetDefinitionId() == duplicateTarget);
    CHECK(duplicateOccurrence->placement() == duplicatePlacement);
    CHECK(duplicateOccurrence->flags() == duplicateFlags);
    CHECK(duplicateOccurrence->displayNameUtf8() == duplicateName);
    CHECK(Access::definitionCount(visibilityEditor) ==
          duplicateDefinitionCount);
    CHECK(Access::productDocumentAddress(visibilityEditor)
              ->definitions()
              .findPart(duplicateTarget) == duplicateDefinition);
    CHECK(&duplicateDefinition->document() == duplicateDocument);
    CHECK(Access::assemblySessionAddress(visibilityEditor)
              ->sceneDiagnostics()
              .meshBuildAttempts == 0);
    CHECK(sameCameraState(duplicateBeforeCamera,
                          Access::assemblyCameraState(visibilityEditor)));
    CHECK(sameRendererResources(
        duplicateBeforeResources,
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics()));

    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) ==
          duplicateSourceId);
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          duplicateBeforeCount);
    Access::redoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) == duplicateId);
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          duplicateBeforeCount + 1);
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) ==
          duplicateSourceId);
    Access::triggerAssemblyDuplicateShortcut(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) == duplicateId);
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) ==
          duplicateSourceId);
    }();

    // A5c6 Remove is immediate and tree-scoped. Context, ribbon and Delete
    // share one command; definitions/Documents remain present, selection is
    // cleared on Remove and rebound to the same ID on Undo.
    [&] {
    CHECK(Access::assemblyRemoveEntrypointsConfigured(visibilityEditor));
    CHECK(Access::assemblyRemoveEnabled(visibilityEditor));
    const auto removeSourceId =
        Access::selectedOccurrenceId(visibilityEditor);
    CHECK(removeSourceId);
    const auto* removeProduct =
        Access::productDocumentAddress(visibilityEditor);
    const auto* removeOccurrence =
        removeProduct->rootAssemblyDefinition()->find(*removeSourceId);
    CHECK(removeOccurrence);
    const solidar::DefinitionId removeTarget =
        removeOccurrence->targetDefinitionId();
    const auto removePlacement = removeOccurrence->placement();
    const auto removeFlags = removeOccurrence->flags();
    const auto removeName = removeOccurrence->displayNameUtf8();
    const auto* removeDefinition =
        removeProduct->definitions().findPart(removeTarget);
    CHECK(removeDefinition);
    const solidar::Document* removeDocument = &removeDefinition->document();
    const auto removeDefinitionCount =
        Access::definitionCount(visibilityEditor);
    const auto removeBeforeCount =
        Access::assemblyOccurrenceCount(visibilityEditor);
    const auto removeBeforeInstances =
        Access::displayedAssemblyInstanceCount(visibilityEditor);
    const auto* removeBeforeSession =
        Access::assemblySessionAddress(visibilityEditor);
    const auto removeBeforePublication =
        removeBeforeSession->publicationHandle();
    const auto removeBeforeEpoch =
        Access::assemblyEpochClock(visibilityEditor);
    const auto removeBeforeCamera =
        Access::assemblyCameraState(visibilityEditor);
    const auto removeBeforeResources =
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics();

    QTimer::singleShot(0, [] {
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      CHECK(menu);
      QAction* removeAction = nullptr;
      for (QAction* action : menu->actions())
        if (action->text() == QString::fromUtf8("Удалить экземпляр"))
          removeAction = action;
      CHECK(removeAction);
      removeAction->trigger();
      menu->close();
    });
    const QModelIndex removeContextIndex =
        Access::assemblyTreeCurrentIndex(visibilityEditor);
    CHECK(removeContextIndex.isValid());
    const QPoint removeContextPosition =
        visibilityEditor
            .findChild<QTreeView*>(QStringLiteral("assemblyTreeView"))
            ->visualRect(removeContextIndex)
            .center();
    CHECK(QMetaObject::invokeMethod(
        visibilityEditor.findChild<QTreeView*>(
            QStringLiteral("assemblyTreeView")),
        "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, removeContextPosition)));
    CHECK(!Access::selectedOccurrenceId(visibilityEditor));
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          removeBeforeCount - 1);
    CHECK(Access::definitionCount(visibilityEditor) ==
          removeDefinitionCount);
    CHECK(Access::productDocumentAddress(visibilityEditor)
              ->definitions()
              .findPart(removeTarget) == removeDefinition);
    CHECK(&removeDefinition->document() == removeDocument);
    CHECK(Access::assemblyEpochClock(visibilityEditor) == removeBeforeEpoch);
    CHECK(Access::assemblySessionAddress(visibilityEditor) !=
          removeBeforeSession);
    CHECK(!Access::assemblySessionAddress(visibilityEditor)
               ->publicationHandle()
               .isSameCurrentPublication(removeBeforePublication));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) ==
          removeBeforeInstances - 1);
    CHECK(!Access::selectedHighlightIdentity(visibilityEditor));
    CHECK(Access::assemblySessionAddress(visibilityEditor)
              ->sceneDiagnostics()
              .meshBuildAttempts == 0);
    CHECK(sameCameraState(removeBeforeCamera,
                          Access::assemblyCameraState(visibilityEditor)));
    CHECK(sameRendererResources(
        removeBeforeResources,
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics()));

    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) == removeSourceId);
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          removeBeforeCount);
    const auto* restoredOccurrence =
        Access::productDocumentAddress(visibilityEditor)
            ->rootAssemblyDefinition()
            ->find(*removeSourceId);
    CHECK(restoredOccurrence &&
          restoredOccurrence->targetDefinitionId() == removeTarget &&
          restoredOccurrence->placement() == removePlacement &&
          restoredOccurrence->flags() == removeFlags &&
          restoredOccurrence->displayNameUtf8() == removeName);
    Access::redoAssemblyInsert(visibilityEditor);
    CHECK(!Access::selectedOccurrenceId(visibilityEditor));
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          removeBeforeCount - 1);
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) == removeSourceId);
    Access::triggerAssemblyRemoveShortcut(visibilityEditor);
    CHECK(!Access::selectedOccurrenceId(visibilityEditor));
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          removeBeforeCount - 1);

    // Remove the final peer as a second command. The root becomes empty while
    // the definition row is retained and marked orphan; Undo restores the
    // exact ID and clears the marker.
    const QModelIndex remaining =
        Access::assemblyOccurrenceIndex(visibilityEditor, 0);
    CHECK(remaining.isValid());
    Access::selectAssemblyTreeIndex(visibilityEditor, remaining);
    const auto finalOccurrenceId =
        Access::selectedOccurrenceId(visibilityEditor);
    CHECK(finalOccurrenceId && *finalOccurrenceId != *removeSourceId);
    const auto finalTarget =
        Access::productDocumentAddress(visibilityEditor)
            ->rootAssemblyDefinition()
            ->find(*finalOccurrenceId)
            ->targetDefinitionId();
    CHECK(Access::assemblyRemoveEnabled(visibilityEditor));
    Access::triggerAssemblyRemove(visibilityEditor);
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) == 0);
    CHECK(!Access::selectedOccurrenceId(visibilityEditor));
    CHECK(Access::definitionCount(visibilityEditor) ==
          removeDefinitionCount);
    CHECK(Access::definitionIsOrphan(visibilityEditor, removeTarget));
    CHECK(Access::definitionIsOrphan(visibilityEditor, finalTarget));
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) ==
          finalOccurrenceId);
    CHECK(Access::definitionIsOrphan(visibilityEditor, removeTarget));
    CHECK(!Access::definitionIsOrphan(visibilityEditor, finalTarget));
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceId(visibilityEditor) == removeSourceId);
    CHECK(Access::assemblyOccurrenceCount(visibilityEditor) ==
          removeBeforeCount);
    CHECK(!Access::definitionIsOrphan(visibilityEditor, removeTarget));
    }();

    CHECK(Access::assemblyVisibilityEnabled(visibilityEditor));
    CHECK(Access::assemblyVisibilityText(visibilityEditor) ==
          QString::fromUtf8("Скрыть экземпляр"));
    CHECK(Access::assemblyIsolationEnabled(visibilityEditor));
    QTimer::singleShot(0, [] {
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      CHECK(menu);
      QAction* visibilityAction = nullptr;
      for (QAction* action : menu->actions())
        if (action->text() == QString::fromUtf8("Скрыть экземпляр"))
          visibilityAction = action;
      CHECK(visibilityAction);
      visibilityAction->trigger();
      menu->close();
    });
    const QModelIndex currentVisibilityIndex =
        Access::assemblyTreeCurrentIndex(visibilityEditor);
    CHECK(currentVisibilityIndex.isValid());
    const QPoint visibilityContextPosition =
        visibilityEditor
            .findChild<QTreeView*>(QStringLiteral("assemblyTreeView"))
            ->visualRect(currentVisibilityIndex)
            .center();
    CHECK(QMetaObject::invokeMethod(
        visibilityEditor.findChild<QTreeView*>(
            QStringLiteral("assemblyTreeView")),
        "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, visibilityContextPosition)));
    CHECK(!Access::selectedOccurrenceVisible(visibilityEditor));
    Access::triggerAssemblyVisibility(visibilityEditor);
    CHECK(Access::selectedOccurrenceVisible(visibilityEditor));
    const auto* beforeVisibilitySession =
        Access::assemblySessionAddress(visibilityEditor);
    const auto beforeVisibilityPublication =
        beforeVisibilitySession->publicationHandle();
    const auto beforeVisibilityEpoch =
        Access::assemblyEpochClock(visibilityEditor);
    const auto beforeVisibilityCamera =
        Access::assemblyCameraState(visibilityEditor);
    const auto historyBeforeVisibility =
        Access::assemblyInsertHistorySize(visibilityEditor);
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 2);

    Access::triggerAssemblyVisibility(visibilityEditor);
    CHECK(!Access::selectedOccurrenceVisible(visibilityEditor));
    CHECK(Access::assemblyVisibilityText(visibilityEditor) ==
          QString::fromUtf8("Показать экземпляр"));
    CHECK(Access::assemblyInsertHistorySize(visibilityEditor) ==
          historyBeforeVisibility + 1);
    CHECK(Access::assemblyEpochClock(visibilityEditor) ==
          beforeVisibilityEpoch);
    CHECK(Access::assemblySessionAddress(visibilityEditor) !=
          beforeVisibilitySession);
    CHECK(!Access::assemblySessionAddress(visibilityEditor)
               ->publicationHandle()
               .isSameCurrentPublication(beforeVisibilityPublication));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 1);
    CHECK(!Access::selectedHighlightIdentity(visibilityEditor));
    CHECK(sameCameraState(beforeVisibilityCamera,
                          Access::assemblyCameraState(visibilityEditor)));

    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceVisible(visibilityEditor));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 2);
    CHECK(Access::selectedHighlightIdentity(visibilityEditor));
    CHECK(sameCameraState(beforeVisibilityCamera,
                          Access::assemblyCameraState(visibilityEditor)));

    Access::redoAssemblyInsert(visibilityEditor);
    CHECK(!Access::selectedOccurrenceVisible(visibilityEditor));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 1);
    Access::undoAssemblyInsert(visibilityEditor);
    CHECK(Access::selectedOccurrenceVisible(visibilityEditor));

    const auto* isolateSession =
        Access::assemblySessionAddress(visibilityEditor);
    const auto isolatePublication = isolateSession->publicationHandle();
    const auto isolateHistorySize =
        Access::assemblyInsertHistorySize(visibilityEditor);
    const auto isolateHistoryCursor =
        Access::assemblyInsertHistoryCursor(visibilityEditor);
    const bool isolateDirty = visibilityEditor.isWindowModified();
    const auto isolateResources =
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics();
    Access::triggerAssemblyIsolation(visibilityEditor);
    CHECK(Access::assemblyIsolationActive(visibilityEditor));
    CHECK(Access::assemblyIsolationText(visibilityEditor) ==
          QString::fromUtf8("Выйти из изоляции"));
    CHECK(!Access::assemblyVisibilityEnabled(visibilityEditor));
    CHECK(!Access::assemblyRenameEnabled(visibilityEditor));
    CHECK(!Access::assemblyGroundingEnabled(visibilityEditor));
    CHECK(!Access::assemblySuppressionEnabled(visibilityEditor));
    CHECK(!Access::assemblyDuplicateEnabled(visibilityEditor));
    CHECK(!Access::assemblyRemoveEnabled(visibilityEditor));
    CHECK(Access::assemblySessionAddress(visibilityEditor) == isolateSession);
    CHECK(Access::assemblySessionAddress(visibilityEditor)
              ->publicationHandle()
              .isSameCurrentPublication(isolatePublication));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 1);
    CHECK(Access::assemblyInsertHistorySize(visibilityEditor) ==
          isolateHistorySize);
    CHECK(Access::assemblyInsertHistoryCursor(visibilityEditor) ==
          isolateHistoryCursor);
    CHECK(visibilityEditor.isWindowModified() == isolateDirty);
    CHECK(sameRendererResources(
        isolateResources,
        Access::viewport(visibilityEditor)->rendererResourceDiagnostics()));

    QString failedIsolationOpen;
    CHECK(!visibilityEditor.loadProject(
        directory.filePath(QStringLiteral("missing-a5c2-open.solidar")),
        &failedIsolationOpen));
    CHECK(!failedIsolationOpen.isEmpty());
    CHECK(Access::assemblyIsolationActive(visibilityEditor));
    CHECK(Access::assemblySessionAddress(visibilityEditor) == isolateSession);

    Access::triggerAssemblyEscape(visibilityEditor);
    CHECK(!Access::assemblyIsolationActive(visibilityEditor));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 2);
    CHECK(Access::assemblySessionAddress(visibilityEditor) == isolateSession);
    CHECK(Access::assemblySessionAddress(visibilityEditor)
              ->publicationHandle()
              .isSameCurrentPublication(isolatePublication));
    CHECK(Access::assemblyInsertHistorySize(visibilityEditor) ==
          isolateHistorySize);

    Access::injectAssemblyTreeResourceFailure(visibilityEditor, true);
    Access::triggerAssemblyIsolation(visibilityEditor);
    Access::injectAssemblyTreeResourceFailure(visibilityEditor, false);
    CHECK(!Access::assemblyIsolationActive(visibilityEditor));
    CHECK(Access::displayedAssemblyInstanceCount(visibilityEditor) == 2);
    CHECK(Access::assemblySessionAddress(visibilityEditor) == isolateSession);
  }

  // Assembly Delete is owned by the assembly tree and cannot consume the
  // Part workspace's unmodified Delete command path.
  {
    auto partDeleteEditorOwner =
        std::make_unique<solidar::MainWindow>(settings);
    solidar::MainWindow& partDeleteEditor = *partDeleteEditorOwner;
    QString partDeleteError;
    CHECK(partDeleteEditor.loadProject(pathA, &partDeleteError));
    CHECK(partDeleteError.isEmpty());
    CHECK(Access::assemblyRemoveEntrypointsConfigured(partDeleteEditor));
    CHECK(!Access::assemblyRemoveEnabled(partDeleteEditor));
    const auto partBodiesBefore = Access::bodyCount(partDeleteEditor);
    Access::triggerAssemblyRemoveShortcut(partDeleteEditor);
    CHECK(Access::bodyCount(partDeleteEditor) == partBodiesBefore);
    CHECK(Access::projectKind(partDeleteEditor) ==
          solidar::EditorProjectKind::Part);
  }

  // The trace owner outlives MainWindow, so destruction can exercise the
  // complete ordered teardown without a callback into a dead stack object.
  {
    using Trace = std::vector<
        std::pair<solidar::AssemblyInsertTeardownStep, bool>>;
    auto destructorTrace = std::make_shared<Trace>();
    std::weak_ptr<Trace> traceLifetime = destructorTrace;
    {
      auto editor = std::make_unique<solidar::MainWindow>(settings);
      CHECK(Access::createNewAssembly(*editor));
      CHECK(Access::stageAssemblyInsert(*editor, pathA));
      editor->setAssemblyReplacementTraceObserverForTesting(
          [destructorTrace](solidar::AssemblyInsertTeardownStep step,
                            bool alive) {
            destructorTrace->emplace_back(step, alive);
          });
    }
    const Trace expectedDestructor{
        {solidar::AssemblyInsertTeardownStep::InsertCallbacksInvalidated,
         false},
        {solidar::AssemblyInsertTeardownStep::InsertControllerCancelled, true},
        {solidar::AssemblyInsertTeardownStep::InsertSessionCancelled, true},
        {solidar::AssemblyInsertTeardownStep::PartDesignPrepared, true},
        {solidar::AssemblyInsertTeardownStep::PartDesignSessionsCancelled,
         true},
        {solidar::AssemblyInsertTeardownStep::GhostUiDetached, false},
        {solidar::AssemblyInsertTeardownStep::AssemblyHistoryCleared, false},
        {solidar::AssemblyInsertTeardownStep::OldWorkspaceDetached, false}};
    CHECK(*destructorTrace == expectedDestructor);
    CHECK(!traceLifetime.expired());
    destructorTrace.reset();
    CHECK(traceLifetime.expired());
  }

  // Part->assembly, assembly->assembly and assembly->part all use the same
  // synchronous replacement protocol. With no staged Insert source, only the
  // two Insert cancellation events carry a false lifetime bit.
  {
    using Step = solidar::AssemblyInsertTeardownStep;
    using Trace = std::vector<std::pair<Step, bool>>;
    const Trace expectedReplacement{
        {Step::InsertCallbacksInvalidated, false},
        {Step::InsertControllerCancelled, false},
        {Step::InsertSessionCancelled, false},
        {Step::PartDesignPrepared, true},
        {Step::PartDesignSessionsCancelled, true},
        {Step::GhostUiDetached, false},
        {Step::AssemblyHistoryCleared, false},
        {Step::OldWorkspaceDetached, false},
        {Step::ProductReplaced, false},
        {Step::WorkspaceInstalled, false}};
    auto trace = std::make_shared<Trace>();
    auto replacementEditor =
        std::make_unique<solidar::MainWindow>(settings);
    QString traceError;
    CHECK(replacementEditor->loadProject(pathA, &traceError));
    CHECK(traceError.isEmpty());
    replacementEditor->setAssemblyReplacementTraceObserverForTesting(
        [trace](Step step, bool alive) { trace->emplace_back(step, alive); });
    const QString assemblyPath =
        QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH);
    CHECK(replacementEditor->loadProject(assemblyPath, &traceError));
    CHECK(traceError.isEmpty());
    CHECK(*trace == expectedReplacement);
    trace->clear();
    CHECK(replacementEditor->loadProject(assemblyPath, &traceError));
    CHECK(traceError.isEmpty());
    CHECK(*trace == expectedReplacement);
    trace->clear();
    CHECK(replacementEditor->loadProject(pathA, &traceError));
    CHECK(traceError.isEmpty());
    CHECK(*trace == expectedReplacement);
    replacementEditor->setAssemblyReplacementTraceObserverForTesting({});
  }

  // The Open command discards its preflight carrier across confirmation and
  // stages the selected path again. Replacing the file in the injected modal
  // seam therefore commits the post-confirmation part, exactly once.
  {
    const QByteArray partABytes = readTestBytes(pathA);
    const QByteArray partBBytes = readTestBytes(pathB);
    CHECK(!partABytes.isEmpty());
    CHECK(!partBBytes.isEmpty());
    const QString commandPath =
        directory.filePath(QStringLiteral("open-command-restage.solidar"));
    CHECK(writeTestBytes(commandPath, partABytes));

    solidar::MainWindow commandEditor(settings);
    CHECK(commandEditor.loadProject(pathA, &error));
    commandEditor.setWindowModified(true);
    int confirmationCount = 0;
    bool replacementWritten = false;
    QString commandError;
    const auto opened = Access::runOpenProjectCommand(
        commandEditor, commandPath,
        [&](solidar::MainWindow&) {
          ++confirmationCount;
          replacementWritten = writeTestBytes(commandPath, partBBytes);
          return replacementWritten;
        },
        &commandError);
    CHECK(opened == Access::OpenProjectTestOutcome::Opened);
    CHECK(confirmationCount == 1);
    CHECK(replacementWritten);
    CHECK(commandError.isEmpty());
    CHECK(commandEditor.windowFilePath() == commandPath);
    CHECK(Access::bodyCount(commandEditor) == 0);
    CHECK(!Access::modified(commandEditor));

    // The post-modal restage may also change the root kind in either
    // direction. The discarded preflight carrier never decides which
    // workspace is committed after the nested event loop returns.
    const QByteArray assemblyBytes = readTestBytes(
        QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH));
    CHECK(!assemblyBytes.isEmpty());
    const QString typeRacePath =
        directory.filePath(QStringLiteral("open-command-type-race.solidar"));
    CHECK(writeTestBytes(typeRacePath, partABytes));
    solidar::MainWindow typeRaceEditor(settings);
    CHECK(typeRaceEditor.loadProject(pathA, &error));
    typeRaceEditor.setWindowModified(true);
    QString typeRaceError;
    const auto openedAssemblyAfterModal = Access::runOpenProjectCommand(
        typeRaceEditor, typeRacePath,
        [&](solidar::MainWindow&) {
          return writeTestBytes(typeRacePath, assemblyBytes);
        },
        &typeRaceError);
    CHECK(openedAssemblyAfterModal ==
          Access::OpenProjectTestOutcome::Opened);
    CHECK(typeRaceError.isEmpty());
    CHECK(Access::projectKind(typeRaceEditor) ==
          solidar::EditorProjectKind::AssemblyReadOnly);

    typeRaceEditor.setWindowModified(true);
    QString reverseTypeRaceError;
    const auto openedPartAfterModal = Access::runOpenProjectCommand(
        typeRaceEditor, typeRacePath,
        [&](solidar::MainWindow&) {
          return writeTestBytes(typeRacePath, partBBytes);
        },
        &reverseTypeRaceError);
    CHECK(openedPartAfterModal == Access::OpenProjectTestOutcome::Opened);
    CHECK(reverseTypeRaceError.isEmpty());
    CHECK(Access::projectKind(typeRaceEditor) ==
          solidar::EditorProjectKind::Part);

    // A queued receiver deletion during the modal seam must be observed by
    // the command before any re-stage, load, or post-modal member access.
    auto* deletingEditor = new solidar::MainWindow(settings);
    CHECK(deletingEditor->loadProject(pathA, &error));
    deletingEditor->setWindowModified(true);
    QPointer<solidar::MainWindow> deletingGuard(deletingEditor);
    int deletingConfirmationCount = 0;
    QString deletingError;
    const auto deleted = Access::runOpenProjectCommand(
        *deletingEditor, pathA,
        [&](solidar::MainWindow& owner) {
          ++deletingConfirmationCount;
          owner.deleteLater();
          QCoreApplication::sendPostedEvents(&owner, QEvent::DeferredDelete);
          return true;
        },
        &deletingError);
    deletingEditor = nullptr;
    CHECK(deleted == Access::OpenProjectTestOutcome::ReceiverDestroyed);
    CHECK(deletingConfirmationCount == 1);
    CHECK(deletingGuard.isNull());
    CHECK(deletingError.isEmpty());

    // Exercise the production QMessageBox path too. The prompt is a guarded
    // heap child, so deleting its owner from the nested event loop destroys
    // both objects once and the static tri-state helper touches neither again.
    auto* modalDeletingEditor = new solidar::MainWindow(settings);
    modalDeletingEditor->setWindowModified(true);
    QPointer<solidar::MainWindow> modalDeletingGuard(modalDeletingEditor);
    QTimer::singleShot(0, [modalDeletingEditor] {
      modalDeletingEditor->deleteLater();
      QCoreApplication::sendPostedEvents(modalDeletingEditor,
                                         QEvent::DeferredDelete);
    });
    const auto modalDeleted =
        Access::confirmProjectReplacement(*modalDeletingEditor);
    modalDeletingEditor = nullptr;
    CHECK(modalDeleted ==
          Access::ConfirmationTestOutcome::ReceiverDestroyed);
    CHECK(modalDeletingGuard.isNull());
  }

  // Native topology picks are V2. A V3 project blocks every commit before
  // model/history allocation and leaves the transient operation retryable.
  // Explicit upgrade is the sole format transition and survives Undo/Redo.
  {
    const solidar::project::ProjectPersistenceState v3{
        solidar::project::ProjectPersistenceFormat::V3};
    const solidar::project::ProjectPersistenceState v4{
        solidar::project::ProjectPersistenceFormat::V4};
    const QString gatePath =
        directory.filePath(QStringLiteral("v3-native-authoring-gate.solidar"));
    const auto source = solidar::ProjectApplicationService{}.stageOpen(pathA);
    CHECK(source.succeeded());
    CHECK(solidar::project::ProjectFile::saveDocument(
        gatePath, source.staged->rootDocument(), &error));

    solidar::MainWindow editor(settings);
    CHECK(editor.loadProject(gatePath, &error));
    auto* upgradeAction = Access::upgradeProjectFormatAction(editor);
    auto* conversionAction = Access::convertToProductDocumentAction(editor);
    CHECK(upgradeAction);
    CHECK(conversionAction);
    CHECK(upgradeAction->objectName() ==
          QStringLiteral("upgradeProjectFormatAction"));
    CHECK(upgradeAction->text() ==
          QString::fromUtf8("Обновить формат проекта до v4…"));
    CHECK(upgradeAction->isEnabled());
    CHECK(conversionAction->isEnabled());

    const auto beforeFaceFeatures = Access::bodyFeatureCount(editor);
    const auto beforeFaceUndo = Access::undoCount(editor);
    CHECK(Access::startV2FaceExtrude(editor));
    CHECK(!Access::projectUpgradeIsQuiescent(editor));
    Access::acceptActiveFaceExtrude(editor);
    CHECK(Access::bodyFeatureCount(editor) == beforeFaceFeatures);
    CHECK(Access::undoCount(editor) == beforeFaceUndo);
    CHECK(Access::activeTool(editor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::viewportHasToolPreview(editor));

    // Upgrade never auto-applies or cancels an unfinished operation.
    upgradeAction->trigger();
    CHECK(Access::persistenceState(editor) == v3);
    CHECK(Access::activeTool(editor) ==
          solidar::PartDesignToolKind::Extrude);
    Access::cancelFaceExtrude(editor);

    const auto beforeEdgeFeatures = Access::bodyFeatureCount(editor);
    const auto beforeEdgeUndo = Access::undoCount(editor);
    CHECK(Access::startV2Fillet(editor));
    Access::acceptActiveFillet(editor);
    CHECK(Access::bodyFeatureCount(editor) == beforeEdgeFeatures);
    CHECK(Access::undoCount(editor) == beforeEdgeUndo);
    CHECK(Access::activeTool(editor) == solidar::PartDesignToolKind::Fillet);
    Access::reset(editor);
    CHECK(editor.loadProject(gatePath, &error));

    const auto beforeSketches = Access::documentSketchCount(editor);
    const auto beforeSketchUndo = Access::undoCount(editor);
    CHECK(Access::finishV2FaceSketch(editor));
    CHECK(Access::sketchWorkspaceActive(editor));
    CHECK(Access::documentSketchCount(editor) == beforeSketches);
    CHECK(Access::undoCount(editor) == beforeSketchUndo);
    CHECK(editor.loadProject(gatePath, &error));

    // Cancel is an exact no-op.
    const auto cancelBodies = Access::bodyCount(editor);
    const auto cancelHistory = Access::historyStepCount(editor);
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->button(QMessageBox::Cancel)->click();
    });
    upgradeAction->trigger();
    CHECK(Access::persistenceState(editor) == v3);
    CHECK(Access::bodyCount(editor) == cancelBodies);
    CHECK(Access::historyStepCount(editor) == cancelHistory);
    CHECK(upgradeAction->isEnabled());

    // Write failure after confirmation preserves the complete editor state.
    const QString failedPath =
        directory.filePath(QStringLiteral("missing/ui-upgrade.solidar"));
    editor.setProjectPath(failedPath);
    const auto failedBodies = Access::bodyCount(editor);
    const auto failedHistory = Access::historyStepCount(editor);
    const auto failedUndo = Access::undoCount(editor);
    const bool failedDirty = Access::modified(editor);
    QTimer modalDriver;
    modalDriver.setInterval(0);
    QObject::connect(&modalDriver, &QTimer::timeout, [&modalDriver] {
      auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
      if (!box) return;
      if (box->windowTitle() ==
          QString::fromUtf8("Обновление формата проекта")) {
        box->button(QMessageBox::Yes)->click();
      } else {
        box->accept();
        modalDriver.stop();
      }
    });
    modalDriver.start();
    upgradeAction->trigger();
    modalDriver.stop();
    CHECK(Access::persistenceState(editor) == v3);
    CHECK(editor.windowFilePath() == failedPath);
    CHECK(Access::bodyCount(editor) == failedBodies);
    CHECK(Access::historyStepCount(editor) == failedHistory);
    CHECK(Access::undoCount(editor) == failedUndo);
    CHECK(Access::modified(editor) == failedDirty);
    CHECK(!QFile::exists(failedPath));

    // A confirmed successful upgrade writes version 4 first, then changes
    // state/savepoint. Subsequent native V2 commit is exactly one command.
    editor.setProjectPath(gatePath);
    CHECK(Access::startTopFaceExtrude(editor, 15.0));
    Access::acceptActiveFaceExtrude(editor);
    CHECK(Access::lastFeatureUsesV1Face(editor));
    CHECK(Access::projectUpgradeIsQuiescent(editor));
    CHECK(upgradeAction->isEnabled());
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->button(QMessageBox::Yes)->click();
    });
    Access::upgradeProjectFormat(editor);
    CHECK(Access::persistenceState(editor) == v4);
    CHECK(!upgradeAction->isEnabled());
    CHECK(conversionAction->isEnabled());
    CHECK(!Access::modified(editor));
    QFile upgraded(gatePath);
    CHECK(upgraded.open(QIODevice::ReadOnly));
    CHECK(QJsonDocument::fromJson(upgraded.readAll())
              .object()
              .value(QStringLiteral("version"))
              .toInteger() == 4);

    // Loading and changing only a parameter preserves the authored V1
    // carrier. Explicit face reselection replaces it with the fresh V2 pick.
    CHECK(editor.loadProject(gatePath, &error));
    CHECK(Access::lastFeatureUsesV1Face(editor));
    CHECK(Access::editLastFaceExtrudeParameter(editor, 3.0));
    CHECK(Access::lastFeatureUsesV1Face(editor));
    CHECK(Access::reselectLastFaceExtrudeV2(editor));
    CHECK(Access::lastFeatureUsesV2Face(editor));

    const auto beforeCommitUndo = Access::undoCount(editor);
    CHECK(Access::startV2FaceExtrude(editor));
    Access::acceptActiveFaceExtrude(editor);
    CHECK(Access::undoCount(editor) == beforeCommitUndo + 1);
    CHECK(Access::lastFeatureUsesV2Face(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::persistenceState(editor) == v4);
    CHECK(!upgradeAction->isEnabled());
    // Undo the explicit V2 reselection as well: the document now contains no
    // authored V2 carrier, but the project format must never downgrade.
    Access::undoAction(editor)->trigger();
    CHECK(Access::lastFeatureUsesV1Face(editor));
    CHECK(Access::persistenceState(editor) == v4);
    CHECK(!upgradeAction->isEnabled());
    Access::redoAction(editor)->trigger();
    CHECK(Access::persistenceState(editor) == v4);
    CHECK(Access::lastFeatureUsesV2Face(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::persistenceState(editor) == v4);
    CHECK(Access::lastFeatureUsesV2Face(editor));

    // Direct V4 invocation is an exact no-op and cannot dirty the savepoint.
    const auto noOpUndo = Access::undoCount(editor);
    const auto noOpBodies = Access::bodyCount(editor);
    const bool noOpDirty = Access::modified(editor);
    Access::upgradeProjectFormat(editor);
    CHECK(Access::persistenceState(editor) == v4);
    CHECK(Access::undoCount(editor) == noOpUndo);
    CHECK(Access::bodyCount(editor) == noOpBodies);
    CHECK(Access::modified(editor) == noOpDirty);
  }

  // Historical v1 projects have a rendered legacy solid but no Body/B-Rep or
  // FaceReference. Its faces still publish a fully typed semantic source: the
  // presentation label is not used to recover placement or source kind.
  {
    const QString legacyPath =
        directory.filePath(QStringLiteral("historical-v1-solid.solidar"));
    solidar::project::ProjectData legacy;
    legacy.box = {60.0, 40.0, 25.0};
    solidar::sketch::Sketch profile;
    profile.addRectangle({-15.0, -10.0}, {15.0, 10.0});
    solidar::sketch::Sketch decoyProfile;
    decoyProfile.addRectangle({100.0, 100.0}, {105.0, 105.0});
    legacy.sketches.push_back(
        {decoyProfile, QStringLiteral("XY")});
    legacy.sketches.push_back(
        {profile, QStringLiteral("arbitrary presentation label")});
    legacy.hasExtrusion = true;
    legacy.extrusionSourceSketch = 1;
    CHECK(solidar::project::ProjectFile::save(legacyPath, legacy, &error));
    const QString missingSourcePath = directory.filePath(
        QStringLiteral("historical-v1-missing-source.solidar"));
    auto missingSource = legacy;
    missingSource.extrusionSourceSketch.reset();
    CHECK(solidar::project::ProjectFile::save(missingSourcePath,
                                              missingSource, &error));

    const auto click = [](solidar::Viewport& viewport, QPointF position) {
      QMouseEvent event(QEvent::MouseButtonPress, position, position,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(&viewport, &event);
    };

    solidar::MainWindow sketchEditor(settings);
    CHECK(sketchEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(sketchEditor));
    const auto stableLegacySource =
        Access::historicalLegacySourceSketchId(sketchEditor);
    CHECK(stableLegacySource);
    auto* legacyConversionAction =
        Access::convertToProductDocumentAction(sketchEditor);
    CHECK(legacyConversionAction);
    CHECK(!legacyConversionAction->isEnabled());
    CHECK(!Access::productConversionIsEligible(sketchEditor));
    const auto* const legacyDocumentBefore =
        Access::documentAddress(sketchEditor);
    const auto legacyIdentityBefore = Access::identity(sketchEditor);
    const auto legacyPathBefore = sketchEditor.windowFilePath();
    const auto legacyRevisionBefore =
        Access::currentHistoryRevision(sketchEditor);
    const auto legacyPreviewBefore =
        Access::previewDocumentGeneration(sketchEditor);
    const QString rejectedLegacyTarget = directory.filePath(
        QStringLiteral("historical-v1-rejected-product.solidar"));
    bool legacyChooserCalled = false;
    bool legacyFactoryCalled = false;
    bool legacyAttempted = true;
    const auto legacyRejected = Access::runProductConversionCommand(
        sketchEditor,
        [&](const QString&) -> std::optional<QString> {
          legacyChooserCalled = true;
          return rejectedLegacyTarget;
        },
        [&](const QString&, solidar::DefinitionId root) {
          legacyFactoryCalled = true;
          return uiConversionRequest(root);
        },
        &legacyAttempted);
    CHECK(!legacyRejected.succeeded());
    CHECK(legacyRejected.code ==
          solidar::ProjectApplicationErrorCode::InvalidProject);
    CHECK(!legacyChooserCalled);
    CHECK(!legacyFactoryCalled);
    CHECK(!legacyAttempted);
    CHECK(!QFile::exists(rejectedLegacyTarget));
    CHECK(Access::documentAddress(sketchEditor) == legacyDocumentBefore);
    CHECK(Access::identity(sketchEditor) == legacyIdentityBefore);
    CHECK(sketchEditor.windowFilePath() == legacyPathBefore);
    CHECK(Access::historicalLegacySourceSketchId(sketchEditor) ==
          stableLegacySource);
    CHECK(Access::currentHistoryRevision(sketchEditor) ==
          legacyRevisionBefore);
    CHECK(Access::previewDocumentGeneration(sketchEditor) ==
          legacyPreviewBefore);
    CHECK(!Access::hasCommittedModernSolid(sketchEditor));
    CHECK(!Access::hasDisplayableModernSolid(sketchEditor));
    QString legacyExportError;
    CHECK(!Access::hasExportableModernSolid(sketchEditor,
                                             &legacyExportError));
    CHECK(!legacyExportError.isEmpty());
    CHECK(Access::viewportSolidVisible(sketchEditor));
    // The v1 positional index is consumed only at the load boundary. Reordering
    // the derived UI rows cannot retarget the historical solid to another
    // sketch; stable ID resolution reconstructs the exact same profile.
    Access::reverseSketchPresentationOrder(sketchEditor);
    CHECK(Access::historicalLegacySourceSketchId(sketchEditor) ==
          stableLegacySource);
    CHECK(Access::legacySolidSketchEquals(sketchEditor, profile));
    Access::reverseSketchPresentationOrder(sketchEditor);
    CHECK(Access::historicalLegacySourceSketchId(sketchEditor) ==
          stableLegacySource);
    // Reapplying the end marker must not erase the presentation-only v1
    // prism merely because its Document intentionally contains no Body.
    Access::moveHistoryToEnd(sketchEditor);
    Access::moveHistoryToEnd(sketchEditor);
    CHECK(Access::viewportSolidVisible(sketchEditor));
    auto* sketchViewport = Access::viewport(sketchEditor);
    sketchViewport->resize(800, 600);
    sketchViewport->viewBottom();
    const QPointF endCap(sketchViewport->width() * 0.5,
                         sketchViewport->height() * 0.52);
    int sketchFaceEvents = 0;
    QObject::connect(
        sketchViewport, &solidar::Viewport::sketchPlanePicked, sketchViewport,
        [&](const solidar::SketchPlanePick& pick) {
          if (std::holds_alternative<solidar::LegacySolidFacePick>(pick.source))
            ++sketchFaceEvents;
        });
    sketchViewport->beginSketchPlaneSelection();
    click(*sketchViewport, endCap);
    Access::finishViewAnimation(*sketchViewport);
    CHECK(sketchFaceEvents == 1);
    CHECK(Access::workspace(sketchEditor)->currentWidget() ==
          Access::sketch(sketchEditor));
    CHECK(std::abs(Access::currentSketchPlacement(sketchEditor).origin.z -
                   25.0) < 1e-9);
    CHECK(Access::historicalLegacyExtrusionActive(sketchEditor));

    solidar::MainWindow extrusionEditor(settings);
    CHECK(extrusionEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(extrusionEditor));
    Access::moveHistoryToEnd(extrusionEditor);
    CHECK(Access::viewportSolidVisible(extrusionEditor));
    auto* extrusionViewport = Access::viewport(extrusionEditor);
    extrusionViewport->resize(800, 600);
    const solidar::ViewportCameraState extrusionCamera{
        extrusionViewport->cameraYawDegrees(),
        extrusionViewport->cameraPitchDegrees(), 1.0F, {},
        extrusionViewport->size()};
    const QPointF extrusionCap =
        extrusionCamera.worldToScreen({0.0, 0.0, 25.0});
    int sourceEvents = 0;
    bool sourceSemanticValid = false;
    bool sourceGeometryValid = false;
    QObject::connect(
        extrusionViewport, &solidar::Viewport::extrusionSourcePicked,
        extrusionViewport, [&](const solidar::ExtrusionSourcePick& pick) {
          if (const auto* face =
                  std::get_if<solidar::LegacySolidFacePick>(&pick.source)) {
            ++sourceEvents;
            sourceSemanticValid =
                (face->face == solidar::LegacySolidFace::EndCap &&
                 std::abs(face->placement.origin.z - 25.0) < 1e-9) ||
                (face->face == solidar::LegacySolidFace::InitialCap &&
                 std::abs(face->placement.origin.z) < 1e-9);
            sourceGeometryValid =
                face->geometry.semanticFingerprint() ==
                    profile.semanticFingerprint() &&
                face->geometry.semanticallyEqual(profile);
          }
        });
    extrusionViewport->beginExtrusionSurfaceSelection();
    click(*extrusionViewport, extrusionCap);
    CHECK(sourceEvents == 1);
    CHECK(sourceSemanticValid);
    CHECK(sourceGeometryValid);
    CHECK(Access::selectedExtrusionIsLegacyFace(extrusionEditor));
    CHECK(Access::legacyExtrusionDockVisible(extrusionEditor));

    // Committing a typed legacy-face pick creates the modern authoritative
    // Body and consumes the compatibility presentation. Deleting that Body
    // must leave an empty viewport, not resurrect the historical prism.
    CHECK(Access::commitLegacyExtrusionAsNewBody(extrusionEditor));
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(Access::convertToProductDocumentAction(extrusionEditor)->isEnabled());
    CHECK(Access::productConversionIsEligible(extrusionEditor));
    const auto modernBodyId = Access::firstBodyId(extrusionEditor);
    CHECK(modernBodyId != solidar::kInvalidBodyId);

    // Undo resets the viewport scene, so the compatibility mesh must be
    // reconstructed from the restored Document sketch rather than from a
    // retained geometry mirror. The next typed pick is the exact source the
    // legacy Extrude command would consume.
    Access::undoAction(extrusionEditor)->trigger();
    CHECK(Access::bodyCount(extrusionEditor) == 0);
    CHECK(Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(!Access::convertToProductDocumentAction(extrusionEditor)->isEnabled());
    CHECK(!Access::productConversionIsEligible(extrusionEditor));
    CHECK(Access::viewportSolidVisible(extrusionEditor));
    CHECK(Access::legacySolidSketchEquals(extrusionEditor, profile));
    sourceEvents = 0;
    sourceSemanticValid = false;
    sourceGeometryValid = false;
    const solidar::ViewportCameraState undoCamera{
        extrusionViewport->cameraYawDegrees(),
        extrusionViewport->cameraPitchDegrees(), 1.0F, {},
        extrusionViewport->size()};
    extrusionViewport->beginExtrusionSurfaceSelection();
    click(*extrusionViewport,
          undoCamera.worldToScreen({0.0, 0.0, 25.0}));
    CHECK(sourceEvents == 1);
    CHECK(sourceSemanticValid);
    CHECK(sourceGeometryValid);
    CHECK(Access::selectedExtrusionIsLegacyFace(extrusionEditor));

    Access::redoAction(extrusionEditor)->trigger();
    CHECK(Access::bodyCount(extrusionEditor) == 1);
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(Access::convertToProductDocumentAction(extrusionEditor)->isEnabled());
    CHECK(Access::productConversionIsEligible(extrusionEditor));
    CHECK(Access::displayedBodyShape(extrusionEditor));

    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeBody(extrusionEditor, modernBodyId);
    CHECK(Access::bodyCount(extrusionEditor) == 0);
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(!Access::viewportSolidVisible(extrusionEditor));

    // The compatibility source is part of the history transition as a stable
    // SketchId. Removing the source must clear the presentation, Undo must
    // restore the exact source, and Redo must fail closed again.
    solidar::MainWindow sourceDeletionEditor(settings);
    CHECK(sourceDeletionEditor.loadProject(legacyPath, &error));
    const auto deletedLegacySource =
        Access::historicalLegacySourceSketchId(sourceDeletionEditor);
    CHECK(deletedLegacySource);
    CHECK(Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistorySketch(sourceDeletionEditor, *deletedLegacySource);
    CHECK(!Access::hasSketch(sourceDeletionEditor, *deletedLegacySource));
    CHECK(!Access::historicalLegacySourceSketchId(sourceDeletionEditor));
    CHECK(!Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    CHECK(!Access::viewportSolidVisible(sourceDeletionEditor));

    Access::undoAction(sourceDeletionEditor)->trigger();
    CHECK(Access::hasSketch(sourceDeletionEditor, *deletedLegacySource));
    CHECK(Access::historicalLegacySourceSketchId(sourceDeletionEditor) ==
          deletedLegacySource);
    CHECK(Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    CHECK(Access::viewportSolidVisible(sourceDeletionEditor));
    CHECK(Access::legacySolidSketchEquals(sourceDeletionEditor, profile));

    Access::redoAction(sourceDeletionEditor)->trigger();
    CHECK(!Access::hasSketch(sourceDeletionEditor, *deletedLegacySource));
    CHECK(!Access::historicalLegacySourceSketchId(sourceDeletionEditor));
    CHECK(!Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    CHECK(!Access::viewportSolidVisible(sourceDeletionEditor));

    // Every project replacement establishes its own presentation generation:
    // neither a modern v2 project nor an empty project may inherit v1 state.
    CHECK(extrusionEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(extrusionEditor.loadProject(pathA, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(Access::displayedBodyShape(extrusionEditor));
    CHECK(extrusionEditor.loadProject(pathB, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(!Access::viewportSolidVisible(extrusionEditor));

    // `hasExtrusion=true` without an explicit source is a valid historical
    // file, but it cannot authorize a guessed box solid. The compatibility
    // path must fail closed across history refresh, a real edit/Undo cycle and
    // project replacement.
    solidar::MainWindow missingSourceEditor(settings);
    CHECK(missingSourceEditor.loadProject(missingSourcePath, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));
    Access::moveHistoryToEnd(missingSourceEditor);
    Access::moveHistoryToEnd(missingSourceEditor);
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));
    auto* missingViewport = Access::viewport(missingSourceEditor);
    missingViewport->resize(800, 600);
    const solidar::ViewportCameraState missingCamera{
        missingViewport->cameraYawDegrees(),
        missingViewport->cameraPitchDegrees(), 1.0F, {},
        missingViewport->size()};
    const QPointF absentSolidPoint =
        missingCamera.worldToScreen({25.0, 15.0, 25.0});
    int missingLegacyPlaneEvents = 0;
    int missingExtrusionEvents = 0;
    QObject::connect(
        missingViewport, &solidar::Viewport::sketchPlanePicked,
        missingViewport, [&](const solidar::SketchPlanePick& pick) {
          if (std::holds_alternative<solidar::LegacySolidFacePick>(pick.source))
            ++missingLegacyPlaneEvents;
        });
    QObject::connect(
        missingViewport, &solidar::Viewport::extrusionSourcePicked,
        missingViewport,
        [&](const solidar::ExtrusionSourcePick&) { ++missingExtrusionEvents; });
    missingViewport->beginSketchPlaneSelection();
    click(*missingViewport, absentSolidPoint);
    CHECK(missingLegacyPlaneEvents == 0);
    missingViewport->beginExtrusionSurfaceSelection();
    click(*missingViewport, absentSolidPoint);
    CHECK(missingExtrusionEvents == 0);
    Access::cancelLegacyExtrusion(missingSourceEditor);

    const auto missingSketchId = Access::firstSketchId(missingSourceEditor);
    CHECK(missingSketchId != solidar::kInvalidSketchId);
    auto editedMissingProfile = profile;
    editedMissingProfile.addCircle({0.0, 0.0}, 2.0);
    CHECK(Access::finishSketchEdit(missingSourceEditor, missingSketchId,
                                   editedMissingProfile,
                                   solidar::SketchPlacement::xy()));
    Access::undoAction(missingSourceEditor)->trigger();
    CHECK(!Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));

    CHECK(missingSourceEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(Access::viewportSolidVisible(missingSourceEditor));
    CHECK(missingSourceEditor.loadProject(missingSourcePath, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));
  }

  // Hiding the final visible Body clears the B-Rep display. It must also turn
  // off the legacy solid fallback; otherwise the viewport draws a stale box
  // with the previous body's dimensions even though no visible Body exists.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow visibilityEditor(settings);
    CHECK(visibilityEditor.loadProject(pathA, &error));
    // Part Design operations belong to the history timeline. The model tree
    // only exposes Body rows and their visibility, without duplicating inert
    // Extrude/Move/Fillet/etc. children.
    CHECK(Access::bodyRowsHaveNoFeatureChildren(visibilityEditor));
    CHECK(Access::displayedBodyShape(visibilityEditor));
    CHECK(Access::viewportSolidVisible(visibilityEditor));
    CHECK(Access::hasDisplayableModernSolid(visibilityEditor));
    CHECK(!Access::foreignShapeIsDisplayable(visibilityEditor));
    CHECK(Access::hasExportableModernSolid(visibilityEditor));
    CHECK(Access::setFirstBodyVisible(visibilityEditor, false));
    CHECK(!Access::displayedBodyShape(visibilityEditor));
    CHECK(!Access::viewportSolidVisible(visibilityEditor));
    CHECK(!Access::hasDisplayableModernSolid(visibilityEditor));
    // Visibility is presentation state. A hidden, otherwise valid committed
    // Body remains an intentional member of the complete export snapshot.
    CHECK(Access::hasExportableModernSolid(visibilityEditor));
    CHECK(Access::setFirstBodyVisible(visibilityEditor, true));
    CHECK(Access::displayedBodyShape(visibilityEditor));
    CHECK(Access::viewportSolidVisible(visibilityEditor));
    CHECK(Access::hasDisplayableModernSolid(visibilityEditor));
  }

  // Committed history, display fallback and exportability are deliberately
  // different queries. An Error feature may retain the last validated shape
  // for presentation, but it must never authorize a stale STL export.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document errorDocument;
    auto& profile = errorDocument.addSketch("Error profile");
    profile.geometry.addRectangle({0.0, 0.0}, {12.0, 8.0});
    auto& body = errorDocument.addBody("Error body");
    auto& feature = body.addFeature(
        std::make_unique<solidar::ExtrudeFeature>(profile.id, 6.0));
    CHECK(errorDocument.recompute());
    auto* extrude = dynamic_cast<solidar::ExtrudeFeature*>(&feature);
    CHECK(extrude);
    extrude->setLengthMm(0.0);
    CHECK(!errorDocument.recompute());

    solidar::MainWindow errorEditor(settings);
    Access::installDocument(errorEditor, std::move(errorDocument));
    CHECK(Access::hasCommittedModernSolid(errorEditor));
    CHECK(Access::hasDisplayableModernSolid(errorEditor));
    QString exportError;
    CHECK(!Access::hasExportableModernSolid(errorEditor, &exportError));
    CHECK(!exportError.isEmpty());
    CHECK(Access::viewportSolidVisible(errorEditor));

    solidar::Document invalidDocument;
    auto& invalidProfile = invalidDocument.addSketch("Invalid profile");
    invalidProfile.geometry.addRectangle({0.0, 0.0}, {9.0, 7.0});
    auto& invalidBody = invalidDocument.addBody("Invalid body");
    invalidBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        invalidProfile.id, 0.0));
    CHECK(!invalidDocument.recompute());
    Access::installDocument(errorEditor, std::move(invalidDocument));
    CHECK(Access::hasCommittedModernSolid(errorEditor));
    CHECK(!Access::hasDisplayableModernSolid(errorEditor));
    exportError.clear();
    CHECK(!Access::hasExportableModernSolid(errorEditor, &exportError));
    CHECK(!exportError.isEmpty());
    CHECK(!Access::viewportSolidVisible(errorEditor));

    solidar::Document noTopologyDocument;
    auto& noTopologyBody = noTopologyDocument.addBody("No topology body");
    noTopologyBody.addFeature(
        std::make_unique<solidar::UnindexedPresentationFeature>(
            std::make_shared<const TopoDS_Shape>(
                BRepPrimAPI_MakeBox(7.0, 8.0, 9.0).Shape())));
    Access::installDocument(errorEditor, std::move(noTopologyDocument));
    CHECK(Access::hasCommittedModernSolid(errorEditor));
    CHECK(!Access::hasDisplayableModernSolid(errorEditor));
    CHECK(!Access::viewportHasBodyPresentations(errorEditor));
    CHECK(!Access::viewportSolidVisible(errorEditor));
  }

  // Entering Sketcher after a downstream Move must show the current moved
  // body without painting every consumed source sketch at its old placement.
  // Those blue upstream contours looked like a duplicate stale solid.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document document;
    auto& baseSketch = document.addSketch("Moved body source");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    const auto baseSketchId = baseSketch.id;
    auto& inspectionSketch = document.addSketch("Inspection sketch");
    inspectionSketch.geometry.addLine({0.0, 0.0}, {10.0, 0.0});
    const auto inspectionSketchId = inspectionSketch.id;
    auto& body = document.addBody("Moved body");
    auto extrusion = std::make_unique<solidar::ExtrudeFeature>(
        baseSketchId, 15.0, "Extrude");
    const auto extrusionId = extrusion->id();
    body.addFeature(std::move(extrusion));
    body.addFeature(std::make_unique<solidar::MoveFeature>(
        extrusionId, solidar::Vector3d{-80.0, 0.0, 0.0}, "Move"));
    CHECK(document.recompute());

    const QString movedPath =
        directory.filePath(QStringLiteral("moved-sketch-scene.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        movedPath, document, &error));
    solidar::MainWindow movedEditor(settings);
    CHECK(movedEditor.loadProject(movedPath, &error));
    Access::editSketch(movedEditor, inspectionSketchId);
    CHECK(Access::workspace(movedEditor)->currentWidget() ==
          Access::sketch(movedEditor));
    CHECK(Access::sketch(movedEditor)->sceneBodyCount() == 1);
    CHECK(Access::sketch(movedEditor)->sceneSketchCount() == 0);
  }

  }();

  // Headless document replacement: repeated loadProject drives the full
  // teardown path (PartDesignToolController cancelActive, session cancel,
  // resetScene, history rebuild). No file dialogs, no OpenGL surface, no
  // platform-specific code.
  solidar::MainWindow editor(settings);
  for (int cycle = 0; cycle < 6; ++cycle) {
    CHECK(editor.loadProject(pathA, &error));
    CHECK(solidar::MainWindowUndoTestAccess::displayedSketchesMatchDocument(
        editor));
    CHECK(editor.loadProject(pathB, &error));
    CHECK(solidar::MainWindowUndoTestAccess::displayedSketchesMatchDocument(
        editor));
  }

  // A rejected begin must pass through the production begin-result binder
  // before any tool UI is allowed to survive.
  for (const auto kind : {solidar::PartDesignToolKind::Fillet,
                          solidar::PartDesignToolKind::Chamfer,
                          solidar::PartDesignToolKind::Shell,
                          solidar::PartDesignToolKind::Draft})
    CHECK(solidar::MainWindowUndoTestAccess::rejectedBeginLeavesNoToolUi(
        editor, kind));

  // Save must commit the sketcher's working copy before serializing.  Without
  // this, the UI reported success while the visible sketch was absent from
  // the file on disk.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    canvas->setRectangle(12.0, 8.0);
    CHECK(Access::modified(editor));
    Access::workspace(editor)->setCurrentWidget(canvas);
    Access::save(editor);
    CHECK(!Access::modified(editor));

    solidar::Document restored;
    CHECK(solidar::project::ProjectFile::loadDocument(pathB, &restored,
                                                       &error));
    CHECK(restored.sketches().size() == 1);
    CHECK(!restored.sketches().front().geometry.lines().empty());
  }

  // Window dirty follows the history revision/savepoint relation. Save moves
  // the savepoint; Undo/Redo become clean only when they return to it.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    CHECK(editor.loadProject(pathB, &error));
    CHECK(Access::pushBodyMarker(editor, "savepoint edit"));
    CHECK(Access::modified(editor));
    Access::save(editor);
    CHECK(!Access::modified(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::modified(editor));
    Access::redoAction(editor)->trigger();
    CHECK(!Access::modified(editor));

    Access::undoAction(editor)->trigger();
    Access::save(editor);
    CHECK(!Access::modified(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::modified(editor));
    Access::undoAction(editor)->trigger();
    CHECK(!Access::modified(editor));
  }

  // Switching between the legacy extrusion picker, the dedicated Revolve
  // panel and the shared Part Design panel must leave exactly one tool UI.
  // This used to reproduce after a longer session because these three paths
  // had independent teardown code.
  CHECK(editor.loadProject(pathA, &error));

  // Model and Sketch histories are separate domains. A new committed model
  // action invalidates a stale model redo, and switching workspaces exposes
  // only the active domain's availability.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    Access::reset(editor);
    const auto initialBodyCount = Access::bodyCount(editor);
    CHECK(Access::pushBodyMarker(editor, "history-domain-marker-1"));
    CHECK(Access::modified(editor));
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == initialBodyCount);
    CHECK(Access::redoCount(editor) == 1);
    CHECK(Access::redoAction(editor)->isEnabled());

    CHECK(Access::pushBodyMarker(editor, "history-domain-marker-2"));
    CHECK(Access::redoCount(editor) == 0);
    CHECK(!Access::redoAction(editor)->isEnabled());

    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    Access::workspace(editor)->setCurrentWidget(canvas);
    QApplication::processEvents();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());

    canvas->setRectangle(12.0, 8.0);
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());
    Access::undoAction(editor)->trigger();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(Access::redoAction(editor)->isEnabled());

    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
    QApplication::processEvents();
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());

    Access::workspace(editor)->setCurrentWidget(canvas);
    QApplication::processEvents();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(Access::redoAction(editor)->isEnabled());
    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
  }

  // Presentation-only transitions are typed history commands. Their first
  // Undo must restore presentation without falling through to the preceding
  // CAD command.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    CHECK(editor.loadProject(pathA, &error));
    Access::reset(editor);
    const std::size_t initialBodies = Access::bodyCount(editor);
    CHECK(Access::pushBodyMarker(editor, "presentation predecessor"));
    const std::size_t bodiesAfterModel = Access::bodyCount(editor);
    CHECK(bodiesAfterModel == initialBodies + 1);
    const std::size_t undoAfterModel = Access::undoCount(editor);

    CHECK(Access::originVisible(editor));
    CHECK(Access::setOriginVisible(editor, false));
    CHECK(!Access::originVisible(editor));
    CHECK(Access::undoCount(editor) == undoAfterModel + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::originVisible(editor));
    CHECK(Access::bodyCount(editor) == bodiesAfterModel);
    Access::redoAction(editor)->trigger();
    CHECK(!Access::originVisible(editor));

    const QPointF initialPosition = Access::bodyPosition(editor);
    const QPointF movedPosition(initialPosition.x() + 17.0,
                                initialPosition.y() - 9.0);
    Access::commitBodyPosition(editor, movedPosition);
    CHECK(Access::bodyPosition(editor) == movedPosition);
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyPosition(editor) == initialPosition);
    CHECK(!Access::originVisible(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::bodyPosition(editor) == movedPosition);

    // Every typed visibility command must update the viewport and matching
    // tree checkbox together.  Exercise both transition directions so the
    // next real click cannot be consumed by stale checkbox state.
    CHECK(editor.loadProject(pathA, &error));
    Access::reset(editor);
    CHECK(Access::historyStepCount(editor) > 0);
    CHECK(Access::presentationMatchesTree(editor));
    const bool initialOriginVisible = Access::originVisible(editor);
    const std::size_t beforeOrigin = Access::undoCount(editor);
    CHECK(Access::setOriginVisible(editor, !initialOriginVisible));
    CHECK(Access::originVisible(editor) != initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    CHECK(Access::undoCount(editor) == beforeOrigin + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) == initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) != initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforeOriginOn = Access::undoCount(editor);
    CHECK(Access::setOriginVisible(editor, initialOriginVisible));
    CHECK(Access::undoCount(editor) == beforeOriginOn + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) != initialOriginVisible);
    Access::redoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) == initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforePlane = Access::undoCount(editor);
    CHECK(Access::setBasePlaneVisible(editor, 0, true));
    CHECK(Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));
    CHECK(Access::undoCount(editor) == beforePlane + 1);
    Access::undoAction(editor)->trigger();
    CHECK(!Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforePlaneOff = Access::undoCount(editor);
    CHECK(Access::setBasePlaneVisible(editor, 0, false));
    CHECK(!Access::basePlaneVisible(editor, 0));
    CHECK(Access::undoCount(editor) == beforePlaneOff + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::basePlaneVisible(editor, 0));
    Access::redoAction(editor)->trigger();
    CHECK(!Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));

    CHECK(Access::sketchViewCount(editor) == 1);
    const bool initialSketchVisible = Access::sketchVisible(editor, 0);
    const std::size_t beforeSketch = Access::undoCount(editor);
    CHECK(Access::setSketchVisible(editor, 0, !initialSketchVisible));
    CHECK(Access::sketchVisible(editor, 0) != initialSketchVisible);
    CHECK(Access::undoCount(editor) == beforeSketch + 1);
    CHECK(Access::presentationMatchesTree(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) == initialSketchVisible);
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) != initialSketchVisible);
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforeSketchBack = Access::undoCount(editor);
    CHECK(Access::setSketchVisible(editor, 0, initialSketchVisible));
    CHECK(Access::undoCount(editor) == beforeSketchBack + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) != initialSketchVisible);
    Access::redoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) == initialSketchVisible);
    CHECK(Access::presentationMatchesTree(editor));

    // Mixed visibility remains internally consistent while the model history
    // scrubber derives the presentation for an earlier step and returns.
    CHECK(Access::setOriginVisible(editor, true));
    CHECK(Access::setBasePlaneVisible(editor, 1, true));
    CHECK(Access::setBasePlaneVisible(editor, 2, true));
    CHECK(Access::setSketchVisible(editor, 0, true));
    CHECK(Access::presentationMatchesTree(editor));
    Access::applyHistoryPosition(editor, 0);
    CHECK(Access::presentationMatchesTree(editor));
    Access::moveHistoryToEnd(editor);
    CHECK(Access::presentationMatchesTree(editor));
  }

  // A localized model command retains only the changed Body slice. Unchanged
  // Bodies are structurally shared by omission from the delta.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document before;
    for (int index = 0; index < 200; ++index)
      before.addBody("unchanged-body-" + std::to_string(index) +
                     std::string(128, 'x'));
    solidar::Document after = before;
    after.findBody(before.bodies()[100].id())->setName("changed-body");
    const auto [sliceCount, retained] =
        Access::documentDeltaSize(before, after);
    CHECK(sliceCount == 1);
    CHECK(retained < 64U * 1024U);
    CHECK(Access::importedHistoryIsColdAndRebuildable());

    solidar::Document featureBefore;
    const auto seed = addBox(featureBefore, 0.0, 20.0, 10.0, "Delta chain");
    CHECK(seed.has_value());
    auto* chainBody = featureBefore.findBody(seed->bodyId);
    CHECK(chainBody);
    auto sourceId = seed->featureId;
    for (int index = 0; index < 80; ++index) {
      auto move = std::make_unique<solidar::MoveFeature>(
          sourceId, solidar::Vector3d{0.1, 0.0, 0.0}, "Chain move");
      sourceId = move->id();
      chainBody->addFeature(std::move(move));
    }
    CHECK(featureBefore.recompute());
    solidar::Document featureAfter = featureBefore;
    auto* changed = dynamic_cast<solidar::MoveFeature*>(
        featureAfter.findFeature(chainBody->features()[40]->id()));
    CHECK(changed);
    changed->setOffsetMm({0.2, 0.0, 0.0});
    const auto [featureSlices, featureRetained] =
        Access::documentDeltaSize(featureBefore, featureAfter);
    CHECK(featureSlices == 1);
    CHECK(featureRetained < 16U * 1024U);
  }

  // Failed command application is transactional: the source entry remains,
  // the committed Document is restored, and the RAII recursion guard drops.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    CHECK(editor.loadProject(pathA, &error));
    Access::reset(editor);
    const auto bodiesBefore = Access::bodyCount(editor);
    Access::pushThrowingUndo(editor);
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == bodiesBefore);
    CHECK(Access::undoCount(editor) == 1);
    CHECK(Access::redoCount(editor) == 0);
    CHECK(!Access::applyingUndo(editor));
  }

  // Model-history sketch comparison is the complete persisted definition,
  // not the solver cache key. Exercise the real Finish Sketch path for a
  // construction-only change and placement-only change, then prove that an
  // unchanged finish is a no-op that does not consume history.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document sketchDocument;
    auto& item = sketchDocument.addSketch("Semantic sketch");
    item.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
    item.placement = solidar::SketchPlacement::xy();
    const auto sketchId = item.id;
    const auto baseGeometry = item.geometry;
    Access::installDocument(editor, std::move(sketchDocument));
    CHECK(Access::displayedSketchesMatchDocument(editor));

    auto construction = baseGeometry;
    construction.setLineDashedById(construction.lineId(0), true);
    CHECK(construction.solverFingerprint() == baseGeometry.solverFingerprint());
    CHECK(Access::finishSketchEdit(editor, sketchId, construction,
                                   solidar::SketchPlacement::xy()));
    CHECK(Access::documentCopy(editor).findSketch(sketchId)->geometry
              .semanticallyEqual(construction));
    const auto constructionSelection = Access::selectionState(editor);
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(Access::documentCopy(editor).findSketch(sketchId)->geometry
                .semanticallyEqual(baseGeometry));
      CHECK(Access::displayedSketchesMatchDocument(editor));
      Access::redoAction(editor)->trigger();
      CHECK(Access::documentCopy(editor).findSketch(sketchId)->geometry
                .semanticallyEqual(construction));
      CHECK(Access::displayedSketchesMatchDocument(editor));
    }
    CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                     constructionSelection));

    auto translatedPlacement = solidar::SketchPlacement::xy();
    translatedPlacement.origin = {3.0, 4.0, 5.0};
    translatedPlacement.xDirection = {0.0, 1.0, 0.0};
    translatedPlacement.yDirection = {0.0, 0.0, 1.0};
    CHECK(Access::finishSketchEdit(editor, sketchId, construction,
                                   translatedPlacement));
    auto placed = Access::documentCopy(editor);
    CHECK(placed.findSketch(sketchId)->placement.origin.x == 3.0);
    CHECK(placed.findSketch(sketchId)->placement.origin.y == 4.0);
    CHECK(placed.findSketch(sketchId)->placement.origin.z == 5.0);
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      auto unplaced = Access::documentCopy(editor);
      CHECK(unplaced.findSketch(sketchId)->placement.origin.x == 0.0);
      CHECK(unplaced.findSketch(sketchId)->placement.origin.y == 0.0);
      CHECK(unplaced.findSketch(sketchId)->placement.origin.z == 0.0);
      CHECK(Access::displayedSketchesMatchDocument(editor));
      Access::redoAction(editor)->trigger();
      placed = Access::documentCopy(editor);
      CHECK(placed.findSketch(sketchId)->placement.origin.x == 3.0);
      CHECK(placed.findSketch(sketchId)->placement.xDirection.y == 1.0);
      CHECK(placed.findSketch(sketchId)->placement.yDirection.z == 1.0);
      CHECK(Access::displayedSketchesMatchDocument(editor));
    }

    CHECK(Access::finishSketchEditNoCommand(editor, sketchId, construction,
                                            translatedPlacement));
    CHECK(Access::transientToolsInactive(editor));

    // A stale derived view must never resurrect geometry or fall back to a
    // different sketch. Both edit and presentation refresh fail closed.
    const auto committedBeforeMissingId = Access::documentCopy(editor);
    Access::injectMissingSketchViewAndRefresh(editor);
    CHECK(!Access::sketchEditActive(editor));
    CHECK(Access::displayedSketchCount(editor) == 0);
    const auto committedAfterMissingId = Access::documentCopy(editor);
    CHECK(committedAfterMissingId.sketches().size() ==
          committedBeforeMissingId.sketches().size());
    CHECK(committedAfterMissingId.findSketch(sketchId)->geometry
              .semanticallyEqual(
                  committedBeforeMissingId.findSketch(sketchId)->geometry));
    CHECK(editor.loadProject(pathA, &error));
    CHECK(Access::displayedSketchesMatchDocument(editor));
  }

  // A rejected sketch add must roll Document and the derived viewport cache
  // back atomically. The legacy top-level support string in v2 is presentation
  // compatibility data only; typed placement controls display and edit overlay.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    Access::installDocument(editor, solidar::Document{});
    solidar::sketch::Sketch profile;
    profile.addRectangle({0.0, 0.0}, {17.0, 9.0});
    auto xzPlacement = solidar::SketchPlacement::xz();
    xzPlacement.origin = {3.0, 4.0, 5.0};
    const bool modifiedBeforeRejectedSketch = Access::modified(editor);

    Access::setHistoryByteBudget(editor, 1);
    Access::finishNewSketch(
        editor, profile, xzPlacement,
        QString::fromUtf8("Произвольная подпись YZ/XY"));
    CHECK(Access::documentSketchCount(editor) == 0);
    CHECK(Access::sketchViewCount(editor) == 0);
    CHECK(Access::displayedSketchCount(editor) == 0);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
    CHECK(Access::workspace(editor)->currentWidget() == Access::sketch(editor));
    CHECK(Access::sketch(editor)->sketch().semanticallyEqual(profile));
    CHECK(Access::statusMessage(editor).contains(
        QString::fromUtf8("Операция отменена")));
    CHECK(!Access::statusMessage(editor).contains(
        QString::fromUtf8("Эскиз завершён")));
    CHECK(Access::modified(editor) == modifiedBeforeRejectedSketch);

    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);
    Access::finishNewSketch(
        editor, profile, xzPlacement,
        QString::fromUtf8("Другая произвольная подпись XY"));
    CHECK(Access::documentSketchCount(editor) == 1);
    CHECK(Access::sketchViewCount(editor) == 1);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::displayedSketchLabel(editor, 0) == QStringLiteral("XZ"));
    const auto typedSketchId = Access::firstSketchId(editor);

    const QString typedPath = directory.filePath(
        QStringLiteral("typed-xz-label-invariance.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        typedPath, Access::documentCopy(editor), &error));
    QFile typedFile(typedPath);
    CHECK(typedFile.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(typedFile.readAll()).object();
    typedFile.close();
    auto legacySketches = root.value(QStringLiteral("sketches")).toArray();
    CHECK(legacySketches.size() == 1);
    auto legacySketch = legacySketches.at(0).toObject();
    legacySketch[QStringLiteral("support")] =
        QString::fromUtf8("Неверная legacy-подпись YZ");
    legacySketches[0] = legacySketch;
    root[QStringLiteral("sketches")] = legacySketches;
    CHECK(typedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    CHECK(typedFile.write(payload) == payload.size());
    typedFile.close();

    CHECK(editor.loadProject(typedPath, &error));
    const auto typedRoundTrip = Access::documentCopy(editor);
    const auto* restoredTyped = typedRoundTrip.findSketch(typedSketchId);
    CHECK(restoredTyped);
    CHECK(restoredTyped->placement.origin.x == 3.0);
    CHECK(restoredTyped->placement.origin.y == 4.0);
    CHECK(restoredTyped->placement.origin.z == 5.0);
    CHECK(std::abs(restoredTyped->placement.normal().y) > 0.9);
    CHECK(Access::displayedSketchLabel(editor, 0) == QStringLiteral("XZ"));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    Access::editSketch(editor, typedSketchId);
    CHECK(std::abs(Access::currentSketchPlacement(editor).normal().y) > 0.9);
    CHECK(!Access::currentSketchFaceReference(editor));

    // A presentation label cannot steer picking or extrusion. Exercise an
    // arbitrary tilted plane that cannot be represented by XY/XZ/YZ strings,
    // then prove two contradictory labels yield the identical typed normal.
    CHECK(editor.loadProject(typedPath, &error));
    solidar::SketchPlacement tiltedPlacement;
    const double root2 = std::sqrt(2.0);
    const double root6 = std::sqrt(6.0);
    tiltedPlacement.origin = {7.0, -3.0, 11.0};
    tiltedPlacement.xDirection = {1.0 / root2, 1.0 / root2, 0.0};
    tiltedPlacement.yDirection = {-1.0 / root6, 1.0 / root6,
                                  2.0 / root6};
    CHECK(Access::finishSketchEdit(editor, typedSketchId, profile,
                                   tiltedPlacement));
    const QPointF directionWithXy = Access::extrusionDirectionForLabel(
        editor, 0, QString::fromUtf8("Ложная XY-подпись"), tiltedPlacement);
    const QPointF directionWithYz = Access::extrusionDirectionForLabel(
        editor, 0, QString::fromUtf8("Ложная YZ-подпись"), tiltedPlacement);
    CHECK(QLineF(directionWithXy, directionWithYz).length() < 1e-9);
    CHECK(QLineF(QPointF{}, directionWithXy).length() > 1e-3);
    Access::startSketchExtrude(editor, 0);
    CHECK(Access::directExtrudeLifecycle(editor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudePreview(editor));
    Access::reset(editor);
    CHECK(editor.loadProject(pathA, &error));
  }

  // Budget rejection is an atomic application boundary for persistent tools
  // as well as modal Pocket. Move/Extrude stay live and retryable; none of the
  // three paths publishes teardown or selection clearing before acceptance.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    for (const std::string kind : {"Move", "FaceExtrude", "Pocket"}) {
      auto fixture = makeHistoryMatrixFixture(kind);
      CHECK(fixture.has_value());
      Access::installDocument(editor, fixture->before);
      const auto beforeDocument = Access::documentCopy(editor);
      const auto beforeSelection = Access::selectionState(editor);
      Access::setHistoryByteBudget(editor, 1);
      CHECK(!Access::acceptFixtureThroughProduction(
          editor, fixture->after, fixture->featureId));
      const auto afterDocument = Access::documentCopy(editor);
      CHECK(Access::documentDeltaSize(beforeDocument, afterDocument).first ==
            0);
      CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                        beforeSelection));
      CHECK(Access::undoCount(editor) == 0);
      if (kind == "Move") {
        CHECK(Access::activeTool(editor) ==
              solidar::PartDesignToolKind::Move);
        CHECK(Access::moveLifecycle(editor) ==
              solidar::ToolLifecycle::PreviewValid);
        CHECK(Access::viewportHasToolPreview(editor));
        Access::cancelMove(editor);
      } else if (kind == "FaceExtrude") {
        CHECK(Access::activeTool(editor) ==
              solidar::PartDesignToolKind::Extrude);
        CHECK(Access::faceExtrudeLifecycle(editor) ==
              solidar::ToolLifecycle::PreviewValid);
        CHECK(Access::viewportHasToolPreview(editor));
        Access::cancelFaceExtrude(editor);
      } else {
        CHECK(Access::transientToolsInactive(editor));
      }
      Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);
    }
  }

  // STEP replacement is a hard history boundary; stale callbacks from the
  // previous document must not be reachable afterwards.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    const auto historyBeforeBoundary = Access::undoCount(editor);
    CHECK(Access::pushBodyMarker(editor, "history-boundary-marker"));
    CHECK(Access::undoCount(editor) == historyBeforeBoundary + 1);
    solidar::Document imported;
    imported.addBody("Imported body");
    Access::applyImportedDocument(editor, std::move(imported));
    CHECK(Access::undoCount(editor) == 0);
    CHECK(Access::redoCount(editor) == 0);
    CHECK(Access::retainedBytes(editor) == 0);
    CHECK(editor.loadProject(pathA, &error));
  }

  // Both history domains evict deterministically by count and retained-byte
  // budget. The model gate is driven by real imported CAD topology archived
  // into cold bytes (never by padded names or retained OCCT handles).
  {
    using Access = solidar::MainWindowUndoTestAccess;
    auto heavy = makeHeavyImportedDocument(3);
    CHECK(heavy.bodies().size() == 3);
    std::vector<solidar::BodyId> heavyIds;
    for (const auto& body : heavy.bodies()) heavyIds.push_back(body.id());
    auto oneRemoved = heavy;
    CHECK(oneRemoved.applyBodySlice(0, std::nullopt));
    const auto [heavySlices, oneCommandBytes] =
        Access::documentDeltaSize(heavy, oneRemoved);
    CHECK(heavySlices == 1);
    CHECK(oneCommandBytes > 16U * 1024U);
    Access::installDocument(editor, std::move(heavy));
    Access::setHistoryByteBudget(editor, oneCommandBytes * 2U + 4096U);
    for (const auto id : heavyIds) CHECK(Access::commitBodyRemoval(editor, id));
    CHECK(Access::retainedBytes(editor) <= oneCommandBytes * 2U + 4096U);
    CHECK(Access::undoCount(editor) < heavyIds.size());
    CHECK(Access::undoCount(editor) > 0);
    const auto countAfterRemoval = Access::bodyCount(editor);
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == countAfterRemoval + 1);
    Access::redoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == countAfterRemoval);

    auto oversize = makeHeavyImportedDocument(1);
    CHECK(oversize.bodies().size() == 1);
    const auto oversizeId = oversize.bodies().front().id();
    solidar::Document empty;
    const auto [oversizeSlices, oversizeBytes] =
        Access::documentDeltaSize(oversize, empty);
    CHECK(oversizeSlices == 1);
    Access::installDocument(editor, std::move(oversize));
    Access::setHistoryByteBudget(editor, oversizeBytes - 1U);
    CHECK(!Access::commitBodyRemoval(editor, oversizeId));
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::undoCount(editor) == 0);
    CHECK(Access::retainedBytes(editor) == 0);
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);

    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    Access::workspace(editor)->setCurrentWidget(canvas);
    for (int index = 0; index < 140; ++index)
      canvas->setRectangle(10.0 + index, 8.0 + index);
    CHECK(canvas->undoHistorySize() == 100);
    CHECK(canvas->undoHistoryRetainedBytes() <= 32U * 1024U * 1024U);
    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
    CHECK(editor.loadProject(pathA, &error));
  }

  // Rejecting a removal for history-budget reasons must restore a committed
  // Error feature without replaying it. Replaying the reverse delta would run
  // recompute on the broken Move and throw from the Qt slot.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document errorDocument;
    const auto valid = addBox(errorDocument, 0.0, 18.0, 12.0,
                              "Error rollback sentinel");
    CHECK(valid.has_value());
    auto* errorBody = errorDocument.findBody(valid->bodyId);
    CHECK(errorBody);
    auto brokenMove = std::make_unique<solidar::MoveFeature>(
        solidar::FeatureId{987654321},
        solidar::Vector3d{3.0, 2.0, 1.0}, "Broken rollback Move");
    const auto brokenMoveId = brokenMove->id();
    errorBody->addFeature(std::move(brokenMove));
    CHECK(!errorDocument.recompute());
    CHECK(errorDocument.findFeature(brokenMoveId));
    CHECK(errorDocument.findFeature(brokenMoveId)->state() ==
          solidar::FeatureState::Error);

    Access::installDocument(editor, std::move(errorDocument));
    CHECK(Access::selectSentinel(editor, valid->bodyId, valid->featureId, 0));
    editor.setWindowModified(false);
    const auto documentBefore = Access::documentCopy(editor);
    const auto selectionBefore = Access::selectionState(editor);
    const auto historyStepsBefore = Access::historyStepCount(editor);
    const auto undoBefore = Access::undoCount(editor);
    const auto redoBefore = Access::redoCount(editor);
    const auto retainedBefore = Access::retainedBytes(editor);
    const auto displayedBefore = Access::displayedBodyShape(editor);
    CHECK(displayedBefore);
    const double volumeBefore = solidar::test::volumeOf(*displayedBefore);

    Access::setHistoryByteBudget(editor, 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistoryFeature(editor, valid->bodyId, brokenMoveId);

    const auto restored = Access::documentCopy(editor);
    CHECK(restored.findFeature(brokenMoveId));
    CHECK(restored.findFeature(brokenMoveId)->state() ==
          solidar::FeatureState::Error);
    auto recomputeProbe = restored;
    CHECK(!recomputeProbe.recompute());
    const auto [changedSlices, changedBytes] =
        Access::documentDeltaSize(documentBefore, restored);
    CHECK(changedSlices == 0);
    CHECK(changedBytes == 0);
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      selectionBefore));
    CHECK(Access::historyStepCount(editor) == historyStepsBefore);
    CHECK(Access::undoCount(editor) == undoBefore);
    CHECK(Access::redoCount(editor) == redoBefore);
    CHECK(Access::retainedBytes(editor) == retainedBefore);
    CHECK(!Access::modified(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    const auto displayedAfter = Access::displayedBodyShape(editor);
    CHECK(displayedAfter);
    CHECK(solidar::test::near(solidar::test::volumeOf(*displayedAfter),
                              volumeBefore, 1e-6));
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);
    CHECK(editor.loadProject(pathA, &error));
  }

  // Production history handler matrix. Each fixture is a valid committed model
  // for the real feature type. Create, payload edit, and removal are applied
  // through MainWindow::pushModelTransition and exercised for two complete
  // Undo/Redo cycles, including stable selection/history restoration.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    const std::vector<std::string> kinds{
        "Extrude", "FaceExtrude", "Revolve", "Pocket", "Fillet", "Chamfer", "Shell",
        "Draft", "Join", "Mirror", "Move", "LinearPattern",
        "CircularPattern", "LinearPatternNewBody",
        "CircularPatternNewBody"};
    std::size_t kindIndex = 0;
    for (const auto& kind : kinds) {
      std::cerr << "history production matrix: " << kind << '\n';
      auto fixture = makeHistoryMatrixFixture(kind);
      CHECK(fixture.has_value());
      const std::string originalName =
          fixture->after.findFeature(fixture->featureId)->name();
      const std::string originalPayload = historyMatrixPayload(
          *fixture->after.findFeature(fixture->featureId));
      const auto expectedCreateShape = matrixShapeSignature(
          fixture->after.findFeature(fixture->featureId));
      CHECK(expectedCreateShape.has_value());

      // Create through the real production ToolSession + accept dispatcher.
      Access::installDocument(editor, fixture->before);
      if (fixture->createSelectionBodyId != solidar::kInvalidBodyId)
        Access::selectBody(editor, fixture->createSelectionBodyId);
      else
        Access::clearSelection(editor);
      const auto createBeforeSelection = Access::selectionState(editor);
      const std::size_t createBeforeBodies = Access::bodyCount(editor);
      const auto idsBefore = Access::featuresOfKind(editor, fixture->featureKind);
      const auto* referenceJoin = dynamic_cast<const solidar::JoinBodiesFeature*>(
          fixture->after.findFeature(fixture->featureId));
      CHECK(Access::acceptFixtureThroughProduction(
          editor, fixture->after, fixture->featureId));
      const auto idsAfter = Access::featuresOfKind(editor, fixture->featureKind);
      CHECK(idsAfter.size() == idsBefore.size() + 1);
      const auto createdId = *std::find_if(
          idsAfter.begin(), idsAfter.end(), [&idsBefore](solidar::FeatureId id) {
            return std::find(idsBefore.begin(), idsBefore.end(), id) ==
                   idsBefore.end();
          });
      const auto* createdFeature = Access::feature(editor, createdId);
      CHECK(createdFeature != nullptr);
      CHECK(historyMatrixPayload(*createdFeature, false) ==
            historyMatrixPayload(
                *fixture->after.findFeature(fixture->featureId), false));
      const auto createdShape = matrixShapeSignature(createdFeature);
      CHECK(createdShape.has_value());
      CHECK(sameMatrixShape(*createdShape, *expectedCreateShape));
      CHECK(matrixReferencesResolve(Access::documentCopy(editor),
                                    *createdFeature));
      const std::string createdPayload = historyMatrixPayload(*createdFeature);
      const bool createsBody =
          fixture->before.findBody(fixture->bodyId) == nullptr;
      CHECK(Access::bodyCount(editor) ==
            createBeforeBodies + static_cast<std::size_t>(createsBody));
      if (createsBody)
        CHECK(Access::featureOwner(editor, createdId) !=
              solidar::kInvalidBodyId);
      else
        CHECK(Access::featureOwner(editor, createdId) == fixture->bodyId);
      CHECK(Access::historyContainsFeature(editor, createdId));
      const auto createAfterSelection = Access::selectionState(editor);
      CHECK(createAfterSelection.atEnd);
      CHECK(Access::transientToolsInactive(editor));
      if (referenceJoin) {
        CHECK(!Access::bodyVisible(editor, referenceJoin->firstBodyId()));
        CHECK(!Access::bodyVisible(editor, referenceJoin->secondBodyId()));
      }
      for (int cycle = 0; cycle < 2; ++cycle) {
        const auto* created = Access::feature(editor, createdId);
        CHECK(created && created->kind() == fixture->featureKind);
        CHECK(historyMatrixPayload(*created) == createdPayload);
        const auto currentShape = matrixShapeSignature(created);
        CHECK(currentShape && sameMatrixShape(*currentShape, *createdShape));
        CHECK(Access::historyAtEnd(editor));
        CHECK(Access::transientToolsInactive(editor));
        Access::undoAction(editor)->trigger();
        CHECK(!Access::feature(editor, createdId));
        CHECK(Access::bodyCount(editor) == createBeforeBodies);
        if (referenceJoin) {
          CHECK(Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         createBeforeSelection));
        CHECK(Access::historyAtEnd(editor));
        CHECK(Access::transientToolsInactive(editor));
        Access::redoAction(editor)->trigger();
        CHECK(Access::feature(editor, createdId));
        const auto* redone = Access::feature(editor, createdId);
        CHECK(historyMatrixPayload(*redone) == createdPayload);
        const auto redoneShape = matrixShapeSignature(redone);
        CHECK(redoneShape && sameMatrixShape(*redoneShape, *createdShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *redone));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         createAfterSelection));
        CHECK(Access::transientToolsInactive(editor));
        if (referenceJoin) {
          CHECK(!Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(!Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
      }

      // Accepting an unchanged supported editor is a precise application/UI
      // no-op.  Rotate raw Body/Edge/Face sentinels and the initial modified
      // flag across the complete 15-variant production matrix.  A second real
      // open+Cancel proves the empty-delta path did not leave stale editor
      // transaction state behind.
      Access::installDocument(editor, fixture->after);
      CHECK(Access::selectSentinel(editor, fixture->sentinelBodyId,
                                   fixture->sentinelFeatureId,
                                   static_cast<int>(kindIndex % 3)));
      const bool unchangedModified = (kindIndex % 2) != 0;
      editor.setWindowModified(unchangedModified);
      const auto unchangedSelection = Access::selectionState(editor);
      const auto unchangedUndo = Access::undoCount(editor);
      const auto unchangedRedo = Access::redoCount(editor);
      const auto unchangedHistory = Access::historyStepCount(editor);
      const auto unchangedBodies = Access::bodyCount(editor);
      const auto unchangedPayload = historyMatrixPayload(
          *Access::feature(editor, fixture->featureId));
      const auto unchangedShape = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      const auto unchangedDefinition = serializedDocumentDefinition(
          Access::documentCopy(editor));
      CHECK(unchangedShape.has_value());
      CHECK(unchangedDefinition.has_value());
      CHECK(Access::acceptUnchangedFeatureEditThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      const auto definitionAfterUnchanged = serializedDocumentDefinition(
          Access::documentCopy(editor));
      CHECK(definitionAfterUnchanged.has_value());
      if (*definitionAfterUnchanged != *unchangedDefinition)
        std::cerr << "unchanged Accept document mismatch for " << kind << ": "
                  << firstSerializedDocumentDifference(
                         *unchangedDefinition, *definitionAfterUnchanged)
                         .toStdString()
                  << '\n';
      CHECK(*definitionAfterUnchanged == *unchangedDefinition);
      CHECK(historyMatrixPayload(
                *Access::feature(editor, fixture->featureId)) ==
            unchangedPayload);
      const auto shapeAfterUnchanged = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(shapeAfterUnchanged &&
            sameMatrixShape(*shapeAfterUnchanged, *unchangedShape));
      CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                        unchangedSelection));
      CHECK(Access::modified(editor) == unchangedModified);
      CHECK(Access::undoCount(editor) == unchangedUndo);
      CHECK(Access::redoCount(editor) == unchangedRedo);
      CHECK(Access::historyStepCount(editor) == unchangedHistory);
      CHECK(Access::bodyCount(editor) == unchangedBodies);
      CHECK(!Access::editTransactionActive(editor));
      CHECK(Access::transientToolsInactive(editor));
      CHECK(Access::cancelFeatureEditThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                        unchangedSelection));
      CHECK(Access::modified(editor) == unchangedModified);
      CHECK(Access::undoCount(editor) == unchangedUndo);
      CHECK(Access::redoCount(editor) == unchangedRedo);
      CHECK(!Access::editTransactionActive(editor));
      CHECK(Access::transientToolsInactive(editor));

      // Edit through the real history dispatcher and the production session
      // accept path (modal Extrude/Pocket editors are auto-accepted).
      Access::installDocument(editor, fixture->after);
      CHECK(Access::selectSentinel(editor, fixture->sentinelBodyId,
                                   fixture->sentinelFeatureId,
                                   static_cast<int>(kindIndex % 3)));
      const auto cancelPayload = historyMatrixPayload(
          *Access::feature(editor, fixture->featureId));
      const auto cancelShape = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(cancelShape.has_value());
      CHECK(Access::cancelFeatureEditThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      CHECK(historyMatrixPayload(*Access::feature(editor, fixture->featureId)) ==
            cancelPayload);
      const auto shapeAfterCancel = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(shapeAfterCancel && sameMatrixShape(*shapeAfterCancel,
                                                *cancelShape));
      const auto originalShape = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(originalShape.has_value());
      const auto expectedEdited = makeExpectedEditedDocument(*fixture);
      CHECK(expectedEdited.has_value());
      const auto* expectedEditedFeature =
          expectedEdited->findFeature(fixture->featureId);
      CHECK(expectedEditedFeature != nullptr);
      const std::string expectedEditedPayload =
          historyMatrixPayload(*expectedEditedFeature);
      const auto expectedEditedShape =
          matrixShapeSignature(expectedEditedFeature);
      CHECK(expectedEditedShape.has_value());
      CHECK(matrixReferencesResolve(*expectedEdited, *expectedEditedFeature));
      CHECK(Access::editFeatureThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      const auto committedEditBeforeSelection =
          Access::expectedEditUndoSelection();
      CHECK(committedEditBeforeSelection.has_value());
      const auto* editedFeature = Access::feature(editor, fixture->featureId);
      CHECK(editedFeature);
      const std::string editedPayload = historyMatrixPayload(*editedFeature);
      CHECK(editedPayload == expectedEditedPayload);
      CHECK(editedPayload != originalPayload);
      const auto editedShape = matrixShapeSignature(editedFeature);
      CHECK(editedShape.has_value());
      CHECK(sameMatrixShape(*editedShape, *expectedEditedShape));
      CHECK(matrixReferencesResolve(Access::documentCopy(editor),
                                    *editedFeature));
      const auto editAfterSelection = Access::selectionState(editor);
      CHECK(editAfterSelection.atEnd);
      CHECK(Access::transientToolsInactive(editor));
      for (int cycle = 0; cycle < 2; ++cycle) {
        CHECK(historyMatrixPayload(
                  *Access::feature(editor, fixture->featureId)) ==
              expectedEditedPayload);
        const auto currentEditedShape = matrixShapeSignature(
            Access::feature(editor, fixture->featureId));
        CHECK(currentEditedShape &&
              sameMatrixShape(*currentEditedShape, *expectedEditedShape));
        Access::undoAction(editor)->trigger();
        const auto* original = Access::feature(editor, fixture->featureId);
        CHECK(original && original->kind() == fixture->featureKind);
        CHECK(historyMatrixPayload(*original) == originalPayload);
        const auto restoredShape = matrixShapeSignature(original);
        CHECK(restoredShape && sameMatrixShape(*restoredShape, *originalShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *original));
        CHECK(Access::editUndoSelectionRestored(editor));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         *committedEditBeforeSelection));
        CHECK(Access::historyAtEnd(editor));
        CHECK(Access::transientToolsInactive(editor));
        Access::redoAction(editor)->trigger();
        const auto* redone = Access::feature(editor, fixture->featureId);
        CHECK(redone && historyMatrixPayload(*redone) ==
                            expectedEditedPayload);
        const auto redoneShape = matrixShapeSignature(redone);
        CHECK(redoneShape &&
              sameMatrixShape(*redoneShape, *expectedEditedShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *redone));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         editAfterSelection));
        CHECK(Access::transientToolsInactive(editor));
      }

      // Removal goes through the real history-row removal path and its
      // dependency/visibility policy.
      Access::installDocument(editor, fixture->after);
      Access::selectBody(editor, fixture->bodyId);
      const auto removeBeforeSelection = Access::selectionState(editor);
      CHECK(Access::removeFeatureThroughProduction(editor,
                                                   fixture->featureId));
      const auto removeAfterSelection = Access::selectionState(editor);
      CHECK(Access::transientToolsInactive(editor));
      if (referenceJoin) {
        CHECK(Access::bodyVisible(editor, referenceJoin->firstBodyId()));
        CHECK(Access::bodyVisible(editor, referenceJoin->secondBodyId()));
      }
      for (int cycle = 0; cycle < 2; ++cycle) {
        CHECK(!Access::feature(editor, fixture->featureId));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         removeAfterSelection));
        Access::undoAction(editor)->trigger();
        const auto* restored = Access::feature(editor, fixture->featureId);
        if (!restored) {
          std::cerr << "missing restored id " << fixture->featureId
                    << " undo=" << Access::undoCount(editor)
                    << " redo=" << Access::redoCount(editor)
                    << " status=" << editor.statusBar()->currentMessage().toStdString()
                    << '\n';
        }
        CHECK(restored && restored->kind() == fixture->featureKind);
        CHECK(restored->name() == originalName);
        CHECK(historyMatrixPayload(*restored) == originalPayload);
        const auto restoredShape = matrixShapeSignature(restored);
        CHECK(restoredShape && sameMatrixShape(*restoredShape,
                                               *expectedCreateShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *restored));
        if (referenceJoin) {
          CHECK(!Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(!Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         removeBeforeSelection));
        CHECK(Access::transientToolsInactive(editor));
        Access::redoAction(editor)->trigger();
        CHECK(!Access::feature(editor, fixture->featureId));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         removeAfterSelection));
        CHECK(Access::transientToolsInactive(editor));
        if (referenceJoin) {
          CHECK(Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
      }
      ++kindIndex;
    }
    CHECK(editor.loadProject(pathA, &error));
  }

  // Mirror plane highlighting is transient tool UI.  A pre-visible user plane
  // must survive unchanged Apply with redo intact, and a real accepted edit
  // must carry that same presentation through Undo/Redo.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    auto fixture = makeHistoryMatrixFixture("Mirror");
    CHECK(fixture.has_value());
    Access::installDocument(editor, fixture->after);
    Access::reset(editor);
    CHECK(Access::setBasePlaneVisible(editor, 1, true));
    CHECK(Access::setOriginVisible(editor, false));
    Access::undoAction(editor)->trigger();
    CHECK(Access::basePlaneVisible(editor, 1));
    CHECK(Access::originVisible(editor));
    CHECK(Access::redoCount(editor) == 1);
    CHECK(Access::presentationMatchesTree(editor));

    const auto unchangedBefore = Access::selectionState(editor);
    const auto unchangedUndo = Access::undoCount(editor);
    const auto unchangedRedo = Access::redoCount(editor);
    const bool unchangedDirty = Access::modified(editor);
    CHECK(Access::acceptUnchangedFeatureEditThroughProduction(
        editor, fixture->bodyId, fixture->featureId));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      unchangedBefore));
    CHECK(Access::undoCount(editor) == unchangedUndo);
    CHECK(Access::redoCount(editor) == unchangedRedo);
    CHECK(Access::modified(editor) == unchangedDirty);
    CHECK(Access::basePlaneVisible(editor, 1));
    CHECK(Access::presentationMatchesTree(editor));

    const auto editedPresentation =
        Access::selectionState(editor).presentation;
    CHECK(Access::editFeatureThroughProduction(
        editor, fixture->bodyId, fixture->featureId));
    CHECK(Access::selectionState(editor).presentation == editedPresentation);
    CHECK(Access::basePlaneVisible(editor, 1));
    CHECK(Access::presentationMatchesTree(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::selectionState(editor).presentation == editedPresentation);
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::selectionState(editor).presentation == editedPresentation);
    CHECK(Access::presentationMatchesTree(editor));
    CHECK(editor.loadProject(pathA, &error));
  }

  // Edit dispatch is atomic even when a clickable history row cannot open an
  // editor. Unsupported features and every invalid-source early return must
  // restore the exact raw selection/history/modified state and leave no stale
  // transaction that could contaminate the next unrelated tool.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    const auto expectAtomicNoEditor = [&](solidar::BodyId bodyId,
                                          solidar::FeatureId featureId) {
      const auto beforeSelection = Access::selectionState(editor);
      const bool beforeModified = Access::modified(editor);
      const auto beforeUndo = Access::undoCount(editor);
      const auto beforeRedo = Access::redoCount(editor);
      const auto beforeBodies = Access::bodyCount(editor);
      const auto beforeHistory = Access::historyStepCount(editor);
      Access::editHistoryFeature(editor, bodyId, featureId);
      return !Access::editTransactionActive(editor) &&
             Access::exactSelectionState(Access::selectionState(editor),
                                         beforeSelection) &&
             Access::modified(editor) == beforeModified &&
             Access::undoCount(editor) == beforeUndo &&
             Access::redoCount(editor) == beforeRedo &&
             Access::bodyCount(editor) == beforeBodies &&
             Access::historyStepCount(editor) == beforeHistory &&
             Access::transientToolsInactive(editor);
    };

    solidar::Document unsupported;
    const auto unsupportedSentinel = addBox(
        unsupported, -50.0, -30.0, 8.0, "Unsupported sentinel");
    CHECK(unsupportedSentinel.has_value());
    auto& importedBody = unsupported.addBody("Imported unsupported");
    auto imported = std::make_unique<solidar::ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(8.0, 7.0, 6.0).Shape()),
        "Imported unsupported");
    const auto importedBodyId = importedBody.id();
    const auto importedId = imported->id();
    importedBody.addFeature(std::move(imported));
    CHECK(unsupported.recompute());
    Access::installDocument(editor, std::move(unsupported));
    CHECK(Access::selectSentinel(editor, unsupportedSentinel->bodyId,
                                 unsupportedSentinel->featureId, 2));
    editor.setWindowModified(false);
    CHECK(expectAtomicNoEditor(importedBodyId, importedId));

    // Missing feature is an earlier no-op boundary and must be equally clean.
    CHECK(Access::selectSentinel(editor, unsupportedSentinel->bodyId,
                                 unsupportedSentinel->featureId, 1));
    CHECK(expectAtomicNoEditor(importedBodyId,
                               solidar::kInvalidFeatureId));

    // A known type with no preceding/source feature reaches an early return
    // inside editPatternFeature after the UI transaction was captured.
    solidar::Document missingSource;
    const auto missingSourceSentinel = addBox(
        missingSource, -40.0, -20.0, 6.0, "Missing source sentinel");
    CHECK(missingSourceSentinel.has_value());
    auto& brokenBody = missingSource.addBody("Broken move owner");
    auto brokenMove = std::make_unique<solidar::MoveFeature>(
        solidar::FeatureId{987654}, solidar::Vector3d{1.0, 2.0, 3.0},
        "Broken Move");
    const auto brokenBodyId = brokenBody.id();
    const auto brokenMoveId = brokenMove->id();
    brokenBody.addFeature(std::move(brokenMove));
    Access::installDocument(editor, std::move(missingSource));
    CHECK(Access::selectSentinel(editor, missingSourceSentinel->bodyId,
                                 missingSourceSentinel->featureId, 0));
    editor.setWindowModified(true);
    CHECK(expectAtomicNoEditor(brokenBodyId, brokenMoveId));

    // Join has an explicit history-at-end precondition. The dispatch guard
    // must restore the stable non-end history key instead of leaking the
    // transaction when that precondition rejects the editor.
    auto joinFixture = makeHistoryMatrixFixture("Join");
    CHECK(joinFixture.has_value());
    Access::installDocument(editor, joinFixture->after);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::selectSentinel(editor, joinFixture->sentinelBodyId,
                                 joinFixture->sentinelFeatureId, 2));
    CHECK(Access::historyStepCount(editor) > 0);
    Access::applyHistoryPosition(editor, 0);
    CHECK(!Access::historyAtEnd(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    editor.setWindowModified(false);
    CHECK(expectAtomicNoEditor(joinFixture->bodyId,
                               joinFixture->featureId));
    CHECK(Access::historyPosition(editor) == 0);

    // An invalid persistent topology reference is still a successfully open
    // editor when the session remains active (PreviewInvalid): the transaction
    // must be retained until the real Cancel path restores the pre-tool state.
    auto faceFixture = makeHistoryMatrixFixture("FaceExtrude");
    CHECK(faceFixture.has_value());
    auto invalidTopology = faceFixture->before;
    const auto* validFaceFeature = dynamic_cast<const solidar::ExtrudeFeature*>(
        faceFixture->after.findFeature(faceFixture->featureId));
    CHECK(validFaceFeature && validFaceFeature->faceReference());
    auto invalidFace = *validFaceFeature->faceReference();
    invalidFace.faceIndex() = 999999;
    invalidFace.persistentTag() = "missing-face";
    invalidFace.signature().reset();
    auto* invalidOwner = invalidTopology.findBody(faceFixture->bodyId);
    CHECK(invalidOwner != nullptr);
    auto invalidExtrude = std::make_unique<solidar::ExtrudeFeature>(
        invalidFace, validFaceFeature->lengthMm(), "Invalid topology edit",
        validFaceFeature->operation(), validFaceFeature->reversed());
    const auto invalidExtrudeId = invalidExtrude->id();
    invalidOwner->addFeature(std::move(invalidExtrude));
    Access::installDocument(editor, std::move(invalidTopology));
    CHECK(Access::selectSentinel(editor, faceFixture->sentinelBodyId,
                                 faceFixture->sentinelFeatureId, 1));
    editor.setWindowModified(false);
    const auto invalidBeforeSelection = Access::selectionState(editor);
    Access::editHistoryFeature(editor, faceFixture->bodyId, invalidExtrudeId);
    CHECK(Access::editTransactionActive(editor));
    CHECK(Access::faceExtrudeEditingFeature(editor) == invalidExtrudeId);
    CHECK(Access::faceExtrudeLifecycle(editor) ==
          solidar::ToolLifecycle::PreviewInvalid);
    Access::cancelFaceExtrude(editor);
    CHECK(!Access::editTransactionActive(editor));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      invalidBeforeSelection));
    CHECK(!Access::modified(editor));
    CHECK(Access::transientToolsInactive(editor));

    // A subsequent valid editor proves failed dispatches left no stale state.
    auto moveFixture = makeHistoryMatrixFixture("Move");
    CHECK(moveFixture.has_value());
    Access::installDocument(editor, moveFixture->after);
    CHECK(Access::selectSentinel(editor, moveFixture->sentinelBodyId,
                                 moveFixture->sentinelFeatureId, 2));
    editor.setWindowModified(false);
    const auto supportedBeforeSelection = Access::selectionState(editor);
    Access::editHistoryFeature(editor, moveFixture->bodyId,
                               moveFixture->featureId);
    CHECK(Access::editTransactionActive(editor));
    CHECK(Access::moveEditingFeature(editor) == moveFixture->featureId);
    CHECK(Access::moveLifecycle(editor) != solidar::ToolLifecycle::Inactive);
    const auto activeEditorSelection = Access::selectionState(editor);
    Access::editHistoryFeature(editor, moveFixture->bodyId,
                               solidar::kInvalidFeatureId);
    CHECK(Access::editTransactionActive(editor));
    CHECK(Access::moveEditingFeature(editor) == moveFixture->featureId);
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      activeEditorSelection));
    Access::cancelMove(editor);
    CHECK(!Access::editTransactionActive(editor));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      supportedBeforeSelection));
    CHECK(!Access::modified(editor));
    CHECK(Access::transientToolsInactive(editor));
    CHECK(editor.loadProject(pathA, &error));
  }

  // Join visibility is a shared-consumer relation, not ownership. J1 and J2
  // both consume A; editing/removing only one of them must not expose A while
  // the other survives. Direct Body removal uses the same policy as history
  // feature removal, and Cancel is an exact no-op.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document shared;
    const auto a = addBox(shared, 0.0, 20.0, 10.0, "Shared A");
    const auto b = addBox(shared, 10.0, 30.0, 10.0, "Shared B");
    const auto c = addBox(shared, 5.0, 25.0, 10.0, "Shared C");
    const auto d = addBox(shared, 10.0, 30.0, 10.0, "Shared D");
    CHECK(a && b && c && d);
    auto& j1Body = shared.addBody("J1 result");
    const auto j1BodyId = j1Body.id();
    auto j1 = std::make_unique<solidar::JoinBodiesFeature>(
        a->bodyId, a->featureId, b->bodyId, b->featureId, "J1");
    const auto j1Id = j1->id();
    j1Body.addFeature(std::move(j1));
    auto& j2Body = shared.addBody("J2 result");
    const auto j2BodyId = j2Body.id();
    auto j2 = std::make_unique<solidar::JoinBodiesFeature>(
        a->bodyId, a->featureId, c->bodyId, c->featureId, "J2");
    const auto j2Id = j2->id();
    j2Body.addFeature(std::move(j2));
    shared.findBody(a->bodyId)->setVisible(false);
    shared.findBody(b->bodyId)->setVisible(false);
    shared.findBody(c->bodyId)->setVisible(false);
    CHECK(shared.recompute());
    Access::installDocument(editor, shared);

    const std::string cancelPayload = historyMatrixPayload(
        *Access::feature(editor, j1Id));
    CHECK(Access::cancelJoinEdit(editor, j1BodyId, j1Id, a->bodyId,
                                 b->bodyId));
    CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) ==
          cancelPayload);
    CHECK(!Access::bodyVisible(editor, a->bodyId));
    CHECK(!Access::bodyVisible(editor, b->bodyId));
    CHECK(!Access::bodyVisible(editor, c->bodyId));

    // Removing the whole J1 result Body releases B, but A remains consumed by
    // J2. Undo re-creates J1 and re-hides B; Redo repeats the exact policy.
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, j1BodyId);
    CHECK(!Access::feature(editor, j1Id));
    CHECK(!Access::bodyVisible(editor, a->bodyId));
    CHECK(Access::bodyVisible(editor, b->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(Access::feature(editor, j1Id));
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(!Access::bodyVisible(editor, b->bodyId));
      Access::redoAction(editor)->trigger();
      CHECK(!Access::feature(editor, j1Id));
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(Access::bodyVisible(editor, b->bodyId));
    }
    Access::undoAction(editor)->trigger();
    CHECK(Access::feature(editor, j1Id));

    CHECK(Access::editJoinOperands(editor, j1BodyId, j1Id, d->bodyId,
                                   b->bodyId));
    const std::string editedJ1 = historyMatrixPayload(
        *Access::feature(editor, j1Id));
    CHECK(!Access::bodyVisible(editor, a->bodyId));
    CHECK(!Access::bodyVisible(editor, b->bodyId));
    CHECK(!Access::bodyVisible(editor, c->bodyId));
    CHECK(!Access::bodyVisible(editor, d->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) ==
            cancelPayload);
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(Access::bodyVisible(editor, d->bodyId));
      Access::redoAction(editor)->trigger();
      CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) == editedJ1);
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(!Access::bodyVisible(editor, d->bodyId));
    }

    CHECK(Access::removeFeatureThroughProduction(editor, j2Id));
    CHECK(Access::bodyVisible(editor, a->bodyId));
    CHECK(Access::bodyVisible(editor, c->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(Access::feature(editor, j2Id));
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(!Access::bodyVisible(editor, c->bodyId));
      Access::redoAction(editor)->trigger();
      CHECK(!Access::feature(editor, j2Id));
      CHECK(Access::bodyVisible(editor, a->bodyId));
      CHECK(Access::bodyVisible(editor, c->bodyId));
    }
    const QString sharedPath =
        directory.filePath(QStringLiteral("shared-join-visibility.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        sharedPath, Access::documentCopy(editor), &error));
    solidar::Document restoredShared;
    CHECK(solidar::project::ProjectFile::loadDocument(
        sharedPath, &restoredShared, &error));
    Access::installDocument(editor, std::move(restoredShared));
    CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) == editedJ1);
    CHECK(!Access::feature(editor, j2Id));
    CHECK(Access::bodyVisible(editor, a->bodyId));
    CHECK(Access::bodyVisible(editor, c->bodyId));
    CHECK(!Access::bodyVisible(editor, b->bodyId));
    CHECK(!Access::bodyVisible(editor, d->bodyId));

    // With the last Join consumer gone, direct result-Body removal releases
    // both operands. The same visibility and payload survive two cycles.
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, j1BodyId);
    CHECK(!Access::feature(editor, j1Id));
    CHECK(Access::bodyVisible(editor, b->bodyId));
    CHECK(Access::bodyVisible(editor, d->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) == editedJ1);
      CHECK(!Access::bodyVisible(editor, b->bodyId));
      CHECK(!Access::bodyVisible(editor, d->bodyId));
      CHECK(Access::transientToolsInactive(editor));
      Access::redoAction(editor)->trigger();
      CHECK(!Access::feature(editor, j1Id));
      CHECK(Access::bodyVisible(editor, b->bodyId));
      CHECK(Access::bodyVisible(editor, d->bodyId));
      CHECK(Access::transientToolsInactive(editor));
    }
    CHECK(Access::transientToolsInactive(editor));
    CHECK(j2BodyId != solidar::kInvalidBodyId);
    CHECK(editor.loadProject(pathA, &error));
  }

  auto* filletButton = editor.findChild<QToolButton*>(
      QStringLiteral("filletCommand"));
  auto* extrudeButton = editor.findChild<QToolButton*>(
      QStringLiteral("extrudeCommand"));
  auto* revolveButton = editor.findChild<QToolButton*>(
      QStringLiteral("revolveCommand"));
  auto* partDesignDock = editor.findChild<QDockWidget*>(
      QStringLiteral("partDesignParametersDock"));
  auto* extrusionDock = editor.findChild<QDockWidget*>(
      QStringLiteral("extrusionParametersDock"));
  auto* revolveDock = editor.findChild<QDockWidget*>(
      QStringLiteral("revolveParametersDock"));
  CHECK(filletButton != nullptr);
  CHECK(extrudeButton != nullptr);
  CHECK(revolveButton != nullptr);
  CHECK(partDesignDock != nullptr);
  CHECK(extrusionDock != nullptr);
  CHECK(revolveDock != nullptr);

  filletButton->click();
  QApplication::processEvents();
  CHECK(!partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(revolveDock->isHidden());

  extrudeButton->click();
  QApplication::processEvents();
  CHECK(partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());  // still selecting an input profile
  CHECK(revolveDock->isHidden());

  revolveButton->click();
  QApplication::processEvents();
  CHECK(partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(!revolveDock->isHidden());

  filletButton->click();
  QApplication::processEvents();
  CHECK(!partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(revolveDock->isHidden());

  // Active-tool teardown at the controller/session layer. A live Fillet
  // session holds B-Rep references and a preview; the same cancelActive +
  // cancel sequence MainWindow::loadProject performs must leave the controller
  // and session fully inactive, with no surviving preview or selection state.
  {
    solidar::Document document;
    auto& baseSketch = document.addSketch("Base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    auto& body = document.addBody("Body");
    auto extrude = std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 15.0, "Extrude");
    const solidar::FeatureId featureId = extrude->id();
    body.addFeature(std::move(extrude));
    CHECK(document.recompute());
    const auto shape = body.resultShape();
    CHECK(shape != nullptr);

    solidar::PartDesignToolController controller;
    solidar::FilletToolSession fillet;
    controller.registerTool(solidar::PartDesignToolKind::Fillet,
                            {&fillet});
    controller.activate(solidar::PartDesignToolKind::Fillet);
    CHECK(controller.activeTool() == solidar::PartDesignToolKind::Fillet);
    CHECK(controller.activeSession() == &fillet);

    // Zero radius keeps the preview equal to the base shape, so the session is
    // deterministically active and holding B-Rep without depending on whether
    // a particular edge is fillet-able.
    fillet.beginLegacyV1(body.id(), featureId, shape,
                 {solidar::EdgeReference{body.id(), featureId, 0}}, 0.0);
    CHECK(fillet.lifecycle() != solidar::ToolLifecycle::Inactive);
    CHECK(fillet.previewShape() != nullptr);

    // Mirror the teardown order of MainWindow::loadProject.
    controller.cancelActive();
    fillet.cancel();

    CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);
    CHECK(controller.activeSession() == nullptr);
    CHECK(fillet.lifecycle() == solidar::ToolLifecycle::Inactive);
    CHECK(fillet.previewShape() == nullptr);
    CHECK(fillet.edges().empty());
  }

  // End-to-end direct Sketch-on-Face Extrude: signed drag chooses outward
  // Join or inward Cut, commit creates one history entry, transient state is
  // cleared, and Undo/Redo plus history scrubbing preserve an editable model.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document document;
    auto& baseSketch = document.addSketch("Direct Extrude base");
    baseSketch.geometry.addRectangle({-20.0, -15.0}, {20.0, 15.0});
    auto& body = document.addBody("Direct Extrude body");
    auto base = std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 20.0, "Extrude");
    const auto baseId = base->id();
    body.addFeature(std::move(base));
    CHECK(document.recompute());
    const auto topFace =
        solidar::test::topPlanarFace(*body.resultShape(), 20.0);
    CHECK(topFace);
    const auto support = solidar::makeFaceReference(
        *body.resultShape(), body.id(), baseId, *topFace);
    auto& faceSketch = document.addSketch("Direct Extrude face profile");
    CHECK(document.attachSketchToFace(faceSketch.id, support));
    faceSketch.geometry.addRectangle({-5.0, -5.0}, {5.0, 5.0});
    CHECK(document.recompute());

    const QString directPath =
        directory.filePath(QStringLiteral("direct-face-extrude.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        directPath, document, &error));
    solidar::MainWindow directEditor(settings);
    CHECK(directEditor.loadProject(directPath, &error));
    directEditor.show();
    directEditor.activateWindow();
    QApplication::processEvents();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    const auto baseShape = Access::bodyShape(directEditor);
    CHECK(baseShape);
    const double baseVolume = solidar::test::volumeOf(*baseShape);

    Access::startSketchExtrude(directEditor, 1);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudeOperation(directEditor) ==
          solidar::ExtrudeOperation::Join);
    CHECK(!Access::directExtrudeReversed(directEditor));
    CHECK(Access::directExtrudePreview(directEditor));
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::activeTool(directEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Escape));
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::Inactive);
    CHECK(!Access::directExtrudePreview(directEditor));
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::undoCount(directEditor) == 0);

    Access::startSketchExtrude(directEditor, 1);
    Access::dragDirectExtrude(directEditor, 8.0);
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Return));
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 0);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::Inactive);
    CHECK(!Access::directExtrudePreview(directEditor));
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(Access::activeTool(directEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(Access::viewport(directEditor)->selectedBodyFaces().empty());
    CHECK(Access::viewport(directEditor)->selectionFilter() ==
          solidar::SelectionFilter::Any);
    CHECK(!Access::viewport(directEditor)->linearToolManipulator());
    const auto joinedShape = Access::bodyShape(directEditor);
    CHECK(joinedShape);
    CHECK(solidar::test::solidCount(*joinedShape) == 1);
    const double joinedVolume = solidar::test::volumeOf(*joinedShape);
    CHECK(joinedVolume > baseVolume);

    // A second Enter/Apply after completion is a no-op, not a duplicate node.
    Access::commitDirectExtrudeFromViewport(directEditor);
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);

    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 1);
    Access::redoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::bodyShape(directEditor)),
        joinedVolume, 1e-4));

    CHECK(Access::historyStepCount(directEditor) == 4);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        joinedVolume, 1e-4));
    Access::applyHistoryPosition(directEditor, 2);
    CHECK(!Access::historyAtEnd(directEditor));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        baseVolume, 1e-4));
    Access::moveHistoryToEnd(directEditor);
    CHECK(Access::historyAtEnd(directEditor));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        joinedVolume, 1e-4));

    // Returning to the tip must restore an immediately editable model.
    CHECK(Access::startTopFaceExtrude(directEditor, 28.0));
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Escape));
    CHECK(!Access::viewportHasToolPreview(directEditor));

    // Return to the base and create the inward Cut through the same UI path.
    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    Access::startSketchExtrude(directEditor, 1);
    Access::dragDirectExtrude(directEditor, -6.0);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudeOperation(directEditor) ==
          solidar::ExtrudeOperation::Cut);
    CHECK(Access::directExtrudeReversed(directEditor));
    CHECK(solidar::test::volumeOf(
              *Access::directExtrudePreview(directEditor)) < baseVolume);
    CHECK(Access::viewportHasToolPreview(directEditor));
    Access::commitDirectExtrudeFromViewport(directEditor);
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 0);
    const auto cutShape = Access::bodyShape(directEditor);
    CHECK(cutShape);
    CHECK(solidar::test::solidCount(*cutShape) == 1);
    const double cutVolume = solidar::test::volumeOf(*cutShape);
    CHECK(cutVolume < baseVolume);
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(!Access::viewport(directEditor)->linearToolManipulator());
    CHECK(Access::viewport(directEditor)->selectedBodyFaces().empty());

    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    Access::redoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::bodyShape(directEditor)), cutVolume,
        1e-4));

    // Deleting a step through its real context menu must let the menu and the
    // originating button finish their event handlers before rebuilding the
    // timeline. Rebuilding synchronously used to destroy both objects while
    // QMenu::exec() and customContextMenuRequested were still on the stack.
    const auto historyButtons =
        directEditor.findChildren<QToolButton*>(QStringLiteral("historyStep"));
    QToolButton* featureButton = nullptr;
    for (QToolButton* button : historyButtons)
      if (button->property("featureId").toULongLong() !=
          solidar::kInvalidFeatureId)
        featureButton = button;
    CHECK(featureButton != nullptr);
    QPointer<QToolButton> deletedButton(featureButton);
    QTimer contextMenuDriver;
    bool deleteChosen = false;
    QObject::connect(&contextMenuDriver, &QTimer::timeout, [&] {
      if (!deleteChosen)
        for (QWidget* widget : QApplication::allWidgets())
          if (auto* menu = qobject_cast<QMenu*>(widget))
            for (QAction* action : menu->actions())
              if (action->text() == QString::fromUtf8("Удалить")) {
                deleteChosen = true;
                menu->setActiveAction(action);
                QKeyEvent choose(QEvent::KeyPress, Qt::Key_Return,
                                 Qt::NoModifier);
                QApplication::sendEvent(menu, &choose);
                return;
              }
      for (QWidget* widget : QApplication::allWidgets()) {
        if (auto* box = qobject_cast<QMessageBox*>(widget)) {
          box->done(QMessageBox::Yes);
          return;
        }
      }
    });
    contextMenuDriver.start(1);
    const bool menuInvoked = QMetaObject::invokeMethod(
        featureButton, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, QPoint(4, 4)));
    QApplication::processEvents();
    contextMenuDriver.stop();
    CHECK(menuInvoked);
    CHECK(deletedButton.isNull());
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::historyStepCount(directEditor) == 3);
  }

  // Removing a Body must synchronize the derived sketch view while modern
  // profile consumption remains resolved from stable Feature dependencies.
  // Exercise the real confirmation plus Undo/Redo.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document twoBodies;
    auto& firstSketch = twoBodies.addSketch("First profile");
    firstSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& firstBody = twoBodies.addBody("First body");
    const auto removedBodyId = firstBody.id();
    firstBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        firstSketch.id, 5.0));
    auto& secondSketch = twoBodies.addSketch("Second profile");
    secondSketch.geometry.addRectangle({30.0, 0.0}, {42.0, 10.0});
    auto& secondBody = twoBodies.addBody("Second body");
    secondBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        secondSketch.id, 7.0));
    CHECK(twoBodies.recompute());
    const QString twoBodiesPath =
        directory.filePath(QStringLiteral("two-bodies-delete.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        twoBodiesPath, twoBodies, &error));
    CHECK(editor.loadProject(twoBodiesPath, &error));
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));

    Access::setHistoryByteBudget(editor, 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, removedBodyId);
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::statusMessage(editor).contains(
        QString::fromUtf8("Операция отменена")));
    CHECK(!Access::statusMessage(editor).contains(
        QString::fromUtf8("Body удалён")));
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);

    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, removedBodyId);
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::hasCommittedModernSolid(editor));
    CHECK(Access::hasDisplayableModernSolid(editor));
    CHECK(Access::hasExportableModernSolid(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));

    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::hasCommittedModernSolid(editor));
    CHECK(Access::hasDisplayableModernSolid(editor));
    CHECK(Access::hasExportableModernSolid(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));

    Access::redoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::hasCommittedModernSolid(editor));
    CHECK(Access::hasDisplayableModernSolid(editor));
    CHECK(Access::hasExportableModernSolid(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));
  }

  // A Feature cascade can remove a face-supported Sketch and Features in
  // other Bodies. The derived UI view must shrink before tree/history
  // rebuild and Undo must restore the same stable dependency graph.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document cascade;
    auto& baseSketch = cascade.addSketch("Cascade base");
    const auto baseSketchId = baseSketch.id;
    baseSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    auto& supportBody = cascade.addBody("Cascade support");
    const auto supportBodyId = supportBody.id();
    auto& supportFeature = supportBody.addFeature(
        std::make_unique<solidar::ExtrudeFeature>(baseSketchId, 10.0));
    const auto supportFeatureId = supportFeature.id();
    CHECK(cascade.recompute());
    const auto topFace =
        solidar::test::topPlanarFace(*supportBody.resultShape(), 10.0);
    CHECK(topFace);
    auto& faceSketch = cascade.addSketch("Cascade face sketch");
    const auto faceSketchId = faceSketch.id;
    faceSketch.geometry.addRectangle({2.0, 2.0}, {8.0, 8.0});
    CHECK(cascade.attachSketchToFace(
        faceSketchId, {supportBodyId, supportFeatureId, *topFace}));
    auto& consumerBody = cascade.addBody("Cascade consumer");
    const auto consumerBodyId = consumerBody.id();
    consumerBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        faceSketchId, 4.0));
    CHECK(cascade.recompute());

    const QString cascadePath =
        directory.filePath(QStringLiteral("feature-cascade-delete.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        cascadePath, cascade, &error));
    CHECK(editor.loadProject(cascadePath, &error));
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    const auto loadedCascade = Access::documentCopy(editor);
    const auto* loadedFaceSketch = loadedCascade.findSketch(faceSketchId);
    CHECK(loadedFaceSketch);
    CHECK(loadedFaceSketch->support.type ==
          solidar::SketchSupportType::Face);
    CHECK(loadedFaceSketch->support.face.bodyId() == supportBodyId);
    CHECK(loadedFaceSketch->support.face.featureId() == supportFeatureId);
    Access::editSketch(editor, faceSketchId);
    const auto editFaceReference = Access::currentSketchFaceReference(editor);
    CHECK(editFaceReference);
    CHECK(editFaceReference->bodyId() == supportBodyId);
    CHECK(editFaceReference->featureId() == supportFeatureId);
    CHECK(editor.loadProject(cascadePath, &error));
    const auto bodyPresentationBeforeRollback =
        Access::displayedBodyShape(editor);
    CHECK(bodyPresentationBeforeRollback);
    const double bodyVolumeBeforeRollback =
        solidar::test::volumeOf(*bodyPresentationBeforeRollback);
    CHECK(Access::sketchConsumedByModernFeature(editor, faceSketchId));
    const auto rollbackSelectionBefore = Access::selectionState(editor);

    // Force production history-budget rejection. The delete path must not
    // remove a viewport row or disturb positional picks before acceptance.
    Access::setHistoryByteBudget(editor, 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistoryFeature(editor, supportBodyId, supportFeatureId);
    CHECK(Access::hasSketch(editor, baseSketchId));
    CHECK(Access::hasSketch(editor, faceSketchId));
    CHECK(Access::bodyFeatureCount(editor, supportBodyId) == 1);
    CHECK(Access::bodyFeatureCount(editor, consumerBodyId) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::sketchConsumedByModernFeature(editor, faceSketchId));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      rollbackSelectionBefore));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
    const auto bodyPresentationAfterRollback =
        Access::displayedBodyShape(editor);
    CHECK(bodyPresentationAfterRollback);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*bodyPresentationAfterRollback),
        bodyVolumeBeforeRollback, 1e-6));
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);

    Access::setRevolveAxisSketchIndex(editor, 1);
    CHECK(Access::revolveAxisSketchIndex(editor) == 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistoryFeature(editor, supportBodyId, supportFeatureId);
    CHECK(Access::hasSketch(editor, baseSketchId));
    CHECK(!Access::hasSketch(editor, faceSketchId));
    CHECK(Access::bodyFeatureCount(editor, supportBodyId) == 0);
    CHECK(Access::bodyFeatureCount(editor, consumerBodyId) == 0);
    CHECK(Access::sketchViewCount(editor) == 1);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(!Access::sketchConsumedByModernFeature(editor, faceSketchId));
    CHECK(Access::noSketchPickState(editor));

    Access::undoAction(editor)->trigger();
    CHECK(Access::hasSketch(editor, baseSketchId));
    CHECK(Access::hasSketch(editor, faceSketchId));
    CHECK(Access::bodyFeatureCount(editor, supportBodyId) == 1);
    CHECK(Access::bodyFeatureCount(editor, consumerBodyId) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::sketchConsumedByModernFeature(editor, faceSketchId));
    CHECK(Access::noSketchPickState(editor));
    Access::redoAction(editor)->trigger();
    CHECK(!Access::hasSketch(editor, faceSketchId));
    CHECK(Access::sketchViewCount(editor) == 1);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::hasSketch(editor, faceSketchId));
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
  }

  // A corrupt v2 must fail before the transactional project-switch boundary:
  // the old model, path, dirty flag and active preview/session all survive.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow transactionalEditor(settings);
    CHECK(transactionalEditor.loadProject(pathA, &error));
    CHECK(Access::startTopFaceExtrude(transactionalEditor, 15.0));
    Access::dragDirectExtrude(transactionalEditor, 4.0);
    CHECK(Access::activeTool(transactionalEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::directExtrudeLifecycle(transactionalEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(transactionalEditor));
    transactionalEditor.setWindowModified(true);

    const QString oldPath = transactionalEditor.windowFilePath();
    const std::size_t oldBodyCount = Access::bodyCount(transactionalEditor);
    const std::size_t oldHistoryCount =
        Access::historyStepCount(transactionalEditor);
    const auto oldShape = Access::bodyShape(transactionalEditor);
    CHECK(oldShape && !oldShape->IsNull());
    const double oldVolume = solidar::test::volumeOf(*oldShape);
    const auto* const oldRootAddress =
        Access::documentAddress(transactionalEditor);
    const auto oldRootShapeWeak =
        Access::firstShapeWeak(transactionalEditor);
    CHECK(!oldRootShapeWeak.expired());

    QFile validFile(pathA);
    CHECK(validFile.open(QIODevice::ReadOnly));
    auto corruptRoot = QJsonDocument::fromJson(validFile.readAll()).object();
    corruptRoot.remove("model");
    const QString corruptPath =
        directory.filePath(QStringLiteral("corrupt-switch.solidar"));
    QFile corruptFile(corruptPath);
    CHECK(corruptFile.open(QIODevice::WriteOnly));
    const QByteArray corruptBytes =
        QJsonDocument(corruptRoot).toJson(QJsonDocument::Compact);
    CHECK(corruptFile.write(corruptBytes) == corruptBytes.size());
    corruptFile.close();

    CHECK(!transactionalEditor.loadProject(corruptPath, &error));
    CHECK(!error.isEmpty());
    CHECK(transactionalEditor.windowFilePath() == oldPath);
    CHECK(Access::documentAddress(transactionalEditor) == oldRootAddress);
    CHECK(!oldRootShapeWeak.expired());
    CHECK(Access::modified(transactionalEditor));
    CHECK(Access::bodyCount(transactionalEditor) == oldBodyCount);
    CHECK(Access::historyStepCount(transactionalEditor) == oldHistoryCount);
    CHECK(Access::activeTool(transactionalEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::directExtrudeLifecycle(transactionalEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(transactionalEditor));
    const auto preservedShape = Access::bodyShape(transactionalEditor);
    CHECK(preservedShape && !preservedShape->IsNull());
    CHECK(solidar::test::near(solidar::test::volumeOf(*preservedShape),
                              oldVolume, 1e-6));
    QFile preservedFile(corruptPath);
    CHECK(preservedFile.open(QIODevice::ReadOnly));
    CHECK(preservedFile.readAll() == corruptBytes);
  }

  // Import is also a Document replacement boundary. A pending NewBody/legacy
  // preview has no Body/Feature source id, so generation invalidation (not
  // source lookup) must stop it before the old B-Rep graph is destroyed.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow importEditor(settings);
    CHECK(importEditor.loadProject(pathA, &error));
    int rebuilds = 0;
    int publications = 0;
    Access::queueInvalidSourcePreview(importEditor, &rebuilds, &publications);
    solidar::Document imported;
    imported.setBox({123.0, 45.0, 67.0});
    auto& importedSketch = imported.addSketch("Imported presentation sketch");
    importedSketch.geometry.addRectangle({1.0, 2.0}, {8.0, 11.0});
    importedSketch.placement = solidar::SketchPlacement::xz();
    Access::applyImportedDocument(importEditor, std::move(imported));
    QApplication::processEvents(QEventLoop::AllEvents);
    CHECK(rebuilds == 0);
    CHECK(publications == 0);
    CHECK(Access::displayedSketchesMatchDocument(importEditor));
  }

  // The legacy operation detector is keyed by semantic inputs. Repeated
  // scheduling for the same body/revision/profile executes once, and Cancel
  // invalidates a queued detector before it can inspect or publish stale UI.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow legacyEditor(settings);
    CHECK(legacyEditor.loadProject(pathA, &error));
    const auto initialCount =
        Access::automaticExtrudeDetectionCount(legacyEditor);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::dispatchPreviewCadence(legacyEditor);
    CHECK(Access::automaticExtrudeDetectionCount(legacyEditor) ==
          initialCount + 1);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::dispatchPreviewCadence(legacyEditor);
    CHECK(Access::automaticExtrudeDetectionCount(legacyEditor) ==
          initialCount + 1);

    // Reset the semantic key, then enqueue one genuinely pending detection.
    Access::cancelLegacyExtrusion(legacyEditor);
    Access::exposeLegacyExtrusion(legacyEditor);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::cancelLegacyExtrusion(legacyEditor);
    Access::dispatchPreviewCadence(legacyEditor);
    CHECK(Access::automaticExtrudeDetectionCount(legacyEditor) ==
          initialCount + 1);
    CHECK(!Access::legacyExtrusionDockVisible(legacyEditor));
    CHECK(!Access::legacyExtrusionSelectionActive(legacyEditor));
  }

  // Body visibility is a model/view generation boundary. It must tear down
  // both a live tool preview and queued work before mutating the visible set.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow visibilityEditor(settings);
    CHECK(visibilityEditor.loadProject(pathA, &error));
    CHECK(Access::startTopFaceExtrude(visibilityEditor, 15.0));
    CHECK(Access::viewportHasToolPreview(visibilityEditor));
    int rebuilds = 0;
    int publications = 0;
    Access::queueInvalidSourcePreview(visibilityEditor, &rebuilds,
                                      &publications);
    CHECK(Access::setFirstBodyVisible(visibilityEditor, false));
    Access::dispatchPreviewCadence(visibilityEditor);
    CHECK(Access::activeTool(visibilityEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(!Access::viewportHasToolPreview(visibilityEditor));
    CHECK(rebuilds == 0);
    CHECK(publications == 0);
  }

  // Return commits the text currently being edited, not the previous spinbox
  // value: interpretText -> coalesced preview flush -> exact model commit.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow moveEditor(settings);
    CHECK(moveEditor.loadProject(pathA, &error));
    moveEditor.show();
    QApplication::processEvents();
    const std::size_t featureCount = Access::bodyFeatureCount(moveEditor);
    CHECK(Access::startMoveForActiveBody(moveEditor));
    CHECK(Access::enterMoveXAndAccept(moveEditor, QStringLiteral("12")));
    QApplication::processEvents();
    CHECK(Access::bodyFeatureCount(moveEditor) == featureCount + 1);
    const auto moveOffset = Access::lastMoveOffset(moveEditor);
    CHECK(moveOffset.has_value());
    CHECK(solidar::test::near(moveOffset->x, 12.0, 1e-9));
    CHECK(solidar::test::near(moveOffset->y, 0.0, 1e-9));
    CHECK(solidar::test::near(moveOffset->z, 0.0, 1e-9));
    CHECK(Access::activeTool(moveEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(!Access::viewportHasToolPreview(moveEditor));
  }

  // End-to-end reproduction of sketch-on-face for the visible cap of a
  // YZ-circle extrusion. The MainWindow route must preserve the exact cap,
  // outward normal and camera hemisphere seen in the 3D viewport.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document cylinder;
    auto& profile = cylinder.addSketch("YZ circle");
    profile.placement = solidar::SketchPlacement::yz();
    profile.geometry.addCircle({0.0, 0.0}, 10.0);
    auto& body = cylinder.addBody("Cylinder");
    body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        profile.id, 50.0, "Cylinder"));
    CHECK(cylinder.recompute());

    solidar::MainWindow faceEditor(settings);
    Access::installDocument(faceEditor, std::move(cylinder));
    CHECK(Access::openSketchOnBodyFaceAt(faceEditor, {0.0, 0.0, 0.0}));
    const auto placement = Access::currentSketchPlacement(faceEditor);
    const auto normal = placement.normal();
    CHECK(std::abs(placement.origin.x) < 1e-6);
    CHECK(normal.x < -0.999);
    const auto sketchEye = solidar::cameraViewingDirection(
        Access::sketch(faceEditor)->viewCubeCamera());
    CHECK(sketchEye.x < -0.999);
    CHECK(std::abs(sketchEye.y) < 1e-6);
    CHECK(std::abs(sketchEye.z) < 1e-6);
    CHECK(std::abs(std::remainder(
              Access::sketch(faceEditor)->viewRotationDegrees() + 90.0,
              360.0)) < 1e-6);
    const QPointF viewportOrigin =
        Access::viewportWorldScreen(faceEditor, {});
    const QPointF sketchOrigin =
        Access::sketch(faceEditor)->worldOriginScreenPosition();
    CHECK(QLineF(viewportOrigin, sketchOrigin).length() < 1e-5);
    CHECK(std::abs(Access::viewportPixelsPerMm(faceEditor) -
                   Access::sketch(faceEditor)->viewPixelsPerMillimeter()) <
          1e-5);
    CHECK(Access::currentSketchFaceReference(faceEditor).has_value());
  }

  // A YZ NewBody extrusion keeps one camera and one world-space shape through
  // preview -> commit. Opening and closing the right tool dock must not move
  // the world origin on screen, while reversed preview bounds must exactly
  // match the committed B-Rep.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document document;
    auto& profile = document.addSketch("YZ circle direct extrude");
    profile.placement = solidar::SketchPlacement::yz();
    profile.geometry.addCircle({0.0, 0.0}, 20.0);
    CHECK(document.recompute());

    solidar::MainWindow directEditor(settings);
    Access::installDocument(directEditor, std::move(document));
    directEditor.resize(1200, 760);
    directEditor.show();
    QApplication::processEvents();
    const float yawBefore = Access::viewport(directEditor)->cameraYawDegrees();
    const float pitchBefore = Access::viewport(directEditor)->cameraPitchDegrees();
    const float zoomBefore = Access::viewportZoom(directEditor);
    const QPointF originBefore =
        Access::viewportWorldScreen(directEditor, {});

    Access::startSketchExtrude(directEditor, 0);
    Access::dragDirectExtrude(directEditor, -25.0);
    QApplication::processEvents();
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudeReversed(directEditor));
    const auto preview = Access::directExtrudePreview(directEditor);
    CHECK(preview);
    const auto previewBounds = solidar::test::boundsOf(*preview);
    CHECK(QLineF(originBefore,
                 Access::viewportWorldScreen(directEditor, {})).length() <
          1.0);

    Access::commitDirectExtrudeFromViewport(directEditor);
    QApplication::processEvents();
    const auto committed = Access::bodyShape(directEditor);
    CHECK(committed);
    const auto committedBounds = solidar::test::boundsOf(*committed);
    CHECK(solidar::test::near(previewBounds.minX, committedBounds.minX,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.maxX, committedBounds.maxX,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.minY, committedBounds.minY,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.maxY, committedBounds.maxY,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.minZ, committedBounds.minZ,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.maxZ, committedBounds.maxZ,
                             1e-6));
    CHECK(solidar::test::near(
        Access::viewport(directEditor)->cameraYawDegrees(), yawBefore, 1e-6));
    CHECK(solidar::test::near(
        Access::viewport(directEditor)->cameraPitchDegrees(), pitchBefore,
        1e-6));
    CHECK(solidar::test::near(Access::viewportZoom(directEditor), zoomBefore,
                             1e-6));
    CHECK(QLineF(originBefore,
                 Access::viewportWorldScreen(directEditor, {})).length() <
          1.0);
  }

  // The ribbon's legacy extrusion route used to draw a screen-space cylinder
  // that could look inside-out on YZ. Its preview must now be the exact OCCT
  // solid that will be committed, without changing the camera hemisphere.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document document;
    auto& profile = document.addSketch("YZ circle legacy extrude");
    profile.placement = solidar::SketchPlacement::yz();
    profile.geometry.addCircle({0.0, 0.0}, 20.0);
    CHECK(document.recompute());

    solidar::MainWindow legacyEditor(settings);
    Access::installDocument(legacyEditor, std::move(document));
    legacyEditor.resize(1200, 760);
    legacyEditor.show();
    QApplication::processEvents();
    const float yawBefore = Access::viewport(legacyEditor)->cameraYawDegrees();
    const float pitchBefore =
        Access::viewport(legacyEditor)->cameraPitchDegrees();

    CHECK(Access::startLegacySketchExtrude(legacyEditor, 0));
    QApplication::processEvents();
    const auto preview = Access::viewportToolPreviewShape(legacyEditor);
    CHECK(preview);
    const auto previewBounds = solidar::test::boundsOf(*preview);
    CHECK(Access::commitLegacyExtrusionAsNewBody(legacyEditor));
    QApplication::processEvents();
    const auto committed = Access::bodyShape(legacyEditor);
    CHECK(committed);
    const auto committedBounds = solidar::test::boundsOf(*committed);
    CHECK(solidar::test::near(previewBounds.minX, committedBounds.minX,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.maxX, committedBounds.maxX,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.minY, committedBounds.minY,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.maxY, committedBounds.maxY,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.minZ, committedBounds.minZ,
                             1e-6));
    CHECK(solidar::test::near(previewBounds.maxZ, committedBounds.maxZ,
                             1e-6));
    CHECK(solidar::test::near(
        Access::viewport(legacyEditor)->cameraYawDegrees(), yawBefore, 1e-6));
    CHECK(solidar::test::near(
        Access::viewport(legacyEditor)->cameraPitchDegrees(), pitchBefore,
        1e-6));
  }

  // Leaving Sketcher transfers its current global camera to the hidden 3D
  // viewport.  The ViewCube hemisphere must therefore stay identical through
  // the first extrusion preview and its commit, including on the YZ plane.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow transitionEditor(settings);
    transitionEditor.resize(1200, 760);
    transitionEditor.show();
    QApplication::processEvents();

    solidar::sketch::Sketch circle;
    circle.addCircle({0.0, 0.0}, 20.0);
    const auto placement = solidar::SketchPlacement::yz();
    const auto sketchCamera = Access::finishNewSketchFromOrbit(
        transitionEditor, circle, placement, 37.0, -22.0,
        QStringLiteral("YZ transition profile"));
    QApplication::processEvents();
    CHECK(std::abs(solidar::shortestAngleDelta(
              Access::viewport(transitionEditor)->cameraYawDegrees(),
              sketchCamera.yaw)) < 1e-5F);
    CHECK(std::abs(solidar::shortestAngleDelta(
              Access::viewport(transitionEditor)->cameraPitchDegrees(),
              sketchCamera.pitch)) < 1e-5F);

    Access::startSketchExtrude(transitionEditor, 0);
    Access::dragDirectExtrude(transitionEditor, 25.0);
    QApplication::processEvents();
    CHECK(Access::directExtrudeLifecycle(transitionEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(std::abs(solidar::shortestAngleDelta(
              Access::viewport(transitionEditor)->cameraYawDegrees(),
              sketchCamera.yaw)) < 1e-5F);
    CHECK(std::abs(solidar::shortestAngleDelta(
              Access::viewport(transitionEditor)->cameraPitchDegrees(),
              sketchCamera.pitch)) < 1e-5F);

    Access::commitDirectExtrudeFromViewport(transitionEditor);
    QApplication::processEvents();
    CHECK(std::abs(solidar::shortestAngleDelta(
              Access::viewport(transitionEditor)->cameraYawDegrees(),
              sketchCamera.yaw)) < 1e-5F);
    CHECK(std::abs(solidar::shortestAngleDelta(
              Access::viewport(transitionEditor)->cameraPitchDegrees(),
              sketchCamera.pitch)) < 1e-5F);
  }

  // Closing a modified project must never discard work without an explicit
  // choice. Cancel keeps the editor alive; Discard permits the close.
  // V9 Open commits its retained runtime diagnostics only with the assembly
  // workspace. Selection and Save preserve the validity seal; a successful
  // authored history command invalidates it without serializing diagnostics.
  // Keep the V9 workspace matrix out of main's already large Debug RTC frame.
  [&] {
    using Access = solidar::MainWindowUndoTestAccess;
    QFile v9Fixture(QString::fromUtf8(SOLIDAR_FULL_V9_FIXTURE_PATH));
    CHECK(v9Fixture.open(QIODevice::ReadOnly));
    auto v9Root = QJsonDocument::fromJson(v9Fixture.readAll()).object();
    auto v9Definitions = v9Root.value("definitions").toArray();
    auto linkedPart = v9Definitions[0].toObject();
    const auto geometryV4 =
        solidar::project::detail::encodeProjectFormatV4DocumentRoot(
            uiLinkedDocument(24.0),
            {QStringLiteral("part"),
             QStringLiteral("2026-10-29T00:00:00Z")});
    CHECK(geometryV4.succeeded());
    linkedPart.insert(
        "payload",
        QJsonObject{
            {"payloadKind", "linked-part-v1"},
            {"binding",
             QJsonObject{
                 {"projectId", "123e4567-e89b-42d3-a456-426614174099"},
                 {"definitionId", 1},
                 {"relativePathHint", "parts/missing-v9-source.solidar"},
                 {"sha256", QString(64, QLatin1Char('0'))}}},
            {"lastValid", *geometryV4.root}});
    v9Definitions[0] = linkedPart;
    v9Root.insert("definitions", v9Definitions);
    const QByteArray authoredV9Bytes =
        QJsonDocument(v9Root).toJson(QJsonDocument::Compact);
    const QString v9Path =
        directory.filePath(QStringLiteral("ui-v9-retained.solidar"));
    CHECK(writeTestBytes(v9Path, authoredV9Bytes));
    solidar::MainWindow v9Editor(settings);
    QString v9Error;
    const bool v9Loaded = v9Editor.loadProject(v9Path, &v9Error);
    if (!v9Loaded)
      std::cerr << "V9 retained fixture open: "
                << v9Error.toStdString() << '\n';
    CHECK(v9Loaded);
    CHECK(Access::persistenceState(v9Editor).format ==
          solidar::project::ProjectPersistenceFormat::V9);
    CHECK(Access::mateDiagnosticsCount(v9Editor) == 1);
    CHECK(Access::firstMateDiagnosticStatus(v9Editor) ==
              solidar::MateSolveStatus::Broken ||
          Access::firstMateDiagnosticStatus(v9Editor) ==
              solidar::MateSolveStatus::Missing);
    CHECK(Access::mateDiagnosticsCurrent(v9Editor));
    CHECK(Access::linkedRuntimeStates(v9Editor).size() == 1);
    CHECK(Access::linkedRuntimeStates(v9Editor).begin()->second.status ==
          solidar::LinkedPartRuntimeStatus::Missing);
    const auto leafPath = solidar::OccurrencePath::create(
        {solidar::OccurrenceId{1}, solidar::OccurrenceId{7}});
    CHECK(leafPath);
    const auto leaf = Access::assemblyOccurrenceIndexForPath(
        v9Editor, *leafPath.path);
    CHECK(leaf.isValid());
    Access::selectAssemblyTreeIndex(v9Editor, leaf);
    CHECK(Access::mateDiagnosticsCurrent(v9Editor));
    CHECK(Access::saveProjectToPath(v9Editor, v9Path, &v9Error));
    CHECK(Access::mateDiagnosticsCurrent(v9Editor));
    CHECK(Access::executeAssemblyRenameForSemanticState(
              v9Editor, Access::assemblyCommandSemanticState(v9Editor),
              Access::assemblyCommandSemanticState(v9Editor)) ==
          solidar::AssemblyCommandHistoryStatus::Committed);
    CHECK(!Access::mateDiagnosticsCurrent(v9Editor));
    CHECK(Access::saveProjectToPath(v9Editor, v9Path, &v9Error));
    CHECK(!v9Editor.isWindowModified());
    Access::undoAssembly(v9Editor);
    CHECK(!Access::mateDiagnosticsCurrent(v9Editor));
    CHECK(v9Editor.isWindowModified());
    CHECK(Access::assemblyCanRedo(v9Editor));

    // A hard resource failure from an otherwise valid V9 is staged over a
    // dirty workspace with a redo tail, selection, hover and isolation. The
    // complete live workspace and all process-global ID cursors are exact
    // no-ops.
    const auto dirtyLeaf = Access::assemblyOccurrenceIndexForPath(
        v9Editor, *leafPath.path);
    CHECK(dirtyLeaf.isValid());
    Access::selectAssemblyTreeIndex(v9Editor, dirtyLeaf);
    const auto selectedBeforeFailure =
        Access::viewportAssemblySelection(v9Editor);
    CHECK(selectedBeforeFailure);
    CHECK(Access::setHoveredAssemblyOccurrence(v9Editor,
                                                selectedBeforeFailure));
    CHECK(Access::assemblyIsolationEnabled(v9Editor));
    Access::triggerAssemblyIsolation(v9Editor);
    CHECK(Access::assemblyIsolationActive(v9Editor));
    const auto* productBeforeFailure =
        Access::productDocumentAddress(v9Editor);
    const auto* sessionBeforeFailure =
        Access::assemblySessionAddress(v9Editor);
    const auto* treeBeforeFailure =
        Access::assemblyTreeModelAddress(v9Editor);
    const auto publicationBeforeFailure =
        sessionBeforeFailure->publicationHandle();
    const auto historySizeBeforeFailure =
        Access::assemblyHistorySize(v9Editor);
    const auto historyCursorBeforeFailure =
        Access::assemblyHistoryCursor(v9Editor);
    const auto historyRetainedBeforeFailure =
        Access::assemblyHistoryRetainedBytes(v9Editor);
    const auto historyRevisionBeforeFailure =
        Access::assemblyHistoryRevision(v9Editor);
    const auto savepointBeforeFailure =
        Access::assemblyHistorySavepoint(v9Editor);
    const bool redoBeforeFailure = Access::assemblyCanRedo(v9Editor);
    const auto persistenceBeforeFailure = Access::persistenceState(v9Editor);
    const auto identityBeforeFailure = Access::identity(v9Editor);
    const auto linkedBeforeFailure = Access::linkedRuntimeStates(v9Editor);
    const auto* mateAddressBeforeFailure =
        Access::mateDiagnosticsAddress(v9Editor);
    const auto mateCountBeforeFailure =
        Access::mateDiagnosticsCount(v9Editor);
    const auto mateStatusBeforeFailure =
        Access::firstMateDiagnosticStatus(v9Editor);
    const bool mateCurrentBeforeFailure =
        Access::mateDiagnosticsCurrent(v9Editor);
    const auto treeSelectionBeforeFailure = Access::occurrenceForIndex(
        v9Editor, Access::assemblyTreeCurrentIndex(v9Editor));
    const auto hoverBeforeFailure = Access::hoveredHighlightIdentity(v9Editor);
    const auto isolationBeforeFailure =
        Access::isolatedOccurrencePath(v9Editor);
    const QString pathBeforeFailure = v9Editor.windowFilePath();
    const bool modifiedBeforeFailure = v9Editor.isWindowModified();
    auto hardRoot = v9Root;
    auto hardDefinitions = hardRoot.value("definitions").toArray();
    auto hardOwner = hardDefinitions[2].toObject();
    auto hardOptions = hardOwner.value("mateSolverOptions").toObject();
    hardOptions.insert("maximumWorkUnits", 1);
    hardOwner.insert("mateSolverOptions", hardOptions);
    hardDefinitions[2] = hardOwner;
    hardRoot.insert("definitions", hardDefinitions);
    const QString hardV9Path =
        directory.filePath(QStringLiteral("ui-v9-hard-resource.solidar"));
    CHECK(writeTestBytes(
        hardV9Path,
        QJsonDocument(hardRoot).toJson(QJsonDocument::Compact)));
    const auto cursorsBeforeHardFailure =
        consumeModelIdCursors("before UI V9 hard failure");
    CHECK(!v9Editor.loadProject(hardV9Path, &v9Error));
    const auto cursorsAfterHardFailure =
        consumeModelIdCursors("after UI V9 hard failure");
    CHECK(cursorsAfterHardFailure.body ==
          cursorsBeforeHardFailure.body + 1);
    CHECK(cursorsAfterHardFailure.sketch ==
          cursorsBeforeHardFailure.sketch + 1);
    CHECK(cursorsAfterHardFailure.feature ==
          cursorsBeforeHardFailure.feature + 1);
    CHECK(cursorsAfterHardFailure.product.value() ==
          cursorsBeforeHardFailure.product.value() + 1);
    CHECK(Access::productDocumentAddress(v9Editor) == productBeforeFailure);
    CHECK(Access::assemblySessionAddress(v9Editor) == sessionBeforeFailure);
    CHECK(Access::assemblyTreeModelAddress(v9Editor) == treeBeforeFailure);
    CHECK(sessionBeforeFailure->publicationHandle().isSameCurrentPublication(
        publicationBeforeFailure));
    CHECK(Access::assemblyHistorySize(v9Editor) == historySizeBeforeFailure);
    CHECK(Access::assemblyHistoryCursor(v9Editor) ==
          historyCursorBeforeFailure);
    CHECK(Access::assemblyHistoryRetainedBytes(v9Editor) ==
          historyRetainedBeforeFailure);
    CHECK(Access::assemblyHistoryRevision(v9Editor) ==
          historyRevisionBeforeFailure);
    CHECK(Access::assemblyHistorySavepoint(v9Editor) ==
          savepointBeforeFailure);
    CHECK(Access::assemblyCanRedo(v9Editor) == redoBeforeFailure);
    CHECK(Access::persistenceState(v9Editor) == persistenceBeforeFailure);
    CHECK(Access::identity(v9Editor) == identityBeforeFailure);
    CHECK(Access::linkedRuntimeStates(v9Editor) == linkedBeforeFailure);
    CHECK(Access::mateDiagnosticsAddress(v9Editor) ==
          mateAddressBeforeFailure);
    CHECK(Access::mateDiagnosticsCount(v9Editor) ==
          mateCountBeforeFailure);
    CHECK(Access::firstMateDiagnosticStatus(v9Editor) ==
          mateStatusBeforeFailure);
    CHECK(Access::mateDiagnosticsCurrent(v9Editor) ==
          mateCurrentBeforeFailure);
    CHECK(Access::occurrenceForIndex(
              v9Editor, Access::assemblyTreeCurrentIndex(v9Editor)) ==
          treeSelectionBeforeFailure);
    CHECK(Access::viewportAssemblySelection(v9Editor) ==
          selectedBeforeFailure);
    CHECK(Access::hoveredHighlightIdentity(v9Editor) == hoverBeforeFailure);
    CHECK(Access::isolatedOccurrencePath(v9Editor) ==
          isolationBeforeFailure);
    CHECK(v9Editor.windowFilePath() == pathBeforeFailure);
    CHECK(v9Editor.isWindowModified() == modifiedBeforeFailure);

    Access::triggerAssemblyIsolation(v9Editor);
    CHECK(!Access::assemblyIsolationActive(v9Editor));
    const auto saveAsTreeSelection = Access::occurrenceForIndex(
        v9Editor, Access::assemblyTreeCurrentIndex(v9Editor));
    const auto saveAsViewportSelection =
        Access::viewportAssemblySelection(v9Editor);
    const auto saveAsHover = Access::hoveredHighlightIdentity(v9Editor);
    const auto saveAsIsolation = Access::isolatedOccurrencePath(v9Editor);
    const QByteArray sourceBeforeSaveAs = readTestBytes(v9Path);
    const QDateTime sourceMtimeBeforeSaveAs =
        QFileInfo(v9Path).lastModified();

    const auto assertRejectedSaveAsNoOp = [&] {
      CHECK(Access::productDocumentAddress(v9Editor) == productBeforeFailure);
      CHECK(Access::assemblyHistorySize(v9Editor) == historySizeBeforeFailure);
      CHECK(Access::assemblyHistoryCursor(v9Editor) ==
            historyCursorBeforeFailure);
      CHECK(Access::assemblyHistoryRetainedBytes(v9Editor) ==
            historyRetainedBeforeFailure);
      CHECK(Access::assemblyHistoryRevision(v9Editor) ==
            historyRevisionBeforeFailure);
      CHECK(Access::assemblyHistorySavepoint(v9Editor) ==
            savepointBeforeFailure);
      CHECK(Access::assemblyCanRedo(v9Editor) == redoBeforeFailure);
      CHECK(Access::persistenceState(v9Editor) == persistenceBeforeFailure);
      CHECK(Access::identity(v9Editor) == identityBeforeFailure);
      CHECK(Access::linkedRuntimeStates(v9Editor) == linkedBeforeFailure);
      CHECK(Access::mateDiagnosticsAddress(v9Editor) ==
            mateAddressBeforeFailure);
      CHECK(Access::mateDiagnosticsCurrent(v9Editor) ==
            mateCurrentBeforeFailure);
      CHECK(Access::occurrenceForIndex(
                v9Editor, Access::assemblyTreeCurrentIndex(v9Editor)) ==
            saveAsTreeSelection);
      CHECK(Access::viewportAssemblySelection(v9Editor) ==
            saveAsViewportSelection);
      CHECK(Access::hoveredHighlightIdentity(v9Editor) == saveAsHover);
      CHECK(Access::isolatedOccurrencePath(v9Editor) == saveAsIsolation);
      CHECK(v9Editor.windowFilePath() == pathBeforeFailure);
      CHECK(v9Editor.isWindowModified() == modifiedBeforeFailure);
      CHECK(readTestBytes(v9Path) == sourceBeforeSaveAs);
      CHECK(QFileInfo(v9Path).lastModified() == sourceMtimeBeforeSaveAs);
    };

    bool aliasAttempted = false;
    const auto aliasRejected = Access::runAssemblySaveAsCommand(
        v9Editor,
        [&](const QString&) -> std::optional<QString> { return v9Path; },
        &aliasAttempted);
    CHECK(!aliasRejected.succeeded());
    CHECK(aliasAttempted);
    assertRejectedSaveAsNoOp();

    const QString crossDirectory =
        directory.filePath(QStringLiteral("v9-cross-directory"));
    CHECK(QDir().mkpath(crossDirectory));
    const QString crossTarget =
        crossDirectory + QStringLiteral("/copy.solidar");
    bool crossAttempted = false;
    const auto crossRejected = Access::runAssemblySaveAsCommand(
        v9Editor,
        [&](const QString&) -> std::optional<QString> { return crossTarget; },
        &crossAttempted);
    CHECK(!crossRejected.succeeded());
    CHECK(crossAttempted);
    CHECK(!QFile::exists(crossTarget));
    assertRejectedSaveAsNoOp();

    // Same-directory V9 Save As preserves the redo tail and invalid diagnostic
    // stamp, marks the current undone revision as the savepoint, never touches
    // the missing linked source, and only then forks identity/path.
    const QString missingLinkedSource =
        QFileInfo(v9Path).absolutePath() +
        QStringLiteral("/parts/missing-v9-source.solidar");
    CHECK(!QFile::exists(missingLinkedSource));
    const QString v9CopyPath =
        directory.filePath(QStringLiteral("ui-v9-copy.solidar"));
    bool v9SaveAsAttempted = false;
    const auto v9SaveAs = Access::runAssemblySaveAsCommand(
        v9Editor,
        [&](const QString&) -> std::optional<QString> { return v9CopyPath; },
        &v9SaveAsAttempted);
    CHECK(v9SaveAs.succeeded());
    CHECK(v9SaveAsAttempted);
    CHECK(v9Editor.windowFilePath() == v9CopyPath);
    CHECK(Access::persistenceState(v9Editor).format ==
          solidar::project::ProjectPersistenceFormat::V9);
    CHECK(Access::identity(v9Editor) != identityBeforeFailure);
    CHECK(Access::assemblyHistorySize(v9Editor) == historySizeBeforeFailure);
    CHECK(Access::assemblyHistoryCursor(v9Editor) ==
          historyCursorBeforeFailure);
    CHECK(Access::assemblyHistoryRetainedBytes(v9Editor) ==
          historyRetainedBeforeFailure);
    CHECK(Access::assemblyCanRedo(v9Editor));
    CHECK(Access::assemblyHistorySavepoint(v9Editor) ==
          Access::assemblyHistoryRevision(v9Editor));
    CHECK(!v9Editor.isWindowModified());
    CHECK(Access::mateDiagnosticsAddress(v9Editor) ==
          mateAddressBeforeFailure);
    CHECK(!Access::mateDiagnosticsCurrent(v9Editor));
    CHECK(Access::linkedRuntimeStates(v9Editor) == linkedBeforeFailure);
    CHECK(readTestBytes(v9Path) == sourceBeforeSaveAs);
    CHECK(QFileInfo(v9Path).lastModified() == sourceMtimeBeforeSaveAs);
    CHECK(!QFile::exists(missingLinkedSource));
    const auto copiedV9 =
        solidar::ProjectApplicationService{}.stageOpenProduct(v9CopyPath);
    CHECK(copiedV9.succeeded());
    CHECK(copiedV9.staged->persistenceState->format ==
          solidar::project::ProjectPersistenceFormat::V9);
    Access::redoAssembly(v9Editor);
    CHECK(Access::assemblyHistoryCursor(v9Editor) == 1);
    CHECK(!Access::mateDiagnosticsCurrent(v9Editor));
    CHECK(Access::persistenceState(v9Editor).format ==
          solidar::project::ProjectPersistenceFormat::V9);
  }();

  CHECK(editor.loadProject(pathA, &error));
  CHECK(solidar::MainWindowUndoTestAccess::pushBodyMarker(
      editor, "close dirty sentinel"));
  editor.show();
  QApplication::processEvents();
  CHECK(solidar::MainWindowUndoTestAccess::modified(editor));
  QTimer::singleShot(0, [] {
    for (QWidget* widget : QApplication::topLevelWidgets())
      if (auto* box = qobject_cast<QMessageBox*>(widget))
        box->done(QMessageBox::Cancel);
  });
  CHECK(!editor.close());
  CHECK(editor.isVisible());

  QTimer::singleShot(0, [] {
    for (QWidget* widget : QApplication::topLevelWidgets())
      if (auto* box = qobject_cast<QMessageBox*>(widget))
        box->done(QMessageBox::Discard);
  });
  CHECK(editor.close());

  return EXIT_SUCCESS;
}

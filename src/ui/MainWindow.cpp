#include "ui/MainWindow.h"
#include "ui/PreviewUpdateCoordinator.h"

#include "model/ExtrudeFeature.h"
#include "model/SketchExtrudeBuilder.h"
#include "ui/interaction/ContextActionResolver.h"
#include "model/ChamferFeature.h"
#include "model/ExtrudeOperationDetector.h"
#include "model/FilletFeature.h"
#include "model/ShellFeature.h"
#include "model/DraftFeature.h"
#include "ui/ToolParametersPanel.h"
#include "ui/ToolIcon.h"
#include "ui/PartDesignToolHelp.h"
#include "ui/PartDesignHistory.h"
#include "ui/PartDesignErrorLocalization.h"
#include "ui/FeatureUiRegistry.h"
#include "ui/HistoryTimelineWidget.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/TopologyReferenceResolver.h"

#include <algorithm>
#include <cmath>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QIcon>
#include <QInputDialog>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QButtonGroup>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMenuBar>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QStatusBar>
#include <QStyle>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QShortcut>
#include <QToolButton>
#include <QTimer>
#include <QVariant>
#include <limits>

#include "drawing/EskdRenderer.h"
#include "sketch/SketchRibbon.h"
#include "ui/DrawingSheetView.h"
#include "ui/ModelRibbon.h"
#include "ui/SettingsWidget.h"
#include "ui/SketchCanvas.h"
#include "ui/Viewport.h"

namespace solidar {

namespace {
template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

const ShapeFeature* presentationOwner(const Body& body,
                                      const ShapeFeature::ShapePtr& shape) {
  if (!shape) return nullptr;
  for (auto feature = body.features().rbegin();
       feature != body.features().rend(); ++feature)
    if ((*feature)->lastValidShape().get() == shape.get()) return feature->get();
  return nullptr;
}

bool bodyConsumedByJoin(const Document& document, BodyId bodyId,
                        std::optional<FeatureId> excludedJoin = std::nullopt) {
  for (const auto& candidateBody : document.bodies())
    for (const auto& candidate : candidateBody.features())
      if (const auto* join =
              dynamic_cast<const JoinBodiesFeature*>(candidate.get()))
        if ((!excludedJoin || join->id() != *excludedJoin) &&
            (join->firstBodyId() == bodyId ||
            join->secondBodyId() == bodyId)
           )
          return true;
  return false;
}

void releaseUnconsumedJoinOperands(Document& document,
                                   const std::vector<BodyId>& operands) {
  for (const BodyId operandId : operands) {
    Body* operand = document.findBody(operandId);
    if (operand && !bodyConsumedByJoin(document, operandId))
      operand->setVisible(true);
  }
}

std::vector<BodyId> removedJoinOperandIds(
    const Document& before, const FeatureRemovalPlan& plan) {
  std::vector<BodyId> result;
  for (const FeatureId featureId : plan.featureIds)
    if (const auto* join = dynamic_cast<const JoinBodiesFeature*>(
            before.findFeature(featureId))) {
      result.push_back(join->firstBodyId());
      result.push_back(join->secondBodyId());
    }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

QString sketchPresentationLabel(const DocumentSketch& sketch) {
  if (sketch.support.type == SketchSupportType::Face)
    return QString::fromUtf8("Грань");
  const Vector3d normal = sketch.placement.normal();
  if (std::abs(normal.y) > 0.9) return QStringLiteral("XZ");
  if (std::abs(normal.x) > 0.9) return QStringLiteral("YZ");
  return QStringLiteral("XY");
}

// Maps stable technical model-layer errors to Russian user messages at the UI
// boundary. The model keeps technical English strings; only the UI translates.
}  // namespace

MainWindow::MainWindow(AppSettings& settings, QWidget* parent)
    : QMainWindow(parent), settings_(settings) {
  previewUpdates_ = std::make_unique<PreviewUpdateCoordinator>(this);
  buildUi();
  buildMenus();
  resize(1200, 760);
  setWindowTitle(QString::fromUtf8("Солидарность CAD — Скетчер ЕСКД [*]"));
}

MainWindow::~MainWindow() {
  // Drop queued lambdas while their captured UI/model state is still alive,
  // then tear down every ToolSession before Document member destruction.
  invalidatePreviewUpdates();
  static_cast<void>(partDesignCoordinator_.prepareDocumentReplacement());
}

void MainWindow::applyActivePartDesignTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Apply));
}

bool MainWindow::applyPartDesignBeginResult(
    PartDesignTransitionOutcome outcome, PartDesignToolKind kind) {
  PartDesignUiEffect effect;
  effect.transition = outcome;
  effect.state = partDesignCoordinator_.snapshot(kind, document_);
  if (outcome.effect == PartDesignTransitionEffect::Activated) {
    applyPartDesignUiEffect(std::move(effect));
    return true;
  }

  effect.clearPreview = true;
  effect.clearSelection = true;
  effect.hidePanel = true;
  applyPartDesignUiEffect(std::move(effect));
  mirrorPresentationBefore_.reset();
  if (outcome.failure.code != OperationFailureCode::None)
    statusBar()->showMessage(localizedPartDesignError(kind, outcome.failure),
                             4000);
  return false;
}

void MainWindow::applyPartDesignUiEffect(PartDesignUiEffect effect) {
  if (!std::holds_alternative<std::monostate>(effect.commitRequest)) {
    std::visit(
        Overloaded{
            [](std::monostate) {},
            [this](PartDesignUiEffect::ExtrudeCommit) { acceptFaceExtrudeTool(); },
            [this](PartDesignUiEffect::RevolveCommit) { acceptRevolveTool(); },
            [this](PartDesignUiEffect::FilletCommit) { acceptFilletTool(); },
            [this](PartDesignUiEffect::ChamferCommit) { acceptChamferTool(); },
            [this](PartDesignUiEffect::JoinBodiesCommit) { acceptJoinBodiesTool(); },
            [this](PartDesignUiEffect::ShellCommit) { acceptShellTool(); },
            [this](PartDesignUiEffect::DraftCommit) { acceptDraftTool(); },
            [this](PartDesignUiEffect::MirrorCommit) { acceptMirrorTool(); },
            [this](PartDesignUiEffect::MoveCommit) { acceptMoveTool(); },
            [this](PartDesignUiEffect::LinearPatternCommit) {
              acceptLinearPatternTool();
            },
            [this](PartDesignUiEffect::CircularPatternCommit) {
              acceptCircularPatternTool();
            }},
        effect.commitRequest);
    return;
  }

  if (effect.refreshPreview || effect.transition.effect ==
                                   PartDesignTransitionEffect::ReselectionCancelled) {
    std::visit(
        Overloaded{
            [](std::monostate) {},
            [this](PartDesignUiEffect::ExtrudeCommit) { updateFaceExtrudeToolPreview(); },
            [this](PartDesignUiEffect::RevolveCommit) { updateRevolveToolPreview(); },
            [this](PartDesignUiEffect::FilletCommit) { updateFilletToolPreview(); },
            [this](PartDesignUiEffect::ChamferCommit) { updateChamferToolPreview(); },
            [this](PartDesignUiEffect::JoinBodiesCommit) { updateJoinBodiesToolPreview(); },
            [this](PartDesignUiEffect::ShellCommit) { updateShellToolPreview(); },
            [this](PartDesignUiEffect::DraftCommit) { updateDraftToolPreview(); },
            [this](PartDesignUiEffect::MirrorCommit) { updateMirrorToolPreview(); },
            [this](PartDesignUiEffect::MoveCommit) { updateMoveToolPreview(); },
            [this](PartDesignUiEffect::LinearPatternCommit) {
              updateLinearPatternToolPreview();
            },
            [this](PartDesignUiEffect::CircularPatternCommit) {
              updateCircularPatternToolPreview();
            }},
        effect.target);
  }

  if (effect.clearPreview) {
    invalidatePreviewUpdates();
    viewport_->resetToolInteraction();
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    viewport_->clearLegacyExtrusionPreview();
  }
  if (effect.clearSelection) {
    viewport_->setSelectedBodies({});
    viewport_->setSelectedBodyEdges({});
    viewport_->setSelectedBodyFaces({});
    if (!effect.preserveSelectionMode) {
      viewport_->setEdgeMultiSelectionMode(false);
      viewport_->setFaceMultiSelectionMode(false);
      viewport_->setSelectionFilter(SelectionFilter::Any);
    }
  }
  if (effect.beginInputSelection) viewport_->beginDraftFaceSelection();
  if (effect.hidePanel) {
    if (toolParametersDock_) toolParametersDock_->hide();
    if (revolveDock_) revolveDock_->hide();
    if (mirrorDock_) mirrorDock_->hide();
    if (moveDock_) moveDock_->hide();
    if (linearPatternDock_) linearPatternDock_->hide();
    if (circularPatternDock_) circularPatternDock_->hide();
    modelRibbon_->clearActiveTool();
    refreshBodyViewFromDocument();
    // A cancelled edit restores immediately.  An accepted edit must defer its
    // no-change selection restoration until after the feature/history views
    // have been rebuilt by the model-commit adapter.
    if (effect.transition.effect == PartDesignTransitionEffect::Cancelled) {
      restoreCancelledEditUiTransaction();
      if (mirrorPresentationBefore_)
        applyPresentationState(*mirrorPresentationBefore_);
      mirrorPresentationBefore_.reset();
    }
  }
}

void MainWindow::openSettings() {
  // Shared settings control inside a modal dialog; switching the theme is a
  // presentation-only change that never touches the open Document.
  QDialog dialog(this);
  dialog.setWindowTitle(QString::fromUtf8("Настройки"));
  dialog.resize(460, 340);
  auto* layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(24, 24, 24, 24);
  auto* settingsWidget = new SettingsWidget(settings_, &dialog);
  layout->addWidget(settingsWidget);
  // The editor dialog must close on Cancel; the Home page leaves the signal
  // unconnected because there is no modal dialog to close.
  connect(settingsWidget, &SettingsWidget::cancelRequested, &dialog,
          &QDialog::reject);
  dialog.exec();
}

void MainWindow::updateSketchConstraintPanel() {
  if (!sketchConstraintsList_ || !sketchCanvas_) return;

  const QSignalBlocker blocker(sketchConstraintsList_);
  sketchConstraintsList_->clear();

  const auto entries = sketchCanvas_->selectedConstraintPanelEntries();

  if (entries.empty()) {
    auto* item = new QTreeWidgetItem(sketchConstraintsList_);
    item->setText(0, QString::fromUtf8("Нет ограничений"));
    // Use the disabled palette role from ThemeManager so this placeholder
    // follows Light/Dark automatically.
    item->setFlags(Qt::NoItemFlags);
    return;
  }

  for (const auto& entry : entries) {
    auto* item = new QTreeWidgetItem(sketchConstraintsList_);
    item->setText(0, entry.description);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                   Qt::ItemIsEnabled);
    item->setCheckState(1, entry.checked ? Qt::Checked : Qt::Unchecked);

    item->setData(
        0, Qt::UserRole,
        QVariant::fromValue<qulonglong>(
            static_cast<qulonglong>(entry.constraintId)));

    item->setData(
        0, Qt::UserRole + 1,
        QVariant::fromValue<qulonglong>(
            entry.isDimension()
                ? static_cast<qulonglong>(entry.dimensionIndex)
                : std::numeric_limits<qulonglong>::max()));
  }
}
void MainWindow::buildMenus() {
  auto* menuHost = new QWidget(this);
  auto* layout = new QVBoxLayout(menuHost);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto* bar = new QMenuBar(menuHost);
  auto* fileMenu = bar->addMenu(QString::fromUtf8("Файл"));
  auto* createAction = fileMenu->addAction(QString::fromUtf8("Создать"));
  createAction->setShortcut(QKeySequence::New);
  auto* openAction = fileMenu->addAction(QString::fromUtf8("Открыть"));
  openAction->setShortcut(QKeySequence::Open);
  auto* saveAction = fileMenu->addAction(QString::fromUtf8("Сохранить"));
  saveAction->setShortcut(QKeySequence::Save);
  auto* importMenu = fileMenu->addMenu(QString::fromUtf8("Импорт"));
  auto* importStepAction =
      importMenu->addAction(QStringLiteral("STEP (*.step *.stp)"));
  auto* exportMenu = fileMenu->addMenu(QString::fromUtf8("Экспорт"));
  auto* exportStepAction =
      exportMenu->addAction(QStringLiteral("STEP (*.step *.stp)"));
  auto* exportStlAction = exportMenu->addAction(QStringLiteral("STL (*.stl)"));

  auto* editMenu = bar->addMenu(QString::fromUtf8("Правка"));
  undoAction_ = editMenu->addAction(QString::fromUtf8("Отменить"));
  undoAction_->setShortcut(QKeySequence::Undo);
  undoAction_->setEnabled(false);
  redoAction_ = editMenu->addAction(QString::fromUtf8("Повторить"));
  redoAction_->setShortcut(QKeySequence::Redo);
  redoAction_->setEnabled(false);
  auto* settingsAction = editMenu->addAction(QString::fromUtf8("Настройки…"));
  settingsAction->setObjectName("settingsAction");
  connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

  // QDockWidget's close button only hides a panel.  Expose the standard
  // toggle actions so a closed construction tree or history panel always has
  // an obvious, state-synchronised way back.
  auto* viewMenu = bar->addMenu(QString::fromUtf8("Вид"));
  if (modelTreeDock_) {
    QAction* action = modelTreeDock_->toggleViewAction();
    action->setText(QString::fromUtf8("Дерево построений"));
    action->setObjectName(QStringLiteral("toggleModelTreeAction"));
    viewMenu->addAction(action);
  }
  if (historyDock_) {
    QAction* action = historyDock_->toggleViewAction();
    action->setText(QString::fromUtf8("История построений"));
    action->setObjectName(QStringLiteral("toggleHistoryAction"));
    viewMenu->addAction(action);
  }

  viewMenu->addSeparator();
  auto* themeMenu = viewMenu->addMenu(QString::fromUtf8("Тема"));
  themeMenu->setObjectName(QStringLiteral("themeMenu"));
  auto* themeGroup = new QActionGroup(themeMenu);
  themeGroup->setExclusive(true);
  const auto addThemeAction = [themeMenu, themeGroup](
                                  const QString& text,
                                  const QString& objectName) {
    QAction* action = themeMenu->addAction(text);
    action->setObjectName(objectName);
    action->setCheckable(true);
    themeGroup->addAction(action);
    return action;
  };
  QAction* systemTheme =
      addThemeAction(QString::fromUtf8("Системная"),
                     QStringLiteral("themeSystemAction"));
  QAction* lightTheme =
      addThemeAction(QString::fromUtf8("Светлая"),
                     QStringLiteral("themeLightAction"));
  QAction* darkTheme =
      addThemeAction(QString::fromUtf8("Тёмная"),
                     QStringLiteral("themeDarkAction"));

  const auto syncThemeActions = [this, systemTheme, lightTheme, darkTheme] {
    const QSignalBlocker systemBlocker(systemTheme);
    const QSignalBlocker lightBlocker(lightTheme);
    const QSignalBlocker darkBlocker(darkTheme);
    systemTheme->setChecked(settings_.theme() == AppTheme::System);
    lightTheme->setChecked(settings_.theme() == AppTheme::Light);
    darkTheme->setChecked(settings_.theme() == AppTheme::Dark);
  };
  syncThemeActions();
  connect(systemTheme, &QAction::triggered, this,
          [this] { settings_.setTheme(AppTheme::System); });
  connect(lightTheme, &QAction::triggered, this,
          [this] { settings_.setTheme(AppTheme::Light); });
  connect(darkTheme, &QAction::triggered, this,
          [this] { settings_.setTheme(AppTheme::Dark); });
  connect(&settings_, &AppSettings::themeChanged, this,
          [syncThemeActions](AppTheme) { syncThemeActions(); });

  layout->addWidget(bar);
  ribbonStack_->setParent(menuHost);
  layout->addWidget(ribbonStack_);
  setMenuWidget(menuHost);

  connect(createAction, &QAction::triggered, this, &MainWindow::createProject);
  connect(openAction, &QAction::triggered, this, &MainWindow::openProject);
  connect(saveAction, &QAction::triggered, this, &MainWindow::saveProject);
  connect(importStepAction, &QAction::triggered, this, &MainWindow::importStep);
  connect(exportStepAction, &QAction::triggered, this, &MainWindow::exportStep);
  connect(exportStlAction, &QAction::triggered, this, &MainWindow::exportStl);
  connect(undoAction_, &QAction::triggered, this, &MainWindow::undoLastAction);
  connect(redoAction_, &QAction::triggered, this, &MainWindow::redoLastAction);
  connect(sketchCanvas_, &SketchCanvas::undoAvailable, this,
          [this](bool) { updateUndoAvailability(); });
  connect(sketchCanvas_, &SketchCanvas::redoAvailable, this,
          [this](bool) { updateUndoAvailability(); });
}

bool MainWindow::pushModelTransition(
    Document previous, HistorySelectionState previousSelection,
    std::optional<HistorySelectionState> committedAfter) {
  if (modelHistory_.isApplying()) return true;
  const bool previousModified = isWindowModified();
  HistorySelectionState beforeSelection = editUiTransaction_
      ? editUiTransaction_->before
      : std::move(previousSelection);
  HistorySelectionState afterSelection = committedAfter
      ? std::move(*committedAfter)
      : captureHistorySelection();
  const auto result = modelHistory_.recordTransition(
      document_, std::move(previous), std::move(beforeSelection),
      std::move(afterSelection));

  if (result.status == HistoryCommitStatus::NoChange) {
    // The caller still owns tool-specific teardown. Defer restoration until
    // that teardown has finished so its selection clearing cannot overwrite
    // the exact pre-edit state.
    if (editUiTransaction_)
      pendingNoChangeUiRestore_ = *editUiTransaction_;
    updateUndoAvailability();
    return true;
  }

  if (result.status == HistoryCommitStatus::Rejected) {
    // Callers publish/tear down transient UI only after acceptance.  The
    // service has restored the Document, so leaving the active editor/session
    // untouched keeps a rejected operation immediately retryable.
    setWindowModified(previousModified);
    try { updateUndoAvailability(); } catch (...) {}
    const QString detail = QString::fromStdString(result.error);
    try {
      statusBar()->showMessage(
          detail.contains(QStringLiteral("budget"), Qt::CaseInsensitive)
              ? QString::fromUtf8("Операция отменена: превышен лимит истории")
              : QString::fromUtf8("Операция отменена: история не создана (%1)")
                    .arg(detail),
          5000);
    } catch (...) {}
    return false;
  }

  setWindowModified(!modelHistory_.isAtSavepoint());
  finishEditUiTransaction();
  updateUndoAvailability();
  return true;
}

void MainWindow::completeModelTransitionUi() {
  if (!pendingNoChangeUiRestore_) return;
  const auto transaction = std::move(*pendingNoChangeUiRestore_);
  pendingNoChangeUiRestore_.reset();
  restoreHistorySelection(transaction.before);
  applyPresentationState(transaction.before.presentation);
  setWindowModified(transaction.modified);
  finishEditUiTransaction();
  updateUndoAvailability();
}

bool MainWindow::pushPresentationTransition(
    HistorySelectionState before, HistorySelectionState after) {
  if (modelHistory_.isApplying()) return true;
  Document previous = document_;
  const bool previousModified = isWindowModified();
  const auto result = modelHistory_.recordTransition(
      document_, std::move(previous), std::move(before), std::move(after));
  if (result.status == HistoryCommitStatus::Rejected) {
    applyPresentationState(result.state.presentation);
    setWindowModified(previousModified);
    updateUndoAvailability();
    return false;
  }
  if (result.status == HistoryCommitStatus::Accepted)
    setWindowModified(!modelHistory_.isAtSavepoint());
  updateUndoAvailability();
  return true;
}

MainWindow::HistorySelectionState MainWindow::captureHistorySelection() const {
  HistorySelectionState state;
  state.sketchViewIds.reserve(sketchViews_.size());
  for (const auto& view : sketchViews_)
    state.sketchViewIds.push_back(view.documentSketchId);
  state.historyPosition = historyPosition_;
  state.atEnd = isHistoryAtEnd();
  state.historicalLegacyExtrusionSourceSketchId =
      historicalLegacyExtrusionSourceSketchId_;
  const QPointF bodyPosition = viewport_->bodyPosition();
  state.presentation.bodyPositionX = bodyPosition.x();
  state.presentation.bodyPositionY = bodyPosition.y();
  state.presentation.originVisible = viewport_->originVisible();
  state.presentation.sketchVisible = viewport_->sketchVisible();
  for (int plane = 0; plane < 3; ++plane)
    state.presentation.basePlanesVisible[static_cast<std::size_t>(plane)] =
        viewport_->basePlaneVisible(plane);
  state.presentation.sketchVisibilities.reserve(sketchViews_.size());
  for (std::size_t index = 0; index < sketchViews_.size(); ++index)
    state.presentation.sketchVisibilities.push_back(
        viewport_->sketchVisible(index) ? 1U : 0U);
  if (!state.atEnd && state.historyPosition > 0 &&
      state.historyPosition <= static_cast<int>(historySteps_.size())) {
    const auto& step = historySteps_[static_cast<std::size_t>(
        state.historyPosition - 1)];
    state.historyBodyId = step.bodyId;
    state.historyFeatureId = step.featureId;
    state.historySketchId = step.sketchId;
  }
  // A history location is represented by one stable selection domain only.
  state.faces = viewport_->selectedBodyFaces();
  if (state.faces.empty()) state.edges = viewport_->selectedBodyEdges();
  if (state.faces.empty() && state.edges.empty())
    state.bodies = viewport_->selectedBodies();
  return state;
}

MainWindow::HistorySelectionState
MainWindow::captureCommittedEndState() const {
  HistorySelectionState state = captureHistorySelection();
  state.bodies.clear();
  state.edges.clear();
  state.faces.clear();
  state.historyBodyId = kInvalidBodyId;
  state.historyFeatureId = kInvalidFeatureId;
  state.historySketchId = kInvalidSketchId;
  state.historyPosition = std::numeric_limits<int>::max();
  state.atEnd = true;
  if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Mirror &&
      mirrorPresentationBefore_)
    state.presentation.basePlanesVisible =
        mirrorPresentationBefore_->basePlanesVisible;
  return state;
}

void MainWindow::restoreHistorySelection(const HistorySelectionState& state) {
  historicalLegacyExtrusionSourceSketchId_ =
      state.historicalLegacyExtrusionSourceSketchId;
  int desiredPosition = state.atEnd
      ? static_cast<int>(historySteps_.size())
      : std::clamp(state.historyPosition, 0,
                   static_cast<int>(historySteps_.size()));
  if (!state.atEnd && (state.historyBodyId != kInvalidBodyId ||
                       state.historyFeatureId != kInvalidFeatureId ||
                       state.historySketchId != kInvalidSketchId)) {
    const auto found = std::find_if(
        historySteps_.begin(), historySteps_.end(),
        [&state](const HistoryStep& step) {
          return step.bodyId == state.historyBodyId &&
                 step.featureId == state.historyFeatureId &&
                 step.sketchId == state.historySketchId;
        });
    if (found != historySteps_.end())
      desiredPosition = static_cast<int>(
          std::distance(historySteps_.begin(), found) + 1);
  }
  applyHistoryPosition(desiredPosition);
  viewport_->setSelectedBodies({});
  viewport_->setSelectedBodyEdges({});
  viewport_->setSelectedBodyFaces({});
  if (!state.faces.empty()) {
    std::vector<FaceReference> valid;
    for (const auto& face : state.faces)
      if (document_.findBody(face.bodyId) &&
          document_.findFeature(face.featureId)) valid.push_back(face);
    viewport_->setSelectedBodyFaces(valid);
  } else if (!state.edges.empty()) {
    std::vector<EdgeReference> valid;
    for (const auto& edge : state.edges)
      if (document_.findBody(edge.bodyId) &&
          document_.findFeature(edge.featureId)) valid.push_back(edge);
    viewport_->setSelectedBodyEdges(valid);
  }
  else if (!state.bodies.empty()) {
    std::vector<BodyId> valid;
    for (const auto id : state.bodies)
      if (document_.findBody(id)) valid.push_back(id);
    viewport_->setSelectedBodies(valid);
  }
}

void MainWindow::applyPresentationState(
    const EditorPresentationState& state) {
  // Presentation history is applied as one typed adapter transaction.  Block
  // the feature tree while both views are updated so programmatic Undo/Redo
  // cannot record another command or leave the checkbox one click behind.
  const QSignalBlocker treeBlocker(featureTree_);
  viewport_->setBodyPosition(
      QPointF(state.bodyPositionX, state.bodyPositionY));
  viewport_->setOriginVisible(state.originVisible);
  viewport_->setSketchVisible(state.sketchVisible);
  for (int plane = 0; plane < 3; ++plane)
    viewport_->setBasePlaneVisible(
        plane, state.basePlanesVisible[static_cast<std::size_t>(plane)]);
  for (std::size_t index = 0;
       index < state.sketchVisibilities.size() &&
       index < sketchViews_.size(); ++index)
    viewport_->setSketchVisible(index,
                                state.sketchVisibilities[index] != 0U);

  const auto items = featureTree_->findItems(
      QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
  for (QTreeWidgetItem* item : items) {
    const int kind = item->data(0, Qt::UserRole).toInt();
    std::optional<bool> visible;
    if (kind == 1)
      visible = state.originVisible;
    else if (kind == 2)
      visible = state.sketchVisible;
    else if (kind >= 10 && kind <= 12)
      visible = state.basePlanesVisible[static_cast<std::size_t>(kind - 10)];
    else if (kind >= 20) {
      const auto index = static_cast<std::size_t>(kind - 20);
      if (index < state.sketchVisibilities.size())
        visible = state.sketchVisibilities[index] != 0U;
    }
    if (visible)
      item->setCheckState(0, *visible ? Qt::Checked : Qt::Unchecked);
  }
}

void MainWindow::synchronizeCommittedState(
    const HistorySelectionState& state) {
  sketchViews_.clear();
  sketchViews_.reserve(state.sketchViewIds.size());
  for (const SketchId id : state.sketchViewIds)
    if (document_.findSketch(id)) sketchViews_.push_back({id});
  historicalLegacyExtrusionSourceSketchId_ =
      state.historicalLegacyExtrusionSourceSketchId;
  viewport_->setBox(document_.box());
  syncSketchPresentationFromDocument();
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  restoreHistorySelection(state);
  // History/model rebuilds derive temporary visibility for the selected
  // history step.  The committed typed presentation is authoritative and is
  // therefore applied last to both viewport and tree.
  applyPresentationState(state.presentation);
  setWindowModified(!modelHistory_.isAtSavepoint());
}

void MainWindow::restoreCancelledEditUiTransaction() {
  if (!editUiTransaction_ || suppressEditCancelRestore_) return;
  const auto transaction = std::move(*editUiTransaction_);
  editUiTransaction_.reset();
  pendingNoChangeUiRestore_.reset();
  restoreHistorySelection(transaction.before);
  applyPresentationState(transaction.before.presentation);
  setWindowModified(transaction.modified);
}

void MainWindow::finishEditUiTransaction() noexcept {
  editUiTransaction_.reset();
  pendingNoChangeUiRestore_.reset();
  suppressEditCancelRestore_ = false;
}

void MainWindow::updateUndoAvailability() {
  if (!undoAction_) return;
  const bool sketchActive =
      workspaceStack_ && workspaceStack_->currentWidget() == sketchCanvas_;
  undoAction_->setEnabled(sketchActive ? sketchCanvas_->canUndo()
                                       : modelHistory_.canUndo());
  if (redoAction_) {
    redoAction_->setEnabled(sketchActive ? sketchCanvas_->canRedo()
                                         : modelHistory_.canRedo());
  }
}

void MainWindow::resetTransientModelingUi() {
  // PartDesignCoordinator owns modern sessions, while legacy sketch
  // extrusion and the dedicated Revolve panel have separate presentation.
  // Reset all presentation surfaces together before another ribbon command
  // starts; otherwise switching between generations can leave two parameter
  // windows active at once.
  invalidatePreviewUpdates();
  static_cast<void>(partDesignCoordinator_.cancelAll());
  resetTransientModelingPresentation();
}

void MainWindow::resetTransientModelingPresentation() {
  viewport_->resetToolInteraction();
  selectedExtrusionSource_.reset();
  if (toolParametersDock_) toolParametersDock_->hide();
  if (revolveDock_) revolveDock_->hide();
  if (mirrorDock_) mirrorDock_->hide();
  if (moveDock_) moveDock_->hide();
  if (linearPatternDock_) linearPatternDock_->hide();
  if (circularPatternDock_) circularPatternDock_->hide();
  if (extrusionDock_) extrusionDock_->hide();
}

void MainWindow::cancelLegacyExtrusion() {
  // Cancellation is a generation boundary: reject a queued automatic
  // operation detector before clearing any of the state it captured.
  invalidatePreviewUpdates();
  viewport_->resetToolInteraction();
  selectedExtrusionSource_.reset();
  if (extrusionDock_) extrusionDock_->hide();
  if (modelRibbon_) modelRibbon_->clearActiveTool();
}

MainWindow::PreviewSourceSnapshot MainWindow::capturePreviewSource(
    BodyId bodyId, FeatureId featureId) const {
  PreviewSourceSnapshot snapshot;
  snapshot.documentGeneration = previewDocumentGeneration_;
  snapshot.toolRevision = partDesignCoordinator_.revisionToken();
  snapshot.bodyId = bodyId;
  snapshot.featureId = featureId;
  if (bodyId == kInvalidBodyId || featureId == kInvalidFeatureId)
    return snapshot;
  if (const auto* feature = findHistoryFeature(
          static_cast<const Document&>(document_), bodyId, featureId)) {
    snapshot.shapeRevision = feature->shapeRevision();
    snapshot.shape = feature->lastValidShape();
  }
  return snapshot;
}

bool MainWindow::previewSourceIsCurrent(
    const PreviewSourceSnapshot& snapshot) const {
  if (snapshot.documentGeneration != previewDocumentGeneration_) return false;
  if (!partDesignCoordinator_.isCurrent(snapshot.toolRevision)) return false;
  if (snapshot.bodyId == kInvalidBodyId ||
      snapshot.featureId == kInvalidFeatureId)
    return !snapshot.shape &&
           snapshot.shapeRevision == kInvalidShapeRevision;
  const auto* feature = findHistoryFeature(
      static_cast<const Document&>(document_), snapshot.bodyId,
      snapshot.featureId);
  return feature && feature->shapeRevision() == snapshot.shapeRevision &&
         feature->lastValidShape().get() == snapshot.shape.get();
}

void MainWindow::schedulePreviewUpdate(BodyId bodyId, FeatureId featureId,
                                       std::function<void()> rebuild,
                                       std::function<void()> publish) {
  if (!previewUpdates_ || !rebuild || !publish) return;
  const PreviewSourceSnapshot source =
      capturePreviewSource(bodyId, featureId);
  static_cast<void>(previewUpdates_->request(
      [this, source, rebuild = std::move(rebuild),
       publish = std::move(publish)](PreviewUpdateCoordinator::Token token) {
        if (!previewUpdates_ || !previewUpdates_->isCurrent(token) ||
            !previewSourceIsCurrent(source))
          return;
        rebuild();
        // OCCT calls are synchronous, but UI callbacks can be re-entered by
        // modal diagnostics or future progress presentation. Revalidate the
        // document/tool generation and immutable source immediately before
        // publishing the result to the viewport.
        if (!previewUpdates_ || !previewUpdates_->isCurrent(token) ||
            !previewSourceIsCurrent(source))
          return;
        if (!previewUpdates_->claimPublication(token)) return;
        publish();
      }));
}

void MainWindow::flushPreviewUpdate() {
  if (previewUpdates_) previewUpdates_->flush();
  // Continuous panel/manipulator routes perform at most one builder call.
  // Boundary refinement is reserved for an explicit exact boundary (release,
  // Enter or Apply), and a valid value is a zero-build no-op in the session.
  applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveInput(
      document_, toolParametersPanel_->parameterValue(),
      PartDesignParameterSource::Boundary));
}

void MainWindow::invalidatePreviewUpdates() noexcept {
  if (previewUpdates_) previewUpdates_->invalidate();
  ++previewDocumentGeneration_;
  automaticExtrudeDetectionKey_.reset();
}

void MainWindow::undoLastAction() {
  resetTransientModelingUi();
  if (workspaceStack_->currentWidget() == sketchCanvas_) {
    if (sketchCanvas_->canUndo()) {
      sketchCanvas_->undo();
      setWindowModified(true);
    }
  } else {
    const auto result =
        modelHistory_.undo(document_, captureHistorySelection());
    if (result.changed) {
      synchronizeCommittedState(result.state);
    } else if (!result.error.empty()) {
      statusBar()->showMessage(
          QString::fromUtf8("Отмена не применена: %1")
              .arg(QString::fromStdString(result.error)),
          4000);
    }
  }
  updateUndoAvailability();
}

void MainWindow::redoLastAction() {
  resetTransientModelingUi();
  if (workspaceStack_->currentWidget() == sketchCanvas_) {
    if (sketchCanvas_->canRedo()) {
      sketchCanvas_->redo();
      setWindowModified(true);
    }
    updateUndoAvailability();
    return;
  }
  const auto result =
      modelHistory_.redo(document_, captureHistorySelection());
  if (result.changed) {
    synchronizeCommittedState(result.state);
  } else if (!result.error.empty()) {
    statusBar()->showMessage(
        QString::fromUtf8("Повтор не применён: %1")
            .arg(QString::fromStdString(result.error)),
        4000);
  }
  updateUndoAvailability();
}

bool MainWindow::confirmProjectReplacement() {
  if (!isWindowModified()) return true;

  QMessageBox prompt(
      QMessageBox::Warning, QString::fromUtf8("Несохранённые изменения"),
      QString::fromUtf8(
          "Проект был изменён. Сохранить изменения перед продолжением?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
  prompt.setDefaultButton(QMessageBox::Save);
  prompt.button(QMessageBox::Save)->setText(QString::fromUtf8("Сохранить"));
  prompt.button(QMessageBox::Discard)->setText(
      QString::fromUtf8("Не сохранять"));
  prompt.button(QMessageBox::Cancel)->setText(QString::fromUtf8("Отмена"));

  const auto result = static_cast<QMessageBox::StandardButton>(prompt.exec());
  if (result == QMessageBox::Cancel) return false;
  if (result == QMessageBox::Discard) return true;
  saveProject();
  return !isWindowModified();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!confirmProjectReplacement()) {
    event->ignore();
    return;
  }
  resetTransientModelingUi();
  event->accept();
}

void MainWindow::createProject() {
  QString path = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Создать проект"), QStringLiteral("Новый проект.solidar"),
      QString::fromUtf8("Проекты Солидарность CAD (*.solidar)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(QStringLiteral(".solidar"), Qt::CaseInsensitive))
    path += QStringLiteral(".solidar");
  if (!confirmProjectReplacement()) return;
  const auto created = projectApplicationService_.createProject(path);
  if (!created.succeeded()) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка создания"),
                          created.detail);
    return;
  }
  // Use exactly the same document-transition path as Open.  Maintaining a
  // second hand-written reset sequence here is what allowed controller/session
  // state to outlive the Document that owned its B-Rep references.
  QString loadError;
  if (!loadProject(path, &loadError)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка создания"),
                          loadError);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("Создан новый проект"), 3000);
}

void MainWindow::openProject() {
  const QString path = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Открыть проект"), QString(),
      QString::fromUtf8("Проекты Солидарность CAD (*.solidar)"));
  if (path.isEmpty()) return;
  if (!confirmProjectReplacement()) return;
  QString error;
  if (!loadProject(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка открытия"), error);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("Проект открыт"), 3000);
}

bool MainWindow::loadProject(const QString& path, QString* error) {
  auto staged = projectApplicationService_.stageOpen(path);
  if (!staged.succeeded()) {
    if (error) *error = staged.status.detail;
    return false;
  }
  commitDocumentReplacement(std::move(*staged.staged),
                            DocumentReplacementOrigin::OpenProject, path);
  return true;
}

void MainWindow::saveProject() {
  // The Sketcher edits a working copy. Commit it before serializing so Save
  // never reports success while silently leaving the visible sketch out of
  // the project file.
  if (workspaceStack_->currentWidget() == sketchCanvas_) {
    finishSketch();
    if (workspaceStack_->currentWidget() == sketchCanvas_) return;
  }

  QString path = windowFilePath();
  if (path.isEmpty()) {
    path = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("Сохранить проект"), QStringLiteral("Новый проект.solidar"),
        QString::fromUtf8("Проекты Солидарность CAD (*.solidar)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".solidar"), Qt::CaseInsensitive))
      path += QStringLiteral(".solidar");
  }
  const auto saved = projectApplicationService_.save(path, document_);
  if (!saved.succeeded()) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка сохранения"),
                          saved.detail);
    return;
  }
  setProjectPath(path);
  modelHistory_.markSavepoint();
  setWindowModified(false);
  statusBar()->showMessage(QString::fromUtf8("Проект сохранён"), 3000);
}

void MainWindow::importStep() {
  const QString fileName = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Импортировать STEP"), QString(),
      QStringLiteral("STEP files (*.step *.stp)"));
  if (fileName.isEmpty()) return;

  const QString importedName = QFileInfo(fileName).completeBaseName();
  auto staged = projectApplicationService_.stageImportStep(
      fileName, document_, importedName);
  if (!staged.succeeded()) {
    QMessageBox::critical(
        this, QString::fromUtf8("Не удалось импортировать STEP"),
        staged.status.detail);
    return;
  }
  commitDocumentReplacement(std::move(*staged.staged),
                            DocumentReplacementOrigin::ImportedStep,
                            fileName);
}

void MainWindow::applyImportedDocument(Document staged,
                                       const QString& importedName) {
  commitDocumentReplacement({std::move(staged), std::nullopt},
                            DocumentReplacementOrigin::ImportedStep,
                            importedName);
}

void MainWindow::commitDocumentReplacement(
    StagedApplicationDocument staged, DocumentReplacementOrigin origin,
    const QString& pathOrLabel) {
  // This is the sole commit protocol for a completely staged replacement.
  // Invalidate queued work and cancel every topology-owning session while the
  // old Document is alive. Failed staging never enters this method.
  invalidatePreviewUpdates();
  static_cast<void>(partDesignCoordinator_.prepareDocumentReplacement());
  resetTransientModelingPresentation();
  if (modelRibbon_) modelRibbon_->clearActiveTool();

  document_ = std::move(staged.document);
  document_.reserveIdsForEditing();
  historicalLegacyExtrusionSourceSketchId_ =
      staged.historicalLegacyExtrusionSourceSketchId;
  selectedExtrusionSource_.reset();
  currentSketchSupport_ = QStringLiteral("XY");
  currentSketchPlacement_ = SketchPlacement::xy();
  currentSketchFaceReference_.reset();
  sketchViews_.clear();
  viewport_->resetScene();
  viewport_->setBox(document_.box());
  for (const auto& item : document_.sketches()) {
    sketchViews_.push_back({item.id});
    viewport_->addSketch(item.id, item.geometry,
                         sketchPresentationLabel(item), item.placement);
  }
  editingSketchIndex_.reset();
  extrudeOperationManuallyChanged_ = false;
  const bool opened = origin == DocumentReplacementOrigin::OpenProject;
  modelHistory_.clear(opened);
  if (opened) sketchCanvas_->resetSketch();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  if (opened) applyHistoryPosition(historyPosition_);
  refreshBodyViewFromDocument();
  workspaceStack_->setCurrentWidget(viewport_);
  if (opened) {
    setProjectPath(pathOrLabel);
    setWindowModified(false);
  } else {
    viewport_->fitAll();
    setWindowModified(true);
  }
  updateUndoAvailability();
  if (!opened)
    statusBar()->showMessage(
        QString::fromUtf8("STEP импортирован: ") + pathOrLabel, 5000);
}

void MainWindow::exportStep() {
  QString fileName = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Экспортировать STEP"),
      windowFilePath().isEmpty()
          ? QStringLiteral("Модель.step")
          : QFileInfo(windowFilePath()).absolutePath() + QLatin1Char('/') +
                QFileInfo(windowFilePath()).completeBaseName() +
                QStringLiteral(".step"),
      QStringLiteral("STEP files (*.step *.stp)"));
  if (fileName.isEmpty()) return;
  if (!fileName.endsWith(QStringLiteral(".step"), Qt::CaseInsensitive) &&
      !fileName.endsWith(QStringLiteral(".stp"), Qt::CaseInsensitive))
    fileName += QStringLiteral(".step");

  const auto exported = projectApplicationService_.exportStep(fileName,
                                                               document_);
  if (!exported.succeeded()) {
    QMessageBox::critical(
        this, QString::fromUtf8("Не удалось экспортировать STEP"),
        exported.detail);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("STEP сохранён: ") + fileName,
                           5000);
}

void MainWindow::exportStl() {
  const auto preflight =
      projectApplicationService_.preflightStlExport(document_);
  if (!preflight.succeeded()) {
    QMessageBox::information(
        this, QString::fromUtf8("Экспорт STL"),
        preflight.detail.isEmpty()
            ? QString::fromUtf8(
                  "Документ не содержит корректного B-Rep тела для экспорта.")
            : preflight.detail);
    return;
  }
  QString fileName = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Экспортировать STL"),
      windowFilePath().isEmpty()
          ? QStringLiteral("Модель.stl")
          : QFileInfo(windowFilePath()).absolutePath() + QLatin1Char('/') +
                QFileInfo(windowFilePath()).completeBaseName() +
                QStringLiteral(".stl"),
      QStringLiteral("STL (*.stl)"));
  if (fileName.isEmpty()) return;
  if (!fileName.endsWith(QStringLiteral(".stl"), Qt::CaseInsensitive))
    fileName += QStringLiteral(".stl");

  const auto exported = projectApplicationService_.exportStl(fileName,
                                                              document_);
  if (!exported.succeeded()) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка экспорта STL"),
                          exported.detail);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("STL сохранён: ") + fileName, 5000);
}

void MainWindow::setProjectPath(const QString& path) {
  setWindowFilePath(path);
  setWindowTitle(QFileInfo(path).completeBaseName() +
                 QString::fromUtf8(" — Солидарность CAD [*]"));
}

void MainWindow::buildUi() {
  workspaceStack_ = new QStackedWidget(this);
  sketchCanvas_ = new SketchCanvas(workspaceStack_);

  connect(
      sketchCanvas_,
      &SketchCanvas::constraintStatusChanged,
      this,
      [this](const QString& status) {
        statusBar()->showMessage(status);
      });

  viewport_ = new Viewport(workspaceStack_);
  drawingSheet_ = new DrawingSheetView(this);
  drawingSheet_->hide();
  viewport_->setBox(document_.box());
  viewport_->setSketch(sketchCanvas_->sketch(), currentSketchPlacement_);
  drawingSheet_->setRectangle(document_.box().widthMm, document_.box().depthMm);
  workspaceStack_->addWidget(viewport_);
  workspaceStack_->addWidget(sketchCanvas_);
  setCentralWidget(workspaceStack_);

  extrusionDock_ = new QDockWidget(QString::fromUtf8("Выдавливание"), this);
  extrusionDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  extrusionDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* extrusionPanel = new QWidget(extrusionDock_);
  auto* extrusionPanelLayout = new QVBoxLayout(extrusionPanel);
  extrusionPanelLayout->setContentsMargins(16, 14, 16, 14);
  extrusionPanelLayout->setSpacing(12);
  const auto* extrudeHelp = partDesignToolHelp(PartDesignToolKind::Extrude);
  auto* extrusionDescription = new QLabel(extrudeHelp->shortDescription,
                                          extrusionPanel);
  extrusionDescription->setWordWrap(true);
  extrusionDescription->setProperty("uiRole", "secondaryText");
  auto* extrusionHint = new QLabel(QString::fromUtf8(
      "Потяните синюю стрелку в 3D-виде или введите точное значение."),
      extrusionPanel);
  extrusionHint->setWordWrap(true);
  extrusionHint->setProperty("uiRole", "secondaryText");
  auto* extrusionForm = new QFormLayout;
  extrusionLengthSpin_ = new QDoubleSpinBox(extrusionPanel);
  extrusionLengthSpin_->setRange(-100000.0, 100000.0);
  extrusionLengthSpin_->setDecimals(2);
  extrusionLengthSpin_->setSuffix(QStringLiteral(" mm"));
  extrusionLengthSpin_->setValue(document_.box().heightMm);
  extrusionForm->addRow(QString::fromUtf8("Длина:"), extrusionLengthSpin_);
  extrusionOperationCombo_ = new QComboBox(extrusionPanel);
  extrusionOperationCombo_->addItems(
      {QString::fromUtf8("Новое тело"), QString::fromUtf8("Объединить"),
       QString::fromUtf8("Вырезать")});
  extrusionForm->addRow(QString::fromUtf8("Операция:"),
                        extrusionOperationCombo_);
  extrusionReverseCheck_ =
      new QCheckBox(QString::fromUtf8("Обратное направление"), extrusionPanel);
  extrusionForm->addRow(QString::fromUtf8("Направление:"),
                        extrusionReverseCheck_);
  auto* extrusionButtons = new QHBoxLayout;
  auto* cancelExtrusion = new QPushButton(QString::fromUtf8("Отмена"), extrusionPanel);
  auto* acceptExtrusion = new QPushButton(QString::fromUtf8("Применить"), extrusionPanel);
  acceptExtrusion->setDefault(true);
  acceptExtrusion->setProperty("uiRole", "primaryAction");
  acceptExtrusion->setObjectName("primaryAction");
  extrusionButtons->addWidget(cancelExtrusion);
  extrusionButtons->addWidget(acceptExtrusion);
  extrusionPanelLayout->addWidget(extrusionDescription);
  extrusionPanelLayout->addWidget(extrusionHint);
  extrusionPanelLayout->addLayout(extrusionForm);
  extrusionPanelLayout->addStretch();
  extrusionPanelLayout->addLayout(extrusionButtons);
  extrusionDock_->setWidget(extrusionPanel);
  extrusionDock_->setObjectName(QStringLiteral("extrusionParametersDock"));
  extrusionDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, extrusionDock_);
  extrusionDock_->hide();

  revolveDock_ = new QDockWidget(QString::fromUtf8("Вращение"), this);
  revolveDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  revolveDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* revolvePanel = new QWidget(revolveDock_);
  auto* revolveLayout = new QVBoxLayout(revolvePanel);
  revolveLayout->setContentsMargins(16, 14, 16, 14);
  revolveLayout->setSpacing(12);
  const auto* revolveHelp = partDesignToolHelp(PartDesignToolKind::Revolve);
  auto* revolveTitle = new QLabel(revolveHelp->title.toUpper(), revolvePanel);
  QFont revolveTitleFont = revolveTitle->font();
  revolveTitleFont.setBold(true); revolveTitle->setFont(revolveTitleFont);
  auto* revolveDescription = new QLabel(revolveHelp->shortDescription,
                                        revolvePanel);
  revolveDescription->setWordWrap(true);
  revolveDescription->setProperty("uiRole", "secondaryText");
  revolveStepHint_ = new QLabel(revolveHelp->selectionHint, revolvePanel);
  revolveStepHint_->setObjectName("revolveStepHint");
  revolveStepHint_->setWordWrap(true);
  auto* revolveForm = new QFormLayout;
  revolveProfileCombo_ = new QComboBox(revolvePanel);
  revolveProfileSummary_ = new QLabel(
      QString::fromUtf8("Профиль не выбран"), revolvePanel);
  revolveProfileSummary_->setProperty("uiRole", "secondaryText");
  revolveProfileSummary_->setWordWrap(true);
  revolveAxisCombo_ = new QComboBox(revolvePanel);
  revolveAngleSpin_ = new QDoubleSpinBox(revolvePanel);
  revolveAngleSpin_->setRange(0.01, 360.0);
  revolveAngleSpin_->setDecimals(2);
  revolveAngleSpin_->setValue(360.0);
  revolveAngleSpin_->setSuffix(QString::fromUtf8(" °"));
  revolveOperationCombo_ = new QComboBox(revolvePanel);
  revolveOperationCombo_->addItems({QString::fromUtf8("Новое тело"),
                                    QString::fromUtf8("Объединить"),
                                    QString::fromUtf8("Вырезать")});
  revolveReverseCheck_ = new QCheckBox(
      QString::fromUtf8("Обратить направление"), revolvePanel);
  auto* profileRow = new QWidget(revolvePanel);
  auto* profileRowLayout = new QVBoxLayout(profileRow);
  profileRowLayout->setContentsMargins(0, 0, 0, 0);
  profileRowLayout->setSpacing(6);
  auto* profileControls = new QHBoxLayout;
  auto* reselectProfile = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), profileRow);
  profileControls->addWidget(revolveProfileCombo_);
  profileControls->addWidget(reselectProfile);
  profileRowLayout->addLayout(profileControls);
  profileRowLayout->addWidget(revolveProfileSummary_);
  auto* axisRow = new QWidget(revolvePanel);
  auto* axisRowLayout = new QHBoxLayout(axisRow);
  axisRowLayout->setContentsMargins(0, 0, 0, 0);
  auto* reselectAxis = new QPushButton(QString::fromUtf8("Выбрать в 3D"), axisRow);
  axisRowLayout->addWidget(revolveAxisCombo_);
  axisRowLayout->addWidget(reselectAxis);
  revolveForm->addRow(QString::fromUtf8("Профили:"), profileRow);
  revolveForm->addRow(QString::fromUtf8("Ось:"), axisRow);
  revolveForm->addRow(QString::fromUtf8("Угол:"), revolveAngleSpin_);
  revolveForm->addRow(QString::fromUtf8("Операция:"), revolveOperationCombo_);
  revolveForm->addRow(QString(), revolveReverseCheck_);
  auto* revolveButtons = new QHBoxLayout;
  auto* cancelRevolve = new QPushButton(QString::fromUtf8("Отмена"), revolvePanel);
  revolveAcceptButton_ = new QPushButton(QString::fromUtf8("Применить"), revolvePanel);
  revolveAcceptButton_->setEnabled(false);
  revolveAcceptButton_->setDefault(true);
  revolveAcceptButton_->setProperty("uiRole", "primaryAction");
  revolveAcceptButton_->setObjectName("primaryAction");
  revolveButtons->addWidget(cancelRevolve);
  revolveButtons->addWidget(revolveAcceptButton_);
  revolveLayout->addWidget(revolveTitle);
  revolveLayout->addWidget(revolveDescription);
  revolveLayout->addLayout(revolveForm);
  revolveLayout->addWidget(revolveStepHint_);
  revolveLayout->addStretch();
  revolveLayout->addLayout(revolveButtons);
  revolveDock_->setWidget(revolvePanel);
  revolveDock_->setObjectName(QStringLiteral("revolveParametersDock"));
  revolveDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, revolveDock_);
  revolveDock_->hide();
  connect(revolveProfileCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            invalidatePreviewUpdates();
            rebuildRevolveAxisChoices();
            partDesignCoordinator_.clearRevolveAxis(document_);
            const auto id = static_cast<SketchId>(revolveProfileCombo_->currentData().toULongLong());
            if (id == kInvalidSketchId) {
              partDesignCoordinator_.clearRevolveProfile(document_);
              revolveProfileSummary_->setText(
                  QString::fromUtf8("Профиль не выбран"));
              viewport_->beginExtrusionSurfaceSelection();
            } else {
              partDesignCoordinator_.setRevolveProfile(document_, id);
              revolveProfileSummary_->setText(
                  QString::fromUtf8("Выбран весь эскиз"));
              const auto found = std::find_if(
                  sketchViews_.begin(), sketchViews_.end(),
                  [id](const auto& entry) {
                    return entry.documentSketchId == id;
                  });
              if (found != sketchViews_.end())
                viewport_->beginRevolveAxisSelection(static_cast<std::size_t>(
                    std::distance(sketchViews_.begin(), found)));
            }
            updateRevolveToolPreview();
          });
  connect(revolveAxisCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            invalidatePreviewUpdates();
            if (revolveAxisCombo_->currentIndex() <= 0) {
              partDesignCoordinator_.clearRevolveAxis(document_); updateRevolveToolPreview(); return;
            }
            AxisReference reference;
            const qulonglong value = revolveAxisCombo_->currentData().toULongLong();
            if (value == Viewport::kGlobalXAxisToken)
              reference.type = AxisReferenceType::GlobalX;
            else if (value == Viewport::kGlobalYAxisToken)
              reference.type = AxisReferenceType::GlobalY;
            else if (value == Viewport::kGlobalZAxisToken)
              reference.type = AxisReferenceType::GlobalZ;
            else {
              reference.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId;
              if (value == 1)
                reference.type = AxisReferenceType::SketchHorizontalAxis;
              else if (value == 2)
                reference.type = AxisReferenceType::SketchVerticalAxis;
              else {
                reference.type = AxisReferenceType::SketchLine;
                reference.lineId = static_cast<sketch::GeometryId>(value - 3);
              }
            }
            partDesignCoordinator_.setRevolveAxis(document_, reference); updateRevolveToolPreview();
          });
  connect(revolveAngleSpin_, &QDoubleSpinBox::valueChanged, this,
          [this](double value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::Inactive)
              return;
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).sourceFeatureId,
                [this, value, effect] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle !=
                      ToolLifecycle::Inactive)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        document_, value, PartDesignParameterSource::Panel);
                },
                [this, effect] { applyPartDesignUiEffect(std::move(*effect)); });
          });
  connect(revolveOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::Inactive)
              return;
            const auto operation = static_cast<ExtrudeOperation>(value);
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).sourceFeatureId,
                [this, operation] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setRevolveOperation(document_, operation);
                },
                [this] { updateRevolveToolPreview(); });
          });
  connect(revolveReverseCheck_, &QCheckBox::toggled, this,
          [this](bool value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setRevolveReversed(document_, value);
                },
                [this] { updateRevolveToolPreview(); });
          });
  connect(revolveAcceptButton_, &QPushButton::clicked, this,
          [this] { applyActivePartDesignTool(); });
  connect(cancelRevolve, &QPushButton::clicked, this,
          &MainWindow::cancelRevolveTool);
  const auto acceptRevolveOnEnter = [this] {
    if (!revolveDock_->isVisible()) return;
    revolveAngleSpin_->interpretText();
    flushPreviewUpdate();
    if (revolveAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  };
  auto* revolveReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), revolveDock_);
  revolveReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  revolveReturnShortcut->setAutoRepeat(false);
  connect(revolveReturnShortcut, &QShortcut::activated, this,
          acceptRevolveOnEnter);
  auto* revolveKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), revolveDock_);
  revolveKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  revolveKeypadShortcut->setAutoRepeat(false);
  connect(revolveKeypadShortcut, &QShortcut::activated, this,
          acceptRevolveOnEnter);
  auto* revolveEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), revolveDock_);
  revolveEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  revolveEscapeShortcut->setAutoRepeat(false);
  connect(revolveEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelRevolveTool);
  connect(reselectProfile, &QPushButton::clicked, this, [this] {
    invalidatePreviewUpdates();
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    partDesignCoordinator_.clearRevolveAxis(document_);
    partDesignCoordinator_.clearRevolveProfile(document_);
    viewport_->clearToolManipulator();
    {
      const QSignalBlocker blocker(revolveProfileCombo_);
      revolveProfileCombo_->setCurrentIndex(0);
    }
    revolveProfileSummary_->setText(QString::fromUtf8(
        "Щёлкните область в 3D-виде. Ctrl добавляет или убирает области."));
    viewport_->beginExtrusionSurfaceSelection();
    viewport_->setFocus();
    statusBar()->showMessage(QString::fromUtf8("1/3 Выберите новый профиль"));
  });
  connect(reselectAxis, &QPushButton::clicked, this, [this] {
    invalidatePreviewUpdates();
    std::size_t sketchIndex = static_cast<std::size_t>(-1);
    const auto found = std::find_if(
        sketchViews_.begin(), sketchViews_.end(), [this](const auto& entry) {
          return entry.documentSketchId == partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId;
        });
    if (found != sketchViews_.end())
      sketchIndex = static_cast<std::size_t>(
          std::distance(sketchViews_.begin(), found));
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingReference);
    partDesignCoordinator_.clearRevolveAxis(document_);
    viewport_->clearToolManipulator();
    updateRevolveToolPreview();
    viewport_->beginRevolveAxisSelection(sketchIndex);
    viewport_->setFocus();
    statusBar()->showMessage(
        sketchIndex < sketchViews_.size()
            ? QString::fromUtf8("2/3 Выберите глобальную ось или линию эскиза")
            : QString::fromUtf8("Выберите глобальную ось; линии эскиза станут доступны после выбора профиля"));
  });
  connect(viewport_, &Viewport::angularToolManipulatorValueChanged, this,
          [this](double angle) {
            if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Revolve) {
              const QSignalBlocker blocker(revolveAngleSpin_);
              revolveAngleSpin_->setValue(angle);
              auto effect = std::make_shared<PartDesignUiEffect>();
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).sourceFeatureId,
                  [this, angle, effect] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::Revolve)
                      *effect = partDesignCoordinator_.dispatchActiveInput(
                          document_, angle,
                          PartDesignParameterSource::Manipulator);
                  },
                  [this, effect] {
                    applyPartDesignUiEffect(std::move(*effect));
                  });
            } else if (partDesignCoordinator_.activeTool() ==
                       PartDesignToolKind::CircularPattern) {
              const QSignalBlocker blocker(circularPatternAngleSpin_);
              circularPatternAngleSpin_->setValue(angle);
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                  [this, angle] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::CircularPattern)
                      partDesignCoordinator_.setCircularPatternAngle(angle);
                  },
                  [this] { updateCircularPatternToolPreview(); });
            }
          });
  connect(viewport_, &Viewport::revolveProfileSelectionChanged, this,
          &MainWindow::updateRevolveProfileSelection);

  mirrorDock_ = new QDockWidget(QString::fromUtf8("Зеркало"), this);
  mirrorDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  mirrorDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  mirrorDock_->setObjectName(QStringLiteral("mirrorParametersDock"));
  auto* mirrorPanel = new QWidget(mirrorDock_);
  auto* mirrorLayout = new QVBoxLayout(mirrorPanel);
  mirrorLayout->setContentsMargins(16, 14, 16, 14);
  mirrorLayout->setSpacing(12);
  const auto* mirrorHelp = partDesignToolHelp(PartDesignToolKind::Mirror);
  auto* mirrorTitle = new QLabel(mirrorHelp->title.toUpper(), mirrorPanel);
  QFont mirrorTitleFont = mirrorTitle->font();
  mirrorTitleFont.setBold(true);
  mirrorTitle->setFont(mirrorTitleFont);
  auto* mirrorDescription = new QLabel(mirrorHelp->shortDescription,
                                       mirrorPanel);
  mirrorDescription->setWordWrap(true);
  mirrorDescription->setProperty("uiRole", "secondaryText");
  auto* mirrorForm = new QFormLayout;

  auto* mirrorBodyRow = new QWidget(mirrorPanel);
  auto* mirrorBodyLayout = new QHBoxLayout(mirrorBodyRow);
  mirrorBodyLayout->setContentsMargins(0, 0, 0, 0);
  mirrorBodyValue_ = new QLabel(QString::fromUtf8("Не выбрано"), mirrorBodyRow);
  mirrorBodyValue_->setObjectName(QStringLiteral("mirrorBodyValue"));
  mirrorBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), mirrorBodyRow);
  mirrorBodySelectButton_->setObjectName(
      QStringLiteral("mirrorBodySelectButton"));
  mirrorBodyLayout->addWidget(mirrorBodyValue_, 1);
  mirrorBodyLayout->addWidget(mirrorBodySelectButton_);

  auto* mirrorPlaneRow = new QWidget(mirrorPanel);
  auto* mirrorPlaneLayout = new QHBoxLayout(mirrorPlaneRow);
  mirrorPlaneLayout->setContentsMargins(0, 0, 0, 0);
  mirrorPlaneValue_ = new QLabel(QString::fromUtf8("Не выбрана"),
                                 mirrorPlaneRow);
  mirrorPlaneValue_->setObjectName(QStringLiteral("mirrorPlaneValue"));
  mirrorPlaneSelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), mirrorPlaneRow);
  mirrorPlaneSelectButton_->setObjectName(
      QStringLiteral("mirrorPlaneSelectButton"));
  mirrorPlaneSelectButton_->setEnabled(false);
  mirrorPlaneLayout->addWidget(mirrorPlaneValue_, 1);
  mirrorPlaneLayout->addWidget(mirrorPlaneSelectButton_);
  mirrorForm->addRow(QString::fromUtf8("Тело:"), mirrorBodyRow);
  mirrorForm->addRow(QString::fromUtf8("Плоскость:"), mirrorPlaneRow);

  mirrorStepHint_ = new QLabel(mirrorHelp->selectionHint, mirrorPanel);
  mirrorStepHint_->setObjectName(QStringLiteral("mirrorStepHint"));
  mirrorStepHint_->setWordWrap(true);
  mirrorStepHint_->setProperty("uiRole", "secondaryText");
  auto* mirrorButtons = new QHBoxLayout;
  auto* cancelMirror = new QPushButton(QString::fromUtf8("Отмена"), mirrorPanel);
  mirrorAcceptButton_ = new QPushButton(QString::fromUtf8("Применить"),
                                        mirrorPanel);
  mirrorAcceptButton_->setObjectName(QStringLiteral("primaryAction"));
  mirrorAcceptButton_->setProperty("uiRole", "primaryAction");
  mirrorAcceptButton_->setDefault(true);
  mirrorAcceptButton_->setEnabled(false);
  mirrorButtons->addWidget(cancelMirror);
  mirrorButtons->addWidget(mirrorAcceptButton_);
  mirrorLayout->addWidget(mirrorTitle);
  mirrorLayout->addWidget(mirrorDescription);
  mirrorLayout->addLayout(mirrorForm);
  mirrorLayout->addWidget(mirrorStepHint_);
  mirrorLayout->addStretch();
  mirrorLayout->addLayout(mirrorButtons);
  mirrorDock_->setWidget(mirrorPanel);
  mirrorDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, mirrorDock_);
  mirrorDock_->hide();

  connect(mirrorBodySelectButton_, &QPushButton::clicked, this, [this] {
    invalidatePreviewUpdates();
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    partDesignCoordinator_.clearMirrorBody();
    mirrorBodyValue_->setText(QString::fromUtf8("Не выбрано"));
    mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
    mirrorPlaneSelectButton_->setEnabled(false);
    viewport_->clearToolPreviewShape();
    viewport_->beginMirrorBodySelection();
    updateMirrorToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
  });
  connect(mirrorPlaneSelectButton_, &QPushButton::clicked, this, [this] {
    invalidatePreviewUpdates();
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).bodyId == kInvalidBodyId) return;
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingReference);
    partDesignCoordinator_.clearMirrorPlane();
    mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
    viewport_->clearToolPreviewShape();
    viewport_->beginMirrorPlaneSelection();
    updateMirrorToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(
        QString::fromUtf8("2/2 Выберите базовую плоскость в 3D-виде"));
  });
  connect(mirrorAcceptButton_, &QPushButton::clicked, this,
          [this] { applyActivePartDesignTool(); });
  connect(cancelMirror, &QPushButton::clicked, this,
          &MainWindow::cancelMirrorTool);
  auto* mirrorReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), mirrorDock_);
  mirrorReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorReturnShortcut->setAutoRepeat(false);
  connect(mirrorReturnShortcut, &QShortcut::activated, this, [this] {
    if (mirrorDock_->isVisible() && mirrorAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* mirrorKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), mirrorDock_);
  mirrorKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorKeypadShortcut->setAutoRepeat(false);
  connect(mirrorKeypadShortcut, &QShortcut::activated, this, [this] {
    if (mirrorDock_->isVisible() && mirrorAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* mirrorEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), mirrorDock_);
  mirrorEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorEscapeShortcut->setAutoRepeat(false);
  connect(mirrorEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelMirrorTool);

  moveDock_ = new QDockWidget(QString::fromUtf8("Перемещение"), this);
  moveDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  moveDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  moveDock_->setObjectName(QStringLiteral("moveParametersDock"));
  auto* movePanel = new QWidget(moveDock_);
  auto* moveLayout = new QVBoxLayout(movePanel);
  moveLayout->setContentsMargins(16, 14, 16, 14);
  moveLayout->setSpacing(12);
  const auto* moveHelp = partDesignToolHelp(PartDesignToolKind::Move);
  auto* moveTitle = new QLabel(moveHelp->title.toUpper(), movePanel);
  QFont moveTitleFont = moveTitle->font();
  moveTitleFont.setBold(true);
  moveTitle->setFont(moveTitleFont);
  auto* moveDescription = new QLabel(moveHelp->shortDescription, movePanel);
  moveDescription->setWordWrap(true);
  moveDescription->setProperty("uiRole", "secondaryText");
  auto* moveForm = new QFormLayout;

  auto* moveBodyRow = new QWidget(movePanel);
  auto* moveBodyLayout = new QHBoxLayout(moveBodyRow);
  moveBodyLayout->setContentsMargins(0, 0, 0, 0);
  moveBodyValue_ = new QLabel(QString::fromUtf8("Не выбрано"), moveBodyRow);
  moveBodyValue_->setObjectName(QStringLiteral("moveBodyValue"));
  moveBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), moveBodyRow);
  moveBodySelectButton_->setObjectName(QStringLiteral("moveBodySelectButton"));
  moveBodyLayout->addWidget(moveBodyValue_, 1);
  moveBodyLayout->addWidget(moveBodySelectButton_);

  const auto makeMoveSpin = [movePanel](const char* objectName) {
    auto* spin = new QDoubleSpinBox(movePanel);
    spin->setObjectName(QLatin1String(objectName));
    spin->setRange(-100000.0, 100000.0);
    spin->setDecimals(2);
    spin->setSingleStep(0.1);
    spin->setSuffix(QStringLiteral(" mm"));
    return spin;
  };
  moveXSpin_ = makeMoveSpin("moveXSpin");
  moveYSpin_ = makeMoveSpin("moveYSpin");
  moveZSpin_ = makeMoveSpin("moveZSpin");
  moveForm->addRow(QString::fromUtf8("Тело:"), moveBodyRow);
  moveForm->addRow(QStringLiteral("X:"), moveXSpin_);
  moveForm->addRow(QStringLiteral("Y:"), moveYSpin_);
  moveForm->addRow(QStringLiteral("Z:"), moveZSpin_);

  moveStepHint_ = new QLabel(moveHelp->selectionHint, movePanel);
  moveStepHint_->setObjectName(QStringLiteral("moveStepHint"));
  moveStepHint_->setWordWrap(true);
  moveStepHint_->setProperty("uiRole", "secondaryText");
  auto* moveButtons = new QHBoxLayout;
  auto* cancelMove = new QPushButton(QString::fromUtf8("Отмена"), movePanel);
  moveAcceptButton_ = new QPushButton(QString::fromUtf8("Применить"), movePanel);
  moveAcceptButton_->setObjectName(QStringLiteral("primaryAction"));
  moveAcceptButton_->setProperty("uiRole", "primaryAction");
  moveAcceptButton_->setDefault(true);
  moveAcceptButton_->setEnabled(false);
  moveButtons->addWidget(cancelMove);
  moveButtons->addWidget(moveAcceptButton_);
  moveLayout->addWidget(moveTitle);
  moveLayout->addWidget(moveDescription);
  moveLayout->addLayout(moveForm);
  moveLayout->addWidget(moveStepHint_);
  moveLayout->addStretch();
  moveLayout->addLayout(moveButtons);
  moveDock_->setWidget(movePanel);
  moveDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, moveDock_);
  moveDock_->hide();

  connect(moveBodySelectButton_, &QPushButton::clicked, this, [this] {
    invalidatePreviewUpdates();
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    partDesignCoordinator_.clearMoveBody();
    moveBodyValue_->setText(QString::fromUtf8("Не выбрано"));
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    viewport_->beginMoveBodySelection();
    updateMoveToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(QString::fromUtf8("Выберите тело в 3D-виде"));
  });
  const auto moveValueChanged = [this](double) {
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive) return;
    const Vector3d offset{moveXSpin_->value(), moveYSpin_->value(),
                          moveZSpin_->value()};
    schedulePreviewUpdate(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId,
        [this, offset] {
          if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::Inactive)
            partDesignCoordinator_.setMoveOffset(offset);
        },
        [this] { updateMoveToolPreview(); });
  };
  connect(moveXSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveYSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveZSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveAcceptButton_, &QPushButton::clicked, this,
          [this] { applyActivePartDesignTool(); });
  connect(cancelMove, &QPushButton::clicked, this,
          &MainWindow::cancelMoveTool);
  const auto acceptMoveOnEnter = [this] {
    if (!moveDock_->isVisible()) return;
    moveXSpin_->interpretText();
    moveYSpin_->interpretText();
    moveZSpin_->interpretText();
    flushPreviewUpdate();
    if (moveAcceptButton_->isEnabled()) applyActivePartDesignTool();
  };
  auto* moveReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), moveDock_);
  moveReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  moveReturnShortcut->setAutoRepeat(false);
  connect(moveReturnShortcut, &QShortcut::activated, this,
          acceptMoveOnEnter);
  auto* moveKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), moveDock_);
  moveKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  moveKeypadShortcut->setAutoRepeat(false);
  connect(moveKeypadShortcut, &QShortcut::activated, this,
          acceptMoveOnEnter);
  auto* moveEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), moveDock_);
  moveEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  moveEscapeShortcut->setAutoRepeat(false);
  connect(moveEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelMoveTool);

  linearPatternDock_ =
      new QDockWidget(QString::fromUtf8("Линейный массив"), this);
  linearPatternDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  linearPatternDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  linearPatternDock_->setObjectName(
      QStringLiteral("linearPatternParametersDock"));
  auto* linearPatternPanel = new QWidget(linearPatternDock_);
  auto* linearPatternLayout = new QVBoxLayout(linearPatternPanel);
  linearPatternLayout->setContentsMargins(16, 14, 16, 14);
  linearPatternLayout->setSpacing(12);
  const auto* linearPatternHelp =
      partDesignToolHelp(PartDesignToolKind::LinearPattern);
  auto* linearPatternTitle =
      new QLabel(linearPatternHelp->title.toUpper(), linearPatternPanel);
  QFont linearPatternTitleFont = linearPatternTitle->font();
  linearPatternTitleFont.setBold(true);
  linearPatternTitle->setFont(linearPatternTitleFont);
  auto* linearPatternDescription =
      new QLabel(linearPatternHelp->shortDescription, linearPatternPanel);
  linearPatternDescription->setWordWrap(true);
  linearPatternDescription->setProperty("uiRole", "secondaryText");
  auto* linearPatternForm = new QFormLayout;

  auto* linearPatternBodyRow = new QWidget(linearPatternPanel);
  auto* linearPatternBodyLayout = new QHBoxLayout(linearPatternBodyRow);
  linearPatternBodyLayout->setContentsMargins(0, 0, 0, 0);
  linearPatternBodyValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), linearPatternBodyRow);
  linearPatternBodyValue_->setObjectName(
      QStringLiteral("linearPatternBodyValue"));
  linearPatternBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), linearPatternBodyRow);
  linearPatternBodySelectButton_->setObjectName(
      QStringLiteral("linearPatternBodySelectButton"));
  linearPatternBodyLayout->addWidget(linearPatternBodyValue_, 1);
  linearPatternBodyLayout->addWidget(linearPatternBodySelectButton_);

  auto* linearPatternAxisRow = new QWidget(linearPatternPanel);
  auto* linearPatternAxisLayout = new QHBoxLayout(linearPatternAxisRow);
  linearPatternAxisLayout->setContentsMargins(0, 0, 0, 0);
  linearPatternAxisValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), linearPatternAxisRow);
  linearPatternAxisValue_->setObjectName(
      QStringLiteral("linearPatternAxisValue"));
  linearPatternAxisSelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), linearPatternAxisRow);
  linearPatternAxisSelectButton_->setObjectName(
      QStringLiteral("linearPatternAxisSelectButton"));
  linearPatternAxisSelectButton_->setEnabled(false);
  linearPatternAxisLayout->addWidget(linearPatternAxisValue_, 1);
  linearPatternAxisLayout->addWidget(linearPatternAxisSelectButton_);

  linearPatternSpacingSpin_ = new QDoubleSpinBox(linearPatternPanel);
  linearPatternSpacingSpin_->setObjectName(
      QStringLiteral("linearPatternSpacingSpin"));
  linearPatternSpacingSpin_->setRange(kMinimumPatternParameter,
                                      kMaximumPatternSpacingMm);
  linearPatternSpacingSpin_->setDecimals(2);
  linearPatternSpacingSpin_->setSingleStep(0.1);
  linearPatternSpacingSpin_->setSuffix(QStringLiteral(" mm"));
  linearPatternSpacingSpin_->setValue(30.0);
  linearPatternCountSpin_ = new QSpinBox(linearPatternPanel);
  linearPatternCountSpin_->setObjectName(
      QStringLiteral("linearPatternCountSpin"));
  linearPatternCountSpin_->setRange(kMinimumPatternCount,
                                    kMaximumPatternCount);
  linearPatternCountSpin_->setValue(3);
  linearPatternOperationCombo_ = new QComboBox(linearPatternPanel);
  linearPatternOperationCombo_->setObjectName(
      QStringLiteral("linearPatternOperationCombo"));
  linearPatternOperationCombo_->addItems(
      {QString::fromUtf8("Новое тело"), QString::fromUtf8("Добавить")});
  linearPatternForm->addRow(QString::fromUtf8("Тело:"), linearPatternBodyRow);
  linearPatternForm->addRow(QString::fromUtf8("Направление:"),
                            linearPatternAxisRow);
  linearPatternForm->addRow(QString::fromUtf8("Шаг:"),
                            linearPatternSpacingSpin_);
  linearPatternForm->addRow(QString::fromUtf8("Количество:"),
                            linearPatternCountSpin_);
  linearPatternForm->addRow(QString::fromUtf8("Операция:"),
                            linearPatternOperationCombo_);

  linearPatternStepHint_ =
      new QLabel(linearPatternHelp->selectionHint, linearPatternPanel);
  linearPatternStepHint_->setObjectName(
      QStringLiteral("linearPatternStepHint"));
  linearPatternStepHint_->setWordWrap(true);
  linearPatternStepHint_->setProperty("uiRole", "secondaryText");
  auto* linearPatternButtons = new QHBoxLayout;
  auto* cancelLinearPattern =
      new QPushButton(QString::fromUtf8("Отмена"), linearPatternPanel);
  linearPatternAcceptButton_ =
      new QPushButton(QString::fromUtf8("Применить"), linearPatternPanel);
  linearPatternAcceptButton_->setObjectName(QStringLiteral("primaryAction"));
  linearPatternAcceptButton_->setProperty("uiRole", "primaryAction");
  linearPatternAcceptButton_->setDefault(true);
  linearPatternAcceptButton_->setEnabled(false);
  linearPatternButtons->addWidget(cancelLinearPattern);
  linearPatternButtons->addWidget(linearPatternAcceptButton_);
  linearPatternLayout->addWidget(linearPatternTitle);
  linearPatternLayout->addWidget(linearPatternDescription);
  linearPatternLayout->addLayout(linearPatternForm);
  linearPatternLayout->addWidget(linearPatternStepHint_);
  linearPatternLayout->addStretch();
  linearPatternLayout->addLayout(linearPatternButtons);
  linearPatternDock_->setWidget(linearPatternPanel);
  linearPatternDock_->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, linearPatternDock_);
  linearPatternDock_->hide();

  connect(linearPatternBodySelectButton_, &QPushButton::clicked, this,
          [this] {
            invalidatePreviewUpdates();
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingInput);
            partDesignCoordinator_.clearLinearPatternBody();
            linearPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
            linearPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            linearPatternAxisSelectButton_->setEnabled(false);
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginLinearPatternBodySelection();
            updateLinearPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
          });
  connect(linearPatternAxisSelectButton_, &QPushButton::clicked, this,
          [this] {
            invalidatePreviewUpdates();
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId == kInvalidBodyId) return;
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingReference);
            partDesignCoordinator_.clearLinearPatternDirection();
            linearPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginLinearPatternAxisSelection();
            updateLinearPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(linearPatternSpacingSpin_,
          qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setLinearPatternSpacing(value);
                },
                [this] { updateLinearPatternToolPreview(); });
          });
  connect(linearPatternCountSpin_, qOverload<int>(&QSpinBox::valueChanged),
          this, [this](int value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setLinearPatternCount(value);
                },
                [this] { updateLinearPatternToolPreview(); });
          });
  connect(linearPatternOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int index) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            const auto operation = index == 0 ? PatternOperation::NewBody
                                              : PatternOperation::Join;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                [this, operation] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setLinearPatternOperation(operation);
                },
                [this] { updateLinearPatternToolPreview(); });
          });
  connect(linearPatternAcceptButton_, &QPushButton::clicked, this,
          [this] { applyActivePartDesignTool(); });
  connect(cancelLinearPattern, &QPushButton::clicked, this,
          &MainWindow::cancelLinearPatternTool);
  auto* linearPatternReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), linearPatternDock_);
  linearPatternReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternReturnShortcut->setAutoRepeat(false);
  connect(linearPatternReturnShortcut, &QShortcut::activated, this, [this] {
    flushPreviewUpdate();
    if (linearPatternDock_->isVisible() &&
        linearPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* linearPatternKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), linearPatternDock_);
  linearPatternKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternKeypadShortcut->setAutoRepeat(false);
  connect(linearPatternKeypadShortcut, &QShortcut::activated, this, [this] {
    flushPreviewUpdate();
    if (linearPatternDock_->isVisible() &&
        linearPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* linearPatternEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), linearPatternDock_);
  linearPatternEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternEscapeShortcut->setAutoRepeat(false);
  connect(linearPatternEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelLinearPatternTool);

  circularPatternDock_ =
      new QDockWidget(QString::fromUtf8("Круговой массив"), this);
  circularPatternDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  circularPatternDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  circularPatternDock_->setObjectName(
      QStringLiteral("circularPatternParametersDock"));
  auto* circularPatternPanel = new QWidget(circularPatternDock_);
  auto* circularPatternLayout = new QVBoxLayout(circularPatternPanel);
  circularPatternLayout->setContentsMargins(16, 14, 16, 14);
  circularPatternLayout->setSpacing(12);
  const auto* circularPatternHelp =
      partDesignToolHelp(PartDesignToolKind::CircularPattern);
  auto* circularPatternTitle =
      new QLabel(circularPatternHelp->title.toUpper(), circularPatternPanel);
  QFont circularPatternTitleFont = circularPatternTitle->font();
  circularPatternTitleFont.setBold(true);
  circularPatternTitle->setFont(circularPatternTitleFont);
  auto* circularPatternDescription =
      new QLabel(circularPatternHelp->shortDescription, circularPatternPanel);
  circularPatternDescription->setWordWrap(true);
  circularPatternDescription->setProperty("uiRole", "secondaryText");
  auto* circularPatternForm = new QFormLayout;

  auto* circularPatternBodyRow = new QWidget(circularPatternPanel);
  auto* circularPatternBodyLayout = new QHBoxLayout(circularPatternBodyRow);
  circularPatternBodyLayout->setContentsMargins(0, 0, 0, 0);
  circularPatternBodyValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), circularPatternBodyRow);
  circularPatternBodyValue_->setObjectName(
      QStringLiteral("circularPatternBodyValue"));
  circularPatternBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), circularPatternBodyRow);
  circularPatternBodySelectButton_->setObjectName(
      QStringLiteral("circularPatternBodySelectButton"));
  circularPatternBodyLayout->addWidget(circularPatternBodyValue_, 1);
  circularPatternBodyLayout->addWidget(circularPatternBodySelectButton_);

  auto* circularPatternAxisRow = new QWidget(circularPatternPanel);
  auto* circularPatternAxisLayout = new QHBoxLayout(circularPatternAxisRow);
  circularPatternAxisLayout->setContentsMargins(0, 0, 0, 0);
  circularPatternAxisValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), circularPatternAxisRow);
  circularPatternAxisValue_->setObjectName(
      QStringLiteral("circularPatternAxisValue"));
  circularPatternAxisSelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), circularPatternAxisRow);
  circularPatternAxisSelectButton_->setObjectName(
      QStringLiteral("circularPatternAxisSelectButton"));
  circularPatternAxisSelectButton_->setEnabled(false);
  circularPatternAxisLayout->addWidget(circularPatternAxisValue_, 1);
  circularPatternAxisLayout->addWidget(circularPatternAxisSelectButton_);

  circularPatternAngleSpin_ = new QDoubleSpinBox(circularPatternPanel);
  circularPatternAngleSpin_->setObjectName(
      QStringLiteral("circularPatternAngleSpin"));
  circularPatternAngleSpin_->setRange(kMinimumPatternParameter,
                                      kMaximumPatternAngleDeg);
  circularPatternAngleSpin_->setDecimals(2);
  circularPatternAngleSpin_->setSingleStep(1.0);
  circularPatternAngleSpin_->setSuffix(QString::fromUtf8(" °"));
  circularPatternAngleSpin_->setValue(kMaximumPatternAngleDeg);
  circularPatternCountSpin_ = new QSpinBox(circularPatternPanel);
  circularPatternCountSpin_->setObjectName(
      QStringLiteral("circularPatternCountSpin"));
  circularPatternCountSpin_->setRange(kMinimumPatternCount,
                                      kMaximumPatternCount);
  circularPatternCountSpin_->setValue(4);
  circularPatternOperationCombo_ = new QComboBox(circularPatternPanel);
  circularPatternOperationCombo_->setObjectName(
      QStringLiteral("circularPatternOperationCombo"));
  circularPatternOperationCombo_->addItems(
      {QString::fromUtf8("Новое тело"), QString::fromUtf8("Добавить")});
  circularPatternForm->addRow(QString::fromUtf8("Тело:"),
                              circularPatternBodyRow);
  circularPatternForm->addRow(QString::fromUtf8("Ось:"),
                              circularPatternAxisRow);
  circularPatternForm->addRow(QString::fromUtf8("Угол:"),
                              circularPatternAngleSpin_);
  circularPatternForm->addRow(QString::fromUtf8("Количество:"),
                              circularPatternCountSpin_);
  circularPatternForm->addRow(QString::fromUtf8("Операция:"),
                              circularPatternOperationCombo_);

  circularPatternStepHint_ =
      new QLabel(circularPatternHelp->selectionHint, circularPatternPanel);
  circularPatternStepHint_->setObjectName(
      QStringLiteral("circularPatternStepHint"));
  circularPatternStepHint_->setWordWrap(true);
  circularPatternStepHint_->setProperty("uiRole", "secondaryText");
  auto* circularPatternButtons = new QHBoxLayout;
  auto* cancelCircularPattern =
      new QPushButton(QString::fromUtf8("Отмена"), circularPatternPanel);
  circularPatternAcceptButton_ =
      new QPushButton(QString::fromUtf8("Применить"), circularPatternPanel);
  circularPatternAcceptButton_->setObjectName(QStringLiteral("primaryAction"));
  circularPatternAcceptButton_->setProperty("uiRole", "primaryAction");
  circularPatternAcceptButton_->setDefault(true);
  circularPatternAcceptButton_->setEnabled(false);
  circularPatternButtons->addWidget(cancelCircularPattern);
  circularPatternButtons->addWidget(circularPatternAcceptButton_);
  circularPatternLayout->addWidget(circularPatternTitle);
  circularPatternLayout->addWidget(circularPatternDescription);
  circularPatternLayout->addLayout(circularPatternForm);
  circularPatternLayout->addWidget(circularPatternStepHint_);
  circularPatternLayout->addStretch();
  circularPatternLayout->addLayout(circularPatternButtons);
  circularPatternDock_->setWidget(circularPatternPanel);
  circularPatternDock_->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, circularPatternDock_);
  circularPatternDock_->hide();

  connect(circularPatternBodySelectButton_, &QPushButton::clicked, this,
          [this] {
            invalidatePreviewUpdates();
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingInput);
            partDesignCoordinator_.clearCircularPatternBody();
            circularPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
            circularPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            circularPatternAxisSelectButton_->setEnabled(false);
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginCircularPatternBodySelection();
            updateCircularPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
          });
  connect(circularPatternAxisSelectButton_, &QPushButton::clicked, this,
          [this] {
            invalidatePreviewUpdates();
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId == kInvalidBodyId) return;
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingReference);
            partDesignCoordinator_.clearCircularPatternAxis();
            circularPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginCircularPatternAxisSelection();
            updateCircularPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(circularPatternAngleSpin_,
          qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setCircularPatternAngle(value);
                },
                [this] { updateCircularPatternToolPreview(); });
          });
  connect(circularPatternCountSpin_, qOverload<int>(&QSpinBox::valueChanged),
          this, [this](int value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setCircularPatternCount(value);
                },
                [this] { updateCircularPatternToolPreview(); });
          });
  connect(circularPatternOperationCombo_, &QComboBox::currentIndexChanged,
          this, [this](int index) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            const auto operation = index == 0 ? PatternOperation::NewBody
                                              : PatternOperation::Join;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                [this, operation] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setCircularPatternOperation(operation);
                },
                [this] { updateCircularPatternToolPreview(); });
          });
  connect(circularPatternAcceptButton_, &QPushButton::clicked, this,
          [this] { applyActivePartDesignTool(); });
  connect(cancelCircularPattern, &QPushButton::clicked, this,
          &MainWindow::cancelCircularPatternTool);
  auto* circularPatternReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), circularPatternDock_);
  circularPatternReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternReturnShortcut->setAutoRepeat(false);
  connect(circularPatternReturnShortcut, &QShortcut::activated, this, [this] {
    flushPreviewUpdate();
    if (circularPatternDock_->isVisible() &&
        circularPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* circularPatternKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), circularPatternDock_);
  circularPatternKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternKeypadShortcut->setAutoRepeat(false);
  connect(circularPatternKeypadShortcut, &QShortcut::activated, this, [this] {
    flushPreviewUpdate();
    if (circularPatternDock_->isVisible() &&
        circularPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* circularPatternEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), circularPatternDock_);
  circularPatternEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternEscapeShortcut->setAutoRepeat(false);
  connect(circularPatternEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelCircularPatternTool);

  toolParametersDock_ = new QDockWidget(QString::fromUtf8("Параметры инструмента"), this);
  toolParametersDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  toolParametersDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  toolParametersPanel_ = new ToolParametersPanel(toolParametersDock_);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Fillet),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Радиус"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.0, 100000.0, 2);
  toolParametersDock_->setWidget(toolParametersPanel_);
  toolParametersDock_->setObjectName(QStringLiteral("partDesignParametersDock"));
  toolParametersDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, toolParametersDock_);
  toolParametersDock_->hide();
  connect(toolParametersPanel_, &ToolParametersPanel::parameterChanged, this,
          [this](double value) {
            const auto kind = partDesignCoordinator_.activeTool();
            if (kind != PartDesignToolKind::Chamfer &&
                kind != PartDesignToolKind::Shell &&
                kind != PartDesignToolKind::Draft &&
                kind != PartDesignToolKind::Extrude &&
                kind != PartDesignToolKind::Fillet)
              return;
            const auto state = partDesignCoordinator_.snapshot(kind, document_);
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                state.bodyId, state.sourceFeatureId,
                [this, kind, value, effect] {
                  if (partDesignCoordinator_.activeTool() == kind)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        document_, value, PartDesignParameterSource::Panel);
                },
                [this, effect] {
                  applyPartDesignUiEffect(std::move(*effect));
                });
          });
  connect(toolParametersPanel_, &ToolParametersPanel::accepted, this,
          [this] { applyActivePartDesignTool(); });
  connect(toolParametersPanel_, &ToolParametersPanel::cancelled, this,
          [this] {
            applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::Cancel));
          });
  connect(toolParametersPanel_, &ToolParametersPanel::selectionRequested, this,
          [this] {
            const auto kind = partDesignCoordinator_.activeTool();
            if (kind == PartDesignToolKind::None) return;
            invalidatePreviewUpdates();
            if (kind == PartDesignToolKind::JoinBodies)
              viewport_->beginJoinBodiesSelection();
            else if (kind == PartDesignToolKind::Draft) {
              partDesignCoordinator_.beginReselection(
                  ToolSelectionStage::SelectingInput);
              partDesignCoordinator_.setDraftFaces(document_, {});
              partDesignCoordinator_.clearDraftPrincipalAxis(document_);
              viewport_->clearToolManipulator();
              viewport_->beginDraftFaceSelection();
              updateDraftToolPreview();
            }
            statusBar()->showMessage(QString::fromUtf8(
                "Выберите геометрию непосредственно в viewport"));
            viewport_->setFocus();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::clearSelectionRequested,
          this, [this] {
            invalidatePreviewUpdates();
            applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::ClearSelection, &document_));
          });
  connect(viewport_, &Viewport::toolManipulatorValueChanged, this,
          [this](double value) {
            const auto kind = partDesignCoordinator_.activeTool();
            const auto state = partDesignCoordinator_.snapshot(kind, document_);
            if (kind == PartDesignToolKind::LinearPattern) {
              const QSignalBlocker blocker(linearPatternSpacingSpin_);
              linearPatternSpacingSpin_->setValue(value);
              schedulePreviewUpdate(
                  state.bodyId, state.sourceFeatureId,
                  [this, kind, value] {
                    if (partDesignCoordinator_.activeTool() == kind)
                      partDesignCoordinator_.setLinearPatternSpacing(value);
                  },
                  [this] { updateLinearPatternToolPreview(); });
              return;
            }
            if (kind != PartDesignToolKind::Chamfer &&
                kind != PartDesignToolKind::Shell &&
                kind != PartDesignToolKind::Extrude &&
                kind != PartDesignToolKind::Fillet)
              return;
            toolParametersPanel_->setParameterValue(
                kind == PartDesignToolKind::Extrude ? std::abs(value) : value);
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                state.bodyId, state.sourceFeatureId,
                [this, kind, value, effect] {
                  if (partDesignCoordinator_.activeTool() == kind)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        document_, value,
                        PartDesignParameterSource::Manipulator);
                },
                [this, effect] {
                  applyPartDesignUiEffect(std::move(*effect));
                });
          });
  connect(viewport_, &Viewport::bodyEdgeSelectionChanged, this, [this] {
    invalidatePreviewUpdates();
    if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Chamfer) {
      partDesignCoordinator_.setChamferEdges(viewport_->selectedBodyEdges());
      updateChamferToolPreview();
      if (!partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges.empty())
        static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Fillet) {
      partDesignCoordinator_.setFilletEdges(viewport_->selectedBodyEdges());
      updateFilletToolPreview();
      if (!partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges.empty())
        static_cast<void>(viewport_->focusToolParameterField(false));
    }
  });
  connect(viewport_, &Viewport::bodyFaceSelectionChanged, this, [this] {
    invalidatePreviewUpdates();
    if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Extrude) {
      const auto faces = viewport_->selectedBodyFaces();
      if (!faces.empty())
        partDesignCoordinator_.setExtrudeFace(faces.front());
      updateFaceExtrudeToolPreview();
      // Keep keyboard focus in the viewport after face selection so the next
      // Tab enters the on-canvas distance field, not the right-hand dock.
      static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Shell) {
      partDesignCoordinator_.setShellRemovedFaces(viewport_->selectedBodyFaces());
      updateShellToolPreview();
      // Keep keyboard focus in the viewport after face selection so the next
      // Tab enters the on-canvas thickness field, not the right-hand dock.
      static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Draft) {
      auto faces = viewport_->selectedBodyFaces();
      if (faces.size() > 1) faces.resize(1);
      partDesignCoordinator_.setDraftFaces(document_, faces);
      partDesignCoordinator_.clearDraftPrincipalAxis(document_);
      updateDraftToolPreview();
      if (!faces.empty()) {
        partDesignCoordinator_.beginReselection(
            ToolSelectionStage::SelectingReference);
        viewport_->beginDraftAxisSelection();
        statusBar()->showMessage(
            QString::fromUtf8(
                "2/3 Выберите ось X, Y, Z или прилегающее прямое ребро"));
      }
    }
  });
  connect(viewport_, &Viewport::bodiesSelected, this,
          [this](const std::vector<BodyId>& bodyIds) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle == ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            std::vector<JoinBodyInput> inputs;
            inputs.reserve(std::min<std::size_t>(bodyIds.size(), 2));
            for (const BodyId bodyId : bodyIds) {
              if (inputs.size() == 2) break;
              if (const auto editingId =
                      partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).editingFeatureId) {
                const Body* owner = nullptr;
                for (const auto& candidate : document_.bodies())
                  if (candidate.featureIndex(*editingId)) {
                    owner = &candidate;
                    break;
                  }
                if (owner && owner->id() == bodyId) continue;
              }
              Body* body = document_.findBody(bodyId);
              if (!body || !body->activeFeature() || !body->resultShape())
                continue;
              inputs.push_back({bodyId, body->activeFeature()->id(),
                                body->resultShape()});
            }
            partDesignCoordinator_.setJoinBodies(std::move(inputs));
            updateJoinBodiesToolPreview();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::optionChanged, this,
          [this](bool checked) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle != ToolLifecycle::Inactive) {
              const auto operation = checked ? ExtrudeOperation::Cut
                                             : ExtrudeOperation::Join;
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sourceFeatureId,
                  [this, operation] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::Extrude)
                      partDesignCoordinator_.setExtrudeOperation(operation);
                  },
                  [this] { updateFaceExtrudeToolPreview(); });
            } else if (partDesignCoordinator_.activeTool() ==
                       PartDesignToolKind::Shell) {
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).sourceFeatureId,
                  [this, checked] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::Shell)
                      partDesignCoordinator_.setShellOutside(checked);
                  },
                  [this] { updateShellToolPreview(); });
            }
          });
  connect(viewport_, &Viewport::angularToolManipulatorValueChanged, this,
          [this](double value) {
            if (partDesignCoordinator_.activeTool() != PartDesignToolKind::Draft)
              return;
            toolParametersPanel_->setParameterValue(value);
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).sourceFeatureId,
                [this, value, effect] {
                  if (partDesignCoordinator_.activeTool() ==
                      PartDesignToolKind::Draft)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        document_, value,
                        PartDesignParameterSource::Manipulator);
                },
                [this, effect] {
                  applyPartDesignUiEffect(std::move(*effect));
                });
          });
  connect(viewport_, &Viewport::toolManipulatorDragFinished, this,
          [this] { flushPreviewUpdate(); });
  connect(viewport_, &Viewport::extrusionManipulatorDragFinished, this,
          [this] { flushPreviewUpdate(); });
  // HUD Enter commit: the value is already interpreted + preview-synced via the
  // value routing above, so perform the active tool's existing Accept exactly
  // like the Готово button. Each accept*Tool guards its own lifecycle, so an
  // invalid preview will not accept and focus stays in the HUD field.
  connect(viewport_, &Viewport::toolParameterCommitted, this, [this] {
    flushPreviewUpdate();
    // Extrude still uses its dedicated on-canvas spinbox. Treat Enter there
    // exactly like the Apply button before dispatching ToolSession tools.
    if (extrusionDock_->isVisible()) {
      extrudeSketch();
      return;
    }
    applyActivePartDesignTool();
  });

  const auto acceptActiveTool = [this] {
    toolParametersPanel_->interpretParameterText();
    flushPreviewUpdate();
    if (!toolParametersPanel_->acceptEnabled()) return;
    applyActivePartDesignTool();
  };
  auto* acceptReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), toolParametersDock_);
  acceptReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  acceptReturnShortcut->setAutoRepeat(false);
  connect(acceptReturnShortcut, &QShortcut::activated, this, acceptActiveTool);
  auto* acceptKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), toolParametersDock_);
  acceptKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  acceptKeypadShortcut->setAutoRepeat(false);
  connect(acceptKeypadShortcut, &QShortcut::activated, this, acceptActiveTool);
  auto* cancelToolShortcut = new QShortcut(QKeySequence(Qt::Key_Escape),
                                           toolParametersDock_);
  cancelToolShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  cancelToolShortcut->setAutoRepeat(false);
  connect(cancelToolShortcut, &QShortcut::activated, this,
          [this] {
            applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::Escape));
          });
  // CAD Tab belongs to the on-canvas HUD, not to the controls in the
  // right-hand Part Design panel. WidgetWithChildrenShortcut catches Tab even
  // when a spinbox/checkbox inside the dock currently owns keyboard focus.
  const auto focusViewportHud = [this](bool backward) {
    static_cast<void>(viewport_->focusToolParameterField(backward));
  };
  auto* hudTabShortcut =
      new QShortcut(QKeySequence(Qt::Key_Tab), toolParametersDock_);
  hudTabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  hudTabShortcut->setAutoRepeat(false);
  connect(hudTabShortcut, &QShortcut::activated, this,
          [focusViewportHud] { focusViewportHud(false); });

  auto* hudBacktabShortcut =
      new QShortcut(QKeySequence(Qt::Key_Backtab), toolParametersDock_);
  hudBacktabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  hudBacktabShortcut->setAutoRepeat(false);
  connect(hudBacktabShortcut, &QShortcut::activated, this,
          [focusViewportHud] { focusViewportHud(true); });
  connect(extrusionLengthSpin_, &QDoubleSpinBox::valueChanged, this,
          [this](double value) {
            const double signedValue = extrusionReverseCheck_->isChecked()
                                           ? -std::abs(value)
                                           : std::abs(value);
            viewport_->setExtrusionPreviewLength(signedValue);
          });
  connect(extrusionLengthSpin_, &QDoubleSpinBox::editingFinished, this,
          &MainWindow::normalizeExtrusionDistance);
  connect(extrusionReverseCheck_, &QCheckBox::toggled, this, [this](bool) {
    viewport_->setExtrusionPreviewLength(
        extrusionReverseCheck_->isChecked()
            ? -std::abs(extrusionLengthSpin_->value())
            : std::abs(extrusionLengthSpin_->value()));
  });
  connect(extrusionOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            extrudeOperationManuallyChanged_ = true;
            invalidatePreviewUpdates();
          });
  connect(viewport_, &Viewport::extrusionPreviewLengthChanged, this,
          [this](double value) {
            const QSignalBlocker distanceBlocker(extrusionLengthSpin_);
            const QSignalBlocker reverseBlocker(extrusionReverseCheck_);
            extrusionLengthSpin_->setValue(std::abs(value));
            extrusionReverseCheck_->setChecked(value < 0.0);
            scheduleAutomaticExtrudeOperation();
          });
  connect(acceptExtrusion, &QPushButton::clicked, this, &MainWindow::extrudeSketch);
  const auto applyExtrusionOnEnter = [this] {
    if (!extrusionDock_->isVisible()) return;
    extrusionLengthSpin_->interpretText();
    extrudeSketch();
  };
  auto* returnShortcut = new QShortcut(QKeySequence(Qt::Key_Return), extrusionDock_);
  returnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  returnShortcut->setAutoRepeat(false);
  connect(returnShortcut, &QShortcut::activated, this, applyExtrusionOnEnter);
  auto* keypadEnterShortcut = new QShortcut(QKeySequence(Qt::Key_Enter), extrusionDock_);
  keypadEnterShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  keypadEnterShortcut->setAutoRepeat(false);
  connect(keypadEnterShortcut, &QShortcut::activated, this,
          applyExtrusionOnEnter);
  connect(cancelExtrusion, &QPushButton::clicked, this, [this] {
    cancelLegacyExtrusion();
  });
  auto* extrusionEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), extrusionDock_);
  extrusionEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  extrusionEscapeShortcut->setAutoRepeat(false);
  connect(extrusionEscapeShortcut, &QShortcut::activated, this, [this] {
    cancelLegacyExtrusion();
  });

  sketchSettingsDock_ =
      new QDockWidget(QString::fromUtf8("Свойства эскиза"), this);
  sketchSettingsDock_->setObjectName(QStringLiteral("sketchSettingsDock"));
  sketchSettingsDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  sketchSettingsDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* settingsPanel = new QWidget(sketchSettingsDock_);
  auto* settingsLayout = new QVBoxLayout(settingsPanel);
  settingsLayout->setContentsMargins(16, 14, 16, 14);
  settingsLayout->setSpacing(12);
  auto* gridCheck = new QCheckBox(QString::fromUtf8("Сетка"), settingsPanel);
  gridCheck->setChecked(true);
  auto* snapCheck =
      new QCheckBox(QString::fromUtf8("Привязка к сетке"), settingsPanel);
  // Exact grid snapping is intentionally opt-in. CAD inference to endpoints,
  // midpoints and carrier geometry remains active independently.
  snapCheck->setChecked(false);
  auto* lineTypeLabel =
      new QLabel(QString::fromUtf8("Тип линии"), settingsPanel);
  sketchLineTypeCombo_ = new QComboBox(settingsPanel);
  sketchLineTypeCombo_->addItem(QString::fromUtf8("Сплошная линия"));
  sketchLineTypeCombo_->addItem(QString::fromUtf8("Пунктирная линия"));
  sketchLineTypeCombo_->setEnabled(false);
  auto* orientSketchPlane = new QPushButton(
      QString::fromUtf8("Вернуть исходную ориентацию"), settingsPanel);
  orientSketchPlane->setObjectName(QStringLiteral("sketchOrientToPlaneButton"));
  orientSketchPlane->setToolTip(QString::fromUtf8(
      "Сбросить свободный поворот и вернуть камеру перпендикулярно "
      "исходной плоскости эскиза"));
  settingsLayout->addWidget(gridCheck);
  settingsLayout->addWidget(snapCheck);
  settingsLayout->addSpacing(8);
  settingsLayout->addWidget(lineTypeLabel);
  settingsLayout->addWidget(sketchLineTypeCombo_);
  settingsLayout->addWidget(orientSketchPlane);

  auto* constraintsSection = new QFrame(settingsPanel);
  constraintsSection->setObjectName(QStringLiteral("constraintsSection"));
  constraintsSection->setProperty("uiRole", "sectionPanel");
  auto* constraintsSectionLayout = new QVBoxLayout(constraintsSection);
  constraintsSectionLayout->setContentsMargins(0, 12, 0, 0);
  constraintsSectionLayout->setSpacing(7);
  auto* constraintsTitle =
      new QLabel(QString::fromUtf8("Ограничения"), constraintsSection);
  QFont constraintsTitleFont = constraintsTitle->font();
  constraintsTitleFont.setBold(true);
  constraintsTitle->setFont(constraintsTitleFont);
  sketchConstraintsList_ = new QTreeWidget(constraintsSection);
  sketchConstraintsList_->setColumnCount(2);
  sketchConstraintsList_->setHeaderHidden(true);
  sketchConstraintsList_->setRootIsDecorated(false);
  sketchConstraintsList_->setItemsExpandable(false);
  sketchConstraintsList_->setSelectionMode(QAbstractItemView::NoSelection);
  sketchConstraintsList_->setFocusPolicy(Qt::StrongFocus);
  sketchConstraintsList_->setMaximumHeight(150);
  sketchConstraintsList_->setColumnWidth(0, 185);
  sketchConstraintsList_->setColumnWidth(1, 28);

  constraintsSectionLayout->addWidget(constraintsTitle);
  constraintsSectionLayout->addWidget(sketchConstraintsList_);
  settingsLayout->addWidget(constraintsSection);

  auto* circlePropertiesSection = new QFrame(settingsPanel);
  circlePropertiesSection->setObjectName(QStringLiteral("circlePropertiesSection"));
  circlePropertiesSection->setProperty("uiRole", "sectionPanel");
  auto* circleLayout = new QFormLayout(circlePropertiesSection);
  circleLayout->setContentsMargins(0, 12, 0, 0);
  circleLayout->setSpacing(10);
  auto* circleTitle = new QLabel(QString::fromUtf8("Окружность"),
                                 circlePropertiesSection);
  QFont circleTitleFont = circleTitle->font();
  circleTitleFont.setBold(true);
  circleTitle->setFont(circleTitleFont);
  circleLayout->addRow(circleTitle);
  auto* circleDiameterSpin = new QDoubleSpinBox(circlePropertiesSection);
  circleDiameterSpin->setRange(0.01, 100000.0);
  circleDiameterSpin->setDecimals(2);
  circleDiameterSpin->setValue(20.0);
  circleDiameterSpin->setSuffix(QString::fromUtf8(" мм"));
  auto* circleModeButtons = new QWidget(circlePropertiesSection);
  auto* circleModeLayout = new QHBoxLayout(circleModeButtons);
  circleModeLayout->setContentsMargins(0, 0, 0, 0);
  circleModeLayout->setSpacing(4);
  auto* circleModeGroup = new QButtonGroup(circleModeButtons);
  circleModeGroup->setExclusive(true);
  struct CircleModeButtonSpec {
    ToolIconKind icon;
    const char* tooltip;
  };
  const CircleModeButtonSpec circleModes[] = {
      {ToolIconKind::CircleCenterRadius, "Из центра"},
      {ToolIconKind::CircleTwoPoints, "По двум точкам"},
      {ToolIconKind::CircleThreePoints, "По трём точкам"},
      {ToolIconKind::CircleThreeTangents, "По трём прямым"},
      {ToolIconKind::CircleTwoTangentsRadius,
       "По двум прямым и радиусу"},
  };
  for (int index = 0; index < 5; ++index) {
    auto* button = new QToolButton(circleModeButtons);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setIcon(toolIcon(circleModes[index].icon));
    button->setIconSize(QSize(26, 26));
    button->setFixedSize(36, 36);
    button->setToolTip(QString::fromUtf8(circleModes[index].tooltip));
    circleModeGroup->addButton(button, index);
    circleModeLayout->addWidget(button);
    if (index == 0) button->setChecked(true);
  }
  circleLayout->addRow(QString::fromUtf8("Диаметр:"), circleDiameterSpin);
  circleLayout->addRow(circleModeButtons);
  settingsLayout->addWidget(circlePropertiesSection);
  circlePropertiesSection->hide();

  auto* rectanglePropertiesSection = new QFrame(settingsPanel);
  rectanglePropertiesSection->setObjectName(
      QStringLiteral("rectanglePropertiesSection"));
  rectanglePropertiesSection->setProperty("uiRole", "sectionPanel");
  auto* rectangleLayout = new QVBoxLayout(rectanglePropertiesSection);
  rectangleLayout->setContentsMargins(0, 12, 0, 0);
  rectangleLayout->setSpacing(10);
  auto* rectangleTitle = new QLabel(QString::fromUtf8("Прямоугольник"),
                                    rectanglePropertiesSection);
  QFont rectangleTitleFont = rectangleTitle->font();
  rectangleTitleFont.setBold(true);
  rectangleTitle->setFont(rectangleTitleFont);
  rectangleLayout->addWidget(rectangleTitle);
  auto* rectangleModeButtons = new QWidget(rectanglePropertiesSection);
  auto* rectangleModeLayout = new QHBoxLayout(rectangleModeButtons);
  rectangleModeLayout->setContentsMargins(0, 0, 0, 0);
  rectangleModeLayout->setSpacing(4);
  auto* rectangleModeGroup = new QButtonGroup(rectangleModeButtons);
  rectangleModeGroup->setExclusive(true);
  const CircleModeButtonSpec rectangleModes[] = {
      {ToolIconKind::RectangleTwoPoints, "По двум точкам"},
      {ToolIconKind::RectangleThreePoints, "По трём точкам"},
      {ToolIconKind::RectangleFromCenter, "Из центра"},
  };
  for (int index = 0; index < 3; ++index) {
    auto* button = new QToolButton(rectangleModeButtons);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setIcon(toolIcon(rectangleModes[index].icon));
    button->setIconSize(QSize(26, 26));
    button->setFixedSize(36, 36);
    button->setToolTip(QString::fromUtf8(rectangleModes[index].tooltip));
    rectangleModeGroup->addButton(button, index);
    rectangleModeLayout->addWidget(button);
    if (index == 0) button->setChecked(true);
  }
  rectangleLayout->addWidget(rectangleModeButtons);
  settingsLayout->addWidget(rectanglePropertiesSection);
  rectanglePropertiesSection->hide();
  settingsLayout->addStretch();
  sketchSettingsDock_->setWidget(settingsPanel);
  sketchSettingsDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, sketchSettingsDock_);
  sketchSettingsDock_->hide();
  connect(gridCheck, &QCheckBox::toggled, sketchCanvas_,
          &SketchCanvas::setGridVisible);
  connect(snapCheck, &QCheckBox::toggled, sketchCanvas_,
          &SketchCanvas::setSnapEnabled);
  connect(orientSketchPlane, &QPushButton::clicked, sketchCanvas_,
          &SketchCanvas::resetViewRotation);
  connect(sketchLineTypeCombo_, &QComboBox::currentIndexChanged, sketchCanvas_,
          [this](int index) { sketchCanvas_->setSelectedDashed(index == 1); });
  connect(circleDiameterSpin, &QDoubleSpinBox::valueChanged, sketchCanvas_,
          &SketchCanvas::setCircleDiameter);
  connect(circleModeGroup, &QButtonGroup::idClicked, sketchCanvas_,
          [this](int index) {
            sketchCanvas_->setCircleMode(
                static_cast<SketchCanvas::CircleMode>(index));
          });
  connect(rectangleModeGroup, &QButtonGroup::idClicked, sketchCanvas_,
          [this](int index) {
            sketchCanvas_->setRectangleMode(
                static_cast<SketchCanvas::RectangleMode>(index));
          });
  connect(sketchCanvas_, &SketchCanvas::primaryDimensionChanged, this,
          [this, circleDiameterSpin](double value) {
            if (sketchCanvas_->tool() != SketchCanvas::Tool::Circle) return;
            const QSignalBlocker blocker(circleDiameterSpin);
            circleDiameterSpin->setValue(value);
          });
  connect(sketchCanvas_, &SketchCanvas::selectionChanged, this,
          [this](const QString&) { updateSketchConstraintPanel(); });
  connect(sketchCanvas_, &SketchCanvas::geometryChanged, this,
          [this](double, double) { updateSketchConstraintPanel(); });
  connect(sketchConstraintsList_, &QTreeWidget::itemChanged, this,
          [this](QTreeWidgetItem* item, int column) {
            if (!item || column != 1) return;

            const auto dimensionIndexValue =
                item->data(0, Qt::UserRole + 1).toULongLong();
            const bool isDimension =
                dimensionIndexValue != std::numeric_limits<qulonglong>::max();

            if (isDimension) {
              const bool driving = item->checkState(1) == Qt::Checked;
              const auto dimensionIndex =
                  static_cast<std::size_t>(dimensionIndexValue);

              QTimer::singleShot(0, this,
                                 [this, dimensionIndex, driving] {
                if (sketchCanvas_)
                  sketchCanvas_->setDimensionDriving(dimensionIndex, driving);
              });
              return;
            }

            if (item->checkState(1) != Qt::Unchecked) return;

            const QVariant idData = item->data(0, Qt::UserRole);
            if (!idData.isValid()) return;

            const auto id =
                static_cast<sketch::ConstraintId>(idData.toULongLong());

            QTimer::singleShot(0, this, [this, id] {
              if (sketchCanvas_)
                sketchCanvas_->removeConstraintById(id);
            });
          });
  connect(sketchCanvas_, &SketchCanvas::lineStyleSelectionChanged, this,
          [this](bool elementSelected, bool dashed) {
            const QSignalBlocker blocker(sketchLineTypeCombo_);
            sketchLineTypeCombo_->setEnabled(elementSelected);
            sketchLineTypeCombo_->setCurrentIndex(dashed ? 1 : 0);
            if (elementSelected) {
              sketchSettingsDock_->show();
              sketchSettingsDock_->raise();
            }
          });
  connect(sketchCanvas_, &SketchCanvas::toolChanged, this,
          [this, circlePropertiesSection,
           rectanglePropertiesSection](SketchCanvas::Tool tool) {
            circlePropertiesSection->setVisible(
                tool == SketchCanvas::Tool::Circle);
            rectanglePropertiesSection->setVisible(
                tool == SketchCanvas::Tool::Rectangle);
            if (tool != SketchCanvas::Tool::Select) {
              sketchLineTypeCombo_->setEnabled(false);
              sketchSettingsDock_->show();
              sketchSettingsDock_->raise();
            }
          });

  sketchRibbon_ = new SketchRibbon(sketchCanvas_, this);
  modelRibbon_ = new ModelRibbon(this);
  ribbonStack_ = new QStackedWidget(this);
  ribbonStack_->addWidget(modelRibbon_);
  ribbonStack_->addWidget(sketchRibbon_);
  connect(modelRibbon_, &ModelRibbon::createSketchRequested, this,
          [this] {
            if (!ensureHistoryAtEnd()) {
              modelRibbon_->clearActiveTool();
              return;
            }
            resetTransientModelingUi();
            editingSketchIndex_.reset();
            statusBar()->showMessage(
                QString::fromUtf8("Выберите базовую плоскость или грань тела"));
            for (auto* item : featureTree_->findItems(
                     QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive)) {
              const int kind = item->data(0, Qt::UserRole).toInt();
              if (kind >= 10 && kind <= 12) item->setCheckState(0, Qt::Checked);
            }
            viewport_->beginSketchPlaneSelection();
          });
  connect(modelRibbon_, &ModelRibbon::extrudeRequested, this,
          [this] {
            if (!ensureHistoryAtEnd()) {
              modelRibbon_->clearActiveTool();
              return;
            }
            resetTransientModelingUi();
            extrudeOperationManuallyChanged_ = false;
            extrusionReverseCheck_->setChecked(false);
            const QSignalBlocker blocker(extrusionOperationCombo_);
            extrusionOperationCombo_->setCurrentIndex(0);
            statusBar()->showMessage(
                QString::fromUtf8("Выберите замкнутый контур или грань тела"));
            viewport_->beginExtrusionSurfaceSelection();
          });
  connect(modelRibbon_, &ModelRibbon::pocketRequested, this,
          [this] {
            if (!ensureHistoryAtEnd()) {
              modelRibbon_->clearActiveTool();
              return;
            }
            if (!document_.activeBody()) {
              QMessageBox::information(this, QString::fromUtf8("Вырезать"),
                  QString::fromUtf8("Сначала создайте Body."));
              return;
            }
            resetTransientModelingUi();
            extrudeOperationManuallyChanged_ = true;
            extrusionOperationCombo_->setCurrentIndex(2);
            extrusionReverseCheck_->setChecked(true);
            statusBar()->showMessage(
                QString::fromUtf8("Выберите профиль для вырезания"));
            viewport_->beginExtrusionSurfaceSelection();
          });
  connect(modelRibbon_, &ModelRibbon::revolveRequested, this,
          &MainWindow::createRevolve);
  connect(modelRibbon_, &ModelRibbon::filletRequested, this,
          &MainWindow::createFillet);
  connect(modelRibbon_, &ModelRibbon::chamferRequested, this,
          &MainWindow::createChamfer);
  connect(modelRibbon_, &ModelRibbon::joinBodiesRequested, this,
          &MainWindow::createJoinBodies);
  connect(modelRibbon_, &ModelRibbon::moveRequested, this,
          &MainWindow::createMove);
  connect(modelRibbon_, &ModelRibbon::shellRequested, this,
          &MainWindow::createShell);
  connect(modelRibbon_, &ModelRibbon::draftRequested, this,
          &MainWindow::createDraft);
  connect(modelRibbon_, &ModelRibbon::mirrorRequested, this,
          &MainWindow::createMirror);
  connect(modelRibbon_, &ModelRibbon::linearPatternRequested, this,
          &MainWindow::createLinearPattern);
  connect(modelRibbon_, &ModelRibbon::circularPatternRequested, this,
          &MainWindow::createCircularPattern);
  connect(modelRibbon_, &ModelRibbon::rulerToggled, this,
          [this](bool active) {
            if (active) {
              resetTransientModelingUi();
              viewport_->beginRulerMeasurement();
              viewport_->setFocus(Qt::MouseFocusReason);
              statusBar()->showMessage(
                  QString::fromUtf8("Линейка: выберите первую точку"));
            } else {
              viewport_->cancelRulerMeasurement();
              statusBar()->showMessage(
                  QString::fromUtf8("Линейка выключена"), 2000);
            }
          });
  connect(viewport_, &Viewport::rulerActiveChanged, modelRibbon_,
          &ModelRibbon::setRulerActive);
  connect(viewport_, &Viewport::rulerPointPicked, this,
          [this](int selectedPointCount) {
            if (selectedPointCount == 1)
              statusBar()->showMessage(
                  QString::fromUtf8("Линейка: выберите вторую точку"));
          });
  connect(viewport_, &Viewport::rulerMeasurementChanged, this,
          [this](double distanceMm) {
            statusBar()->showMessage(
                QString::fromUtf8("Расстояние: %1 мм. Щёлкните ещё раз для нового измерения")
                    .arg(QLocale().toString(distanceMm, 'f', 2)));
          });
  connect(modelRibbon_, &ModelRibbon::displayModeRequested, viewport_,
          [this](int mode) {
            viewport_->setDisplayMode(static_cast<ViewportDisplayMode>(mode));
          });
  connect(modelRibbon_, &ModelRibbon::meshQualityRequested, viewport_,
          [this](int quality) {
            viewport_->setMeshQuality(static_cast<ViewportMeshQuality>(quality));
          });
  connect(viewport_, &Viewport::sketchPlanePicked, this,
          [this](const SketchPlanePick& pick) {
            viewport_->setSelectionFilter(SelectionFilter::Any);
            modelRibbon_->clearActiveTool();
            currentSketchSupport_ = pick.presentationLabel;
            currentSketchFaceReference_.reset();
            if (const auto* datum = std::get_if<DatumPlanePick>(&pick.source)) {
              currentSketchPlacement_ = datum->placement;
            } else if (const auto* legacy =
                           std::get_if<LegacySolidFacePick>(&pick.source)) {
              currentSketchPlacement_ = legacy->placement;
            } else if (const auto* bodyFace =
                           std::get_if<BodyFacePick>(&pick.source)) {
              const FaceReference& faceReference = bodyFace->face;
              if (faceReference.bodyId == kInvalidBodyId ||
                  faceReference.featureId == kInvalidFeatureId) {
                QMessageBox::warning(this, QString::fromUtf8("Sketch on Face"),
                                     QString::fromUtf8("Не удалось определить грань Body."));
                return;
              }
              const Body* body = document_.findBody(faceReference.bodyId);
              const ShapeFeature* feature = nullptr;
              if (body)
                for (const auto& candidate : body->features())
                  if (candidate->id() == faceReference.featureId) {
                    feature = candidate.get();
                    break;
                  }
              const auto shape = feature ? feature->shape() : ShapeFeature::ShapePtr{};
              if (!shape) {
                QMessageBox::warning(
                    this, QString::fromUtf8("Sketch on Face"),
                    QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
                return;
              }
              const auto topology = feature->topologyIndex();
              const auto resolvedFace = topology
                                            ? topology->resolveFace(
                                                  faceReference.topology())
                                            : FaceResolution{};
              const auto resolved = resolvedFace
                                        ? resolveFacePlacement(
                                              *resolvedFace.subshape)
                                        : ResolvedFacePlacement{};
              if (!resolved.resolved) {
                QMessageBox::warning(
                    this, QString::fromUtf8("Sketch on Face"),
                    QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
                return;
              }
              if (!resolved.planar) {
                QMessageBox::information(
                    this, QString::fromUtf8("Sketch on Face"),
                    QString::fromUtf8("Создание эскиза на криволинейной поверхности пока не поддерживается."));
                return;
              }
              currentSketchPlacement_ = resolved.placement;
              currentSketchFaceReference_ = faceReference;
            }
            sketchCanvas_->resetSketch();
            sketchCanvas_->clearSketchEditContext();
            // Sketcher opens in a stable CAD orientation.  The current 3D
            // camera may be orbiting or may use a front-view convention whose
            // screen-up is opposite to world +Z; carrying that roll into the
            // sketch can place the selected face 180 degrees around Z.
            sketchCanvas_->setInitialViewUp({});
            sketchCanvas_->setReferenceBody(document_.box(),
                                             currentSketchPlacement_,
                                             hasDisplayableModernSolid() ||
                                                 hasHistoricalLegacyExtrusion());
            const bool historicalCap =
                std::holds_alternative<LegacySolidFacePick>(pick.source) &&
                hasHistoricalLegacyExtrusion();
            sketchCanvas_->setReferenceProfile(viewport_->solidSketch(),
                                               historicalCap);
            if (currentSketchFaceReference_ &&
                !configureSketchEditContext(true))
              return;
            configureSketchSceneReferences();
            workspaceStack_->setCurrentWidget(sketchCanvas_);
            ribbonStack_->setCurrentWidget(sketchRibbon_);
            statusBar()->showMessage(QString::fromUtf8("Рабочая плоскость: ") +
                                     pick.presentationLabel);
          });
  connect(viewport_, &Viewport::extrusionSourcePicked, this,
          [this](const ExtrusionSourcePick& pick) {
            if (const auto* face = std::get_if<BodyFacePick>(&pick.source)) {
              createFaceExtrude(face->face);
              return;
            }
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle != ToolLifecycle::Inactive) {
              updateRevolveProfileSelection(pick);
              return;
            }
            selectedExtrusionSource_ = pick;
            statusBar()->showMessage(QString::fromUtf8("Поверхность: ") +
                                     pick.presentationLabel);
            extrusionLengthSpin_->setValue(document_.box().heightMm);
            viewport_->showExtrusionManipulator(
                extrusionReverseCheck_->isChecked()
                    ? -extrusionLengthSpin_->value()
                    : extrusionLengthSpin_->value());
            scheduleAutomaticExtrudeOperation();
            extrusionDock_->show();
            extrusionDock_->raise();
          });
  connect(viewport_, &Viewport::directProfilePicked, this,
          [this](const ExtrusionSourcePick& pick) { createSketchExtrude(pick); });
  connect(viewport_, &Viewport::bodyMoveCommitted, this,
          [this](QPointF previous, QPointF current) {
            HistorySelectionState after = captureHistorySelection();
            HistorySelectionState before = after;
            before.presentation.bodyPositionX = previous.x();
            before.presentation.bodyPositionY = previous.y();
            after.presentation.bodyPositionX = current.x();
            after.presentation.bodyPositionY = current.y();
            static_cast<void>(pushPresentationTransition(
                std::move(before), std::move(after)));
          });
  connect(sketchRibbon_, &SketchRibbon::finishRequested, this,
          &MainWindow::finishSketch);
  connect(sketchCanvas_, &SketchCanvas::geometryChanged, this,
          &MainWindow::updateFromSketch);
  connect(workspaceStack_, &QStackedWidget::currentChanged, this, [this](int index) {
    const bool sketchMode = workspaceStack_->widget(index) == sketchCanvas_;
    ribbonStack_->setCurrentWidget(workspaceStack_->widget(index) == viewport_
                                       ? static_cast<QWidget*>(modelRibbon_)
                                       : static_cast<QWidget*>(sketchRibbon_));
    if (sketchSettingsDock_) {
      sketchSettingsDock_->setVisible(sketchMode);
      if (sketchMode) sketchSettingsDock_->raise();
    }
    if (auto* modelTreeDock =
            findChild<QDockWidget*>(QStringLiteral("modelTreeDock"))) {
      modelTreeDock->setVisible(!sketchMode);
      if (!sketchMode) modelTreeDock->raise();
    }
    updateUndoAvailability();
  });
  workspaceStack_->setCurrentWidget(viewport_);
  ribbonStack_->setCurrentWidget(modelRibbon_);

  modelTreeDock_ =
      new QDockWidget(QString::fromUtf8("Дерево построений"), this);
  modelTreeDock_->setObjectName(QStringLiteral("modelTreeDock"));
  featureTree_ = new QTreeWidget(modelTreeDock_);
  featureTree_->setHeaderHidden(true);
  featureTree_->setAlternatingRowColors(true);
  featureTree_->setContextMenuPolicy(Qt::CustomContextMenu);
  rebuildFeatureTree();
  connect(featureTree_, &QTreeWidget::customContextMenuRequested, this,
          [this](const QPoint& point) {
            auto* item = featureTree_->itemAt(point);
            if (!item) return;
            const auto bodyId = static_cast<BodyId>(
                item->data(0, Qt::UserRole + 2).toULongLong());
            if (bodyId == kInvalidBodyId) return;
            QMenu menu(featureTree_);
            QAction* remove = menu.addAction(QString::fromUtf8("Удалить Body"));
            if (menu.exec(featureTree_->viewport()->mapToGlobal(point)) == remove)
              removeBody(bodyId);
          });
  auto* deleteBodyShortcut =
      new QShortcut(QKeySequence(Qt::Key_Delete), featureTree_);
  deleteBodyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(deleteBodyShortcut, &QShortcut::activated, this, [this] {
    auto* item = featureTree_->currentItem();
    if (!item) return;
    const auto bodyId = static_cast<BodyId>(
        item->data(0, Qt::UserRole + 2).toULongLong());
    if (bodyId != kInvalidBodyId) removeBody(bodyId);
  });
  connect(featureTree_, &QTreeWidget::itemChanged, this,
          [this](QTreeWidgetItem* item, int) {
            const int kind = item->data(0, Qt::UserRole).toInt();
            const bool visible = item->checkState(0) == Qt::Checked;
            const auto bodyId = static_cast<BodyId>(
                item->data(0, Qt::UserRole + 2).toULongLong());
            if (kind == 3 && bodyId != kInvalidBodyId) {
              Body* body = document_.findBody(bodyId);
              if (!body || body->visible() == visible) return;
              const auto previousSelection = captureHistorySelection();
              Document previous = document_;
              auto committedAfter = captureCommittedEndState();
              body->setVisible(visible);
              if (!pushModelTransition(std::move(previous), previousSelection,
                                       std::move(committedAfter))) {
                const QSignalBlocker blocker(featureTree_);
                item->setCheckState(0, visible ? Qt::Unchecked : Qt::Checked);
                return;
              }
              resetTransientModelingUi();
              refreshBodyViewFromDocument();
              completeModelTransitionUi();
              return;
            }
            const HistorySelectionState before = captureHistorySelection();
            if (kind == 1) viewport_->setOriginVisible(visible);
            if (kind == 2) viewport_->setSketchVisible(visible);
            if (kind >= 10 && kind <= 12)
              viewport_->setBasePlaneVisible(kind - 10, visible);
            if (kind >= 20)
              viewport_->setSketchVisible(static_cast<std::size_t>(kind - 20), visible);
            const HistorySelectionState after = captureHistorySelection();
            if (!pushPresentationTransition(before, after)) {
              const QSignalBlocker blocker(featureTree_);
              item->setCheckState(0, visible ? Qt::Unchecked : Qt::Checked);
            }
          });
  connect(viewport_, &Viewport::revolveAxisPicked, this,
          [this](qulonglong axisToken) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::Inactive) return;
            invalidatePreviewUpdates();
            AxisReference axis;
            if (axisToken == Viewport::kGlobalXAxisToken) {
              axis.type = AxisReferenceType::GlobalX;
            } else if (axisToken == Viewport::kGlobalYAxisToken) {
              axis.type = AxisReferenceType::GlobalY;
            } else if (axisToken == Viewport::kGlobalZAxisToken) {
              axis.type = AxisReferenceType::GlobalZ;
            } else if (axisToken == 1) {
              axis.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId;
              axis.type = AxisReferenceType::SketchHorizontalAxis;
            } else if (axisToken == 2) {
              axis.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId;
              axis.type = AxisReferenceType::SketchVerticalAxis;
            } else {
              axis.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId;
              axis.type = AxisReferenceType::SketchLine;
              axis.lineId = static_cast<sketch::GeometryId>(axisToken - 3);
            }
            partDesignCoordinator_.setRevolveAxis(document_, axis);
            partDesignCoordinator_.finishReselection();
            const int comboIndex = revolveAxisCombo_->findData(axisToken);
            if (comboIndex >= 0) {
              const QSignalBlocker blocker(revolveAxisCombo_);
              revolveAxisCombo_->setCurrentIndex(comboIndex);
            }
            updateRevolveToolPreview();
            static_cast<void>(viewport_->focusToolParameterField(false));
            statusBar()->showMessage(QString::fromUtf8("Задайте угол дугой или полем у манипулятора"));
          });
  connect(viewport_, &Viewport::moveBodyPicked, this, [this](BodyId bodyId) {
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive) return;
    invalidatePreviewUpdates();
    Body* body = document_.findBody(bodyId);
    if (!body || !body->activeFeature() || !body->resultShape()) {
      statusBar()->showMessage(
          QString::fromUtf8("Выбранное тело не содержит геометрии"), 3000);
      return;
    }
    partDesignCoordinator_.setMoveBody(body->id(), body->activeFeature()->id(),
                             body->resultShape());
    partDesignCoordinator_.finishReselection();
    moveBodyValue_->setText(QString::fromStdString(body->name()));
    viewport_->showMovePreview();
    updateMoveToolPreview();
    static_cast<void>(viewport_->focusToolParameterField(false));
    statusBar()->showMessage(
        QString::fromUtf8("Потяните стрелку X, Y или Z либо введите смещение"));
  });
  connect(viewport_, &Viewport::translationToolManipulatorValueChanged, this,
          [this](int axisIndex, double value) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive)
              return;
            const QSignalBlocker xBlocker(moveXSpin_);
            const QSignalBlocker yBlocker(moveYSpin_);
            const QSignalBlocker zBlocker(moveZSpin_);
            if (axisIndex == 0)
              moveXSpin_->setValue(value);
            else if (axisIndex == 1)
              moveYSpin_->setValue(value);
            else if (axisIndex == 2)
              moveZSpin_->setValue(value);
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId,
                [this, axisIndex, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::Inactive)
                    partDesignCoordinator_.setMoveOffsetComponent(axisIndex, value);
                },
                [this] { updateMoveToolPreview(); });
          });
  connect(viewport_, &Viewport::mirrorBodyPicked, this,
          [this](BodyId bodyId) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            Body* body = document_.findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            partDesignCoordinator_.setMirrorBody(body->id(), body->activeFeature()->id(),
                                       body->resultShape());
            partDesignCoordinator_.finishReselection();
            mirrorBodyValue_->setText(QString::fromStdString(body->name()));
            mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
            mirrorPlaneSelectButton_->setEnabled(true);
            viewport_->beginMirrorPlaneSelection();
            updateMirrorToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую плоскость в 3D-виде"));
          });
  connect(viewport_, &Viewport::mirrorPlanePicked, this,
          [this](int planeIndex) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive ||
                planeIndex < 0 || planeIndex > 2)
              return;
            invalidatePreviewUpdates();
            const auto plane = static_cast<MirrorPlane>(planeIndex);
            partDesignCoordinator_.setMirrorPlane(plane);
            partDesignCoordinator_.finishReselection();
            mirrorPlaneValue_->setText(
                plane == MirrorPlane::XY
                    ? QStringLiteral("XY")
                    : plane == MirrorPlane::XZ ? QStringLiteral("XZ")
                                               : QStringLiteral("YZ"));
            updateMirrorToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("Предпросмотр зеркала построен"), 3000);
          });
  connect(viewport_, &Viewport::linearPatternBodyPicked, this,
          [this](BodyId bodyId) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            Body* body = document_.findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            partDesignCoordinator_.setLinearPatternBody(
                body->id(), body->activeFeature()->id(), body->resultShape());
            partDesignCoordinator_.finishReselection();
            linearPatternBodyValue_->setText(
                QString::fromStdString(body->name()));
            linearPatternAxisValue_->setText(
                QString::fromUtf8("Не выбрано"));
            linearPatternAxisSelectButton_->setEnabled(true);
            viewport_->beginLinearPatternAxisSelection();
            updateLinearPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(viewport_, &Viewport::linearPatternAxisPicked, this,
          [this](int axisIndex) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                    ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            invalidatePreviewUpdates();
            const auto direction = static_cast<PrincipalAxis>(axisIndex);
            partDesignCoordinator_.setLinearPatternDirection(direction);
            partDesignCoordinator_.finishReselection();
            linearPatternAxisValue_->setText(
                axisIndex == 0 ? QStringLiteral("X")
                : axisIndex == 1 ? QStringLiteral("Y")
                                 : QStringLiteral("Z"));
            viewport_->showLinearPatternAxisSelection(axisIndex);
            updateLinearPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("Предпросмотр линейного массива построен"),
                3000);
          });
  connect(viewport_, &Viewport::circularPatternBodyPicked, this,
          [this](BodyId bodyId) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            Body* body = document_.findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            partDesignCoordinator_.setCircularPatternBody(
                body->id(), body->activeFeature()->id(), body->resultShape());
            partDesignCoordinator_.finishReselection();
            circularPatternBodyValue_->setText(
                QString::fromStdString(body->name()));
            circularPatternAxisValue_->setText(
                QString::fromUtf8("Не выбрано"));
            circularPatternAxisSelectButton_->setEnabled(true);
            viewport_->beginCircularPatternAxisSelection();
            updateCircularPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(viewport_, &Viewport::circularPatternAxisPicked, this,
          [this](int axisIndex) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                    ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            invalidatePreviewUpdates();
            const auto axis = static_cast<PrincipalAxis>(axisIndex);
            partDesignCoordinator_.setCircularPatternAxis(axis);
            partDesignCoordinator_.finishReselection();
            circularPatternAxisValue_->setText(
                axisIndex == 0 ? QStringLiteral("X")
                : axisIndex == 1 ? QStringLiteral("Y")
                                 : QStringLiteral("Z"));
            viewport_->showCircularPatternAxisSelection(axisIndex);
            updateCircularPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("Предпросмотр кругового массива построен"),
                3000);
          });
  connect(viewport_, &Viewport::draftAxisPicked, this,
          [this](int axisIndex) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).lifecycle == ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            invalidatePreviewUpdates();
            if (!partDesignCoordinator_.setDraftPrincipalAxis(document_, axisIndex)) return;
            partDesignCoordinator_.finishReselection();
            viewport_->showDraftAxisSelection(axisIndex);
            updateDraftToolPreview();
            static_cast<void>(viewport_->focusToolParameterField(false));
            statusBar()->showMessage(QString::fromUtf8(
                "3/3 Потяните дугу или введите угол и нажмите Enter"));
          });
  connect(viewport_, &Viewport::draftEdgeAxisPicked, this,
          [this](const EdgeReference& edge) {
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).lifecycle == ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            if (!partDesignCoordinator_.setDraftRotationEdge(document_, edge)) {
              updateDraftToolPreview();
              viewport_->beginDraftAxisSelection();
              statusBar()->showMessage(QString::fromUtf8(
                  "Ребро должно быть прямым и принадлежать выбранной поверхности"));
              return;
            }
            partDesignCoordinator_.finishReselection();
            viewport_->showDraftEdgeAxisSelection(edge);
            updateDraftToolPreview();
            static_cast<void>(viewport_->focusToolParameterField(false));
            statusBar()->showMessage(QString::fromUtf8(
                "3/3 Потяните дугу или введите угол и нажмите Enter"));
          });
  connect(viewport_, &Viewport::selectionChanged, this,
          [this](const QString& text) {
            statusBar()->showMessage(text.isEmpty()
                                         ? QString::fromUtf8("Выделение снято")
                                         : text);
          });
  connect(viewport_, &Viewport::interactionCancelled, this,
          [this](ViewportCancelReason reason) {
            const auto effect = partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::Escape);
            if (effect.transition.effect != PartDesignTransitionEffect::None) {
              applyPartDesignUiEffect(effect);
              statusBar()->showMessage(
                  reason == ViewportCancelReason::NestedReselection
                      ? QString::fromUtf8("Повторный выбор отменён")
                      : QString::fromUtf8("Инструмент отменён"),
                  2000);
              return;
            }
            viewport_->clearLegacyExtrusionPreview();
            selectedExtrusionSource_.reset();
            if (extrusionDock_) extrusionDock_->hide();
            modelRibbon_->clearActiveTool();
            if (reason == ViewportCancelReason::SketchPlaneSelection)
              rebuildFeatureTree();
            statusBar()->showMessage(QString::fromUtf8("Инструменты сброшены"),
                                     2000);
          });
  modelTreeDock_->setWidget(featureTree_);
  addDockWidget(Qt::LeftDockWidgetArea, modelTreeDock_);

  historyDock_ =
      new QDockWidget(QString::fromUtf8("История построений"), this);
  historyDock_->setObjectName(QStringLiteral("historyDock"));
  historyDock_->setAllowedAreas(Qt::BottomDockWidgetArea);
  historyDock_->setFeatures(QDockWidget::DockWidgetClosable);
  historyDock_->setMinimumHeight(96);
  historyDock_->setMaximumHeight(108);
  auto* historyHost = new QWidget(historyDock_);
  auto* historyHostLayout = new QVBoxLayout(historyHost);
  historyHostLayout->setContentsMargins(8, 2, 8, 3);
  historyHostLayout->setSpacing(1);
  historyScroll_ = new QScrollArea(historyDock_);
  historyScroll_->setWidgetResizable(true);
  historyScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  historyScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  historyScroll_->setFrameShape(QFrame::NoFrame);
  historyTimeline_ = new HistoryTimelineWidget(historyScroll_);
  historyContent_ = historyTimeline_;
  historyLayout_ = historyTimeline_->stepLayout();
  historyScroll_->setWidget(historyContent_);
  connect(historyTimeline_, &HistoryTimelineWidget::positionChanged, this,
          [this](int position) {
            resetTransientModelingUi();
            applyHistoryPosition(position);
          });
  historyHostLayout->addWidget(historyScroll_, 1);
  historyDock_->setWidget(historyHost);
  addDockWidget(Qt::BottomDockWidgetArea, historyDock_);
  rebuildHistoryPanel();

  statusBar()->showMessage(
      QString::fromUtf8("Готово к параметрическому моделированию"));
}

void MainWindow::updateFromSketch(double widthMm, double heightMm) {
  invalidatePreviewUpdates();
  setWindowModified(true);
  if (widthMm <= 0.0 || heightMm <= 0.0) {
    statusBar()->showMessage(QString::fromUtf8("Эскиз пуст"));
    return;
  }
  // Editing a sketch must not resize an existing solid. Body dimensions are
  // changed only when a modelling operation (for example extrusion) commits.
  viewport_->setSketch(sketchCanvas_->sketch(), currentSketchPlacement_);
  drawingSheet_->setRectangle(widthMm, heightMm);
  statusBar()->showMessage(
      QString::fromUtf8("Геометрия эскиза обновлена"), 1200);
}

void MainWindow::finishSketch() {
  invalidatePreviewUpdates();
  const auto& sketch = sketchCanvas_->sketch();
  const HistorySelectionState previousSelection = captureHistorySelection();
  QString completionMessage =
      QString::fromUtf8("Эскиз завершён — модель перестроена");
  if (sketch.lines().empty() && sketch.circles().empty() &&
      sketch.arcs().empty()) {
    workspaceStack_->setCurrentWidget(viewport_);
    statusBar()->showMessage(
        QString::fromUtf8("Пустой эскиз закрыт без сохранения"), 3000);
    return;
  }
  if (editingSketchIndex_) {
    const std::size_t index = *editingSketchIndex_;
    if (index >= sketchViews_.size() ||
        !document_.findSketch(sketchViews_[index].documentSketchId)) {
      editingSketchIndex_.reset();
      workspaceStack_->setCurrentWidget(viewport_);
      statusBar()->showMessage(
          QString::fromUtf8("Эскиз больше не существует в документе"), 4000);
      return;
    }
  }
  updateFromSketch(sketch.widthMm(), sketch.heightMm());
  if (editingSketchIndex_) {
    const std::size_t index = *editingSketchIndex_;
    const SketchId sketchId = sketchViews_[index].documentSketchId;
    Document previousDocument = document_;
    if (!document_.replaceSketchGeometry(sketchId, sketch)) {
      editingSketchIndex_.reset();
      workspaceStack_->setCurrentWidget(viewport_);
      statusBar()->showMessage(
          QString::fromUtf8("Эскиз больше не существует в документе"), 4000);
      return;
    }
    document_.findSketch(sketchId)->placement = currentSketchPlacement_;
    const bool recomputeSucceeded = document_.recompute();
    if (!recomputeSucceeded) {
      completionMessage =
          QString::fromUtf8(
              "Эскиз обновлён, перестроение модели завершилось с ошибкой: %1")
              .arg(QString::fromStdString(document_.rebuildError()));
    }
    auto committedAfter = captureCommittedEndState();
    if (!pushModelTransition(std::move(previousDocument), previousSelection,
                             std::move(committedAfter)))
      return;
    refreshBodyViewFromDocument();
    const auto* committedSketch = document_.findSketch(sketchId);
    viewport_->updateSketch(index, sketch,
                            sketchPresentationLabel(*committedSketch),
                            currentSketchPlacement_);
    if (hasHistoricalLegacyExtrusion() &&
        historicalLegacyExtrusionSourceSketchId_ == sketchId)
      viewport_->setSolidSketch(sketch, currentSketchPlacement_);
  } else {
    Document previousDocument = document_;
    auto& modelSketch = document_.addSketch(
        "Sketch " + std::to_string(document_.sketches().size() + 1));
    modelSketch.geometry = sketch;
    modelSketch.placement = currentSketchPlacement_;
    if (currentSketchFaceReference_)
      document_.attachSketchToFace(modelSketch.id,
                                   *currentSketchFaceReference_);
    const SketchId modelSketchId = modelSketch.id;
    auto committedAfter = captureCommittedEndState();
    committedAfter.sketchViewIds.push_back(modelSketchId);
    committedAfter.presentation.sketchVisibilities.push_back(1U);
    if (!pushModelTransition(std::move(previousDocument), previousSelection,
                             std::move(committedAfter)))
      return;
    viewport_->addSketch(modelSketchId, sketch,
                         sketchPresentationLabel(modelSketch),
                         currentSketchPlacement_);
    sketchViews_.push_back({modelSketchId});
  }
  editingSketchIndex_.reset();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
  completeModelTransitionUi();
  statusBar()->showMessage(completionMessage, 5000);
}

void MainWindow::normalizeExtrusionDistance() {
  const auto input = normalizeExtrusionInput(
      extrusionLengthSpin_->value(), extrusionReverseCheck_->isChecked());
  const QSignalBlocker distanceBlocker(extrusionLengthSpin_);
  const QSignalBlocker reverseBlocker(extrusionReverseCheck_);
  extrusionLengthSpin_->setValue(input.distanceMm);
  extrusionReverseCheck_->setChecked(input.reversed);
  viewport_->setExtrusionPreviewLength(input.reversed ? -input.distanceMm
                                                       : input.distanceMm);
  scheduleAutomaticExtrudeOperation();
}

std::optional<ExtrudeOperation>
MainWindow::detectAutomaticExtrudeOperation() const {
  if (extrudeOperationManuallyChanged_) return std::nullopt;
  ExtrudeOperation operation = ExtrudeOperation::NewBody;
  const Body* body = document_.activeBody();
  const std::size_t index = viewport_->extrusionCandidateSketchIndex();
  const DocumentSketch* profile =
      index < sketchViews_.size()
          ? document_.findSketch(sketchViews_[index].documentSketchId)
          : nullptr;
  const auto targetShape = body ? body->resultShape() : ShapeFeature::ShapePtr{};
  if (profile && targetShape) {
    DocumentSketch operationProfile = *profile;
    const auto& pickedProfile = viewport_->extrusionCandidateSketch();
    if (!pickedProfile.lines().empty() || !pickedProfile.circles().empty() ||
        !pickedProfile.arcs().empty())
      operationProfile.geometry = pickedProfile;
    operation = detectExtrudeOperation(
        operationProfile, extrusionLengthSpin_->value(),
        extrusionReverseCheck_->isChecked(), targetShape.get(),
        profile->support.type == SketchSupportType::Face);
  }
  return operation;
}

void MainWindow::updateAutomaticExtrudeOperation() {
  const auto operation = detectAutomaticExtrudeOperation();
  if (!operation) return;
  const QSignalBlocker blocker(extrusionOperationCombo_);
  extrusionOperationCombo_->setCurrentIndex(static_cast<int>(*operation));
}

void MainWindow::scheduleAutomaticExtrudeOperation() {
  if (extrudeOperationManuallyChanged_) return;
  const Body* body = document_.activeBody();
  const BodyId bodyId = body ? body->id() : kInvalidBodyId;
  const FeatureId featureId = body && body->activeFeature()
                                  ? body->activeFeature()->id()
                                  : kInvalidFeatureId;
  const ShapeRevision shapeRevision =
      body && body->activeFeature() ? body->activeFeature()->shapeRevision()
                                    : kInvalidShapeRevision;
  AutomaticExtrudeDetectionKey key{
      previewDocumentGeneration_, bodyId, featureId, shapeRevision,
      viewport_->extrusionCandidateSketchIndex(),
      viewport_->extrusionCandidateSupport(),
      viewport_->extrusionPreviewBaseBounds(),
      extrusionLengthSpin_->value(), extrusionReverseCheck_->isChecked()};
  if (automaticExtrudeDetectionKey_ == key) return;
  automaticExtrudeDetectionKey_ = std::move(key);
  auto detected = std::make_shared<std::optional<ExtrudeOperation>>();
  schedulePreviewUpdate(
      bodyId, featureId,
      [this, detected] {
        ++automaticExtrudeDetectionCount_;
        *detected = detectAutomaticExtrudeOperation();
      },
      [this, detected] {
        if (!*detected) return;
        const QSignalBlocker blocker(extrusionOperationCombo_);
        extrusionOperationCombo_->setCurrentIndex(
            static_cast<int>(**detected));
      });
}

void MainWindow::extrudeSketch() {
  flushPreviewUpdate();
  invalidatePreviewUpdates();
  if (!ensureHistoryAtEnd()) return;
  const auto* legacySource =
      selectedExtrusionSource_
          ? std::get_if<LegacySolidFacePick>(
                &selectedExtrusionSource_->source)
          : nullptr;
  const auto& pickedSketch = viewport_->extrusionCandidateSketch();
  const std::size_t pickedIndex = viewport_->extrusionCandidateSketchIndex();
  const std::size_t sourceIndex =
      pickedIndex != static_cast<std::size_t>(-1)
          ? pickedIndex
          : !sketchViews_.empty() ? sketchViews_.size() - 1
                             : static_cast<std::size_t>(-1);
  DocumentSketch* modelSketch =
      sourceIndex < sketchViews_.size()
          ? document_.findSketch(sketchViews_[sourceIndex].documentSketchId)
          : nullptr;
  // The viewport owns the user's exact region pick.  The source DocumentSketch
  // may contain several independent contours, so preferring the whole sketch
  // here loses the selection and makes Apply fail with "one profile at a time".
  const bool hasPickedProfile =
      !pickedSketch.lines().empty() || !pickedSketch.circles().empty() ||
      !pickedSketch.arcs().empty();
  const sketch::Sketch sketch =
      hasPickedProfile
          ? pickedSketch
          : modelSketch ? modelSketch->geometry : sketchCanvas_->sketch();
  if (sketch.lines().empty() && sketch.circles().empty() &&
      sketch.arcs().empty()) {
    QMessageBox::information(this, QString::fromUtf8("Выдавливание"),
                             QString::fromUtf8("Сначала создайте замкнутый контур эскиза."));
    return;
  }
  // Use the same authoritative multi-region validation as preview and final
  // recompute. Sketch::isClosed() is a single-sketch convenience check and,
  // in particular, rejects a valid selection containing both circular and
  // line/Arc regions.
  DocumentSketch selectedProfile = modelSketch ? *modelSketch : DocumentSketch{};
  selectedProfile.geometry = sketch;
  selectedProfile.placement = legacySource
                                  ? legacySource->placement
                                  : modelSketch ? modelSketch->placement
                                                : currentSketchPlacement_;
  if (selectedProfile.id == kInvalidSketchId) selectedProfile.id = 1;
  std::string profileError;
  OperationFailureCode profileFailureCode{OperationFailureCode::None};
  if (!isSupportedSketchProfile(selectedProfile, &profileError,
                                &profileFailureCode)) {
    const bool openContour =
        profileFailureCode == OperationFailureCode::InvalidProfileOpen;
    QMessageBox::warning(
        this,
        openContour ? QString::fromUtf8("Контур не замкнут")
                    : QString::fromUtf8("Некорректный профиль"),
        openContour
            ? QString::fromUtf8(
                  "Соедините конечные точки линий замкнутого контура.")
            : profileFailureCode == OperationFailureCode::InvalidProfileOverlap
                  ? QString::fromUtf8(
                        "Выбранные области пересекаются или образуют отверстие.")
                  : QString::fromUtf8("Выбранный профиль нельзя выдавить."));
    return;
  }

  const auto input = normalizeExtrusionInput(
      extrusionLengthSpin_->value(), extrusionReverseCheck_->isChecked());
  const double height = input.distanceMm;
  if (!std::isfinite(height) || height == 0.0) {
    QMessageBox::warning(this, QString::fromUtf8("Некорректная длина"),
                         QString::fromUtf8("Длина выдавливания не должна быть равна нулю."));
    return;
  }
  const auto operation = static_cast<ExtrudeOperation>(
      std::clamp(extrusionOperationCombo_->currentIndex(), 0, 2));
  const bool reversed = input.reversed;
  if (operation != ExtrudeOperation::NewBody && !document_.activeBody()) {
    QMessageBox::warning(this, QString::fromUtf8("Выдавливание"),
                         QString::fromUtf8(
                             "Для объединения или вырезания нужен активный Body."));
    return;
  }

  Document previousDocument = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (!modelSketch || legacySource) {
    modelSketch = &document_.addSketch(
        "Extrude profile " + std::to_string(document_.sketches().size() + 1));
    modelSketch->geometry = sketch;
    modelSketch->placement = selectedProfile.placement;
  }
  Body* modelBody = operation == ExtrudeOperation::NewBody
                        ? &document_.addBody()
                        : document_.activeBody();
  auto extrudeFeature = std::make_unique<ExtrudeFeature>(
      modelSketch->id, height,
      "Extrude " + std::to_string(modelBody->features().size() + 1),
      operation, reversed);
  // If this feature references an existing sketch, persist the exact selected
  // region inside the feature.  Do not create a hidden duplicate DocumentSketch.
  if (hasPickedProfile)
    extrudeFeature->setProfileOverride(sketch);
  modelBody->addFeature(std::move(extrudeFeature));
  if (!document_.rebuild()) {
    const std::string error = document_.rebuildError();
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    QMessageBox::warning(this, QString::fromUtf8("Ошибка выдавливания"),
                         QString::fromStdString(error));
    return;
  }
  // The v1 prism is only a compatibility presentation. Once the user commits
  // a real parametric Body it must never reappear after hiding or deleting the
  // modern result. Undo restores the stable source ID through
  // HistorySelectionState.
  auto committedAfter = captureCommittedEndState();
  committedAfter.historicalLegacyExtrusionSourceSketchId.reset();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  historicalLegacyExtrusionSourceSketchId_.reset();
  refreshBodyViewFromDocument();
  viewport_->setSketch(sketch);
  selectedExtrusionSource_.reset();
  viewport_->hideExtrusionManipulator();
  extrusionDock_->hide();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Создано твёрдое тело: выдавливание %1 мм").arg(height), 4000);
}

void MainWindow::syncSketchPresentationFromDocument() {
  if (historicalLegacyExtrusionSourceSketchId_ &&
      !document_.findSketch(*historicalLegacyExtrusionSourceSketchId_))
    historicalLegacyExtrusionSourceSketchId_.reset();
  for (std::size_t index = sketchViews_.size(); index-- > 0;) {
    if (document_.findSketch(sketchViews_[index].documentSketchId)) continue;
    sketchViews_.erase(sketchViews_.begin() +
                       static_cast<std::ptrdiff_t>(index));
    if (editingSketchIndex_) {
      if (*editingSketchIndex_ == index)
        editingSketchIndex_.reset();
      else if (*editingSketchIndex_ > index)
        --*editingSketchIndex_;
    }
  }

  std::vector<SketchPresentationSnapshot> snapshots;
  snapshots.reserve(sketchViews_.size());
  for (const auto& view : sketchViews_) {
    const auto* modelSketch = document_.findSketch(view.documentSketchId);
    if (!modelSketch) continue;
    snapshots.push_back({modelSketch->id, modelSketch->geometry,
                         sketchPresentationLabel(*modelSketch),
                         modelSketch->placement});
  }
  viewport_->replaceSketchPresentations(std::move(snapshots));
}

bool MainWindow::hasCommittedModernSolid() const noexcept {
  return std::any_of(document_.bodies().begin(), document_.bodies().end(),
                     [](const Body& body) {
                       return !body.features().empty();
                     });
}

std::optional<MainWindow::ModernSolidPresentation>
MainWindow::resolveModernSolidPresentation(
    const Body& body, const ShapeFeature::ShapePtr& candidate,
    bool effectivelyVisible) {
  if (!effectivelyVisible || !candidate || candidate->IsNull())
    return std::nullopt;
  const ShapeFeature* owner = presentationOwner(body, candidate);
  if (!owner) return std::nullopt;
  std::string topologyError;
  auto topology = owner->lastValidTopologyIndex(&topologyError);
  if (!topology) return std::nullopt;
  return ModernSolidPresentation{owner, candidate, std::move(topology)};
}

bool MainWindow::hasDisplayableModernSolid() const {
  return std::any_of(
      document_.bodies().begin(), document_.bodies().end(),
      [](const Body& body) {
        return resolveModernSolidPresentation(
                   body, body.lastValidResultShape(), body.visible())
            .has_value();
      });
}

bool MainWindow::hasExportableModernSolid(QString* error) const {
  const auto result = projectApplicationService_.preflightStlExport(document_);
  if (!result.succeeded() && error) *error = result.detail;
  return result.succeeded();
}

bool MainWindow::hasHistoricalLegacyExtrusion() const noexcept {
  return historicalLegacyExtrusionSourceSketchId_ &&
         document_.findSketch(*historicalLegacyExtrusionSourceSketchId_);
}

void MainWindow::refreshBodyViewFromDocument(
    const std::vector<BodyId>& transientVisibleBodies) {
  if (historicalLegacyExtrusionSourceSketchId_ &&
      !document_.findSketch(*historicalLegacyExtrusionSourceSketchId_))
    historicalLegacyExtrusionSourceSketchId_.reset();
  std::vector<BodyViewShape> shapes;
  for (const Body& body : document_.bodies()) {
    // Rendering explicitly uses the last validated committed result. The
    // authoritative resultShape() stays empty while the active Feature is in
    // Error, so tool/dependency code cannot consume this presentation fallback.
    const bool effectivelyVisible =
        body.visible() ||
        std::find(transientVisibleBodies.begin(), transientVisibleBodies.end(),
                  body.id()) != transientVisibleBodies.end();
    auto presentation = resolveModernSolidPresentation(
        body, body.lastValidResultShape(), effectivelyVisible);
    if (!presentation) continue;
    shapes.push_back(
        {body.id(), presentation->owner->id(), std::move(presentation->shape),
         std::move(presentation->topologyIndex),
         presentation->owner->shapeRevision()});
  }
  bool showHistoricalLegacyExtrusion = false;
  const bool anyVisibleBodyShape = !shapes.empty();
  viewport_->setBodyShapes(std::move(shapes));
  if (document_.bodies().empty() && hasHistoricalLegacyExtrusion()) {
    const auto* sourceSketch = document_.findSketch(
        *historicalLegacyExtrusionSourceSketchId_);
    // resetScene() deliberately clears the cached legacy mesh. Reconstruct it
    // from the authoritative Sketch selected by stable ID on every refresh.
    viewport_->setSolidSketch(sourceSketch->geometry, sourceSketch->placement);
    viewport_->setSolidSupport(sketchPresentationLabel(*sourceSketch));
    showHistoricalLegacyExtrusion = true;
  }
  // Keep document occupancy and viewport visibility separate.  A hidden last
  // Body still means the document contains Part Design geometry, but the
  // viewport must not enable its legacy box fallback after setBodyShapes({})
  // has cleared the B-Rep mesh.
  viewport_->setSolidVisible(anyVisibleBodyShape ||
                             showHistoricalLegacyExtrusion);
  drawingSheet_->setDocument(document_);
  // A view entry can only observe an existing committed sketch. If an
  // interrupted/corrupt transition leaves a stale ID, discard the derived
  // presentation and repair its positional UI references; never keep drawing
  // the old cached geometry or map the entry to a different sketch.
  for (std::size_t index = sketchViews_.size(); index-- > 0;) {
    if (document_.findSketch(sketchViews_[index].documentSketchId)) continue;
    viewport_->removeSketch(index);
    sketchViews_.erase(sketchViews_.begin() +
                       static_cast<std::ptrdiff_t>(index));
    if (editingSketchIndex_) {
      if (*editingSketchIndex_ == index)
        editingSketchIndex_.reset();
      else if (*editingSketchIndex_ > index)
        --*editingSketchIndex_;
    }
  }
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const auto* modelSketch =
        document_.findSketch(sketchViews_[index].documentSketchId);
    if (!modelSketch) continue;
    viewport_->updateSketch(index, modelSketch->geometry,
                            sketchPresentationLabel(*modelSketch),
                            modelSketch->placement);
  }
}

void MainWindow::createRevolve() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  Body* body = document_.activeBody();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginRevolve(
              document_, body ? body->id() : kInvalidBodyId,
              body && body->activeFeature() ? body->activeFeature()->id()
                                            : kInvalidFeatureId,
              body ? body->resultShape() : ShapeFeature::ShapePtr{}),
          PartDesignToolKind::Revolve))
    return;
  {
    const QSignalBlocker blocker(revolveProfileCombo_);
    revolveProfileCombo_->clear();
    revolveProfileCombo_->addItem(QString::fromUtf8("Выбрать профиль"),
                                  QVariant::fromValue<qulonglong>(kInvalidSketchId));
    for (const auto& sketch : document_.sketches())
      revolveProfileCombo_->addItem(QString::fromStdString(sketch.name),
                                    QVariant::fromValue<qulonglong>(sketch.id));
  }
  rebuildRevolveAxisChoices();
  revolveAngleSpin_->setValue(360.0);
  const auto initialOperation = body && body->resultShape()
                                    ? ExtrudeOperation::Join
                                    : ExtrudeOperation::NewBody;
  {
    const QSignalBlocker blocker(revolveOperationCombo_);
    revolveOperationCombo_->setCurrentIndex(
        static_cast<int>(initialOperation));
  }
  partDesignCoordinator_.setRevolveOperation(document_, initialOperation);
  revolveReverseCheck_->setChecked(false);
  revolveProfileSummary_->setText(QString::fromUtf8(
      "Щёлкните область в 3D-виде. Ctrl добавляет или убирает области."));
  revolveDock_->show(); revolveDock_->raise();
  viewport_->beginExtrusionSurfaceSelection();
  updateRevolveToolPreview();
  statusBar()->showMessage(
      QString::fromUtf8("Выберите профиль мышью во viewport"));
}

void MainWindow::updateRevolveProfileSelection(
    const ExtrusionSourcePick& pick) {
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::Inactive) return;
  invalidatePreviewUpdates();

  const auto* region = std::get_if<SketchRegionPick>(&pick.source);
  const sketch::Sketch emptyProfile;
  const auto& selectedProfile = region ? region->geometry : emptyProfile;
  const bool empty = selectedProfile.lines().empty() &&
                     selectedProfile.circles().empty() &&
                     selectedProfile.arcs().empty();
  const SketchId id = region ? region->sketchId : kInvalidSketchId;
  if (empty || id == kInvalidSketchId || !document_.findSketch(id)) {
    partDesignCoordinator_.clearRevolveAxis(document_);
    partDesignCoordinator_.clearRevolveProfile(document_);
    {
      const QSignalBlocker blocker(revolveProfileCombo_);
      revolveProfileCombo_->setCurrentIndex(0);
    }
    revolveProfileSummary_->setText(QString::fromUtf8(
        "Профиль не выбран. Щёлкните область в 3D-виде."));
    viewport_->beginExtrusionSurfaceSelection();
    updateRevolveToolPreview();
    statusBar()->showMessage(QString::fromUtf8("Выберите профиль вращения"));
    return;
  }

  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId != id &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).axisReference &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).axisReference->type != AxisReferenceType::GlobalX &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).axisReference->type != AxisReferenceType::GlobalY &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).axisReference->type != AxisReferenceType::GlobalZ)
    partDesignCoordinator_.clearRevolveAxis(document_);
  partDesignCoordinator_.setRevolveProfile(document_, id, selectedProfile);
  partDesignCoordinator_.finishReselection();
  {
    const QSignalBlocker blocker(revolveProfileCombo_);
    revolveProfileCombo_->setCurrentIndex(
        revolveProfileCombo_->findData(QVariant::fromValue<qulonglong>(id)));
  }
  rebuildRevolveAxisChoices();
  const auto historyEntry = std::find_if(
      sketchViews_.begin(), sketchViews_.end(),
      [id](const SketchViewEntry& entry) {
        return entry.documentSketchId == id;
      });
  if (historyEntry == sketchViews_.end()) return;
  const std::size_t sketchIndex = static_cast<std::size_t>(
      std::distance(sketchViews_.begin(), historyEntry));
  viewport_->beginRevolveAxisSelection(sketchIndex);
  const std::size_t count = viewport_->selectedProfileRegionCount();
  revolveProfileSummary_->setText(
      QString::fromUtf8("Выбрано областей: %1. Ctrl — добавить или убрать.")
          .arg(count));
  updateRevolveToolPreview();
  statusBar()->showMessage(QString::fromUtf8(
      "Профили выбраны. Ctrl добавляет области; щёлкните ось или прямую эскиза."));
}

void MainWindow::rebuildRevolveAxisChoices() {
  const auto sketchId = static_cast<SketchId>(
      revolveProfileCombo_->currentData().toULongLong());
  const QSignalBlocker blocker(revolveAxisCombo_);
  revolveAxisCombo_->clear();
  revolveAxisCombo_->addItem(QString::fromUtf8("Выбрать ось"), 0);
  revolveAxisCombo_->addItem(QStringLiteral("Global X"),
                             QVariant::fromValue<qulonglong>(Viewport::kGlobalXAxisToken));
  revolveAxisCombo_->addItem(QStringLiteral("Global Y"),
                             QVariant::fromValue<qulonglong>(Viewport::kGlobalYAxisToken));
  revolveAxisCombo_->addItem(QStringLiteral("Global Z"),
                             QVariant::fromValue<qulonglong>(Viewport::kGlobalZAxisToken));
  if (sketchId == kInvalidSketchId) return;
  revolveAxisCombo_->addItem(QString::fromUtf8("Горизонтальная ось"), 1);
  revolveAxisCombo_->addItem(QString::fromUtf8("Вертикальная ось"), 2);
  const auto* sketch = document_.findSketch(sketchId);
  if (!sketch) return;
  for (std::size_t i = 0; i < sketch->geometry.lines().size(); ++i)
    revolveAxisCombo_->addItem(QString::fromUtf8("Линия %1").arg(i + 1),
        QVariant::fromValue<qulonglong>(sketch->geometry.lineId(i) + 3));
}

void MainWindow::updateRevolveToolPreview() {
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::Inactive) return;
  const bool valid = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::PreviewValid;
  revolveAcceptButton_->setEnabled(valid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).previewShape) {
    viewport_->setToolPreviewPresentation(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).extrudeOperation == ExtrudeOperation::NewBody
            ? ToolPreviewPresentation::OverlaySourceSelection
            : ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).bodyId,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).sourceFeatureId, partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).previewShape);
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).extrudeOperation != ExtrudeOperation::NewBody)
      viewport_->setToolPreviewReplacedBodies(
          {partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).bodyId});
  } else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).angularManipulator)
    viewport_->setAngularToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle == ToolLifecycle::PreviewInvalid) {
    revolveStepHint_->setText(QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).error));
    revolveStepHint_->setProperty("uiRole", "danger");
    statusBar()->showMessage(QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).error));
  } else {
    const QString hint = valid
        ? partDesignToolStepHint(PartDesignToolKind::Revolve,
                                 ToolSelectionStage::EditingParameters)
        : partDesignToolStepHint(PartDesignToolKind::Revolve,
                                 partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).selectionStage);
    revolveStepHint_->setText(QString::fromUtf8("Сейчас: ") + hint);
    revolveStepHint_->setProperty("uiRole", "secondaryText");
  }
  revolveStepHint_->style()->unpolish(revolveStepHint_);
  revolveStepHint_->style()->polish(revolveStepHint_);
}

void MainWindow::cancelRevolveTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptRevolveTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle != ToolLifecycle::PreviewValid ||
      !partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).axisReference) return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  Body* body = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).editingFeatureId
                   ? document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).bodyId)
                   : partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).extrudeOperation == ExtrudeOperation::NewBody
                         ? &document_.addBody()
                         : document_.activeBody();
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* feature = dynamic_cast<RevolveFeature*>(body->features()[index].get());
      if (!feature || feature->id() != *editingId) continue;
      feature->setProfileSketchId(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId);
      feature->setProfileOverride(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileOverride);
      feature->setAxis(*partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).axisReference);
      feature->setAngleDeg(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).angleDeg);
      feature->setOperation(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).extrudeOperation);
      feature->setReversed(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).reversed);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    auto feature = std::make_unique<RevolveFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileSketchId, *partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).axisReference,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).angleDeg,
        "Revolve " + std::to_string(body->features().size() + 1),
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).extrudeOperation, partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).reversed);
    feature->setProfileOverride(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).profileOverride);
    body->addFeature(std::move(feature));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous; refreshBodyViewFromDocument();
    statusBar()->showMessage(error); return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree(); rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::createPocket() {
  if (!ensureHistoryAtEnd()) return;
  Body* body = document_.activeBody();
  if (!body || !body->resultShape() || sketchViews_.empty()) {
    QMessageBox::information(this, QString::fromUtf8("Карман"),
                             QString::fromUtf8("Сначала создайте Body и эскиз на его грани."));
    return;
  }
  const auto& historySketch = sketchViews_.back();
  DocumentSketch* profile = document_.findSketch(historySketch.documentSketchId);
  if (!profile || profile->support.type != SketchSupportType::Face ||
      !profile->supportResolved || !profile->geometry.isClosed()) {
    QMessageBox::warning(this, QString::fromUtf8("Карман"),
                         QString::fromUtf8("Нужен замкнутый эскиз на разрешённой плоской грани."));
    return;
  }
  bool accepted = false;
  const double depth = QInputDialog::getDouble(
      this, QString::fromUtf8("Создать карман"),
      QString::fromUtf8("Глубина, мм:"), 20.0, 0.01, 100000.0, 2, &accepted);
  if (!accepted) return;
  static_cast<void>(acceptPocketTransition(profile->id, depth));
}

bool MainWindow::acceptPocketTransition(
    SketchId profileId, double depthMm,
    std::optional<FeatureId> editingFeatureId) {
  Body* body = document_.activeBody();
  if (editingFeatureId)
    for (const auto& candidate : document_.bodies())
      if (candidate.featureIndex(*editingFeatureId)) {
        body = document_.findBody(candidate.id());
        break;
      }
  if (!body || !document_.findSketch(profileId) || depthMm <= 0.0)
    return false;
  Document previousDocument = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (editingFeatureId) {
    auto* pocket = dynamic_cast<PocketFeature*>(
        document_.findFeature(*editingFeatureId));
    if (!pocket) return false;
    pocket->setDepthMm(depthMm);
  } else {
    body->addFeature(std::make_unique<PocketFeature>(
        profileId, depthMm,
        "Pocket " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = std::move(previousDocument);
    refreshBodyViewFromDocument();
    statusBar()->showMessage(error, 4000);
    return false;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return false;
  refreshBodyViewFromDocument();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      editingFeatureId
          ? QString::fromUtf8("Глубина кармана изменена: %1 мм").arg(depthMm)
          : QString::fromUtf8("Создан карман глубиной %1 мм").arg(depthMm),
      editingFeatureId ? 3000 : 4000);
  return true;
}

void MainWindow::createFillet() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto edges = viewport_->selectedBodyEdges();
  Body* body = edges.empty() ? document_.activeBody()
                             : document_.findBody(edges.front().bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) {
    QMessageBox::information(this, QString::fromUtf8("Скругление"),
                             QString::fromUtf8("Сначала создайте тело."));
    return;
  }
  if (!edges.empty() && body->activeFeature()->id() != edges.front().featureId)
    edges.clear();

  for (const auto& edge : edges)
    if (edge.bodyId != body->id() ||
        edge.featureId != body->activeFeature()->id()) {
      QMessageBox::warning(this, QString::fromUtf8("Скругление"),
                           QString::fromUtf8("Рёбра должны принадлежать одному телу"));
      return;
    }
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginFillet(
              body->id(), body->activeFeature()->id(), body->resultShape(),
              edges, 0.0, std::nullopt,
              body->activeFeature()->topologyIndex()),
          PartDesignToolKind::Fillet))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  viewport_->setSelectedBodyEdges(edges);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Fillet),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Радиус"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.0, 100000.0, 2);
  toolParametersPanel_->setParameterValue(0.0);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFilletToolPreview();
  if (!edges.empty())
    static_cast<void>(viewport_->focusToolParameterField(false));
  if (edges.empty())
    statusBar()->showMessage(
        QString::fromUtf8("Нажмите «Выбрать» и укажите рёбра в viewport"));
}

void MainWindow::editPatternFeature(FeatureId featureId,
                                    FeatureEditorRoute route) {
  Body* owner = document_.findBodyForFeature(featureId);
  ShapeFeature* feature = document_.findFeature(featureId);
  const auto featureIndex =
      owner ? owner->featureIndex(featureId) : std::nullopt;
  if (!owner || !feature || !featureIndex) return;
  Body& body = *owner;
  const std::size_t index = *featureIndex;
      if (route == FeatureEditorRoute::Revolve) {
        auto* revolve = dynamic_cast<RevolveFeature*>(feature);
        if (!revolve) return;
        ShapeFeature::ShapePtr upstream = index == 0
                                             ? ShapeFeature::ShapePtr{}
                                             : body.features()[index - 1]->shape();
        const FeatureId sourceId = index == 0
                                       ? kInvalidFeatureId
                                       : body.features()[index - 1]->id();
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginRevolve(
                    document_, body.id(), sourceId, upstream, revolve->id()),
                PartDesignToolKind::Revolve))
          return;
        partDesignCoordinator_.setRevolveProfile(document_, revolve->profileSketchId(),
                                       revolve->profileOverride());
        partDesignCoordinator_.setRevolveAxis(document_, revolve->axis());
        partDesignCoordinator_.setRevolveAngleFromPanel(document_, revolve->angleDeg());
        partDesignCoordinator_.setRevolveOperation(document_, revolve->operation());
        partDesignCoordinator_.setRevolveReversed(document_, revolve->reversed());
        {
          const QSignalBlocker profileBlocker(revolveProfileCombo_);
          const QSignalBlocker angleBlocker(revolveAngleSpin_);
          const QSignalBlocker operationBlocker(revolveOperationCombo_);
          const QSignalBlocker reverseBlocker(revolveReverseCheck_);
          revolveProfileCombo_->setCurrentIndex(revolveProfileCombo_->findData(
              QVariant::fromValue<qulonglong>(revolve->profileSketchId())));
          revolveAngleSpin_->setValue(revolve->angleDeg());
          revolveOperationCombo_->setCurrentIndex(
              static_cast<int>(revolve->operation()));
          revolveReverseCheck_->setChecked(revolve->reversed());
        }
        revolveProfileSummary_->setText(
            revolve->profileOverride()
                ? QString::fromUtf8("Сохранён выбранный набор областей")
                : QString::fromUtf8("Выбран весь эскиз"));
        rebuildRevolveAxisChoices();
        qulonglong axisToken = 0;
        switch (revolve->axis().type) {
          case AxisReferenceType::GlobalX: axisToken = Viewport::kGlobalXAxisToken; break;
          case AxisReferenceType::GlobalY: axisToken = Viewport::kGlobalYAxisToken; break;
          case AxisReferenceType::GlobalZ: axisToken = Viewport::kGlobalZAxisToken; break;
          case AxisReferenceType::SketchHorizontalAxis: axisToken = 1; break;
          case AxisReferenceType::SketchVerticalAxis: axisToken = 2; break;
          case AxisReferenceType::SketchLine:
            axisToken = static_cast<qulonglong>(revolve->axis().lineId) + 3;
            break;
        }
        {
          const QSignalBlocker axisBlocker(revolveAxisCombo_);
          revolveAxisCombo_->setCurrentIndex(revolveAxisCombo_->findData(axisToken));
        }
        updateRevolveToolPreview();
        revolveDock_->show();
        revolveDock_->raise();
        statusBar()->showMessage(QString::fromUtf8(
            "Редактирование вращения: измените угол или выберите ось"));
        return;
      }
      if (route == FeatureEditorRoute::Shell) {
        auto* shell = dynamic_cast<ShellFeature*>(feature);
        if (!shell) return;
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginShell(
                    body.id(), body.features()[index - 1]->id(), source,
                    shell->removedFaces(), shell->thicknessMm(),
                    shell->outside(), shell->id(),
                    body.features()[index - 1]->topologyIndex()),
                PartDesignToolKind::Shell))
          return;
        viewport_->setFaceMultiSelectionMode(true);
        viewport_->setSelectionFilter(SelectionFilter::Face);
        viewport_->setSelectedBodyFaces(shell->removedFaces());
        toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Shell),
            QString::fromUtf8("Удаляемые грани"), QString::fromUtf8("Толщина"),
            QStringLiteral(" mm"));
        toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
        toolParametersPanel_->setParameterValue(shell->thicknessMm());
        toolParametersPanel_->configureOption(QString::fromUtf8("Наружу"),
                                               shell->outside());
        toolParametersDock_->show(); toolParametersDock_->raise();
        updateShellToolPreview();
        return;
      }
      if (route == FeatureEditorRoute::Draft) {
        auto* draft = dynamic_cast<DraftFeature*>(feature);
        if (!draft) return;
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginDraft(
                    document_, body.id(), body.features()[index - 1]->id(),
                    source, draft->draftedFaces(), draft->neutralPlane(),
                    draft->pullDirection(), draft->angleDeg(),
                    draft->reversed(), draft->id(), draft->rotationEdge(),
                    body.features()[index - 1]->topologyIndex()),
                PartDesignToolKind::Draft))
          return;
        viewport_->setFaceMultiSelectionMode(false);
        viewport_->setSelectionFilter(SelectionFilter::Face);
        viewport_->setSelectedBodyFaces(draft->draftedFaces());
        toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Draft),
            QString::fromUtf8("Поверхность"), QString::fromUtf8("Угол"),
            QString::fromUtf8("°"));
        toolParametersPanel_->setParameterRange(-89.99, 89.99, 2);
        toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).angleDeg);
        toolParametersDock_->show(); toolParametersDock_->raise();
        if (const auto axis = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).principalAxisIndex)
          viewport_->showDraftAxisSelection(*axis);
        else if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).rotationEdge)
          viewport_->showDraftEdgeAxisSelection(
              *partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).rotationEdge);
        updateDraftToolPreview();
        static_cast<void>(viewport_->focusToolParameterField(false));
        return;
      }
      if (route == FeatureEditorRoute::Move) {
        auto* move = dynamic_cast<MoveFeature*>(feature);
        if (!move) return;
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!source) return;
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginMove(move->offsetMm(), move->id()),
                PartDesignToolKind::Move))
          return;
        partDesignCoordinator_.setMoveBody(body.id(), body.features()[index - 1]->id(),
                                 source);
        moveBodyValue_->setText(QString::fromStdString(body.name()));
        moveBodySelectButton_->setEnabled(false);
        {
          const QSignalBlocker xBlocker(moveXSpin_);
          const QSignalBlocker yBlocker(moveYSpin_);
          const QSignalBlocker zBlocker(moveZSpin_);
          const auto offset = move->offsetMm();
          moveXSpin_->setValue(offset.x);
          moveYSpin_->setValue(offset.y);
          moveZSpin_->setValue(offset.z);
        }
        viewport_->setSelectedBodies({body.id()});
        viewport_->showMovePreview();
        updateMoveToolPreview();
        moveDock_->show();
        moveDock_->raise();
        statusBar()->showMessage(
            QString::fromUtf8("Редактирование перемещения: потяните стрелку или задайте координаты"));
        return;
      }
      if (route == FeatureEditorRoute::Mirror) {
        auto* mirror = dynamic_cast<MirrorFeature*>(feature);
        if (!mirror) return;
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!source) return;
        mirrorPresentationBefore_ = editUiTransaction_
            ? std::optional<EditorPresentationState>{
                  editUiTransaction_->before.presentation}
            : std::optional<EditorPresentationState>{
                  captureHistorySelection().presentation};
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginMirror(mirror->id()),
                PartDesignToolKind::Mirror))
          return;
        partDesignCoordinator_.setMirrorBody(body.id(), body.features()[index - 1]->id(),
                                   source);
        partDesignCoordinator_.setMirrorPlane(mirror->plane());
        mirrorBodyValue_->setText(QString::fromStdString(body.name()));
        mirrorPlaneValue_->setText(
            mirror->plane() == MirrorPlane::XY
                ? QStringLiteral("XY")
                : mirror->plane() == MirrorPlane::XZ ? QStringLiteral("XZ")
                                                     : QStringLiteral("YZ"));
        // A Mirror feature belongs to its Body; editing changes the reference
        // plane only. Body re-selection is intentionally available on creation
        // and disabled here to avoid moving a history feature across Bodies.
        mirrorBodySelectButton_->setEnabled(false);
        mirrorPlaneSelectButton_->setEnabled(true);
        viewport_->setSelectedBodies({body.id()});
        viewport_->showMirrorPlaneSelection(
            static_cast<int>(mirror->plane()));
        updateMirrorToolPreview();
        mirrorDock_->show();
        mirrorDock_->raise();
        statusBar()->showMessage(
            QString::fromUtf8("Редактирование зеркала: выберите плоскость в 3D-виде"));
        return;
      }
      if (route == FeatureEditorRoute::LinearPattern) {
        auto* linear = dynamic_cast<LinearPatternFeature*>(feature);
        if (!linear) return;
        Body* sourceBody = &body;
        ShapeFeature::ShapePtr source;
        FeatureId sourceFeatureId = linear->sourceFeatureId();
        if (linear->operation() == PatternOperation::Join) {
          if (index == 0) return;
          source = body.features()[index - 1]->shape();
          sourceFeatureId = body.features()[index - 1]->id();
        } else {
          sourceBody = document_.findBody(linear->sourceBodyId());
          if (sourceBody)
            for (const auto& candidate : sourceBody->features())
              if (candidate->id() == linear->sourceFeatureId()) {
                source = candidate->shape();
                break;
              }
        }
        if (!sourceBody) return;
        if (!source) return;
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginLinearPattern(
                    linear->spacingMm(), linear->count(), linear->operation(),
                    linear->id()),
                PartDesignToolKind::LinearPattern))
          return;
        partDesignCoordinator_.setLinearPatternBody(
            sourceBody->id(), sourceFeatureId, source);
        partDesignCoordinator_.setLinearPatternDirection(linear->direction());
        linearPatternBodyValue_->setText(
            QString::fromStdString(sourceBody->name()));
        linearPatternAxisValue_->setText(
            linear->direction() == PrincipalAxis::X
                ? QStringLiteral("X")
                : linear->direction() == PrincipalAxis::Y
                      ? QStringLiteral("Y")
                      : QStringLiteral("Z"));
        {
          const QSignalBlocker spacingBlocker(linearPatternSpacingSpin_);
          const QSignalBlocker countBlocker(linearPatternCountSpin_);
          const QSignalBlocker operationBlocker(linearPatternOperationCombo_);
          linearPatternSpacingSpin_->setValue(linear->spacingMm());
          linearPatternCountSpin_->setValue(linear->count());
          linearPatternOperationCombo_->setCurrentIndex(
              linear->operation() == PatternOperation::NewBody ? 0 : 1);
        }
        // The feature stays in its owning Body while editing; only direction
        // and numeric parameters may be changed.
        linearPatternBodySelectButton_->setEnabled(false);
        linearPatternAxisSelectButton_->setEnabled(true);
        linearPatternOperationCombo_->setEnabled(false);
        viewport_->setSelectedBodies({sourceBody->id()});
        viewport_->showLinearPatternAxisSelection(
            static_cast<int>(linear->direction()));
        updateLinearPatternToolPreview();
        linearPatternDock_->show();
        linearPatternDock_->raise();
        statusBar()->showMessage(QString::fromUtf8(
            "Редактирование линейного массива: выберите ось или задайте параметры"));
        return;
      }
      if (route == FeatureEditorRoute::CircularPattern) {
        auto* circular = dynamic_cast<CircularPatternFeature*>(feature);
        if (!circular) return;
        Body* sourceBody = &body;
        ShapeFeature::ShapePtr source;
        FeatureId sourceFeatureId = circular->sourceFeatureId();
        if (circular->operation() == PatternOperation::Join) {
          if (index == 0) return;
          source = body.features()[index - 1]->shape();
          sourceFeatureId = body.features()[index - 1]->id();
        } else {
          sourceBody = document_.findBody(circular->sourceBodyId());
          if (sourceBody)
            for (const auto& candidate : sourceBody->features())
              if (candidate->id() == circular->sourceFeatureId()) {
                source = candidate->shape();
                break;
              }
        }
        if (!sourceBody) return;
        if (!source) return;
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginCircularPattern(
                    circular->angleDeg(), circular->count(),
                    circular->operation(), circular->id()),
                PartDesignToolKind::CircularPattern))
          return;
        partDesignCoordinator_.setCircularPatternBody(
            sourceBody->id(), sourceFeatureId, source);
        partDesignCoordinator_.setCircularPatternAxis(circular->axis());
        circularPatternBodyValue_->setText(
            QString::fromStdString(sourceBody->name()));
        circularPatternAxisValue_->setText(
            circular->axis() == PrincipalAxis::X
                ? QStringLiteral("X")
                : circular->axis() == PrincipalAxis::Y
                      ? QStringLiteral("Y")
                      : QStringLiteral("Z"));
        {
          const QSignalBlocker angleBlocker(circularPatternAngleSpin_);
          const QSignalBlocker countBlocker(circularPatternCountSpin_);
          const QSignalBlocker operationBlocker(circularPatternOperationCombo_);
          circularPatternAngleSpin_->setValue(circular->angleDeg());
          circularPatternCountSpin_->setValue(circular->count());
          circularPatternOperationCombo_->setCurrentIndex(
              circular->operation() == PatternOperation::NewBody ? 0 : 1);
        }
        circularPatternBodySelectButton_->setEnabled(false);
        circularPatternAxisSelectButton_->setEnabled(true);
        circularPatternOperationCombo_->setEnabled(false);
        viewport_->setSelectedBodies({sourceBody->id()});
        viewport_->showCircularPatternAxisSelection(
            static_cast<int>(circular->axis()));
        updateCircularPatternToolPreview();
        circularPatternDock_->show();
        circularPatternDock_->raise();
        statusBar()->showMessage(QString::fromUtf8(
            "Редактирование кругового массива: выберите ось или задайте параметры"));
        return;
      }
      return;
}

void MainWindow::createMove() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      document_.bodies().begin(), document_.bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Перемещение"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  if (!applyPartDesignBeginResult(partDesignCoordinator_.beginMove(),
                                  PartDesignToolKind::Move))
    return;
  moveBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  moveBodySelectButton_->setEnabled(true);
  moveAcceptButton_->setEnabled(false);
  {
    const QSignalBlocker xBlocker(moveXSpin_);
    const QSignalBlocker yBlocker(moveYSpin_);
    const QSignalBlocker zBlocker(moveZSpin_);
    moveXSpin_->setValue(0.0);
    moveYSpin_->setValue(0.0);
    moveZSpin_->setValue(0.0);
  }
  moveDock_->show();
  moveDock_->raise();
  viewport_->beginMoveBodySelection();
  updateMoveToolPreview();
  statusBar()->showMessage(QString::fromUtf8("Выберите тело в 3D-виде"));
}

void MainWindow::updateMoveToolPreview() {
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::PreviewValid;
  moveAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::Move).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Move).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId});
    if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Move).translationManipulator)
      viewport_->setTranslationToolManipulator(*manipulator);
    moveStepHint_->setText(QString::fromUtf8(
        "Потяните цветную стрелку X, Y или Z либо задайте точные смещения."));
    moveStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed =
        partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::PreviewInvalid;
    moveStepHint_->setText(
        failed ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Move).error)
               : QString::fromUtf8("Сейчас: ") +
                     partDesignToolStepHint(PartDesignToolKind::Move,
                                            partDesignCoordinator_.snapshot(PartDesignToolKind::Move).selectionStage));
    moveStepHint_->setProperty("uiRole",
                               failed ? "danger" : "secondaryText");
  }
  moveStepHint_->style()->unpolish(moveStepHint_);
  moveStepHint_->style()->polish(moveStepHint_);
}

void MainWindow::cancelMoveTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptMoveTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Body* body = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId);
  if (!body) return;
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Move).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* move = dynamic_cast<MoveFeature*>(body->features()[index].get());
      if (!move || move->id() != *editingId) continue;
      move->setOffsetMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Move).offsetMm);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    if (!body->activeFeature() ||
        body->activeFeature()->id() != partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId) {
      moveStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      moveStepHint_->setProperty("uiRole", "danger");
      return;
    }
    body->addFeature(std::make_unique<MoveFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId, partDesignCoordinator_.snapshot(PartDesignToolKind::Move).offsetMm,
        "Move " + std::to_string(body->features().size() + 1)));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    moveStepHint_->setText(error);
    moveStepHint_->setProperty("uiRole", "danger");
    moveStepHint_->style()->unpolish(moveStepHint_);
    moveStepHint_->style()->polish(moveStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Перемещение применено"), 3000);
}

void MainWindow::createMirror() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      document_.bodies().begin(), document_.bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Зеркало"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  mirrorPresentationBefore_ = captureHistorySelection().presentation;
  if (!applyPartDesignBeginResult(partDesignCoordinator_.beginMirror(),
                                  PartDesignToolKind::Mirror))
    return;
  mirrorBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
  mirrorBodySelectButton_->setEnabled(true);
  mirrorPlaneSelectButton_->setEnabled(false);
  mirrorAcceptButton_->setEnabled(false);
  mirrorDock_->show();
  mirrorDock_->raise();
  viewport_->beginMirrorBodySelection();
  updateMirrorToolPreview();
  statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
}

void MainWindow::updateMirrorToolPreview() {
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::PreviewValid;
  mirrorAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).bodyId});
    mirrorStepHint_->setText(QString::fromUtf8(
        "Предпросмотр построен. Нажмите «Применить» для создания зеркала."));
    mirrorStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    const bool failed =
        partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::PreviewInvalid;
    mirrorStepHint_->setText(
        failed
            ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).error)
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(PartDesignToolKind::Mirror,
                                         partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).selectionStage));
    mirrorStepHint_->setProperty("uiRole",
                                 failed ? "danger" : "secondaryText");
  }
  mirrorStepHint_->style()->unpolish(mirrorStepHint_);
  mirrorStepHint_->style()->polish(mirrorStepHint_);
}

void MainWindow::cancelMirrorTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptMirrorTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle != ToolLifecycle::PreviewValid ||
      !partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).mirrorPlane)
    return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Body* body = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).bodyId);
  if (!body) return;
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* mirror =
          dynamic_cast<MirrorFeature*>(body->features()[index].get());
      if (!mirror || mirror->id() != *editingId) continue;
      mirror->setPlane(*partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).mirrorPlane);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    if (!body->activeFeature() ||
        body->activeFeature()->id() != partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).sourceFeatureId) {
      mirrorStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      mirrorStepHint_->setProperty("uiRole", "danger");
      return;
    }
    body->addFeature(std::make_unique<MirrorFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).sourceFeatureId, *partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).mirrorPlane,
        "Mirror " + std::to_string(body->features().size() + 1)));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    mirrorStepHint_->setText(error);
    mirrorStepHint_->setProperty("uiRole", "danger");
    mirrorStepHint_->style()->unpolish(mirrorStepHint_);
    mirrorStepHint_->style()->polish(mirrorStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  const auto presentationBefore = mirrorPresentationBefore_;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  if (presentationBefore) applyPresentationState(*presentationBefore);
  mirrorPresentationBefore_.reset();
  statusBar()->showMessage(QString::fromUtf8("Зеркало применено"), 3000);
}

void MainWindow::createLinearPattern() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      document_.bodies().begin(), document_.bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Линейный массив"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginLinearPattern(30.0, 3),
          PartDesignToolKind::LinearPattern))
    return;
  linearPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  linearPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
  linearPatternBodySelectButton_->setEnabled(true);
  linearPatternAxisSelectButton_->setEnabled(false);
  linearPatternOperationCombo_->setEnabled(true);
  linearPatternAcceptButton_->setEnabled(false);
  {
    const QSignalBlocker spacingBlocker(linearPatternSpacingSpin_);
    const QSignalBlocker countBlocker(linearPatternCountSpin_);
    const QSignalBlocker operationBlocker(linearPatternOperationCombo_);
    linearPatternSpacingSpin_->setValue(30.0);
    linearPatternCountSpin_->setValue(3);
    linearPatternOperationCombo_->setCurrentIndex(0);
  }
  linearPatternDock_->show();
  linearPatternDock_->raise();
  viewport_->beginLinearPatternBodySelection();
  updateLinearPatternToolPreview();
  statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
}

void MainWindow::updateLinearPatternToolPreview() {
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid =
      partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle == ToolLifecycle::PreviewValid;
  linearPatternAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).previewShape);
    viewport_->setToolPreviewReplacedBodies(
        {partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId});
    if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).linearManipulator)
      viewport_->setToolManipulator(*manipulator);
    linearPatternStepHint_->setText(QString::fromUtf8(
        "Потяните стрелку для изменения шага или задайте шаг и количество числом."));
    linearPatternStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                        ToolLifecycle::PreviewInvalid;
    linearPatternStepHint_->setText(
        failed
            ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).error)
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(
                      PartDesignToolKind::LinearPattern,
                      partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).selectionStage));
    linearPatternStepHint_->setProperty("uiRole",
                                        failed ? "danger" : "secondaryText");
  }
  linearPatternStepHint_->style()->unpolish(linearPatternStepHint_);
  linearPatternStepHint_->style()->polish(linearPatternStepHint_);
}

void MainWindow::cancelLinearPatternTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptLinearPatternTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle != ToolLifecycle::PreviewValid ||
      !partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).principalDirection)
    return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Body* sourceBody = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId);
  if (!sourceBody) return;
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).editingFeatureId) {
    auto* linear =
        dynamic_cast<LinearPatternFeature*>(document_.findFeature(*editingId));
    if (!linear) return;
    linear->setDirection(*partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).principalDirection);
    linear->setSpacingMm(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).spacingMm);
    linear->setCount(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).count);
  } else {
    if (!sourceBody->activeFeature() ||
        sourceBody->activeFeature()->id() !=
            partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId) {
      linearPatternStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      linearPatternStepHint_->setProperty("uiRole", "danger");
      return;
    }
    const BodyId sourceBodyId = sourceBody->id();
    Body* targetBody = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).patternOperation ==
                               PatternOperation::NewBody
                           ? &document_.addBody()
                           : sourceBody;
    targetBody->addFeature(std::make_unique<LinearPatternFeature>(
        sourceBodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
        *partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).principalDirection,
        partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).count,
        partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).spacingMm,
        partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).patternOperation,
        "Linear Pattern " +
            std::to_string(targetBody->features().size() + 1)));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    linearPatternStepHint_->setText(error);
    linearPatternStepHint_->setProperty("uiRole", "danger");
    linearPatternStepHint_->style()->unpolish(linearPatternStepHint_);
    linearPatternStepHint_->style()->polish(linearPatternStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Линейный массив применён"),
                           3000);
}

void MainWindow::createCircularPattern() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      document_.bodies().begin(), document_.bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Круговой массив"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginCircularPattern(
              kMaximumPatternAngleDeg, 4),
          PartDesignToolKind::CircularPattern))
    return;
  circularPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  circularPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
  circularPatternBodySelectButton_->setEnabled(true);
  circularPatternAxisSelectButton_->setEnabled(false);
  circularPatternOperationCombo_->setEnabled(true);
  circularPatternAcceptButton_->setEnabled(false);
  {
    const QSignalBlocker angleBlocker(circularPatternAngleSpin_);
    const QSignalBlocker countBlocker(circularPatternCountSpin_);
    const QSignalBlocker operationBlocker(circularPatternOperationCombo_);
    circularPatternAngleSpin_->setValue(kMaximumPatternAngleDeg);
    circularPatternCountSpin_->setValue(4);
    circularPatternOperationCombo_->setCurrentIndex(0);
  }
  circularPatternDock_->show();
  circularPatternDock_->raise();
  viewport_->beginCircularPatternBodySelection();
  updateCircularPatternToolPreview();
  statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
}

void MainWindow::updateCircularPatternToolPreview() {
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                     ToolLifecycle::PreviewValid;
  circularPatternAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).previewShape);
    viewport_->setToolPreviewReplacedBodies(
        {partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId});
    if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).angularManipulator)
      viewport_->setAngularToolManipulator(*manipulator);
    circularPatternStepHint_->setText(QString::fromUtf8(
        "Потяните дугу для изменения угла или задайте угол и количество числом."));
    circularPatternStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                        ToolLifecycle::PreviewInvalid;
    circularPatternStepHint_->setText(
        failed
            ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).error)
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(
                      PartDesignToolKind::CircularPattern,
                      partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).selectionStage));
    circularPatternStepHint_->setProperty(
        "uiRole", failed ? "danger" : "secondaryText");
  }
  circularPatternStepHint_->style()->unpolish(circularPatternStepHint_);
  circularPatternStepHint_->style()->polish(circularPatternStepHint_);
}

void MainWindow::cancelCircularPatternTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptCircularPatternTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
          ToolLifecycle::PreviewValid ||
      !partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).principalAxis)
    return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Body* sourceBody = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId);
  if (!sourceBody) return;
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).editingFeatureId) {
    auto* circular = dynamic_cast<CircularPatternFeature*>(
        document_.findFeature(*editingId));
    if (!circular) return;
    circular->setAxis(*partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).principalAxis);
    circular->setAngleDeg(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).angleDeg);
    circular->setCount(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).count);
  } else {
    if (!sourceBody->activeFeature() ||
        sourceBody->activeFeature()->id() !=
            partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId) {
      circularPatternStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      circularPatternStepHint_->setProperty("uiRole", "danger");
      return;
    }
    const BodyId sourceBodyId = sourceBody->id();
    Body* targetBody = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).patternOperation ==
                               PatternOperation::NewBody
                           ? &document_.addBody()
                           : sourceBody;
    targetBody->addFeature(std::make_unique<CircularPatternFeature>(
        sourceBodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
        *partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).principalAxis,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).count,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).angleDeg,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).patternOperation,
        "Circular Pattern " +
            std::to_string(targetBody->features().size() + 1)));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    circularPatternStepHint_->setText(error);
    circularPatternStepHint_->setProperty("uiRole", "danger");
    circularPatternStepHint_->style()->unpolish(circularPatternStepHint_);
    circularPatternStepHint_->style()->polish(circularPatternStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Круговой массив применён"),
                           3000);
}

void MainWindow::createChamfer() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto edges = viewport_->selectedBodyEdges();
  Body* body = edges.empty() ? document_.activeBody()
                             : document_.findBody(edges.front().bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) {
    QMessageBox::information(this, QString::fromUtf8("Фаска"),
                             QString::fromUtf8("Сначала создайте тело."));
    return;
  }
  if (!edges.empty() && body->activeFeature()->id() != edges.front().featureId)
    edges.clear();
  for (const auto& edge : edges)
    if (edge.bodyId != body->id() ||
        edge.featureId != body->activeFeature()->id()) {
      QMessageBox::warning(this, QString::fromUtf8("Фаска"),
                           QString::fromUtf8("Рёбра должны принадлежать одному телу"));
      return;
    }
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginChamfer(
              body->id(), body->activeFeature()->id(), body->resultShape(),
              edges, 0.0, std::nullopt,
              body->activeFeature()->topologyIndex()),
          PartDesignToolKind::Chamfer))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  viewport_->setSelectedBodyEdges(edges);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Chamfer),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Размер"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.0, 100000.0, 2);
  toolParametersPanel_->setParameterValue(0.0);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateChamferToolPreview();
  if (!edges.empty())
    static_cast<void>(viewport_->focusToolParameterField(false));
  if (edges.empty())
    statusBar()->showMessage(
        QString::fromUtf8("Нажмите «Выбрать» и укажите рёбра в viewport"));
}

void MainWindow::createJoinBodies() {
  if (!ensureHistoryAtEnd()) return;
  std::size_t availableBodies = 0;
  for (const Body& body : document_.bodies())
    if (body.visible() && body.resultShape()) ++availableBodies;
  if (availableBodies < 2) {
    QMessageBox::information(
        this, QString::fromUtf8("Соединить тела"),
        QString::fromUtf8("Для объединения нужны два видимых твёрдых тела."));
    modelRibbon_->clearActiveTool();
    return;
  }
  resetTransientModelingUi();
  if (!applyPartDesignBeginResult(partDesignCoordinator_.beginJoinBodies(),
                                  PartDesignToolKind::JoinBodies))
    return;
  toolParametersPanel_->configureSelectionOnly(
      *partDesignToolHelp(PartDesignToolKind::JoinBodies),
      QString::fromUtf8("Тела"));
  toolParametersPanel_->setSelectionCount(0);
  toolParametersPanel_->setAcceptEnabled(false);
  toolParametersPanel_->setStatus(
      QString::fromUtf8("Выберите первое, затем второе тело"));
  toolParametersDock_->show();
  toolParametersDock_->raise();
  viewport_->beginJoinBodiesSelection();
  statusBar()->showMessage(
      QString::fromUtf8("Соединить тела: выберите два тела в 3D-виде"));
}

void MainWindow::editJoinBodiesStep(BodyId bodyId, FeatureId featureId) {
  if (!ensureHistoryAtEnd()) return;
  auto* join = dynamic_cast<JoinBodiesFeature*>(
      findHistoryFeature(document_, bodyId, featureId));
  if (!join) return;
  const auto makeInput = [this](BodyId sourceBodyId,
                                FeatureId sourceFeatureId)
      -> std::optional<JoinBodyInput> {
    const Body* sourceBody = document_.findBody(sourceBodyId);
    const auto* source = dynamic_cast<const ShapeFeature*>(
        document_.findFeature(sourceFeatureId));
    if (!sourceBody || !source || !source->lastValidShape()) return std::nullopt;
    return JoinBodyInput{sourceBodyId, sourceFeatureId,
                         source->lastValidShape()};
  };
  const auto first = makeInput(join->firstBodyId(), join->firstFeatureId());
  const auto second = makeInput(join->secondBodyId(), join->secondFeatureId());
  if (!first || !second) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginJoinBodies(featureId, bodyId),
          PartDesignToolKind::JoinBodies))
    return;
  partDesignCoordinator_.setJoinBodies({*first, *second});
  // The original operands are normally hidden by the committed Join. Expose
  // them only in the viewport presentation so Select can replace either input
  // without mutating committed visibility before the edit is accepted.
  std::vector<BodyId> selectableOperands;
  for (const BodyId operandId : {join->firstBodyId(), join->secondBodyId()})
    if (!bodyConsumedByJoin(document_, operandId, featureId))
      selectableOperands.push_back(operandId);
  refreshBodyViewFromDocument(selectableOperands);
  toolParametersPanel_->configureSelectionOnly(
      *partDesignToolHelp(PartDesignToolKind::JoinBodies),
      QString::fromUtf8("Тела"));
  toolParametersDock_->show();
  toolParametersDock_->raise();
  viewport_->beginJoinBodiesSelection();
  viewport_->setSelectedBodies(selectableOperands);
  updateJoinBodiesToolPreview();
  statusBar()->showMessage(
      QString::fromUtf8("Редактирование объединения тел"), 3000);
}

void MainWindow::updateJoinBodiesToolPreview() {
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  QString status;
  bool error = false;
  if (valid) {
    status = QString::fromUtf8("Предпросмотр объединения построен");
  } else if (state == ToolLifecycle::PreviewInvalid) {
    error = true;
    status = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).errorCode ==
                     OperationFailureCode::BodiesDoNotTouch
                 ? QString::fromUtf8(
                       "Тела должны соприкасаться или пересекаться")
                 : QString::fromUtf8("Не удалось объединить выбранные тела");
  } else {
    status = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.empty()
                 ? QString::fromUtf8("Выберите первое тело")
                 : QString::fromUtf8("Выберите второе тело");
  }
  toolParametersPanel_->setStatus(status, error);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(kInvalidBodyId, kInvalidFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).previewShape);
    std::vector<BodyId> replaced;
    replaced.reserve(partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.size());
    for (const auto& input : partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies)
      replaced.push_back(input.bodyId);
    viewport_->setToolPreviewReplacedBodies(std::move(replaced));
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (error) statusBar()->showMessage(status, 5000);
}

void MainWindow::cancelJoinBodiesTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptJoinBodiesTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle != ToolLifecycle::PreviewValid ||
      partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.size() != 2)
    return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  const auto inputs = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies;
  Body* first = document_.findBody(inputs[0].bodyId);
  Body* second = document_.findBody(inputs[1].bodyId);
  auto* firstFeature = dynamic_cast<ShapeFeature*>(
      document_.findFeature(inputs[0].featureId));
  auto* secondFeature = dynamic_cast<ShapeFeature*>(
      document_.findFeature(inputs[1].featureId));
  if (!first || !second || first == second || !firstFeature ||
      !secondFeature || !first->featureIndex(inputs[0].featureId) ||
      !second->featureIndex(inputs[1].featureId) ||
      !firstFeature->lastValidShape() || !secondFeature->lastValidShape()) {
    toolParametersPanel_->setStatus(
        QString::fromUtf8("Исходные тела изменились. Выберите их заново."),
        true);
    return;
  }

  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).editingFeatureId) {
    Body* owner = nullptr;
    for (const auto& candidate : document_.bodies())
      if (candidate.featureIndex(*editingId)) {
        owner = document_.findBody(candidate.id());
        break;
      }
    auto* join = dynamic_cast<JoinBodiesFeature*>(
        document_.findFeature(*editingId));
    if (!owner || !join || owner == first || owner == second) {
      document_ = std::move(previous);
      return;
    }
    const BodyId oldFirst = join->firstBodyId();
    const BodyId oldSecond = join->secondBodyId();
    join->setInputs(inputs[0].bodyId, inputs[0].featureId,
                    inputs[1].bodyId, inputs[1].featureId);
    std::vector<BodyId> detached;
    for (const BodyId oldId : {oldFirst, oldSecond})
      if (oldId != inputs[0].bodyId && oldId != inputs[1].bodyId)
        detached.push_back(oldId);
    releaseUnconsumedJoinOperands(document_, detached);
  } else {
    Body& resultBody = document_.addBody(
        "Joined Body " + std::to_string(document_.bodies().size() + 1));
    resultBody.addFeature(std::make_unique<JoinBodiesFeature>(
        inputs[0].bodyId, inputs[0].featureId, inputs[1].bodyId,
        inputs[1].featureId, "Соединение тел"));
  }
  first = document_.findBody(inputs[0].bodyId);
  second = document_.findBody(inputs[1].bodyId);
  first->setVisible(false);
  second->setVisible(false);
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }

  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Тела объединены"), 3000);
}

void MainWindow::createShell() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto faces = viewport_->selectedBodyFaces();
  Body* body = faces.empty() ? document_.activeBody()
                             : document_.findBody(faces.front().bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) return;
  if (!faces.empty() && faces.front().featureId != body->activeFeature()->id())
    faces.clear();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginShell(
              body->id(), body->activeFeature()->id(), body->resultShape(),
              faces, 2.0, false, std::nullopt,
              body->activeFeature()->topologyIndex()),
          PartDesignToolKind::Shell))
    return;
  viewport_->setFaceMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setSelectedBodyFaces(faces);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Shell),
                                  QString::fromUtf8("Удаляемые грани"),
                                  QString::fromUtf8("Толщина"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
  toolParametersPanel_->setParameterValue(2.0);
  toolParametersPanel_->configureOption(QString::fromUtf8("Наружу"), false);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateShellToolPreview();
  // Shell HUD owns CAD Tab focus; do not leave focus on the ribbon button when
  // the operation starts from a preselected face.
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::updateShellToolPreview() {
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).faces.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  const QString limitStatus = QString::fromUtf8(
      "Достигнута предельная толщина: %1 мм")
                                  .arg(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm, 0, 'f', 2);
  toolParametersPanel_->setParameterRange(
      0.01, partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).maximumValidValueMm.value_or(100000.0), 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm);
  toolParametersPanel_->setAcceptEnabled(valid);
  toolParametersPanel_->setStatus(
      valid ? partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).limitReached
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Shell,
                                           ToolSelectionStage::SelectingInput)
                  : localizedPartDesignError(PartDesignToolKind::Shell,
                                             partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).failure),
      state == ToolLifecycle::PreviewInvalid);
  if (valid) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId});
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).limitReached)
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelShellTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptShellTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  Body* body = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId);
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* shell = dynamic_cast<ShellFeature*>(body->features()[index].get());
      if (!shell || shell->id() != *editingId) continue;
      shell->setRemovedFaces(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).faces);
      shell->setThicknessMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm);
      shell->setOutside(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).outside);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ShellFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).sourceFeatureId, partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).faces,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm, partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).outside,
        "Оболочка " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::createDraft() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto faces = viewport_->selectedBodyFaces();
  if (faces.size() > 1) faces.resize(1);
  Body* body = faces.empty() ? document_.activeBody()
                             : document_.findBody(faces.front().bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) return;
  if (!faces.empty() && faces.front().featureId != body->activeFeature()->id())
    faces.clear();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginDraft(
              document_, body->id(), body->activeFeature()->id(),
              body->resultShape(), faces, std::nullopt, std::nullopt, 5.0,
              false, std::nullopt, std::nullopt,
              body->activeFeature()->topologyIndex()),
          PartDesignToolKind::Draft))
    return;
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces(faces);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Draft),
                                  QString::fromUtf8("Поверхность"),
                                  QString::fromUtf8("Угол"),
                                  QString::fromUtf8("°"));
  toolParametersPanel_->setParameterRange(-89.99, 89.99, 2);
  toolParametersPanel_->setParameterValue(5.0);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateDraftToolPreview();
  if (faces.empty()) {
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    viewport_->beginDraftFaceSelection();
    statusBar()->showMessage(
        QString::fromUtf8("1/3 Выберите поверхность в 3D-виде"));
  } else {
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingReference);
    viewport_->beginDraftAxisSelection();
    statusBar()->showMessage(
        QString::fromUtf8(
            "2/3 Выберите ось X, Y, Z или прилегающее прямое ребро"));
  }
}

void MainWindow::updateDraftToolPreview() {
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).faces.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  toolParametersPanel_->setStatus(
      valid ? QString::fromUtf8("Предпросмотр построен · поверхность и ось выбраны")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Draft,
                                           ToolSelectionStage::SelectingInput)
                  : state == ToolLifecycle::SelectingReference
                        ? partDesignToolStepHint(
                              PartDesignToolKind::Draft,
                              ToolSelectionStage::SelectingReference)
                  : localizedPartDesignError(PartDesignToolKind::Draft,
                                             partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).failure),
      state == ToolLifecycle::PreviewInvalid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).bodyId});
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).angularManipulator)
    viewport_->setAngularToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
}

void MainWindow::cancelDraftTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptDraftTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).lifecycle != ToolLifecycle::PreviewValid ||
      (!partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).rotationEdge &&
       (!partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).neutralPlane ||
        !partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).pullDirection)))
    return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  Body* body = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).bodyId);
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* draft = dynamic_cast<DraftFeature*>(body->features()[index].get());
      if (!draft || draft->id() != *editingId) continue;
      draft->setDraftedFaces(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).faces);
      if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).neutralPlane)
        draft->setNeutralPlane(*partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).neutralPlane);
      if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).pullDirection)
        draft->setPullDirection(*partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).pullDirection);
      draft->setRotationEdge(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).rotationEdge);
      draft->setAngleDeg(std::abs(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).angleDeg));
      draft->setReversed(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).angleDeg < 0.0);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    const PlaneReference plane = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).neutralPlane.value_or(
        PlaneReference{NeutralPlaneType::GlobalXY});
    const AxisReference direction = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).pullDirection.value_or(
        AxisReference{AxisReferenceType::GlobalZ, kInvalidSketchId,
                      sketch::kInvalidGeometryId});
    body->addFeature(std::make_unique<DraftFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).sourceFeatureId, partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).faces,
        plane, direction,
        std::abs(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).angleDeg),
        partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).angleDeg < 0.0,
        "Уклон " + std::to_string(body->features().size()),
        partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, document_).rotationEdge));
  }
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::createSketchExtrude(const ExtrusionSourcePick& pick) {
  if (!ensureHistoryAtEnd()) return;
  const auto* region = std::get_if<SketchRegionPick>(&pick.source);
  if (!region || region->sketchId == kInvalidSketchId) return;
  const SketchId sketchId = region->sketchId;
  DocumentSketch* profile = document_.findSketch(sketchId);
  if (!profile) return;
  Body* activeBody = document_.activeBody();
  SketchProfileSelectionContext context;
  context.profile = *profile;
  std::optional<sketch::Sketch> profileOverride;
  const auto& pickedProfile = region->geometry;
  if (!pickedProfile.lines().empty() || !pickedProfile.circles().empty() ||
      !pickedProfile.arcs().empty()) {
    context.profile.geometry = pickedProfile;
    if (!isSupportedSingleSketchProfile(*profile))
      profileOverride = pickedProfile;
  }
  context.activeBodyId = activeBody ? activeBody->id() : kInvalidBodyId;
  context.activeFeatureId =
      activeBody && activeBody->activeFeature() ? activeBody->activeFeature()->id()
                                                : kInvalidFeatureId;
  context.activeBodyShape =
      activeBody ? activeBody->resultShape() : ShapeFeature::ShapePtr{};
  const auto capability = resolveSketchProfileExtrude(context);
  if (!capability) {
    statusBar()->showMessage(
        QString::fromUtf8("Прямое выдавливание: профиль не поддерживается"),
        4000);
    return;
  }
  resetTransientModelingUi();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginSketchExtrude(
              *profile, sketchId, capability->baseShape, 10.0,
              capability->operation, false, std::nullopt,
              std::move(profileOverride)),
          PartDesignToolKind::Extrude))
    return;
  partDesignCoordinator_.setExtrudeOperationFollowsDirection(
      capability->operationFollowsDirection);
  viewport_->clearLegacyExtrusionPreview();
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Профиль"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::createFaceExtrude(const FaceReference& face) {
  // Native face extrusion shares the Extrude ribbon entry; picking a real
  // B-Rep body face (rather than a sketch contour) starts this face-source
  // session. Never interrupt a running revolve profile re-selection.
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, document_).lifecycle != ToolLifecycle::Inactive) return;
  Body* body = document_.findBody(face.bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) {
    statusBar()->showMessage(QString::fromUtf8("Сначала создайте тело."), 3000);
    return;
  }
  resetTransientModelingUi();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginExtrude(
              body->id(), body->activeFeature()->id(), body->resultShape(),
              face, 10.0, ExtrudeOperation::Join, false, std::nullopt,
              body->activeFeature()->topologyIndex()),
          PartDesignToolKind::Extrude))
    return;
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces({face});
  viewport_->clearLegacyExtrusionPreview();
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Грань"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  const auto parameters = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).parameters;
  const double minimum = parameters.empty() ? 0.01 : parameters.front().minimum;
  const double maximum =
      parameters.empty() ? 100000.0 : parameters.front().maximum;
  toolParametersPanel_->setParameterRange(minimum, maximum, 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
  toolParametersPanel_->configureOption(QString::fromUtf8("Вырезать"), false);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::updateFaceExtrudeToolPreview() {
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setSelectionCount(
      partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sketchSource ||
              partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).face.bodyId != kInvalidBodyId
          ? 1
          : 0);
  toolParametersPanel_->setAcceptEnabled(valid);
  if (valid) {
    toolParametersPanel_->setStatus(QString::fromUtf8("Предпросмотр построен"),
                                    false);
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).previewShape);
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation != ExtrudeOperation::NewBody)
      viewport_->setToolPreviewReplacedBodies(
          {partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId});
    viewport_->setToolCutPreviewShape(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).subtractivePreviewShape);
  } else {
    viewport_->clearToolPreviewShape();
    toolParametersPanel_->setStatus(
        state == ToolLifecycle::PreviewInvalid
            ? localizedFaceToolError(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).failure)
            : QString::fromUtf8("Выберите грань тела"),
        state == ToolLifecycle::PreviewInvalid);
  }
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(
        localizedFaceToolError(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).failure));
}

void MainWindow::acceptFaceExtrudeTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sketchSource) {
    Body* targetBody = nullptr;
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation == ExtrudeOperation::NewBody) {
      targetBody = &document_.addBody();
    } else {
      targetBody = document_.activeBody();
      if (!targetBody) return;
    }
    auto feature = std::make_unique<ExtrudeFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).profileSketchId, partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm,
        "Extrude", partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed);
    feature->setProfileOverride(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).profileOverride);
    targetBody->addFeature(std::move(feature));
    if (!document_.recompute()) {
      const QString error = QString::fromStdString(document_.rebuildError());
      document_ = previous;
      refreshBodyViewFromDocument();
      toolParametersPanel_->setStatus(error, true);
      return;
    }
    auto committedAfter = captureCommittedEndState();
    if (!pushModelTransition(std::move(previous), previousSelection,
                             std::move(committedAfter)))
      return;
    applyPartDesignUiEffect(commit.accept());
    rebuildFeatureTree();
    rebuildHistoryPanel();
    completeModelTransitionUi();
    statusBar()->showMessage(QString::fromUtf8("Создано выдавливание профиля"), 3000);
    return;
  }
  Body* body = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId);
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* extrude =
          dynamic_cast<ExtrudeFeature*>(body->features()[index].get());
      if (!extrude || extrude->id() != *editingId) continue;
      extrude->setLengthMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
      extrude->setOperation(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation);
      extrude->setReversed(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ExtrudeFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).face, partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm, "Extrude",
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation, partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Создано выдавливание грани"), 3000);
}

void MainWindow::cancelFaceExtrudeTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::editFaceExtrudeStep(Body* body, ExtrudeFeature* extrude,
                                     std::size_t extrudeIndex) {
  if (!extrude->faceReference()) return;
  if (extrudeIndex == 0) {
    statusBar()->showMessage(
        QString::fromUtf8("Выдавливание грани не может быть первой операцией"),
        3000);
    return;
  }
  ShapeFeature::ShapePtr baseShape = body->features()[extrudeIndex - 1]->shape();
  if (!baseShape || baseShape->IsNull()) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось восстановить исходное тело"), 3000);
    return;
  }
  const FeatureId sourceFeatureId = body->features()[extrudeIndex - 1]->id();
  const FaceReference face = *extrude->faceReference();
  viewport_->clearLegacyExtrusionPreview();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginExtrude(
              body->id(), sourceFeatureId, baseShape, face,
              extrude->lengthMm(), extrude->operation(), extrude->reversed(),
              extrude->id(),
              body->features()[extrudeIndex - 1]->topologyIndex()),
          PartDesignToolKind::Extrude))
    return;
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces({face});
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Грань"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  const auto parameters = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).parameters;
  const double minimum = parameters.empty() ? 0.01 : parameters.front().minimum;
  const double maximum =
      parameters.empty() ? 100000.0 : parameters.front().maximum;
  toolParametersPanel_->setParameterRange(minimum, maximum, 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
  toolParametersPanel_->configureOption(
      QString::fromUtf8("Вырезать"),
      partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation == ExtrudeOperation::Cut);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
  statusBar()->showMessage(
      QString::fromUtf8("Редактирование выдавливания грани"), 3000);
}

void MainWindow::updateChamferToolPreview() {
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  const auto maximum = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).maximumValidValueMm;
  const QString error = maximum
                            ? QString::fromUtf8(
                                  "Размер фаски слишком велик. Максимально "
                                  "допустимое значение: %1 мм.")
                                  .arg(*maximum, 0, 'f', 2)
                            : localizedPartDesignError(
                                  PartDesignToolKind::Chamfer,
                                  partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).failure);
  const QString limitStatus =
      maximum
          ? QString::fromUtf8("Достигнут предельный размер фаски: %1 мм")
                .arg(*maximum, 0, 'f', 2)
          : QString{};
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm);
  toolParametersPanel_->setStatus(
      valid ? partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).limitReached
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Chamfer,
                                           ToolSelectionStage::SelectingInput)
                  : error,
      state == ToolLifecycle::PreviewInvalid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).bodyId});
  } else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(error, 5000);
  else if (partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).limitReached)
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelChamferTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptChamferTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previousDocument = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  Body* body = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).bodyId);
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* chamfer = dynamic_cast<ChamferFeature*>(body->features()[index].get());
      if (!chamfer || chamfer->id() != *editingId) continue;
      chamfer->setEdges(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges);
      chamfer->setDistanceMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ChamferFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges, partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm,
        "Фаска " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = localizedPartDesignError(
        PartDesignToolKind::Chamfer,
        {OperationFailureCode::Unknown, document_.rebuildError()});
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  const double distance = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm;
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Фаска применена: %1 мм").arg(distance), 3000);
}

void MainWindow::updateFilletToolPreview() {
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  const auto maximum = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).maximumValidValueMm;
  const QString error = maximum
                            ? QString::fromUtf8(
                                  "Радиус скругления слишком велик. Максимально "
                                  "допустимое значение: %1 мм.")
                                  .arg(*maximum, 0, 'f', 2)
                            : localizedPartDesignError(
                                  PartDesignToolKind::Fillet,
                                  partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).failure);
  const QString limitStatus =
      maximum
          ? QString::fromUtf8("Достигнут предельный радиус: %1 мм")
                .arg(*maximum, 0, 'f', 2)
          : QString{};
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm);
  toolParametersPanel_->setStatus(
      valid ? partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).limitReached
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Fillet,
                                           ToolSelectionStage::SelectingInput)
                  : error,
      state == ToolLifecycle::PreviewInvalid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).bodyId});
  } else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(error, 5000);
  else if (partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).limitReached)
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelFilletTool() {
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptFilletTool() {
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = partDesignCoordinator_.startCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previousDocument = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  Body* body = document_.findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).bodyId);
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* fillet = dynamic_cast<FilletFeature*>(body->features()[index].get());
      if (!fillet || fillet->id() != *editingId) continue;
      fillet->setEdges(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges);
      fillet->setRadiusMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<FilletFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges, partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm,
        "Скругление " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = localizedPartDesignError(
        PartDesignToolKind::Fillet,
        {OperationFailureCode::Unknown, document_.rebuildError()});
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  const double radius = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm;
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Скругление применено: %1 мм").arg(radius), 3000);
}

void MainWindow::rebuildHistoryPanel() {
  if (!historyLayout_) return;
  const int previousCount = static_cast<int>(historySteps_.size());
  const bool wasAtEnd = historyPosition_ >= previousCount;
  while (QLayoutItem* item = historyLayout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  historySteps_.clear();
  historySteps_ = buildPartDesignHistory(document_, document_.activeBody());
  if (wasAtEnd) historyPosition_ = static_cast<int>(historySteps_.size());
  auto addStep = [this](const HistoryStep& step, int position) {
    auto* button = new QToolButton(historyContent_);
    configureHistoryButton(*button, step, position == historyPosition_);
    button->setProperty("bodyId", QVariant::fromValue<qulonglong>(step.bodyId));
    button->setProperty("featureId", QVariant::fromValue<qulonglong>(step.featureId));
    button->setProperty("sketchId", QVariant::fromValue<qulonglong>(step.sketchId));
    connect(button, &QToolButton::clicked, this, [this, step] {
      if (step.sketchId != kInvalidSketchId) editSketchById(step.sketchId);
      else if (step.editable) editHistoryFeature(step.bodyId, step.featureId);
    });
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(button, &QToolButton::customContextMenuRequested, this,
            [this, button, step](const QPoint& point) {
      // The history panel is rebuilt after removal. Keep the stack menu out
      // of the button's ownership and defer mutation until both QMenu::exec()
      // and the button's signal delivery have returned; otherwise rebuilding
      // destroys objects that Qt is still dispatching through.
      QMenu menu(this);
      QAction* edit = menu.addAction(QString::fromUtf8("Редактировать"));
      edit->setEnabled(step.editable);
      QAction* remove = menu.addAction(QString::fromUtf8("Удалить"));
      QAction* chosen = menu.exec(button->mapToGlobal(point));
      if (chosen == edit) {
        if (step.sketchId != kInvalidSketchId) editSketchById(step.sketchId);
        else editHistoryFeature(step.bodyId, step.featureId);
      } else if (chosen == remove) {
        QTimer::singleShot(0, this,
                           [this, step] { removeHistoryStep(step); });
      }
    });
    historyLayout_->addWidget(button);
  };
  for (std::size_t index = 0; index < historySteps_.size(); ++index)
    addStep(historySteps_[index], static_cast<int>(index + 1));
  if (historyTimeline_) {
    const QSignalBlocker blocker(historyTimeline_);
    const int lastPosition = static_cast<int>(historySteps_.size());
    historyTimeline_->setStepCount(lastPosition);
    historyPosition_ = std::clamp(historyPosition_, 0, lastPosition);
    historyTimeline_->setPosition(historyPosition_);
  }
  if (historyScroll_ && historyLayout_->count() > 0)
    historyScroll_->ensureWidgetVisible(
        historyLayout_->itemAt(historyLayout_->count() - 1)->widget());
}

bool MainWindow::ensureHistoryAtEnd() {
  if (isHistoryAtEnd()) return true;
  statusBar()->showMessage(
      QString::fromUtf8("Вернитесь к последнему шагу истории, чтобы добавить "
                        "новую операцию."),
      4000);
  return false;
}

bool MainWindow::isHistoryAtEnd() const {
  return historyPosition_ >= static_cast<int>(historySteps_.size());
}

void MainWindow::moveHistoryToEnd() {
  historyPosition_ = static_cast<int>(historySteps_.size());
  if (historyTimeline_) historyTimeline_->setPosition(historyPosition_);
}

void MainWindow::removeHistoryStep(const HistoryStep& step) {
  const FeatureRemovalPlan plan = step.sketchId != kInvalidSketchId
      ? document_.planSketchRemoval(step.sketchId)
      : document_.planFeatureRemoval(step.bodyId, step.featureId);
  if (!plan.applicable) {
    QMessageBox::warning(
        this, QString::fromUtf8("Ошибка удаления"),
        QString::fromStdString(plan.diagnostic.empty()
                                   ? "Не удалось построить план удаления"
                                   : plan.diagnostic));
    return;
  }
  if (plan.empty()) return;
  QStringList dependents;
  // historySteps_ contains only the active Body. Resolve the grouped plan
  // against Document so the confirmation names every cross-Body dependent
  // that will actually be removed.
  for (const auto& range : plan.bodyRanges) {
    const Body* body = document_.findBody(range.bodyId);
    if (!body) continue;
    for (const FeatureId featureId : range.featureIds) {
      if (featureId == step.featureId) continue;
      const auto feature =
          std::find_if(body->features().begin(), body->features().end(),
                       [featureId](const auto& candidate) {
                         return candidate && candidate->id() == featureId;
                       });
      if (feature == body->features().end()) continue;
      dependents << QString::fromUtf8("• %1 / %2")
                        .arg(QString::fromStdString(body->name()),
                             QString::fromStdString((*feature)->name()));
    }
  }
  for (const SketchId sketchId : plan.sketchIds) {
    if (sketchId == step.sketchId) continue;
    const DocumentSketch* sketch = document_.findSketch(sketchId);
    if (sketch)
      dependents << QString::fromUtf8("• Эскиз / %1")
                        .arg(QString::fromStdString(sketch->name));
  }
  QString message = QString::fromUtf8("Удалить «%1»?").arg(step.title);
  if (!dependents.isEmpty())
    message += QString::fromUtf8("\n\nОт этой операции зависят:\n") +
               dependents.join(QLatin1Char('\n')) +
               QString::fromUtf8("\n\nОни также будут удалены.");
  QMessageBox box(QMessageBox::Warning, QString::fromUtf8("Удаление операции"),
                  message, QMessageBox::Yes | QMessageBox::Cancel, this);
  box.button(QMessageBox::Yes)->setText(QString::fromUtf8("Удалить"));
  box.button(QMessageBox::Cancel)->setText(QString::fromUtf8("Отмена"));
  if (box.exec() != QMessageBox::Yes) return;

  const HistorySelectionState previousSelection = captureHistorySelection();
  Document previous = document_;
  const auto removedJoinOperands = removedJoinOperandIds(previous, plan);
  std::string error;
  const bool removed = document_.applyRemovalPlan(plan, &error);
  if (removed)
    releaseUnconsumedJoinOperands(document_, removedJoinOperands);
  const bool recomputed = removed && document_.recompute();
  const std::string rebuildError = recomputed ? std::string{} : document_.rebuildError();
  if (!recomputed) {
    document_ = previous;
    QMessageBox::warning(this, QString::fromUtf8("Ошибка удаления"),
                         QString::fromStdString(
                             error.empty() ? rebuildError : error));
    return;
  }
  auto committedAfter = captureCommittedEndState();
  committedAfter.sketchViewIds.clear();
  committedAfter.presentation.sketchVisibilities.clear();
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const SketchId id = sketchViews_[index].documentSketchId;
    if (!document_.findSketch(id)) continue;
    committedAfter.sketchViewIds.push_back(id);
    committedAfter.presentation.sketchVisibilities.push_back(
        previousSelection.presentation.sketchVisibilities.size() > index
            ? previousSelection.presentation.sketchVisibilities[index]
            : 1U);
  }
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  resetTransientModelingUi();
  for (std::size_t index = sketchViews_.size(); index-- > 0;)
    if (!document_.findSketch(sketchViews_[index].documentSketchId)) {
      viewport_->removeSketch(index);
      sketchViews_.erase(sketchViews_.begin() +
                           static_cast<std::ptrdiff_t>(index));
    }
  viewport_->clearToolPreviewShape(); viewport_->clearToolManipulator();
  viewport_->setSelectedBodyEdges({}); viewport_->setSelectedBodyFaces({});
  toolParametersDock_->hide();
  revolveDock_->hide();
  mirrorDock_->hide();
  linearPatternDock_->hide();
  circularPatternDock_->hide();
  historyPosition_ = 1000000;
  refreshBodyViewFromDocument(); rebuildFeatureTree(); rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::removeBody(BodyId bodyId) {
  if (!ensureHistoryAtEnd()) return;
  const Body* body = document_.findBody(bodyId);
  if (!body) return;
  const FeatureRemovalPlan plan = document_.planBodyRemoval(bodyId);
  if (!plan.applicable) {
    QMessageBox::warning(
        this, QString::fromUtf8("Ошибка удаления"),
        QString::fromStdString(plan.diagnostic.empty()
                                   ? "Не удалось построить план удаления"
                                   : plan.diagnostic));
    return;
  }
  QStringList dependents;
  for (const auto& range : plan.bodyRanges) {
    const Body* dependentBody = document_.findBody(range.bodyId);
    if (!dependentBody) continue;
    for (const FeatureId featureId : range.featureIds) {
      const auto feature = std::find_if(
          dependentBody->features().begin(), dependentBody->features().end(),
          [featureId](const auto& candidate) {
            return candidate && candidate->id() == featureId;
          });
      if (feature != dependentBody->features().end())
        dependents << QString::fromUtf8("• %1 / %2")
                          .arg(QString::fromStdString(dependentBody->name()),
                               QString::fromStdString((*feature)->name()));
    }
  }
  for (const SketchId sketchId : plan.sketchIds) {
    const DocumentSketch* sketch = document_.findSketch(sketchId);
    if (sketch)
      dependents << QString::fromUtf8("• Эскиз / %1")
                        .arg(QString::fromStdString(sketch->name));
  }
  QString removalMessage = QString::fromUtf8("Удалить «%1»?")
                               .arg(QString::fromStdString(body->name()));
  if (!dependents.isEmpty())
    removalMessage += QString::fromUtf8("\n\nТакже будут удалены:\n") +
                      dependents.join(QLatin1Char('\n'));
  QMessageBox box(QMessageBox::Warning, QString::fromUtf8("Удаление Body"),
                  removalMessage, QMessageBox::Yes | QMessageBox::Cancel, this);
  box.button(QMessageBox::Yes)->setText(QString::fromUtf8("Удалить"));
  box.button(QMessageBox::Cancel)->setText(QString::fromUtf8("Отмена"));
  if (box.exec() != QMessageBox::Yes) return;

  const HistorySelectionState previousSelection = captureHistorySelection();
  Document previousDocument = document_;
  const auto removedJoinOperands =
      removedJoinOperandIds(previousDocument, plan);
  std::string error;
  const bool removed = document_.applyRemovalPlan(plan, &error);
  if (removed)
    releaseUnconsumedJoinOperands(document_, removedJoinOperands);
  if (!removed || !document_.recompute()) {
    const std::string rebuildError = document_.rebuildError();
    document_ = previousDocument;
    QMessageBox::warning(
        this, QString::fromUtf8("Ошибка удаления"),
        QString::fromStdString(error.empty() ? rebuildError : error));
    return;
  }

  auto committedAfter = captureCommittedEndState();
  committedAfter.sketchViewIds.clear();
  committedAfter.presentation.sketchVisibilities.clear();
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const SketchId id = sketchViews_[index].documentSketchId;
    if (!document_.findSketch(id)) continue;
    committedAfter.sketchViewIds.push_back(id);
    committedAfter.presentation.sketchVisibilities.push_back(
        previousSelection.presentation.sketchVisibilities.size() > index
            ? previousSelection.presentation.sketchVisibilities[index]
            : 1U);
  }
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  resetTransientModelingUi();
  for (std::size_t index = sketchViews_.size(); index-- > 0;)
    if (!document_.findSketch(sketchViews_[index].documentSketchId)) {
      viewport_->removeSketch(index);
      sketchViews_.erase(sketchViews_.begin() +
                           static_cast<std::ptrdiff_t>(index));
    }
  viewport_->setSelectedBodies({});
  viewport_->setSelectedBodyEdges({});
  viewport_->setSelectedBodyFaces({});
  historyPosition_ = 1000000;
  refreshBodyViewFromDocument();
  rebuildHistoryPanel();
  rebuildFeatureTree();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Body удалён"), 2500);
}

void MainWindow::applyHistoryPosition(int position) {
  const int lastPosition = static_cast<int>(historySteps_.size());
  historyPosition_ = std::clamp(position, 0, lastPosition);
  if (historyTimeline_) historyTimeline_->setPosition(historyPosition_);
  if (historyPosition_ == lastPosition) {
    refreshBodyViewFromDocument();
  } else {
    std::vector<BodyViewShape> shapes;
    for (const Body& body : document_.bodies()) {
      if (const Body* active = document_.activeBody(); active && body.id() == active->id())
        continue;
      if (auto shape = body.lastValidResultShape()) {
        const ShapeFeature* feature = presentationOwner(body, shape);
        if (feature) {
          auto topology = feature->lastValidTopologyIndex();
          if (topology)
            shapes.push_back({body.id(), feature->id(), std::move(shape),
                              std::move(topology), feature->shapeRevision()});
        }
      }
    }
    if (historyPosition_ > 0) {
      const HistoryStep& step = historySteps_[historyPosition_ - 1];
      if (step.shape) {
        const auto* feature = findHistoryFeature(
            static_cast<const Document&>(document_), step.bodyId,
            step.featureId);
        auto topology = feature ? feature->lastValidTopologyIndex() : nullptr;
        if (topology)
          shapes.push_back(
              {step.bodyId, step.featureId, step.shape, std::move(topology),
               feature ? feature->shapeRevision() : kInvalidShapeRevision});
      }
    }
    const bool hasHistoricalShape = !shapes.empty();
    viewport_->setBodyShapes(std::move(shapes));
    viewport_->setSolidVisible(hasHistoricalShape);
  }
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const bool selectedSketch = historyPosition_ > 0 &&
        historySteps_[historyPosition_ - 1].sketchId ==
            sketchViews_[index].documentSketchId;
    viewport_->setSketchVisible(index, selectedSketch);
  }
  const auto buttons = historyContent_->findChildren<QToolButton*>("historyStep");
  for (int index = 0; index < buttons.size(); ++index)
    buttons[index]->setChecked(index + 1 == historyPosition_);
  rebuildFeatureTree();
  statusBar()->showMessage(
      historyPosition_ == lastPosition
          ? QString::fromUtf8("Активно последнее состояние модели")
          : QString::fromUtf8("История откачена до шага %1").arg(historyPosition_),
      2500);
}

bool MainWindow::configureSketchEditContext(bool autoProjectSupportFace) {
  if (!currentSketchFaceReference_) return true;
  const FaceReference reference = *currentSketchFaceReference_;
  const Body* body = document_.findBody(reference.bodyId);
  const ShapeFeature* feature = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == reference.featureId) {
        feature = candidate.get();
        break;
      }
  const auto shape = feature ? feature->shape() : ShapeFeature::ShapePtr{};
  if (!shape) {
    QMessageBox::warning(
        this, QString::fromUtf8("Sketch on Face"),
        QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
    return false;
  }
  const auto topology = feature->topologyIndex();
  const auto resolvedFace = topology
                                ? topology->resolveFace(reference.topology())
                                : FaceResolution{};
  const auto resolved = resolvedFace
                            ? resolveFacePlacement(*resolvedFace.subshape)
                            : ResolvedFacePlacement{};
  if (!resolved.resolved) {
    QMessageBox::warning(
        this, QString::fromUtf8("Sketch on Face"),
        QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
    return false;
  }
  if (!resolved.planar) {
    QMessageBox::information(
        this, QString::fromUtf8("Sketch on Face"),
        QString::fromUtf8("Создание эскиза на криволинейной поверхности пока не поддерживается."));
    return false;
  }
  currentSketchPlacement_ = resolved.placement;
  sketchCanvas_->setSketchEditContext(
      {kInvalidSketchId, currentSketchPlacement_, shape, topology, reference,
       autoProjectSupportFace});
  return true;
}

void MainWindow::configureSketchSceneReferences(SketchId excludedSketchId) {
  const BodyId supportBodyId = currentSketchFaceReference_
                                   ? currentSketchFaceReference_->bodyId
                                   : kInvalidBodyId;
  std::vector<ShapeFeature::ShapePtr> bodyShapes;
  bodyShapes.reserve(document_.bodies().size());
  for (const auto& body : document_.bodies()) {
    if (!body.visible()) continue;
    if (body.id() == supportBodyId) continue;
    if (auto shape = body.resultShape()) bodyShapes.push_back(std::move(shape));
  }

  std::vector<SketchSceneReference> sketches;
  sketches.reserve(document_.sketches().size());
  for (const auto& documentSketch : document_.sketches()) {
    if (documentSketch.id == excludedSketchId) continue;
    // Consumed construction sketches remain upstream of Move and other body
    // features.  Drawing all of them in blue made the pre-transform contours
    // look like a second, stale solid.  Match the 3D tree: show only sketches
    // that are still independently visible construction geometry.
    if (isSketchConsumedByPartDesign(document_, documentSketch.id)) continue;
    sketches.push_back(
        {documentSketch.geometry, documentSketch.placement});
  }
  sketchCanvas_->setSceneReferences(currentSketchPlacement_, bodyShapes,
                                    std::move(sketches));
}

void MainWindow::editSketchStep(std::size_t index) {
  if (index >= sketchViews_.size()) return;
  const SketchId sketchId = sketchViews_[index].documentSketchId;
  if (!document_.findSketch(sketchId)) return;
  resetTransientModelingUi();
  const auto* modelSketch = document_.findSketch(sketchId);
  if (!modelSketch) return;
  editingSketchIndex_ = index;
  currentSketchSupport_ = sketchPresentationLabel(*modelSketch);
  currentSketchFaceReference_.reset();
  currentSketchPlacement_ = modelSketch->placement;
  if (modelSketch->support.type == SketchSupportType::Face)
    currentSketchFaceReference_ = modelSketch->support.face;
  sketchCanvas_->clearSketchEditContext();
  sketchCanvas_->setInitialViewUp({});
  if (currentSketchFaceReference_ && !configureSketchEditContext()) {
    editingSketchIndex_.reset();
    return;
  }
  sketchCanvas_->loadSketch(modelSketch->geometry);
  configureSketchSceneReferences(sketchId);
  sketchCanvas_->setReferenceBody(document_.box(), currentSketchPlacement_,
                                  hasDisplayableModernSolid() ||
                                      hasHistoricalLegacyExtrusion());
  workspaceStack_->setCurrentWidget(sketchCanvas_);
  ribbonStack_->setCurrentWidget(sketchRibbon_);
  statusBar()->showMessage(
      QString::fromUtf8("Редактируется эскиз %1 • Завершите эскиз для перестроения")
          .arg(index + 1));
}

void MainWindow::editSketchById(SketchId sketchId) {
  const auto found = std::find_if(
      sketchViews_.begin(), sketchViews_.end(), [sketchId](const auto& entry) {
        return entry.documentSketchId == sketchId;
      });
  if (found != sketchViews_.end())
    editSketchStep(static_cast<std::size_t>(
        std::distance(sketchViews_.begin(), found)));
}

void MainWindow::editHistoryFeature(BodyId bodyId, FeatureId featureId) {
  Body* body = document_.findBody(bodyId);
  if (!body) return;
  ShapeFeature* feature = findHistoryFeature(document_, bodyId, featureId);
  if (!feature) return;
  const auto* descriptor = featureUiDescriptor(feature->kind());
  if (!descriptor || !descriptor->editable ||
      descriptor->editorRoute == FeatureEditorRoute::None)
    return;
  // Switching directly from one editor to another first terminates the old
  // transaction. resetTransientModelingUi() continues clearing shared
  // viewport state after controller callbacks, so restore the old committed
  // state only after the complete teardown, then snapshot the new editor.
  if (editUiTransaction_) {
    const auto previousEdit = *editUiTransaction_;
    suppressEditCancelRestore_ = true;
    try {
      resetTransientModelingUi();
    } catch (...) {
      suppressEditCancelRestore_ = false;
      editUiTransaction_.reset();
      restoreHistorySelection(previousEdit.before);
      applyPresentationState(previousEdit.before.presentation);
      setWindowModified(previousEdit.modified);
      throw;
    }
    suppressEditCancelRestore_ = false;
    editUiTransaction_.reset();
    restoreHistorySelection(previousEdit.before);
    applyPresentationState(previousEdit.before.presentation);
    setWindowModified(previousEdit.modified);
  }
  const auto before = captureHistorySelection();
  const bool modified = isWindowModified();
  editUiTransaction_ = EditUiTransaction{
      before, bodyId, featureId, modified};
  suppressEditCancelRestore_ = true;
  try {
    resetTransientModelingUi();
  } catch (...) {
    suppressEditCancelRestore_ = false;
    editUiTransaction_.reset();
    restoreHistorySelection(before);
    applyPresentationState(before.presentation);
    setWindowModified(modified);
    throw;
  }
  suppressEditCancelRestore_ = false;

  const auto transactionMatchesRequest = [this, bodyId, featureId] {
    return editUiTransaction_ && editUiTransaction_->bodyId == bodyId &&
           editUiTransaction_->featureId == featureId;
  };
  const auto editorSuccessfullyActive = [this, featureId] {
    const auto state = partDesignCoordinator_.snapshot(
        partDesignCoordinator_.activeTool(), document_);
    return state.lifecycle != ToolLifecycle::Inactive &&
           state.editingFeatureId == std::optional<FeatureId>{featureId};
  };
  const auto abortFailedDispatch = [this, &transactionMatchesRequest] {
    if (!transactionMatchesRequest()) return;
    const EditUiTransaction transaction = *editUiTransaction_;
    suppressEditCancelRestore_ = true;
    try {
      resetTransientModelingUi();
    } catch (...) {
      suppressEditCancelRestore_ = false;
      editUiTransaction_.reset();
      throw;
    }
    suppressEditCancelRestore_ = false;
    editUiTransaction_.reset();
    restoreHistorySelection(transaction.before);
    applyPresentationState(transaction.before.presentation);
    setWindowModified(transaction.modified);
  };

  try {
    switch (descriptor->editorRoute) {
      case FeatureEditorRoute::Extrude:
        editExtrusionStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Pocket:
        editPocketStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Fillet:
        editFilletStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Chamfer:
        editChamferStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::JoinBodies:
        editJoinBodiesStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Revolve:
      case FeatureEditorRoute::Mirror:
      case FeatureEditorRoute::Move:
      case FeatureEditorRoute::LinearPattern:
      case FeatureEditorRoute::CircularPattern:
      case FeatureEditorRoute::Shell:
      case FeatureEditorRoute::Draft:
        editPatternFeature(featureId, descriptor->editorRoute);
        break;
      case FeatureEditorRoute::None:
        break;
    }
  } catch (...) {
    abortFailedDispatch();
    throw;
  }

  // Modal editors consume the transaction before returning. Persistent
  // editors retain it only when their exact feature/session is active. Every
  // unsupported or invalid-source early return is therefore an atomic no-op,
  // including selection/history/modified state and transient presentation.
  if (transactionMatchesRequest() && !editorSuccessfullyActive())
    abortFailedDispatch();
}

void MainWindow::editExtrusionStep(BodyId bodyId, FeatureId featureId) {
  Body* body = document_.findBody(bodyId);
  ExtrudeFeature* extrude = nullptr;
  std::size_t extrudeIndex = 0;
  if (body)
    for (std::size_t index = 0; index < body->features().size(); ++index)
      if (auto* candidate =
              dynamic_cast<ExtrudeFeature*>(body->features()[index].get());
          candidate && candidate->id() == featureId) {
        extrude = candidate;
        extrudeIndex = index;
        break;
      }
  if (!extrude) return;
  if (extrude->isFaceSource()) {
    editFaceExtrudeStep(body, extrude, extrudeIndex);
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(QString::fromUtf8("Изменить выдавливание"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* form = new QFormLayout;
  auto* distance = new QDoubleSpinBox(&dialog);
  distance->setRange(-100000.0, 100000.0);
  distance->setDecimals(2);
  distance->setSuffix(QStringLiteral(" mm"));
  distance->setValue(extrude->reversed() ? -extrude->lengthMm()
                                           : extrude->lengthMm());
  auto* operation = new QComboBox(&dialog);
  operation->addItems({QString::fromUtf8("Новое тело"),
                       QString::fromUtf8("Объединить"),
                       QString::fromUtf8("Вырезать")});
  operation->setCurrentIndex(static_cast<int>(extrude->operation()));
  // Moving an existing feature between Bodies is intentionally outside 2.0.
  if (extrude->operation() == ExtrudeOperation::NewBody)
    operation->setEnabled(false);
  else
    operation->setItemData(0, 0, Qt::UserRole - 1);
  form->addRow(QString::fromUtf8("Расстояние:"), distance);
  form->addRow(QString::fromUtf8("Операция:"), operation);
  layout->addLayout(form);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                        QDialogButtonBox::Cancel, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const double height = distance->value();
  Document previousDocument = document_;
  const HistorySelectionState previousSelection = captureHistorySelection();
  extrude->setLengthMm(std::abs(height));
  extrude->setReversed(height < 0.0);
  if (extrude->operation() != ExtrudeOperation::NewBody)
    extrude->setOperation(static_cast<ExtrudeOperation>(operation->currentIndex()));
  body->markDirtyFrom(extrudeIndex);
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(extrude->error());
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    QMessageBox::warning(this, QString::fromUtf8("Ошибка выдавливания"), error);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Выдавливание изменено: %1 мм").arg(height), 3000);
}

void MainWindow::editPocketStep(BodyId bodyId, FeatureId featureId) {
  Body* body = document_.findBody(bodyId);
  PocketFeature* pocket = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == featureId) {
        pocket = dynamic_cast<PocketFeature*>(candidate.get()); break;
      }
  if (!pocket) return;
  bool accepted = false;
  const double depth = QInputDialog::getDouble(
      this, QString::fromUtf8("Изменить карман"),
      QString::fromUtf8("Глубина, мм:"), pocket->depthMm(),
      0.01, 100000.0, 2, &accepted);
  if (!accepted) {
    restoreCancelledEditUiTransaction();
    return;
  }
  if (!acceptPocketTransition(pocket->profileSketchId(), depth, pocket->id()))
    restoreCancelledEditUiTransaction();
}

void MainWindow::editFilletStep(BodyId bodyId, FeatureId featureId) {
  Body* body = document_.findBody(bodyId);
  FilletFeature* fillet = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == featureId) {
        fillet = dynamic_cast<FilletFeature*>(candidate.get()); break;
      }
  if (!fillet) return;
  const auto& features = body->features();
  if (features.size() < 2) return;
  ShapeFeature::ShapePtr upstream;
  ShapeFeature* upstreamFeature = nullptr;
  for (std::size_t index = 1; index < features.size(); ++index)
    if (features[index].get() == fillet) {
      upstream = features[index - 1]->shape();
      upstreamFeature = features[index - 1].get();
      break;
    }
  if (!upstream || !upstreamFeature) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginFillet(
              body->id(), fillet->edge().featureId, upstream, fillet->edges(),
              fillet->radiusMm(), fillet->id(),
              upstreamFeature->topologyIndex()),
          PartDesignToolKind::Fillet))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Fillet),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Радиус"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterValue(fillet->radiusMm());
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFilletToolPreview();
  viewport_->setSelectedBodyEdges(fillet->edges());
}

void MainWindow::editChamferStep(BodyId bodyId, FeatureId featureId) {
  Body* body = document_.findBody(bodyId);
  ChamferFeature* chamfer = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == featureId) {
        chamfer = dynamic_cast<ChamferFeature*>(candidate.get()); break;
      }
  if (!chamfer) return;
  const auto& features = body->features();
  if (features.size() < 2) return;
  ShapeFeature::ShapePtr upstream;
  ShapeFeature* upstreamFeature = nullptr;
  for (std::size_t index = 1; index < features.size(); ++index)
    if (features[index].get() == chamfer) {
      upstream = features[index - 1]->shape();
      upstreamFeature = features[index - 1].get();
      break;
    }
  if (!upstream || !upstreamFeature) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginChamfer(
              body->id(), chamfer->edge().featureId, upstream,
              chamfer->edges(), chamfer->distanceMm(), chamfer->id(),
              upstreamFeature->topologyIndex()),
          PartDesignToolKind::Chamfer))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Chamfer),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Размер"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterValue(chamfer->distanceMm());
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateChamferToolPreview();
  viewport_->setSelectedBodyEdges(chamfer->edges());
}

void MainWindow::rebuildFeatureTree() {
  const QSignalBlocker blocker(featureTree_);
  featureTree_->clear();
  auto* project = new QTreeWidgetItem(featureTree_, {QString::fromUtf8("Корпус детали")});
  auto* origin = new QTreeWidgetItem(project, {QString::fromUtf8("Начало координат")});
  origin->setData(0, Qt::UserRole, 1);
  origin->setFlags(origin->flags() | Qt::ItemIsUserCheckable);
  origin->setCheckState(
      0, viewport_->originVisible() ? Qt::Checked : Qt::Unchecked);
  for (int plane = 0; plane < 3; ++plane) {
    const QString name = plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ";
    auto* planeItem = new QTreeWidgetItem(
        origin, {QString::fromUtf8("Плоскость ") + name});
    planeItem->setData(0, Qt::UserRole, 10 + plane);
    planeItem->setFlags(planeItem->flags() | Qt::ItemIsUserCheckable);
    planeItem->setCheckState(
        0, viewport_->basePlaneVisible(plane) ? Qt::Checked : Qt::Unchecked);
  }

  auto* sketches = new QTreeWidgetItem(project, {QString::fromUtf8("Эскизы")});
  const std::size_t activeSketchCount = std::min<std::size_t>(
      static_cast<std::size_t>(std::max(historyPosition_, 0)),
      sketchViews_.size());
  // During a just-committed direct operation rebuildFeatureTree() can run
  // before rebuildHistoryPanel() has appended the new history step. Treat the
  // previous end marker as "at end" so the newly created Body is not hidden.
  const bool historyAtEnd =
      historyPosition_ >= static_cast<int>(historySteps_.size());
  const bool activeSolid = historyAtEnd
      ? hasCommittedModernSolid() || hasHistoricalLegacyExtrusion()
      : historyPosition_ > 0 &&
            historyPosition_ <= static_cast<int>(historySteps_.size()) &&
            historySteps_[historyPosition_ - 1].shape;
  if (activeSketchCount == 0) {
    new QTreeWidgetItem(sketches, {QString::fromUtf8("Эскизов нет")});
  } else {
    for (std::size_t index = 0; index < activeSketchCount; ++index) {
      auto* sketchItem = new QTreeWidgetItem(
          sketches, {QString::fromUtf8("⌞  Эскиз %1").arg(index + 1)});
      sketchItem->setData(0, Qt::UserRole, 20 + static_cast<int>(index));
      sketchItem->setFlags(sketchItem->flags() | Qt::ItemIsUserCheckable);
      const SketchId sketchId = sketchViews_[index].documentSketchId;
      bool visible = true;
      if (historyAtEnd) {
        // Modern consumption comes only from Document dependencies: direct
        // Join/Cut and downstream features may consume several profiles. A
        // historical v1 extrusion has no Body graph, so its source SketchId
        // is intentionally used only by this compatibility presentation.
        const bool consumedByHistoricalLegacyExtrusion =
            hasHistoricalLegacyExtrusion() &&
            historicalLegacyExtrusionSourceSketchId_ == sketchId;
        visible = !consumedByHistoricalLegacyExtrusion &&
                  !isSketchConsumedByPartDesign(document_, sketchId);
      } else {
        // Keep the tree checkbox synchronized with applyHistoryPosition():
        // when inspecting an earlier history step, only the selected sketch
        // step is shown; feature steps show no construction sketch by default.
        visible =
            historyPosition_ > 0 &&
            historyPosition_ <= static_cast<int>(historySteps_.size()) &&
            historySteps_[historyPosition_ - 1].sketchId == sketchId;
      }
      sketchItem->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      viewport_->setSketchVisible(index, visible);
    }
  }
  auto* models = new QTreeWidgetItem(project, {QString::fromUtf8("Модели")});
  if (!activeSolid || document_.bodies().empty()) {
    new QTreeWidgetItem(models, {QString::fromUtf8("Твёрдых тел нет")});
  } else {
    for (std::size_t bodyIndex = 0; bodyIndex < document_.bodies().size(); ++bodyIndex) {
      const Body& body = document_.bodies()[bodyIndex];
      const QString bodyName = body.name().empty()
                                   ? QString::fromUtf8("Body%1").arg(
                                         bodyIndex + 1, 3, 10,
                                         QLatin1Char('0'))
                                   : QString::fromStdString(body.name());
      auto* bodyItem = new QTreeWidgetItem(
          models, {QString::fromUtf8("▣  ") + bodyName});
      bodyItem->setData(0, Qt::UserRole, 3);
      bodyItem->setData(0, Qt::UserRole + 2,
                        QVariant::fromValue<qulonglong>(body.id()));
      bodyItem->setFlags(bodyItem->flags() | Qt::ItemIsUserCheckable);
      bodyItem->setCheckState(0,
                              body.visible() ? Qt::Checked : Qt::Unchecked);
    }
  }
  featureTree_->expandAll();
}

void MainWindow::exportPdf() {
  const QString fileName = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Экспорт чертежа в PDF"), "drawing-a4.pdf",
      QString::fromUtf8("PDF (*.pdf)"));
  if (fileName.isEmpty()) return;

  QPrinter printer(QPrinter::HighResolution);
  printer.setOutputFormat(QPrinter::PdfFormat);
  printer.setOutputFileName(fileName);
  printer.setPageSize(QPageSize(QPageSize::A4));
  printer.setPageOrientation(QPageLayout::Portrait);
  printer.setFullPage(true);

  QPainter painter;
  if (!painter.begin(&printer)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка"),
                          QString::fromUtf8("Не удалось создать PDF."));
    return;
  }
  drawing::EskdRenderer::renderA4(painter, painter.viewport(),
                                  sketchCanvas_->sketch(), {},
                                  drawingSheet_->sourceShape());
  painter.end();
  statusBar()->showMessage(QString::fromUtf8("PDF сохранён: ") + fileName,
                           4000);
}

void MainWindow::printDrawing() {
  QPrinter printer(QPrinter::HighResolution);
  printer.setPageSize(QPageSize(QPageSize::A4));
  printer.setPageOrientation(QPageLayout::Portrait);
  printer.setFullPage(true);
  QPrintDialog dialog(&printer, this);
  dialog.setWindowTitle(QString::fromUtf8("Печать чертежа A4"));
  if (dialog.exec() != QDialog::Accepted) return;

  QPainter painter(&printer);
  drawing::EskdRenderer::renderA4(painter, painter.viewport(),
                                  sketchCanvas_->sketch(), {},
                                  drawingSheet_->sourceShape());
}

}  // namespace solidar

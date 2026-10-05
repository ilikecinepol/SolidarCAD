#include "ui/MainWindow.h"

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
#include "ui/HistoryTimelineWidget.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/CircularPatternFeature.h"

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
#include "io/StlExporter.h"
#include "io/StepExchange.h"
#include "project/ProjectFile.h"
#include "sketch/SketchRibbon.h"
#include "ui/DrawingSheetView.h"
#include "ui/ModelRibbon.h"
#include "ui/SettingsWidget.h"
#include "ui/SketchCanvas.h"
#include "ui/Viewport.h"

namespace solidar {

namespace {
// Maps stable technical model-layer errors to Russian user messages at the UI
// boundary. The model keeps technical English strings; only the UI translates.
QString localizedFaceToolError(const std::string& technical) {
  const QString error = QString::fromStdString(technical);
  if (error.contains(QStringLiteral("does not intersect the body")))
    return QString::fromUtf8("Выдавливание не пересекает тело.");
  if (error.contains(QStringLiteral("ambiguous")))
    return QString::fromUtf8("Выбранная грань не может быть однозначно определена.");
  if (error.contains(QStringLiteral("planar")) ||
      error.contains(QStringLiteral("must be planar")))
    return QString::fromUtf8("Для выдавливания выберите плоскую грань.");
  if (error.contains(QStringLiteral("could not be resolved")) ||
      error.contains(QStringLiteral("no longer has a geometric match")) ||
      error.contains(QStringLiteral("legacy fallback failed")) ||
      error.contains(QStringLiteral("face no longer matches")) ||
      error.contains(QStringLiteral("unavailable")))
    return QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели.");
  if (error.contains(QStringLiteral("curved surfaces")))
    return QString::fromUtf8("Создание эскиза на криволинейной поверхности пока не поддерживается.");
  return QString::fromUtf8("Не удалось определить выбранную поверхность.");
}

QString localizedPartDesignError(PartDesignToolKind kind,
                                 const std::string& technical) {
  const QString error = QString::fromStdString(technical);
  if (error.contains(QStringLiteral("could not be resolved")) ||
      error.contains(QStringLiteral("no longer match")) ||
      error.contains(QStringLiteral("different Body")))
    return QString::fromUtf8(
        "Выбранная геометрия больше не соответствует текущей модели. "
        "Выберите её заново.");

  switch (kind) {
    case PartDesignToolKind::Fillet:
      return QString::fromUtf8(
          "Не удалось построить скругление. Радиус слишком велик для "
          "выбранной геометрии или приводит к самопересечению.");
    case PartDesignToolKind::Chamfer:
      return QString::fromUtf8(
          "Не удалось построить фаску. Размер слишком велик для выбранной "
          "геометрии или приводит к самопересечению.");
    case PartDesignToolKind::Shell:
      return QString::fromUtf8(
          "Не удалось построить оболочку. Толщина слишком велика или "
          "приводит к самопересечению.");
    case PartDesignToolKind::Draft:
      return QString::fromUtf8(
          "Не удалось построить уклон. Проверьте угол, направление и "
          "выбранные грани.");
    default:
      break;
  }
  return error.isEmpty()
             ? QString::fromUtf8("Не удалось построить предпросмотр операции.")
             : error;
}
}  // namespace

MainWindow::MainWindow(AppSettings& settings, QWidget* parent)
    : QMainWindow(parent), settings_(settings) {
  buildUi();
  buildMenus();
  resize(1200, 760);
  setWindowTitle(QString::fromUtf8("Солидарность CAD — Скетчер ЕСКД [*]"));
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

void MainWindow::pushUndoAction(std::function<void()> action) {
  if (applyingUndo_) return;
  modelUndoStack_.push_back({std::move(action), nullptr});
  modelRedoStack_.clear();
  if (modelUndoStack_.size() > 100) modelUndoStack_.erase(modelUndoStack_.begin());
  setWindowModified(true);
  updateUndoAvailability();
}

void MainWindow::pushUndoRedoAction(std::function<void()> undo,
                                    std::function<void()> redo) {
  if (applyingUndo_) return;
  modelUndoStack_.push_back({std::move(undo), std::move(redo)});
  modelRedoStack_.clear();
  if (modelUndoStack_.size() > 100) modelUndoStack_.erase(modelUndoStack_.begin());
  setWindowModified(true);
  updateUndoAvailability();
}

void MainWindow::updateUndoAvailability() {
  if (!undoAction_) return;
  const bool sketchActive =
      workspaceStack_ && workspaceStack_->currentWidget() == sketchCanvas_;
  undoAction_->setEnabled(sketchActive ? sketchCanvas_->canUndo()
                                       : !modelUndoStack_.empty());
  if (redoAction_) {
    redoAction_->setEnabled(sketchActive ? sketchCanvas_->canRedo()
                                         : !modelRedoStack_.empty());
  }
}

void MainWindow::resetTransientModelingUi() {
  // PartDesignToolController owns modern sessions, while legacy sketch
  // extrusion and the dedicated Revolve panel have separate presentation.
  // Reset all presentation surfaces together before another ribbon command
  // starts; otherwise switching between generations can leave two parameter
  // windows active at once.
  partDesignTools_.cancelActive();
  viewport_->resetToolInteraction();
  selectedExtrusionSurface_.clear();
  if (toolParametersDock_) toolParametersDock_->hide();
  if (revolveDock_) revolveDock_->hide();
  if (mirrorDock_) mirrorDock_->hide();
  if (moveDock_) moveDock_->hide();
  if (linearPatternDock_) linearPatternDock_->hide();
  if (circularPatternDock_) circularPatternDock_->hide();
  if (extrusionDock_) extrusionDock_->hide();
}

void MainWindow::undoLastAction() {
  bool changed = false;
  if (workspaceStack_->currentWidget() == sketchCanvas_) {
    if (sketchCanvas_->canUndo()) {
      sketchCanvas_->undo();
      changed = true;
    }
  } else if (!modelUndoStack_.empty()) {
    auto entry = std::move(modelUndoStack_.back());
    modelUndoStack_.pop_back();
    applyingUndo_ = true;
    entry.first();
    applyingUndo_ = false;
    if (entry.second) modelRedoStack_.push_back(std::move(entry));
    changed = true;
  }
  if (changed) setWindowModified(true);
  updateUndoAvailability();
}

void MainWindow::redoLastAction() {
  if (workspaceStack_->currentWidget() == sketchCanvas_) {
    if (sketchCanvas_->canRedo()) {
      sketchCanvas_->redo();
      setWindowModified(true);
    }
    updateUndoAvailability();
    return;
  }
  if (modelRedoStack_.empty()) return;
  auto entry = std::move(modelRedoStack_.back());
  modelRedoStack_.pop_back();
  applyingUndo_ = true;
  entry.second();
  applyingUndo_ = false;
  modelUndoStack_.push_back(std::move(entry));
  setWindowModified(true);
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
  QString error;
  if (!project::ProjectFile::create(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка создания"), error);
    return;
  }
  // Use exactly the same document-transition path as Open.  Maintaining a
  // second hand-written reset sequence here is what allowed controller/session
  // state to outlive the Document that owned its B-Rep references.
  if (!loadProject(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка создания"), error);
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
  project::ProjectData projectData;
  if (!project::ProjectFile::load(path, &projectData, error)) return false;
  Document restoredDocument;
  QString modelError;
  const bool hasParametricHistory = project::ProjectFile::loadDocument(
      path, &restoredDocument, &modelError);

  // Tear down every piece of transient UI state while the old Document is
  // still alive.  PartDesignToolController is authoritative for the active
  // tool; cancelling only the individual sessions leaves controller.active_
  // pointing at an inactive session and allows later viewport events to enter
  // stale presentation callbacks.
  partDesignTools_.cancelActive();
  filletToolSession_.cancel();
  chamferToolSession_.cancel();
  shellToolSession_.cancel();
  draftToolSession_.cancel();
  faceExtrudeSession_.cancel();
  revolveToolSession_.cancel();
  mirrorToolSession_.cancel();
  moveToolSession_.cancel();
  linearPatternToolSession_.cancel();
  circularPatternToolSession_.cancel();

  if (modelRibbon_) modelRibbon_->clearActiveTool();
  if (toolParametersDock_) toolParametersDock_->hide();
  if (revolveDock_) revolveDock_->hide();
  if (mirrorDock_) mirrorDock_->hide();
  if (moveDock_) moveDock_->hide();
  if (linearPatternDock_) linearPatternDock_->hide();
  if (circularPatternDock_) circularPatternDock_->hide();
  if (extrusionDock_) extrusionDock_->hide();

  viewport_->clearToolManipulator();
  viewport_->clearToolPreviewShape();
  viewport_->hideExtrusionManipulator();
  viewport_->setEdgeMultiSelectionMode(false);
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyEdges({});
  viewport_->setSelectedBodyFaces({});

  // Only now is it safe to destroy the old model/B-Rep graph.
  document_ = hasParametricHistory ? std::move(restoredDocument) : Document{};
  if (!hasParametricHistory) document_.setBox(projectData.box);
  selectedExtrusionSurface_.clear();
  currentSketchSupport_ = QStringLiteral("XY");
  currentSketchPlacement_ = SketchPlacement::xy();
  currentSketchFaceReference_.reset();
  sketchHistory_.clear();
  viewport_->resetScene();
  viewport_->setBox(document_.box());
  for (std::size_t index = 0; index < projectData.sketches.size(); ++index) {
    const auto& saved = projectData.sketches[index];
    DocumentSketch* modelSketch =
        hasParametricHistory && index < document_.sketches().size()
            ? &document_.sketches()[index]
            : &document_.addSketch("Loaded sketch");
    if (!hasParametricHistory) {
      modelSketch->geometry = saved.geometry;
      modelSketch->placement = saved.support.contains(QStringLiteral("XZ"))
                                   ? SketchPlacement::xz()
                                   : saved.support.contains(QStringLiteral("YZ"))
                                         ? SketchPlacement::yz()
                                         : SketchPlacement::xy();
    }
    sketchHistory_.push_back(
        {modelSketch->geometry, saved.support, modelSketch->id});
    viewport_->addSketch(modelSketch->geometry, saved.support,
                         modelSketch->placement);
  }
  sketchCount_ = sketchHistory_.size();
  hasExtrusion_ = hasParametricHistory ? !document_.bodies().empty()
                                       : projectData.hasExtrusion;
  extrusionSourceSketch_ = projectData.extrusionSourceSketch;
  if (hasExtrusion_ && extrusionSourceSketch_ &&
      *extrusionSourceSketch_ < sketchHistory_.size()) {
    const auto& source = sketchHistory_[*extrusionSourceSketch_];
    viewport_->setSolidSketch(source.geometry);
    viewport_->setSolidSupport(source.support);
  }
  viewport_->setSolidVisible(hasExtrusion_);
  if (hasParametricHistory) refreshBodyViewFromDocument();
  editingSketchIndex_.reset();
  extrudeOperationManuallyChanged_ = false;
  modelUndoStack_.clear();
  modelRedoStack_.clear();
  sketchCanvas_->resetSketch();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  // The modern source of truth is buildPartDesignHistory (via historySteps_):
  // after load/recompute the marker sits at the actual end and the viewport
  // shows the final Body result.
  moveHistoryToEnd();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
  setProjectPath(path);
  setWindowModified(false);
  return true;
}

void MainWindow::saveProject() {
  // The Sketcher edits a working copy. Commit it before serializing so Save
  // never reports success while silently leaving the visible sketch out of
  // the project file.
  if (workspaceStack_->currentWidget() == sketchCanvas_) finishSketch();

  QString path = windowFilePath();
  if (path.isEmpty()) {
    path = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("Сохранить проект"), QStringLiteral("Новый проект.solidar"),
        QString::fromUtf8("Проекты Солидарность CAD (*.solidar)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".solidar"), Qt::CaseInsensitive))
      path += QStringLiteral(".solidar");
  }
  QString error;
  if (!project::ProjectFile::saveDocument(path, document_, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка сохранения"), error);
    return;
  }
  setProjectPath(path);
  setWindowModified(false);
  statusBar()->showMessage(QString::fromUtf8("Проект сохранён"), 3000);
}

void MainWindow::importStep() {
  const QString fileName = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Импортировать STEP"), QString(),
      QStringLiteral("STEP files (*.step *.stp)"));
  if (fileName.isEmpty()) return;

  // Build the complete result separately. No model or UI state changes until
  // the translator and imported feature have both succeeded.
  Document staged = document_;
  QString error;
  const QString importedName = QFileInfo(fileName).completeBaseName();
  if (!io::importDocumentStep(fileName, &staged, importedName, &error)) {
    QMessageBox::critical(
        this, QString::fromUtf8("Не удалось импортировать STEP"), error);
    return;
  }

  // Sessions can retain topology references into the current Document. Tear
  // them down before replacing its B-Rep graph.
  partDesignTools_.cancelActive();
  filletToolSession_.cancel();
  chamferToolSession_.cancel();
  shellToolSession_.cancel();
  draftToolSession_.cancel();
  faceExtrudeSession_.cancel();
  revolveToolSession_.cancel();
  viewport_->clearToolManipulator();
  viewport_->clearToolPreviewShape();
  viewport_->setSelectedBodyEdges({});
  viewport_->setSelectedBodyFaces({});

  document_ = std::move(staged);
  hasExtrusion_ = !document_.bodies().empty();
  historyPosition_ = 1000000;
  rebuildHistoryPanel();
  moveHistoryToEnd();
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  workspaceStack_->setCurrentWidget(viewport_);
  viewport_->fitAll();
  setWindowModified(true);
  statusBar()->showMessage(
      QString::fromUtf8("STEP импортирован: ") + fileName, 5000);
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

  QString error;
  if (!io::exportDocumentStep(fileName, document_, &error)) {
    QMessageBox::critical(
        this, QString::fromUtf8("Не удалось экспортировать STEP"), error);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("STEP сохранён: ") + fileName,
                           5000);
}

void MainWindow::exportStl() {
  if (!hasExtrusion_) {
    QMessageBox::information(
        this, QString::fromUtf8("Экспорт STL"),
        QString::fromUtf8("Сначала создайте выдавленное трёхмерное тело."));
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

  QString error;
  if (!io::exportDocumentAsciiStl(fileName, document_, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка экспорта STL"), error);
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
            rebuildRevolveAxisChoices();
            revolveToolSession_.clearAxis();
            const auto id = static_cast<SketchId>(revolveProfileCombo_->currentData().toULongLong());
            if (id == kInvalidSketchId) {
              revolveToolSession_.clearProfile();
              revolveProfileSummary_->setText(
                  QString::fromUtf8("Профиль не выбран"));
              viewport_->beginExtrusionSurfaceSelection();
            } else {
              revolveToolSession_.setProfile(id);
              revolveProfileSummary_->setText(
                  QString::fromUtf8("Выбран весь эскиз"));
              const auto found = std::find_if(
                  sketchHistory_.begin(), sketchHistory_.end(),
                  [id](const auto& entry) {
                    return entry.documentSketchId == id;
                  });
              if (found != sketchHistory_.end())
                viewport_->beginRevolveAxisSelection(static_cast<std::size_t>(
                    std::distance(sketchHistory_.begin(), found)));
            }
            updateRevolveToolPreview();
          });
  connect(revolveAxisCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            if (revolveAxisCombo_->currentIndex() <= 0) {
              revolveToolSession_.clearAxis(); updateRevolveToolPreview(); return;
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
              reference.sketchId = revolveToolSession_.profileSketchId();
              if (value == 1)
                reference.type = AxisReferenceType::SketchHorizontalAxis;
              else if (value == 2)
                reference.type = AxisReferenceType::SketchVerticalAxis;
              else {
                reference.type = AxisReferenceType::SketchLine;
                reference.lineId = static_cast<sketch::GeometryId>(value - 3);
              }
            }
            revolveToolSession_.setAxis(reference); updateRevolveToolPreview();
          });
  connect(revolveAngleSpin_, &QDoubleSpinBox::valueChanged, this,
          [this](double value) { revolveToolSession_.setAngleFromPanel(value);
                                 updateRevolveToolPreview(); });
  connect(revolveOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int value) { revolveToolSession_.setOperation(
              static_cast<ExtrudeOperation>(value)); updateRevolveToolPreview(); });
  connect(revolveReverseCheck_, &QCheckBox::toggled, this,
          [this](bool value) { revolveToolSession_.setReversed(value);
                               updateRevolveToolPreview(); });
  connect(revolveAcceptButton_, &QPushButton::clicked, this,
          &MainWindow::acceptRevolveTool);
  connect(cancelRevolve, &QPushButton::clicked, this,
          &MainWindow::cancelRevolveTool);
  const auto acceptRevolveOnEnter = [this] {
    if (!revolveDock_->isVisible()) return;
    revolveAngleSpin_->interpretText();
    if (revolveAcceptButton_->isEnabled())
      acceptRevolveTool();
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
    partDesignTools_.beginReselection(ToolSelectionStage::SelectingInput);
    revolveToolSession_.clearAxis();
    revolveToolSession_.clearProfile();
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
    std::size_t sketchIndex = static_cast<std::size_t>(-1);
    const auto found = std::find_if(
        sketchHistory_.begin(), sketchHistory_.end(), [this](const auto& entry) {
          return entry.documentSketchId == revolveToolSession_.profileSketchId();
        });
    if (found != sketchHistory_.end())
      sketchIndex = static_cast<std::size_t>(
          std::distance(sketchHistory_.begin(), found));
    partDesignTools_.beginReselection(ToolSelectionStage::SelectingReference);
    revolveToolSession_.clearAxis();
    viewport_->clearToolManipulator();
    updateRevolveToolPreview();
    viewport_->beginRevolveAxisSelection(sketchIndex);
    viewport_->setFocus();
    statusBar()->showMessage(
        sketchIndex < sketchHistory_.size()
            ? QString::fromUtf8("2/3 Выберите глобальную ось или линию эскиза")
            : QString::fromUtf8("Выберите глобальную ось; линии эскиза станут доступны после выбора профиля"));
  });
  connect(viewport_, &Viewport::angularToolManipulatorValueChanged, this,
          [this](double angle) {
            if (revolveToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              revolveToolSession_.setAngleFromManipulator(angle);
              revolveAngleSpin_->setValue(revolveToolSession_.angleDeg());
              updateRevolveToolPreview();
            } else if (circularPatternToolSession_.lifecycle() !=
                       ToolLifecycle::Inactive) {
              circularPatternToolSession_.setAngleDeg(angle);
              const QSignalBlocker blocker(circularPatternAngleSpin_);
              circularPatternAngleSpin_->setValue(
                  circularPatternToolSession_.angleDeg());
              updateCircularPatternToolPreview();
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
    partDesignTools_.beginReselection(ToolSelectionStage::SelectingInput);
    mirrorToolSession_.clearBody();
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
    if (mirrorToolSession_.bodyId() == kInvalidBodyId) return;
    partDesignTools_.beginReselection(ToolSelectionStage::SelectingReference);
    mirrorToolSession_.clearPlane();
    mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
    viewport_->clearToolPreviewShape();
    viewport_->beginMirrorPlaneSelection();
    updateMirrorToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(
        QString::fromUtf8("2/2 Выберите базовую плоскость в 3D-виде"));
  });
  connect(mirrorAcceptButton_, &QPushButton::clicked, this,
          &MainWindow::acceptMirrorTool);
  connect(cancelMirror, &QPushButton::clicked, this,
          &MainWindow::cancelMirrorTool);
  auto* mirrorReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), mirrorDock_);
  mirrorReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorReturnShortcut->setAutoRepeat(false);
  connect(mirrorReturnShortcut, &QShortcut::activated, this, [this] {
    if (mirrorDock_->isVisible() && mirrorAcceptButton_->isEnabled())
      acceptMirrorTool();
  });
  auto* mirrorKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), mirrorDock_);
  mirrorKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorKeypadShortcut->setAutoRepeat(false);
  connect(mirrorKeypadShortcut, &QShortcut::activated, this, [this] {
    if (mirrorDock_->isVisible() && mirrorAcceptButton_->isEnabled())
      acceptMirrorTool();
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
    partDesignTools_.beginReselection(ToolSelectionStage::SelectingInput);
    moveToolSession_.clearBody();
    moveBodyValue_->setText(QString::fromUtf8("Не выбрано"));
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    viewport_->beginMoveBodySelection();
    updateMoveToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(QString::fromUtf8("Выберите тело в 3D-виде"));
  });
  const auto moveValueChanged = [this](double) {
    moveToolSession_.setOffsetMm(
        {moveXSpin_->value(), moveYSpin_->value(), moveZSpin_->value()});
    updateMoveToolPreview();
  };
  connect(moveXSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveYSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveZSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveAcceptButton_, &QPushButton::clicked, this,
          &MainWindow::acceptMoveTool);
  connect(cancelMove, &QPushButton::clicked, this,
          &MainWindow::cancelMoveTool);
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
  linearPatternSpacingSpin_->setRange(0.01, 100000.0);
  linearPatternSpacingSpin_->setDecimals(2);
  linearPatternSpacingSpin_->setSingleStep(0.1);
  linearPatternSpacingSpin_->setSuffix(QStringLiteral(" mm"));
  linearPatternSpacingSpin_->setValue(30.0);
  linearPatternCountSpin_ = new QSpinBox(linearPatternPanel);
  linearPatternCountSpin_->setObjectName(
      QStringLiteral("linearPatternCountSpin"));
  linearPatternCountSpin_->setRange(2, 100);
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
            partDesignTools_.beginReselection(
                ToolSelectionStage::SelectingInput);
            linearPatternToolSession_.clearBody();
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
            if (linearPatternToolSession_.bodyId() == kInvalidBodyId) return;
            partDesignTools_.beginReselection(
                ToolSelectionStage::SelectingReference);
            linearPatternToolSession_.clearDirection();
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
            if (linearPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            linearPatternToolSession_.setSpacingMm(value);
            updateLinearPatternToolPreview();
          });
  connect(linearPatternCountSpin_, qOverload<int>(&QSpinBox::valueChanged),
          this, [this](int value) {
            if (linearPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            linearPatternToolSession_.setCount(value);
            updateLinearPatternToolPreview();
          });
  connect(linearPatternOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int index) {
            if (linearPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            linearPatternToolSession_.setOperation(
                index == 0 ? PatternOperation::NewBody
                           : PatternOperation::Join);
            updateLinearPatternToolPreview();
          });
  connect(linearPatternAcceptButton_, &QPushButton::clicked, this,
          &MainWindow::acceptLinearPatternTool);
  connect(cancelLinearPattern, &QPushButton::clicked, this,
          &MainWindow::cancelLinearPatternTool);
  auto* linearPatternReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), linearPatternDock_);
  linearPatternReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternReturnShortcut->setAutoRepeat(false);
  connect(linearPatternReturnShortcut, &QShortcut::activated, this, [this] {
    if (linearPatternDock_->isVisible() &&
        linearPatternAcceptButton_->isEnabled())
      acceptLinearPatternTool();
  });
  auto* linearPatternKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), linearPatternDock_);
  linearPatternKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternKeypadShortcut->setAutoRepeat(false);
  connect(linearPatternKeypadShortcut, &QShortcut::activated, this, [this] {
    if (linearPatternDock_->isVisible() &&
        linearPatternAcceptButton_->isEnabled())
      acceptLinearPatternTool();
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
  circularPatternAngleSpin_->setRange(0.01, 360.0);
  circularPatternAngleSpin_->setDecimals(2);
  circularPatternAngleSpin_->setSingleStep(1.0);
  circularPatternAngleSpin_->setSuffix(QString::fromUtf8(" °"));
  circularPatternAngleSpin_->setValue(360.0);
  circularPatternCountSpin_ = new QSpinBox(circularPatternPanel);
  circularPatternCountSpin_->setObjectName(
      QStringLiteral("circularPatternCountSpin"));
  circularPatternCountSpin_->setRange(2, 100);
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
            partDesignTools_.beginReselection(
                ToolSelectionStage::SelectingInput);
            circularPatternToolSession_.clearBody();
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
            if (circularPatternToolSession_.bodyId() == kInvalidBodyId) return;
            partDesignTools_.beginReselection(
                ToolSelectionStage::SelectingReference);
            circularPatternToolSession_.clearAxis();
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
            if (circularPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            circularPatternToolSession_.setAngleDeg(value);
            updateCircularPatternToolPreview();
          });
  connect(circularPatternCountSpin_, qOverload<int>(&QSpinBox::valueChanged),
          this, [this](int value) {
            if (circularPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            circularPatternToolSession_.setCount(value);
            updateCircularPatternToolPreview();
          });
  connect(circularPatternOperationCombo_, &QComboBox::currentIndexChanged,
          this, [this](int index) {
            if (circularPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            circularPatternToolSession_.setOperation(
                index == 0 ? PatternOperation::NewBody
                           : PatternOperation::Join);
            updateCircularPatternToolPreview();
          });
  connect(circularPatternAcceptButton_, &QPushButton::clicked, this,
          &MainWindow::acceptCircularPatternTool);
  connect(cancelCircularPattern, &QPushButton::clicked, this,
          &MainWindow::cancelCircularPatternTool);
  auto* circularPatternReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), circularPatternDock_);
  circularPatternReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternReturnShortcut->setAutoRepeat(false);
  connect(circularPatternReturnShortcut, &QShortcut::activated, this, [this] {
    if (circularPatternDock_->isVisible() &&
        circularPatternAcceptButton_->isEnabled())
      acceptCircularPatternTool();
  });
  auto* circularPatternKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), circularPatternDock_);
  circularPatternKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternKeypadShortcut->setAutoRepeat(false);
  connect(circularPatternKeypadShortcut, &QShortcut::activated, this, [this] {
    if (circularPatternDock_->isVisible() &&
        circularPatternAcceptButton_->isEnabled())
      acceptCircularPatternTool();
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
            if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              chamferToolSession_.setDistanceFromPanel(value);
              updateChamferToolPreview();
            } else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              shellToolSession_.setThicknessFromPanel(value);
              toolParametersPanel_->setParameterValue(
                  shellToolSession_.thicknessMm());
              updateShellToolPreview();
            } else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              draftToolSession_.setAngleFromPanel(value);
              updateDraftToolPreview();
            } else if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive) {
              faceExtrudeSession_.setLengthFromPanel(value);
              updateFaceExtrudeToolPreview();
            } else if (filletToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              filletToolSession_.setRadiusFromPanel(value);
              updateFilletToolPreview();
            }
          });
  connect(toolParametersPanel_, &ToolParametersPanel::accepted, this,
          [this] {
            if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive)
              acceptChamferTool();
            else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive)
              acceptShellTool();
            else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive)
              acceptDraftTool();
            else if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive)
              acceptFaceExtrudeTool();
            else
              acceptFilletTool();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::cancelled, this,
          [this] {
            if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelChamferTool();
            else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelShellTool();
            else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelDraftTool();
            else if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelFaceExtrudeTool();
            else
              cancelFilletTool();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::selectionRequested, this,
          [this] {
            if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                chamferToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                shellToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                draftToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                faceExtrudeSession_.lifecycle() == ToolLifecycle::Inactive) return;
            statusBar()->showMessage(
                QString::fromUtf8("Выберите геометрию непосредственно в viewport"));
            viewport_->setFocus();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::clearSelectionRequested,
          this, [this] {
            if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                chamferToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                shellToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                draftToolSession_.lifecycle() == ToolLifecycle::Inactive &&
                faceExtrudeSession_.lifecycle() == ToolLifecycle::Inactive) return;
            if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive) {
              viewport_->setSelectedBodyFaces({});
              faceExtrudeSession_.setFace(FaceReference{});
              updateFaceExtrudeToolPreview();
              return;
            }
            if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              viewport_->setSelectedBodyFaces({});
              shellToolSession_.setRemovedFaces({});
              updateShellToolPreview();
              return;
            }
            if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              viewport_->setSelectedBodyFaces({});
              draftToolSession_.setFaces({});
              updateDraftToolPreview();
              return;
            }
            viewport_->setSelectedBodyEdges({});
            if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              chamferToolSession_.setEdges({});
              updateChamferToolPreview();
            } else {
              filletToolSession_.setEdges({});
              updateFilletToolPreview();
            }
          });
  connect(viewport_, &Viewport::toolManipulatorValueChanged, this,
          [this](double value) {
            if (linearPatternToolSession_.lifecycle() !=
                ToolLifecycle::Inactive) {
              linearPatternToolSession_.setSpacingMm(value);
              const QSignalBlocker blocker(linearPatternSpacingSpin_);
              linearPatternSpacingSpin_->setValue(
                  linearPatternToolSession_.spacingMm());
              updateLinearPatternToolPreview();
            } else if (chamferToolSession_.lifecycle() !=
                       ToolLifecycle::Inactive) {
              chamferToolSession_.setDistanceFromManipulator(value);
              toolParametersPanel_->setParameterValue(chamferToolSession_.distanceMm());
              updateChamferToolPreview();
            } else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              shellToolSession_.setThicknessFromManipulator(value);
              toolParametersPanel_->setParameterValue(shellToolSession_.thicknessMm());
              updateShellToolPreview();
            } else if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive) {
              faceExtrudeSession_.setLengthFromManipulator(value);
              toolParametersPanel_->setParameterValue(faceExtrudeSession_.lengthMm());
              if (!faceExtrudeSession_.isSketchSource())
                toolParametersPanel_->setOptionChecked(
                    faceExtrudeSession_.operation() == ExtrudeOperation::Cut);
              updateFaceExtrudeToolPreview();
            } else if (filletToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              filletToolSession_.setRadiusFromManipulator(value);
              toolParametersPanel_->setParameterValue(filletToolSession_.radiusMm());
              updateFilletToolPreview();
            }
          });
  connect(viewport_, &Viewport::bodyEdgeSelectionChanged, this, [this] {
    if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive) {
      chamferToolSession_.setEdges(viewport_->selectedBodyEdges());
      updateChamferToolPreview();
      if (!chamferToolSession_.edges().empty())
        static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (filletToolSession_.lifecycle() != ToolLifecycle::Inactive) {
      filletToolSession_.setEdges(viewport_->selectedBodyEdges());
      updateFilletToolPreview();
      if (!filletToolSession_.edges().empty())
        static_cast<void>(viewport_->focusToolParameterField(false));
    }
  });
  connect(viewport_, &Viewport::bodyFaceSelectionChanged, this, [this] {
    if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive) {
      const auto faces = viewport_->selectedBodyFaces();
      if (!faces.empty())
        faceExtrudeSession_.setFace(faces.front());
      updateFaceExtrudeToolPreview();
      // Keep keyboard focus in the viewport after face selection so the next
      // Tab enters the on-canvas distance field, not the right-hand dock.
      static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive) {
      shellToolSession_.setRemovedFaces(viewport_->selectedBodyFaces());
      updateShellToolPreview();
      // Keep keyboard focus in the viewport after face selection so the next
      // Tab enters the on-canvas thickness field, not the right-hand dock.
      static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive) {
      draftToolSession_.setFaces(viewport_->selectedBodyFaces());
      updateDraftToolPreview();
      // Keep keyboard focus in the viewport after face selection so the next
      // Tab enters the on-canvas angle field.
      static_cast<void>(viewport_->focusToolParameterField(false));
    }
  });
  connect(toolParametersPanel_, &ToolParametersPanel::optionChanged, this,
          [this](bool checked) {
            if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive) {
              faceExtrudeSession_.setOperation(
                  checked ? ExtrudeOperation::Cut : ExtrudeOperation::Join);
              updateFaceExtrudeToolPreview();
            } else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              shellToolSession_.setOutside(checked);
              updateShellToolPreview();
            } else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              draftToolSession_.setReversed(checked);
              updateDraftToolPreview();
            }
          });
  connect(viewport_, &Viewport::angularToolManipulatorValueChanged, this,
          [this](double value) {
            if (draftToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
            draftToolSession_.setAngleFromManipulator(value);
            toolParametersPanel_->setParameterValue(draftToolSession_.angleDeg());
            updateDraftToolPreview();
          });
  // HUD Enter commit: the value is already interpreted + preview-synced via the
  // value routing above, so perform the active tool's existing Accept exactly
  // like the Готово button. Each accept*Tool guards its own lifecycle, so an
  // invalid preview will not accept and focus stays in the HUD field.
  connect(viewport_, &Viewport::toolParameterCommitted, this, [this] {
    // Extrude still uses its dedicated on-canvas spinbox. Treat Enter there
    // exactly like the Apply button before dispatching ToolSession tools.
    if (extrusionDock_->isVisible()) {
      extrudeSketch();
      return;
    }
    if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptFaceExtrudeTool();
    else if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptChamferTool();
    else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptShellTool();
    else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptDraftTool();
    else if (filletToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptFilletTool();
    else if (moveToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptMoveTool();
    else if (revolveToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptRevolveTool();
    else if (linearPatternToolSession_.lifecycle() !=
             ToolLifecycle::Inactive)
      acceptLinearPatternTool();
    else if (circularPatternToolSession_.lifecycle() !=
             ToolLifecycle::Inactive)
      acceptCircularPatternTool();
  });

  const auto acceptActiveTool = [this] {
    toolParametersPanel_->interpretParameterText();
    if (!toolParametersPanel_->acceptEnabled()) return;
    if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptChamferTool();
    else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptShellTool();
    else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptDraftTool();
    else if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive)
      acceptFaceExtrudeTool();
    else
      acceptFilletTool();
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
            if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelChamferTool();
            else if (shellToolSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelShellTool();
            else if (draftToolSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelDraftTool();
            else if (faceExtrudeSession_.lifecycle() != ToolLifecycle::Inactive)
              cancelFaceExtrudeTool();
            else
              cancelFilletTool();
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
            updateAutomaticExtrudeOperation();
          });
  connect(extrusionLengthSpin_, &QDoubleSpinBox::editingFinished, this,
          &MainWindow::normalizeExtrusionDistance);
  connect(extrusionReverseCheck_, &QCheckBox::toggled, this, [this](bool) {
    viewport_->setExtrusionPreviewLength(
        extrusionReverseCheck_->isChecked()
            ? -std::abs(extrusionLengthSpin_->value())
            : std::abs(extrusionLengthSpin_->value()));
    updateAutomaticExtrudeOperation();
  });
  connect(extrusionOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) { extrudeOperationManuallyChanged_ = true; });
  connect(viewport_, &Viewport::extrusionPreviewLengthChanged, this,
          [this](double value) {
            const QSignalBlocker distanceBlocker(extrusionLengthSpin_);
            const QSignalBlocker reverseBlocker(extrusionReverseCheck_);
            extrusionLengthSpin_->setValue(std::abs(value));
            extrusionReverseCheck_->setChecked(value < 0.0);
            updateAutomaticExtrudeOperation();
          });
  connect(viewport_, &Viewport::bodyMoveCommitted, this,
          [this](QPointF previous, QPointF) {
            pushUndoAction(
                [this, previous] { viewport_->setBodyPosition(previous); });
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
    viewport_->hideExtrusionManipulator();
    extrusionDock_->hide();
    selectedExtrusionSurface_.clear();
    modelRibbon_->clearActiveTool();
  });
  auto* extrusionEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), extrusionDock_);
  extrusionEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  extrusionEscapeShortcut->setAutoRepeat(false);
  connect(extrusionEscapeShortcut, &QShortcut::activated, this, [this] {
    viewport_->hideExtrusionManipulator();
    extrusionDock_->hide();
    selectedExtrusionSurface_.clear();
    modelRibbon_->clearActiveTool();
  });

  sketchSettingsDock_ =
      new QDockWidget(QString::fromUtf8("Свойства эскиза"), this);
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
  settingsLayout->addWidget(gridCheck);
  settingsLayout->addWidget(snapCheck);
  settingsLayout->addSpacing(8);
  settingsLayout->addWidget(lineTypeLabel);
  settingsLayout->addWidget(sketchLineTypeCombo_);

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
          });  connect(sketchCanvas_, &SketchCanvas::lineStyleSelectionChanged, this,
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
          [this](const QString& plane) {
            viewport_->setSelectionFilter(SelectionFilter::Any);
            modelRibbon_->clearActiveTool();
            currentSketchSupport_ = plane;
            currentSketchFaceReference_.reset();
            currentSketchPlacement_ = plane.contains(QStringLiteral("XZ"))
                                          ? SketchPlacement::xz()
                                          : plane.contains(QStringLiteral("YZ"))
                                                ? SketchPlacement::yz()
                                                : SketchPlacement::xy();
            if (plane.startsWith(QString::fromUtf8("Грань тела"))) {
              const auto faceReference = viewport_->selectedBodyFace();
              if (!faceReference) {
                QMessageBox::warning(this, QString::fromUtf8("Sketch on Face"),
                                     QString::fromUtf8("Не удалось определить грань Body."));
                return;
              }
              const Body* body = document_.findBody(faceReference->bodyId);
              const ShapeFeature* feature = nullptr;
              if (body)
                for (const auto& candidate : body->features())
                  if (candidate->id() == faceReference->featureId) {
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
              const auto resolved =
                  resolveFacePlacement(*shape, faceReference->topology());
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
              currentSketchFaceReference_ = *faceReference;
            }
            sketchCanvas_->resetSketch();
            sketchCanvas_->clearSketchEditContext();
            sketchCanvas_->setReferenceBody(document_.box(), plane,
                                             hasExtrusion_);
            const QString support = viewport_->solidSupport();
            const bool capFace =
                (support.contains("XZ") &&
                 (plane.contains(QString::fromUtf8("Передняя")) ||
                  plane.contains(QString::fromUtf8("Задняя")))) ||
                (support.contains("YZ") &&
                 (plane.contains(QString::fromUtf8("Правая")) ||
                  plane.contains(QString::fromUtf8("Левая")))) ||
                (!support.contains("XZ") && !support.contains("YZ") &&
                 (plane.contains(QString::fromUtf8("Верхняя")) ||
                  plane.contains(QString::fromUtf8("Нижняя"))));
            sketchCanvas_->setReferenceProfile(viewport_->solidSketch(),
                                               hasExtrusion_ && capFace);
            if (currentSketchFaceReference_ &&
                !configureSketchEditContext(true))
              return;
            configureSketchSceneReferences();
            workspaceStack_->setCurrentWidget(sketchCanvas_);
            ribbonStack_->setCurrentWidget(sketchRibbon_);
            statusBar()->showMessage(QString::fromUtf8("Рабочая плоскость: ") + plane);
          });
  connect(viewport_, &Viewport::extrusionSurfacePicked, this,
          [this](const QString& surface) {
            // A native body-face pick now routes to face extrusion via
            // extrusionFacePicked; the legacy string signal only drives
            // sketch-contour extrusion from here on.
            if (surface.startsWith(QString::fromUtf8("Грань тела"))) return;
            if (revolveToolSession_.lifecycle() != ToolLifecycle::Inactive) {
              updateRevolveProfileSelection(
                  viewport_->extrusionCandidateSketchIndex());
              return;
            }
            selectedExtrusionSurface_ = surface;
            statusBar()->showMessage(QString::fromUtf8("Поверхность: ") + surface);
            extrusionLengthSpin_->setValue(document_.box().heightMm);
            viewport_->showExtrusionManipulator(
                extrusionReverseCheck_->isChecked()
                    ? -extrusionLengthSpin_->value()
                    : extrusionLengthSpin_->value());
            updateAutomaticExtrudeOperation();
            extrusionDock_->show();
            extrusionDock_->raise();
          });
  connect(viewport_, &Viewport::extrusionFacePicked, this,
          [this](const FaceReference& face) { createFaceExtrude(face); });
  connect(viewport_, &Viewport::directProfilePicked, this,
          [this](std::size_t sketchIndex) { createSketchExtrude(sketchIndex); });
  connect(sketchRibbon_, &SketchRibbon::finishRequested, this,
          &MainWindow::finishSketch);
  connect(sketchCanvas_, &SketchCanvas::geometryChanged, this,
          &MainWindow::updateFromSketch);
  connect(workspaceStack_, &QStackedWidget::currentChanged, this, [this](int index) {
    const bool sketchMode = workspaceStack_->widget(index) == sketchCanvas_;
    ribbonStack_->setCurrentWidget(workspaceStack_->widget(index) == viewport_
                                       ? static_cast<QWidget*>(modelRibbon_)
                                       : static_cast<QWidget*>(sketchRibbon_));
    if (!sketchMode && sketchSettingsDock_) sketchSettingsDock_->hide();
    updateUndoAvailability();
  });
  workspaceStack_->setCurrentWidget(viewport_);
  ribbonStack_->setCurrentWidget(modelRibbon_);

  auto* modelDock = new QDockWidget(QString::fromUtf8("Дерево построений"), this);
  featureTree_ = new QTreeWidget(modelDock);
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
            if (!applyingUndo_ && kind != 0) {
              pushUndoAction([this, kind, visible] {
                for (auto* candidate : featureTree_->findItems(
                         QStringLiteral("*"),
                         Qt::MatchWildcard | Qt::MatchRecursive)) {
                  if (candidate->data(0, Qt::UserRole).toInt() == kind) {
                    candidate->setCheckState(
                        0, visible ? Qt::Unchecked : Qt::Checked);
                    break;
                  }
                }
              });
            }
            if (kind == 1) viewport_->setOriginVisible(visible);
            if (kind == 2) viewport_->setSketchVisible(visible);
            if (kind == 3) viewport_->setSolidVisible(visible && hasExtrusion_);
            if (kind >= 10 && kind <= 12)
              viewport_->setBasePlaneVisible(kind - 10, visible);
            if (kind >= 20)
              viewport_->setSketchVisible(static_cast<std::size_t>(kind - 20), visible);
          });
  connect(viewport_, &Viewport::revolveAxisPicked, this,
          [this](qulonglong axisToken) {
            if (revolveToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
            AxisReference axis;
            if (axisToken == Viewport::kGlobalXAxisToken) {
              axis.type = AxisReferenceType::GlobalX;
            } else if (axisToken == Viewport::kGlobalYAxisToken) {
              axis.type = AxisReferenceType::GlobalY;
            } else if (axisToken == Viewport::kGlobalZAxisToken) {
              axis.type = AxisReferenceType::GlobalZ;
            } else if (axisToken == 1) {
              axis.sketchId = revolveToolSession_.profileSketchId();
              axis.type = AxisReferenceType::SketchHorizontalAxis;
            } else if (axisToken == 2) {
              axis.sketchId = revolveToolSession_.profileSketchId();
              axis.type = AxisReferenceType::SketchVerticalAxis;
            } else {
              axis.sketchId = revolveToolSession_.profileSketchId();
              axis.type = AxisReferenceType::SketchLine;
              axis.lineId = static_cast<sketch::GeometryId>(axisToken - 3);
            }
            revolveToolSession_.setAxis(axis);
            partDesignTools_.finishReselection();
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
    if (moveToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
    Body* body = document_.findBody(bodyId);
    if (!body || !body->activeFeature() || !body->resultShape()) {
      statusBar()->showMessage(
          QString::fromUtf8("Выбранное тело не содержит геометрии"), 3000);
      return;
    }
    moveToolSession_.setBody(body->id(), body->activeFeature()->id(),
                             body->resultShape());
    partDesignTools_.finishReselection();
    moveBodyValue_->setText(QString::fromStdString(body->name()));
    viewport_->showMovePreview();
    updateMoveToolPreview();
    static_cast<void>(viewport_->focusToolParameterField(false));
    statusBar()->showMessage(
        QString::fromUtf8("Потяните стрелку X, Y или Z либо введите смещение"));
  });
  connect(viewport_, &Viewport::translationToolManipulatorValueChanged, this,
          [this](int axisIndex, double value) {
            if (moveToolSession_.lifecycle() == ToolLifecycle::Inactive)
              return;
            moveToolSession_.setOffsetComponent(axisIndex, value);
            const auto offset = moveToolSession_.offsetMm();
            const QSignalBlocker xBlocker(moveXSpin_);
            const QSignalBlocker yBlocker(moveYSpin_);
            const QSignalBlocker zBlocker(moveZSpin_);
            moveXSpin_->setValue(offset.x);
            moveYSpin_->setValue(offset.y);
            moveZSpin_->setValue(offset.z);
            updateMoveToolPreview();
          });
  connect(viewport_, &Viewport::mirrorBodyPicked, this,
          [this](BodyId bodyId) {
            if (mirrorToolSession_.lifecycle() == ToolLifecycle::Inactive)
              return;
            Body* body = document_.findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            mirrorToolSession_.setBody(body->id(), body->activeFeature()->id(),
                                       body->resultShape());
            partDesignTools_.finishReselection();
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
            if (mirrorToolSession_.lifecycle() == ToolLifecycle::Inactive ||
                planeIndex < 0 || planeIndex > 2)
              return;
            const auto plane = static_cast<MirrorPlane>(planeIndex);
            mirrorToolSession_.setPlane(plane);
            partDesignTools_.finishReselection();
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
            if (linearPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            Body* body = document_.findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            linearPatternToolSession_.setBody(
                body->id(), body->activeFeature()->id(), body->resultShape());
            partDesignTools_.finishReselection();
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
            if (linearPatternToolSession_.lifecycle() ==
                    ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            const auto direction = static_cast<PrincipalAxis>(axisIndex);
            linearPatternToolSession_.setDirection(direction);
            partDesignTools_.finishReselection();
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
            if (circularPatternToolSession_.lifecycle() ==
                ToolLifecycle::Inactive)
              return;
            Body* body = document_.findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            circularPatternToolSession_.setBody(
                body->id(), body->activeFeature()->id(), body->resultShape());
            partDesignTools_.finishReselection();
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
            if (circularPatternToolSession_.lifecycle() ==
                    ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            const auto axis = static_cast<PrincipalAxis>(axisIndex);
            circularPatternToolSession_.setAxis(axis);
            partDesignTools_.finishReselection();
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
  partDesignTools_.registerTool(
      PartDesignToolKind::Revolve,
      {&revolveToolSession_, [this] { cancelRevolveTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::Mirror,
      {&mirrorToolSession_, [this] { cancelMirrorTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::Move,
      {&moveToolSession_, [this] { cancelMoveTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::LinearPattern,
      {&linearPatternToolSession_, [this] { cancelLinearPatternTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::CircularPattern,
      {&circularPatternToolSession_,
       [this] { cancelCircularPatternTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::Fillet,
      {&filletToolSession_, [this] { cancelFilletTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::Chamfer,
      {&chamferToolSession_, [this] { cancelChamferTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::Shell,
      {&shellToolSession_, [this] { cancelShellTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::Draft,
      {&draftToolSession_, [this] { cancelDraftTool(); }, {}});
  partDesignTools_.registerTool(
      PartDesignToolKind::Extrude,
      {&faceExtrudeSession_, [this] { cancelFaceExtrudeTool(); }, {}});
  connect(featureTree_, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem* item, int) {
            const auto id = static_cast<FeatureId>(
                item->data(0, Qt::UserRole + 1).toULongLong());
            if (id != kInvalidFeatureId) editPatternFeature(id);
          });
  connect(viewport_, &Viewport::selectionChanged, this,
          [this](const QString& text) {
            if (text == QStringLiteral("__cancel_tools__") ||
                text == QStringLiteral("__cancel_sketch_plane__")) {
              if (partDesignTools_.handleEscape()) {
                if (partDesignTools_.activeTool() ==
                    PartDesignToolKind::Revolve)
                  updateRevolveToolPreview();
                else if (partDesignTools_.activeTool() ==
                         PartDesignToolKind::Mirror)
                  updateMirrorToolPreview();
                else if (partDesignTools_.activeTool() ==
                         PartDesignToolKind::Move)
                  updateMoveToolPreview();
                else if (partDesignTools_.activeTool() ==
                         PartDesignToolKind::LinearPattern)
                  updateLinearPatternToolPreview();
                else if (partDesignTools_.activeTool() ==
                         PartDesignToolKind::CircularPattern)
                  updateCircularPatternToolPreview();
                statusBar()->showMessage(
                    QString::fromUtf8("Повторный выбор отменён"), 2000);
                return;
              }
              viewport_->clearLegacyExtrusionPreview();
              selectedExtrusionSurface_.clear();
              if (extrusionDock_) extrusionDock_->hide();
              modelRibbon_->clearActiveTool();
              if (text == QStringLiteral("__cancel_sketch_plane__"))
                rebuildFeatureTree();
              statusBar()->showMessage(
                  QString::fromUtf8("Инструменты сброшены"), 2000);
              return;
            }
            statusBar()->showMessage(text.isEmpty()
                                         ? QString::fromUtf8("Выделение снято")
                                         : text);
          });
  modelDock->setWidget(featureTree_);
  addDockWidget(Qt::LeftDockWidgetArea, modelDock);

  auto* historyDock = new QDockWidget(QString::fromUtf8("История построений"), this);
  historyDock->setAllowedAreas(Qt::BottomDockWidgetArea);
  historyDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  historyDock->setMinimumHeight(96);
  historyDock->setMaximumHeight(108);
  auto* historyHost = new QWidget(historyDock);
  auto* historyHostLayout = new QVBoxLayout(historyHost);
  historyHostLayout->setContentsMargins(8, 2, 8, 3);
  historyHostLayout->setSpacing(1);
  historyScroll_ = new QScrollArea(historyDock);
  historyScroll_->setWidgetResizable(true);
  historyScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  historyScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  historyScroll_->setFrameShape(QFrame::NoFrame);
  historyTimeline_ = new HistoryTimelineWidget(historyScroll_);
  historyContent_ = historyTimeline_;
  historyLayout_ = historyTimeline_->stepLayout();
  historyScroll_->setWidget(historyContent_);
  connect(historyTimeline_, &HistoryTimelineWidget::positionChanged, this,
          &MainWindow::applyHistoryPosition);
  historyHostLayout->addWidget(historyScroll_, 1);
  historyDock->setWidget(historyHost);
  addDockWidget(Qt::BottomDockWidgetArea, historyDock);
  rebuildHistoryPanel();

  statusBar()->showMessage(
      QString::fromUtf8("Готово к параметрическому моделированию"));
}

void MainWindow::updateFromSketch(double widthMm, double heightMm) {
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
  const auto& sketch = sketchCanvas_->sketch();
  QString completionMessage =
      QString::fromUtf8("Эскиз завершён — модель перестроена");
  if (sketch.lines().empty() && sketch.circles().empty() &&
      sketch.arcs().empty()) {
    workspaceStack_->setCurrentWidget(viewport_);
    statusBar()->showMessage(
        QString::fromUtf8("Пустой эскиз закрыт без сохранения"), 3000);
    return;
  }
  updateFromSketch(sketch.widthMm(), sketch.heightMm());
  if (editingSketchIndex_ && *editingSketchIndex_ < sketchHistory_.size()) {
    const std::size_t index = *editingSketchIndex_;
    const SketchHistoryEntry previous = sketchHistory_[index];
    const Document previousDocument = document_;
    pushUndoAction([this, index, previous, previousDocument] {
      if (index >= sketchHistory_.size()) return;
      document_ = previousDocument;
      sketchHistory_[index] = previous;
      const auto* restored = document_.findSketch(previous.documentSketchId);
      viewport_->updateSketch(index, previous.geometry, previous.support,
                              restored ? restored->placement
                                       : SketchPlacement::xy());
      if (hasExtrusion_ && extrusionSourceSketch_ == index)
        viewport_->setSolidSketch(previous.geometry);
      refreshBodyViewFromDocument();
      rebuildFeatureTree();
      rebuildHistoryPanel();
    });
    sketchHistory_[index].geometry = sketch;
    sketchHistory_[index].support = currentSketchSupport_;
    document_.replaceSketchGeometry(
        sketchHistory_[index].documentSketchId, sketch);
    if (auto* modelSketch =
            document_.findSketch(sketchHistory_[index].documentSketchId))
      modelSketch->placement = currentSketchPlacement_;
    const bool recomputeSucceeded = document_.recompute();
    refreshBodyViewFromDocument();
    viewport_->updateSketch(index, sketch, currentSketchSupport_,
                            currentSketchPlacement_);
    if (hasExtrusion_ && extrusionSourceSketch_ == index)
      viewport_->setSolidSketch(sketch);
    if (!recomputeSucceeded) {
      completionMessage =
          QString::fromUtf8(
              "Эскиз обновлён, перестроение модели завершилось с ошибкой: %1")
              .arg(QString::fromStdString(document_.rebuildError()));
    }
  } else {
    const std::size_t index = sketchHistory_.size();
    const Document previousDocument = document_;
    auto& modelSketch = document_.addSketch(
        "Sketch " + std::to_string(document_.sketches().size() + 1));
    modelSketch.geometry = sketch;
    modelSketch.placement = currentSketchPlacement_;
    if (currentSketchFaceReference_)
      document_.attachSketchToFace(modelSketch.id,
                                   *currentSketchFaceReference_);
    const SketchId modelSketchId = modelSketch.id;
    pushUndoAction([this, index, previousDocument] {
      if (index >= sketchHistory_.size()) return;
      document_ = previousDocument;
      sketchHistory_.erase(sketchHistory_.begin() +
                           static_cast<std::ptrdiff_t>(index));
      viewport_->removeSketch(index);
      sketchCanvas_->resetSketch();
      viewport_->setSketch(sketch::Sketch{});
      sketchCount_ = sketchHistory_.size();
      rebuildFeatureTree();
      rebuildHistoryPanel();
      moveHistoryToEnd();
    });
    viewport_->addSketch(sketch, currentSketchSupport_,
                         currentSketchPlacement_);
    sketchHistory_.push_back(
        {sketch, currentSketchSupport_, modelSketchId});
    ++sketchCount_;
  }
  editingSketchIndex_.reset();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
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
  updateAutomaticExtrudeOperation();
}

void MainWindow::updateAutomaticExtrudeOperation() {
  if (extrudeOperationManuallyChanged_) return;
  ExtrudeOperation operation = ExtrudeOperation::NewBody;
  const Body* body = document_.activeBody();
  const std::size_t index = viewport_->extrusionCandidateSketchIndex();
  const DocumentSketch* profile =
      index < sketchHistory_.size()
          ? document_.findSketch(sketchHistory_[index].documentSketchId)
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
  const QSignalBlocker blocker(extrusionOperationCombo_);
  extrusionOperationCombo_->setCurrentIndex(static_cast<int>(operation));
}

void MainWindow::extrudeSketch() {
  if (!ensureHistoryAtEnd()) return;
  const bool fromBodyFace = selectedExtrusionSurface_.startsWith(
      QString::fromUtf8("Грань тела"));
  const auto& pickedSketch = viewport_->extrusionCandidateSketch();
  const std::size_t pickedIndex = viewport_->extrusionCandidateSketchIndex();
  const std::size_t sourceIndex =
      pickedIndex != static_cast<std::size_t>(-1)
          ? pickedIndex
          : sketchCount_ > 0 ? sketchCount_ - 1
                             : static_cast<std::size_t>(-1);
  DocumentSketch* modelSketch =
      sourceIndex < sketchHistory_.size()
          ? document_.findSketch(sketchHistory_[sourceIndex].documentSketchId)
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
  selectedProfile.placement = modelSketch ? modelSketch->placement
                                          : currentSketchPlacement_;
  if (selectedProfile.id == kInvalidSketchId) selectedProfile.id = 1;
  std::string profileError;
  if (!isSupportedSketchProfile(selectedProfile, &profileError)) {
    const QString technical = QString::fromStdString(profileError);
    const bool openContour = technical.contains(QStringLiteral("closed wire")) ||
                             technical.contains(QStringLiteral("insufficient geometry"));
    QMessageBox::warning(
        this,
        openContour ? QString::fromUtf8("Контур не замкнут")
                    : QString::fromUtf8("Некорректный профиль"),
        openContour
            ? QString::fromUtf8(
                  "Соедините конечные точки линий замкнутого контура.")
            : technical.contains(QStringLiteral("overlap or form a hole"))
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

  const Document previousDocument = document_;
  const bool previousHasExtrusion = hasExtrusion_;
  const auto previousSource = extrusionSourceSketch_;
  const sketch::Sketch previousSolidSketch = viewport_->solidSketch();
  const QString previousSolidSupport = viewport_->solidSupport();
  if (!modelSketch) {
    modelSketch = &document_.addSketch(
        kInvalidSketchId,
        "Extrude profile " + std::to_string(document_.sketches().size() + 1),
        sketch);
    modelSketch->placement = currentSketchPlacement_;
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
  pushUndoAction([this, previousDocument, previousHasExtrusion, previousSource,
                  previousSolidSketch, previousSolidSupport] {
    document_ = previousDocument;
    extrusionSourceSketch_ = previousSource;
    viewport_->setSolidSketch(previousSolidSketch);
    viewport_->setSolidSupport(previousSolidSupport);
    refreshBodyViewFromDocument();
    // Compatibility for projects saved before Body owned the B-Rep.
    if (!hasExtrusion_ && previousHasExtrusion) {
      viewport_->setBox(document_.box());
      viewport_->setSolidVisible(true);
      hasExtrusion_ = true;
    }
    rebuildFeatureTree();
    rebuildHistoryPanel();
    moveHistoryToEnd();
  });

  refreshBodyViewFromDocument();
  viewport_->setSketch(sketch);
  const QString pickedSupport = viewport_->extrusionCandidateSupport();
  const QString operationSupport = fromBodyFace ? previousSolidSupport
      : pickedSupport.isEmpty() ? currentSketchSupport_ : pickedSupport;
  viewport_->setSolidSketch(sketch);
  viewport_->setSolidSupport(operationSupport);
  extrusionSourceSketch_ = pickedIndex != static_cast<std::size_t>(-1)
                               ? std::optional<std::size_t>(pickedIndex)
                               : sketchCount_ > 0
                                     ? std::optional<std::size_t>(sketchCount_ - 1)
                                     : std::nullopt;
  selectedExtrusionSurface_.clear();
  viewport_->hideExtrusionManipulator();
  extrusionDock_->hide();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
  statusBar()->showMessage(
      QString::fromUtf8("Создано твёрдое тело: выдавливание %1 мм").arg(height), 4000);
}

void MainWindow::refreshBodyViewFromDocument() {
  std::vector<BodyViewShape> shapes;
  for (const Body& body : document_.bodies()) {
    auto shape = body.resultShape();
    if (!shape) continue;
    const ShapeFeature* feature = body.activeFeature();
    shapes.push_back({body.id(), feature ? feature->id() : kInvalidFeatureId,
                      std::move(shape)});
  }
  hasExtrusion_ = !shapes.empty();
  viewport_->setBodyShapes(std::move(shapes));
  viewport_->setSolidVisible(hasExtrusion_);
  drawingSheet_->setDocument(document_);
  for (std::size_t index = 0; index < sketchHistory_.size(); ++index) {
    const auto* modelSketch =
        document_.findSketch(sketchHistory_[index].documentSketchId);
    if (!modelSketch) continue;
    viewport_->updateSketch(index, modelSketch->geometry,
                            sketchHistory_[index].support,
                            modelSketch->placement);
  }
}

void MainWindow::createRevolve() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  partDesignTools_.activate(PartDesignToolKind::Revolve);
  Body* body = document_.activeBody();
  revolveToolSession_.begin(document_, body ? body->id() : kInvalidBodyId,
                            body && body->activeFeature()
                                ? body->activeFeature()->id() : kInvalidFeatureId,
                            body ? body->resultShape() : ShapeFeature::ShapePtr{});
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
  revolveToolSession_.setOperation(initialOperation);
  revolveReverseCheck_->setChecked(false);
  revolveProfileSummary_->setText(QString::fromUtf8(
      "Щёлкните область в 3D-виде. Ctrl добавляет или убирает области."));
  revolveDock_->show(); revolveDock_->raise();
  viewport_->beginExtrusionSurfaceSelection();
  updateRevolveToolPreview();
  statusBar()->showMessage(
      QString::fromUtf8("Выберите профиль мышью во viewport"));
}

void MainWindow::updateRevolveProfileSelection(std::size_t sketchIndex) {
  if (revolveToolSession_.lifecycle() == ToolLifecycle::Inactive) return;

  const auto& selectedProfile = viewport_->extrusionCandidateSketch();
  const bool empty = selectedProfile.lines().empty() &&
                     selectedProfile.circles().empty() &&
                     selectedProfile.arcs().empty();
  if (empty || sketchIndex >= sketchHistory_.size()) {
    revolveToolSession_.clearAxis();
    revolveToolSession_.clearProfile();
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

  const SketchId id = sketchHistory_[sketchIndex].documentSketchId;
  if (revolveToolSession_.profileSketchId() != id &&
      revolveToolSession_.axis() &&
      revolveToolSession_.axis()->type != AxisReferenceType::GlobalX &&
      revolveToolSession_.axis()->type != AxisReferenceType::GlobalY &&
      revolveToolSession_.axis()->type != AxisReferenceType::GlobalZ)
    revolveToolSession_.clearAxis();
  revolveToolSession_.setProfile(id, selectedProfile);
  partDesignTools_.finishReselection();
  {
    const QSignalBlocker blocker(revolveProfileCombo_);
    revolveProfileCombo_->setCurrentIndex(
        revolveProfileCombo_->findData(QVariant::fromValue<qulonglong>(id)));
  }
  rebuildRevolveAxisChoices();
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
  if (revolveToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  const bool valid = revolveToolSession_.lifecycle() == ToolLifecycle::PreviewValid;
  revolveAcceptButton_->setEnabled(valid);
  if (revolveToolSession_.previewShape())
    viewport_->setToolPreviewShape(revolveToolSession_.bodyId(),
        revolveToolSession_.sourceFeatureId(), revolveToolSession_.previewShape());
  else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = revolveToolSession_.manipulator())
    viewport_->setAngularToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (revolveToolSession_.lifecycle() == ToolLifecycle::PreviewInvalid) {
    revolveStepHint_->setText(QString::fromStdString(revolveToolSession_.error()));
    revolveStepHint_->setProperty("uiRole", "danger");
    statusBar()->showMessage(QString::fromStdString(revolveToolSession_.error()));
  } else {
    const QString hint = valid
        ? partDesignToolStepHint(PartDesignToolKind::Revolve,
                                 ToolSelectionStage::EditingParameters)
        : partDesignToolStepHint(PartDesignToolKind::Revolve,
                                 revolveToolSession_.selectionStage());
    revolveStepHint_->setText(QString::fromUtf8("Сейчас: ") + hint);
    revolveStepHint_->setProperty("uiRole", "secondaryText");
  }
  revolveStepHint_->style()->unpolish(revolveStepHint_);
  revolveStepHint_->style()->polish(revolveStepHint_);
}

void MainWindow::cancelRevolveTool() {
  revolveToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Revolve);
  viewport_->resetToolInteraction();
  revolveDock_->hide(); modelRibbon_->clearActiveTool();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(QString::fromUtf8("Инструмент вращения отменён"), 2000);
}

void MainWindow::acceptRevolveTool() {
  if (revolveToolSession_.lifecycle() != ToolLifecycle::PreviewValid ||
      !revolveToolSession_.axis()) return;
  const Document previous = document_;
  Body* body = revolveToolSession_.editingFeatureId()
                   ? document_.findBody(revolveToolSession_.bodyId())
                   : revolveToolSession_.operation() == ExtrudeOperation::NewBody
                         ? &document_.addBody()
                         : document_.activeBody();
  if (!body) return;
  if (const auto editingId = revolveToolSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* feature = dynamic_cast<RevolveFeature*>(body->features()[index].get());
      if (!feature || feature->id() != *editingId) continue;
      feature->setProfileSketchId(revolveToolSession_.profileSketchId());
      feature->setProfileOverride(revolveToolSession_.profileOverride());
      feature->setAxis(*revolveToolSession_.axis());
      feature->setAngleDeg(revolveToolSession_.angleDeg());
      feature->setOperation(revolveToolSession_.operation());
      feature->setReversed(revolveToolSession_.reversed());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    auto feature = std::make_unique<RevolveFeature>(
        revolveToolSession_.profileSketchId(), *revolveToolSession_.axis(),
        revolveToolSession_.angleDeg(),
        "Revolve " + std::to_string(body->features().size() + 1),
        revolveToolSession_.operation(), revolveToolSession_.reversed());
    feature->setProfileOverride(revolveToolSession_.profileOverride());
    body->addFeature(std::move(feature));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous; refreshBodyViewFromDocument();
    statusBar()->showMessage(error); return;
  }
  revolveToolSession_.cancel(); viewport_->resetToolInteraction();
  partDesignTools_.deactivate(PartDesignToolKind::Revolve);
  revolveDock_->hide();
  pushUndoAction([this, previous] { document_ = previous; refreshBodyViewFromDocument();
                                    rebuildFeatureTree(); rebuildHistoryPanel(); });
  refreshBodyViewFromDocument(); rebuildFeatureTree(); rebuildHistoryPanel();
  modelRibbon_->clearActiveTool();
}

void MainWindow::createPocket() {
  if (!ensureHistoryAtEnd()) return;
  Body* body = document_.activeBody();
  if (!body || !body->resultShape() || sketchHistory_.empty()) {
    QMessageBox::information(this, QString::fromUtf8("Карман"),
                             QString::fromUtf8("Сначала создайте Body и эскиз на его грани."));
    return;
  }
  const auto& historySketch = sketchHistory_.back();
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

  const Document previousDocument = document_;
  body->addFeature(std::make_unique<PocketFeature>(
      profile->id, depth,
      "Pocket " + std::to_string(body->features().size())));
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    QMessageBox::warning(this, QString::fromUtf8("Ошибка кармана"), error);
    return;
  }
  pushUndoAction([this, previousDocument] {
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  statusBar()->showMessage(
      QString::fromUtf8("Создан карман глубиной %1 мм").arg(depth), 4000);
}

void MainWindow::createFillet() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  partDesignTools_.activate(PartDesignToolKind::Fillet);
  if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive)
    cancelChamferTool();
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
  filletToolSession_.begin(body->id(), body->activeFeature()->id(),
                           body->resultShape(), edges, 0.0);
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

void MainWindow::editPatternFeature(FeatureId featureId) {
  for (Body& body : document_.bodies()) {
    for (std::size_t index = 0; index < body.features().size(); ++index) {
      ShapeFeature* feature = body.features()[index].get();
      if (feature->id() != featureId) continue;
      resetTransientModelingUi();
      if (auto* revolve = dynamic_cast<RevolveFeature*>(feature)) {
        partDesignTools_.activate(PartDesignToolKind::Revolve);
        ShapeFeature::ShapePtr upstream = index == 0
                                             ? ShapeFeature::ShapePtr{}
                                             : body.features()[index - 1]->shape();
        const FeatureId sourceId = index == 0
                                       ? kInvalidFeatureId
                                       : body.features()[index - 1]->id();
        revolveToolSession_.begin(document_, body.id(), sourceId, upstream,
                                  revolve->id());
        revolveToolSession_.setProfile(revolve->profileSketchId(),
                                       revolve->profileOverride());
        revolveToolSession_.setAxis(revolve->axis());
        revolveToolSession_.setAngleFromPanel(revolve->angleDeg());
        revolveToolSession_.setOperation(revolve->operation());
        revolveToolSession_.setReversed(revolve->reversed());
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
      if (auto* shell = dynamic_cast<ShellFeature*>(feature)) {
        if (index == 0) return;
        partDesignTools_.activate(PartDesignToolKind::Shell);
        const auto source = body.features()[index - 1]->shape();
        shellToolSession_.begin(body.id(), body.features()[index - 1]->id(),
            source, shell->removedFaces(), shell->thicknessMm(),
            shell->outside(), shell->id());
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
      if (auto* draft = dynamic_cast<DraftFeature*>(feature)) {
        if (index == 0) return;
        partDesignTools_.activate(PartDesignToolKind::Draft);
        const auto source = body.features()[index - 1]->shape();
        draftToolSession_.begin(document_, body.id(),
            body.features()[index - 1]->id(), source, draft->draftedFaces(),
            draft->neutralPlane(), draft->pullDirection(), draft->angleDeg(),
            draft->reversed(), draft->id());
        viewport_->setFaceMultiSelectionMode(true);
        viewport_->setSelectionFilter(SelectionFilter::Face);
        viewport_->setSelectedBodyFaces(draft->draftedFaces());
        toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Draft),
            QString::fromUtf8("Грани"), QString::fromUtf8("Угол"),
            QString::fromUtf8("°"));
        toolParametersPanel_->setParameterRange(0.01, 89.0, 2);
        toolParametersPanel_->setParameterValue(draft->angleDeg());
        toolParametersPanel_->configureOption(QString::fromUtf8("Обратный уклон"),
                                               draft->reversed());
        toolParametersDock_->show(); toolParametersDock_->raise();
        updateDraftToolPreview();
        return;
      }
      if (auto* move = dynamic_cast<MoveFeature*>(feature)) {
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!source) return;
        partDesignTools_.activate(PartDesignToolKind::Move);
        moveToolSession_.begin(move->offsetMm(), move->id());
        moveToolSession_.setBody(body.id(), body.features()[index - 1]->id(),
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
      if (auto* mirror = dynamic_cast<MirrorFeature*>(feature)) {
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!source) return;
        partDesignTools_.activate(PartDesignToolKind::Mirror);
        mirrorToolSession_.begin(mirror->id());
        mirrorToolSession_.setBody(body.id(), body.features()[index - 1]->id(),
                                   source);
        mirrorToolSession_.setPlane(mirror->plane());
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
      if (auto* linear = dynamic_cast<LinearPatternFeature*>(feature)) {
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
        partDesignTools_.activate(PartDesignToolKind::LinearPattern);
        linearPatternToolSession_.begin(
            linear->spacingMm(), linear->count(), linear->operation(),
            linear->id());
        linearPatternToolSession_.setBody(
            sourceBody->id(), sourceFeatureId, source);
        linearPatternToolSession_.setDirection(linear->direction());
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
      if (auto* circular = dynamic_cast<CircularPatternFeature*>(feature)) {
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
        partDesignTools_.activate(PartDesignToolKind::CircularPattern);
        circularPatternToolSession_.begin(
            circular->angleDeg(), circular->count(), circular->operation(),
            circular->id());
        circularPatternToolSession_.setBody(
            sourceBody->id(), sourceFeatureId, source);
        circularPatternToolSession_.setAxis(circular->axis());
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
  }
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
  partDesignTools_.activate(PartDesignToolKind::Move);
  moveToolSession_.begin();
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
  if (moveToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  const bool valid =
      moveToolSession_.lifecycle() == ToolLifecycle::PreviewValid;
  moveAcceptButton_->setEnabled(valid);
  if (valid && moveToolSession_.previewShape()) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(moveToolSession_.bodyId(),
                                   moveToolSession_.sourceFeatureId(),
                                   moveToolSession_.previewShape());
    if (const auto manipulator = moveToolSession_.manipulator())
      viewport_->setTranslationToolManipulator(*manipulator);
    moveStepHint_->setText(QString::fromUtf8(
        "Потяните цветную стрелку X, Y или Z либо задайте точные смещения."));
    moveStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed =
        moveToolSession_.lifecycle() == ToolLifecycle::PreviewInvalid;
    moveStepHint_->setText(
        failed ? QString::fromStdString(moveToolSession_.error())
               : QString::fromUtf8("Сейчас: ") +
                     partDesignToolStepHint(PartDesignToolKind::Move,
                                            moveToolSession_.selectionStage()));
    moveStepHint_->setProperty("uiRole",
                               failed ? "danger" : "secondaryText");
  }
  moveStepHint_->style()->unpolish(moveStepHint_);
  moveStepHint_->style()->polish(moveStepHint_);
}

void MainWindow::cancelMoveTool() {
  if (moveToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  moveToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Move);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  moveDock_->hide();
  modelRibbon_->clearActiveTool();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(QString::fromUtf8("Перемещение отменено"), 2000);
}

void MainWindow::acceptMoveTool() {
  if (moveToolSession_.lifecycle() != ToolLifecycle::PreviewValid) return;
  Body* body = document_.findBody(moveToolSession_.bodyId());
  if (!body) return;
  const Document previous = document_;
  if (const auto editingId = moveToolSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* move = dynamic_cast<MoveFeature*>(body->features()[index].get());
      if (!move || move->id() != *editingId) continue;
      move->setOffsetMm(moveToolSession_.offsetMm());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    if (!body->activeFeature() ||
        body->activeFeature()->id() != moveToolSession_.sourceFeatureId()) {
      moveStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      moveStepHint_->setProperty("uiRole", "danger");
      return;
    }
    body->addFeature(std::make_unique<MoveFeature>(
        moveToolSession_.sourceFeatureId(), moveToolSession_.offsetMm(),
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
  moveToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Move);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  moveDock_->hide();
  pushUndoAction([this, previous] {
    document_ = previous;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  modelRibbon_->clearActiveTool();
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
  partDesignTools_.activate(PartDesignToolKind::Mirror);
  mirrorToolSession_.begin();
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
  if (mirrorToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  const bool valid =
      mirrorToolSession_.lifecycle() == ToolLifecycle::PreviewValid;
  mirrorAcceptButton_->setEnabled(valid);
  if (valid && mirrorToolSession_.previewShape()) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(mirrorToolSession_.bodyId(),
                                   mirrorToolSession_.sourceFeatureId(),
                                   mirrorToolSession_.previewShape());
    mirrorStepHint_->setText(QString::fromUtf8(
        "Предпросмотр построен. Нажмите «Применить» для создания зеркала."));
    mirrorStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    const bool failed =
        mirrorToolSession_.lifecycle() == ToolLifecycle::PreviewInvalid;
    mirrorStepHint_->setText(
        failed
            ? QString::fromStdString(mirrorToolSession_.error())
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(PartDesignToolKind::Mirror,
                                         mirrorToolSession_.selectionStage()));
    mirrorStepHint_->setProperty("uiRole",
                                 failed ? "danger" : "secondaryText");
  }
  mirrorStepHint_->style()->unpolish(mirrorStepHint_);
  mirrorStepHint_->style()->polish(mirrorStepHint_);
}

void MainWindow::cancelMirrorTool() {
  if (mirrorToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  mirrorToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Mirror);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  mirrorDock_->hide();
  modelRibbon_->clearActiveTool();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(QString::fromUtf8("Инструмент зеркала отменён"),
                           2000);
}

void MainWindow::acceptMirrorTool() {
  if (mirrorToolSession_.lifecycle() != ToolLifecycle::PreviewValid ||
      !mirrorToolSession_.plane())
    return;
  Body* body = document_.findBody(mirrorToolSession_.bodyId());
  if (!body) return;
  const Document previous = document_;
  if (const auto editingId = mirrorToolSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* mirror =
          dynamic_cast<MirrorFeature*>(body->features()[index].get());
      if (!mirror || mirror->id() != *editingId) continue;
      mirror->setPlane(*mirrorToolSession_.plane());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    if (!body->activeFeature() ||
        body->activeFeature()->id() != mirrorToolSession_.sourceFeatureId()) {
      mirrorStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      mirrorStepHint_->setProperty("uiRole", "danger");
      return;
    }
    body->addFeature(std::make_unique<MirrorFeature>(
        mirrorToolSession_.sourceFeatureId(), *mirrorToolSession_.plane(),
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
  mirrorToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Mirror);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  mirrorDock_->hide();
  pushUndoAction([this, previous] {
    document_ = previous;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  modelRibbon_->clearActiveTool();
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
  partDesignTools_.activate(PartDesignToolKind::LinearPattern);
  linearPatternToolSession_.begin(30.0, 3);
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
  if (linearPatternToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  const bool valid =
      linearPatternToolSession_.lifecycle() == ToolLifecycle::PreviewValid;
  linearPatternAcceptButton_->setEnabled(valid);
  if (valid && linearPatternToolSession_.previewShape()) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(linearPatternToolSession_.bodyId(),
                                   linearPatternToolSession_.sourceFeatureId(),
                                   linearPatternToolSession_.previewShape());
    if (const auto manipulator = linearPatternToolSession_.manipulator())
      viewport_->setToolManipulator(*manipulator);
    linearPatternStepHint_->setText(QString::fromUtf8(
        "Потяните стрелку для изменения шага или задайте шаг и количество числом."));
    linearPatternStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed = linearPatternToolSession_.lifecycle() ==
                        ToolLifecycle::PreviewInvalid;
    linearPatternStepHint_->setText(
        failed
            ? QString::fromStdString(linearPatternToolSession_.error())
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(
                      PartDesignToolKind::LinearPattern,
                      linearPatternToolSession_.selectionStage()));
    linearPatternStepHint_->setProperty("uiRole",
                                        failed ? "danger" : "secondaryText");
  }
  linearPatternStepHint_->style()->unpolish(linearPatternStepHint_);
  linearPatternStepHint_->style()->polish(linearPatternStepHint_);
}

void MainWindow::cancelLinearPatternTool() {
  if (linearPatternToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  linearPatternToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::LinearPattern);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  linearPatternDock_->hide();
  modelRibbon_->clearActiveTool();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(
      QString::fromUtf8("Инструмент линейного массива отменён"), 2000);
}

void MainWindow::acceptLinearPatternTool() {
  if (linearPatternToolSession_.lifecycle() != ToolLifecycle::PreviewValid ||
      !linearPatternToolSession_.direction())
    return;
  Body* sourceBody = document_.findBody(linearPatternToolSession_.bodyId());
  if (!sourceBody) return;
  const Document previous = document_;
  if (const auto editingId = linearPatternToolSession_.editingFeatureId()) {
    for (Body& owner : document_.bodies())
      for (std::size_t index = 0; index < owner.features().size(); ++index) {
        auto* linear = dynamic_cast<LinearPatternFeature*>(
            owner.features()[index].get());
        if (!linear || linear->id() != *editingId) continue;
        linear->setDirection(*linearPatternToolSession_.direction());
        linear->setSpacingMm(linearPatternToolSession_.spacingMm());
        linear->setCount(linearPatternToolSession_.count());
        owner.markDirtyFrom(index);
        break;
      }
  } else {
    if (!sourceBody->activeFeature() ||
        sourceBody->activeFeature()->id() !=
            linearPatternToolSession_.sourceFeatureId()) {
      linearPatternStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      linearPatternStepHint_->setProperty("uiRole", "danger");
      return;
    }
    const BodyId sourceBodyId = sourceBody->id();
    Body* targetBody = linearPatternToolSession_.operation() ==
                               PatternOperation::NewBody
                           ? &document_.addBody()
                           : sourceBody;
    targetBody->addFeature(std::make_unique<LinearPatternFeature>(
        sourceBodyId, linearPatternToolSession_.sourceFeatureId(),
        *linearPatternToolSession_.direction(),
        linearPatternToolSession_.count(),
        linearPatternToolSession_.spacingMm(),
        linearPatternToolSession_.operation(),
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
  linearPatternToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::LinearPattern);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  linearPatternDock_->hide();
  pushUndoAction([this, previous] {
    document_ = previous;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  modelRibbon_->clearActiveTool();
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
  partDesignTools_.activate(PartDesignToolKind::CircularPattern);
  circularPatternToolSession_.begin(360.0, 4);
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
    circularPatternAngleSpin_->setValue(360.0);
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
  if (circularPatternToolSession_.lifecycle() == ToolLifecycle::Inactive)
    return;
  const bool valid = circularPatternToolSession_.lifecycle() ==
                     ToolLifecycle::PreviewValid;
  circularPatternAcceptButton_->setEnabled(valid);
  if (valid && circularPatternToolSession_.previewShape()) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(
        circularPatternToolSession_.bodyId(),
        circularPatternToolSession_.sourceFeatureId(),
        circularPatternToolSession_.previewShape());
    if (const auto manipulator = circularPatternToolSession_.manipulator())
      viewport_->setAngularToolManipulator(*manipulator);
    circularPatternStepHint_->setText(QString::fromUtf8(
        "Потяните дугу для изменения угла или задайте угол и количество числом."));
    circularPatternStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed = circularPatternToolSession_.lifecycle() ==
                        ToolLifecycle::PreviewInvalid;
    circularPatternStepHint_->setText(
        failed
            ? QString::fromStdString(circularPatternToolSession_.error())
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(
                      PartDesignToolKind::CircularPattern,
                      circularPatternToolSession_.selectionStage()));
    circularPatternStepHint_->setProperty(
        "uiRole", failed ? "danger" : "secondaryText");
  }
  circularPatternStepHint_->style()->unpolish(circularPatternStepHint_);
  circularPatternStepHint_->style()->polish(circularPatternStepHint_);
}

void MainWindow::cancelCircularPatternTool() {
  if (circularPatternToolSession_.lifecycle() == ToolLifecycle::Inactive)
    return;
  circularPatternToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::CircularPattern);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  circularPatternDock_->hide();
  modelRibbon_->clearActiveTool();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(
      QString::fromUtf8("Инструмент кругового массива отменён"), 2000);
}

void MainWindow::acceptCircularPatternTool() {
  if (circularPatternToolSession_.lifecycle() !=
          ToolLifecycle::PreviewValid ||
      !circularPatternToolSession_.axis())
    return;
  Body* sourceBody = document_.findBody(circularPatternToolSession_.bodyId());
  if (!sourceBody) return;
  const Document previous = document_;
  if (const auto editingId = circularPatternToolSession_.editingFeatureId()) {
    for (Body& owner : document_.bodies())
      for (std::size_t index = 0; index < owner.features().size(); ++index) {
        auto* circular = dynamic_cast<CircularPatternFeature*>(
            owner.features()[index].get());
        if (!circular || circular->id() != *editingId) continue;
        circular->setAxis(*circularPatternToolSession_.axis());
        circular->setAngleDeg(circularPatternToolSession_.angleDeg());
        circular->setCount(circularPatternToolSession_.count());
        owner.markDirtyFrom(index);
        break;
      }
  } else {
    if (!sourceBody->activeFeature() ||
        sourceBody->activeFeature()->id() !=
            circularPatternToolSession_.sourceFeatureId()) {
      circularPatternStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      circularPatternStepHint_->setProperty("uiRole", "danger");
      return;
    }
    const BodyId sourceBodyId = sourceBody->id();
    Body* targetBody = circularPatternToolSession_.operation() ==
                               PatternOperation::NewBody
                           ? &document_.addBody()
                           : sourceBody;
    targetBody->addFeature(std::make_unique<CircularPatternFeature>(
        sourceBodyId, circularPatternToolSession_.sourceFeatureId(),
        *circularPatternToolSession_.axis(),
        circularPatternToolSession_.count(),
        circularPatternToolSession_.angleDeg(),
        circularPatternToolSession_.operation(),
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
  circularPatternToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::CircularPattern);
  viewport_->resetToolInteraction();
  viewport_->setSelectedBodies({});
  circularPatternDock_->hide();
  pushUndoAction([this, previous] {
    document_ = previous;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  modelRibbon_->clearActiveTool();
  statusBar()->showMessage(QString::fromUtf8("Круговой массив применён"),
                           3000);
}

void MainWindow::createChamfer() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  partDesignTools_.activate(PartDesignToolKind::Chamfer);
  if (filletToolSession_.lifecycle() != ToolLifecycle::Inactive)
    cancelFilletTool();
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
  chamferToolSession_.begin(body->id(), body->activeFeature()->id(),
                             body->resultShape(), edges, 0.0);
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

void MainWindow::createShell() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  partDesignTools_.activate(PartDesignToolKind::Shell);
  auto faces = viewport_->selectedBodyFaces();
  Body* body = faces.empty() ? document_.activeBody()
                             : document_.findBody(faces.front().bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) return;
  if (!faces.empty() && faces.front().featureId != body->activeFeature()->id())
    faces.clear();
  shellToolSession_.begin(body->id(), body->activeFeature()->id(),
                          body->resultShape(), faces, 2.0, false);
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
  const auto state = shellToolSession_.lifecycle();
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(shellToolSession_.removedFaces().size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  const QString limitStatus = QString::fromUtf8(
      "Достигнута предельная толщина: %1 мм")
                                  .arg(shellToolSession_.thicknessMm(), 0, 'f', 2);
  toolParametersPanel_->setParameterRange(
      0.01, shellToolSession_.maximumValidThicknessMm().value_or(100000.0), 2);
  toolParametersPanel_->setParameterValue(shellToolSession_.thicknessMm());
  toolParametersPanel_->setAcceptEnabled(valid);
  toolParametersPanel_->setStatus(
      valid ? shellToolSession_.limitReached()
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Shell,
                                           ToolSelectionStage::SelectingInput)
                  : localizedPartDesignError(PartDesignToolKind::Shell,
                                             shellToolSession_.error()),
      state == ToolLifecycle::PreviewInvalid);
  if (valid) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(shellToolSession_.bodyId(),
                                   shellToolSession_.sourceFeatureId(),
                                   shellToolSession_.previewShape());
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (const auto manipulator = shellToolSession_.manipulator())
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (shellToolSession_.limitReached())
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelShellTool() {
  if (shellToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  shellToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Shell);
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyFaces({});
  toolParametersDock_->hide();
  refreshBodyViewFromDocument();
}

void MainWindow::acceptShellTool() {
  if (shellToolSession_.lifecycle() != ToolLifecycle::PreviewValid) return;
  const Document previous = document_;
  Body* body = document_.findBody(shellToolSession_.bodyId());
  if (!body) return;
  if (const auto editingId = shellToolSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* shell = dynamic_cast<ShellFeature*>(body->features()[index].get());
      if (!shell || shell->id() != *editingId) continue;
      shell->setRemovedFaces(shellToolSession_.removedFaces());
      shell->setThicknessMm(shellToolSession_.thicknessMm());
      shell->setOutside(shellToolSession_.outside());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ShellFeature>(
        shellToolSession_.sourceFeatureId(), shellToolSession_.removedFaces(),
        shellToolSession_.thicknessMm(), shellToolSession_.outside(),
        "Оболочка " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  cancelShellTool();
  pushUndoAction([this, previous] {
    document_ = previous; refreshBodyViewFromDocument();
    rebuildFeatureTree(); rebuildHistoryPanel();
  });
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
}

void MainWindow::createDraft() {
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  partDesignTools_.activate(PartDesignToolKind::Draft);
  auto faces = viewport_->selectedBodyFaces();
  Body* body = faces.empty() ? document_.activeBody()
                             : document_.findBody(faces.front().bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) return;
  if (!faces.empty() && faces.front().featureId != body->activeFeature()->id())
    faces.clear();
  const PlaneReference plane{NeutralPlaneType::GlobalXY, std::nullopt};
  const AxisReference direction{AxisReferenceType::GlobalZ, kInvalidSketchId,
                                sketch::kInvalidGeometryId};
  draftToolSession_.begin(document_, body->id(), body->activeFeature()->id(),
                          body->resultShape(), faces, plane, direction, 5.0,
                          false);
  viewport_->setFaceMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setSelectedBodyFaces(faces);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Draft),
                                  QString::fromUtf8("Грани"),
                                  QString::fromUtf8("Угол"),
                                  QString::fromUtf8("°"));
  toolParametersPanel_->setParameterRange(0.01, 89.0, 2);
  toolParametersPanel_->setParameterValue(5.0);
  toolParametersPanel_->configureOption(QString::fromUtf8("Обратный уклон"), false);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateDraftToolPreview();
  // Draft HUD owns CAD Tab focus for the same preselection workflow as Shell.
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::updateDraftToolPreview() {
  const auto state = draftToolSession_.lifecycle();
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(draftToolSession_.faces().size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  toolParametersPanel_->setStatus(
      valid ? QString::fromUtf8("Предпросмотр построен · плоскость XY · направление Z")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Draft,
                                           ToolSelectionStage::SelectingInput)
                  : localizedPartDesignError(PartDesignToolKind::Draft,
                                             draftToolSession_.error()),
      state == ToolLifecycle::PreviewInvalid);
  if (draftToolSession_.previewShape()) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(draftToolSession_.bodyId(),
                                   draftToolSession_.sourceFeatureId(),
                                   draftToolSession_.previewShape());
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (const auto manipulator = draftToolSession_.manipulator())
    viewport_->setAngularToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
}

void MainWindow::cancelDraftTool() {
  if (draftToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  draftToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Draft);
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyFaces({});
  toolParametersDock_->hide();
  refreshBodyViewFromDocument();
}

void MainWindow::acceptDraftTool() {
  if (draftToolSession_.lifecycle() != ToolLifecycle::PreviewValid ||
      !draftToolSession_.neutralPlane() || !draftToolSession_.pullDirection()) return;
  const Document previous = document_;
  Body* body = document_.findBody(draftToolSession_.bodyId());
  if (!body) return;
  if (const auto editingId = draftToolSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* draft = dynamic_cast<DraftFeature*>(body->features()[index].get());
      if (!draft || draft->id() != *editingId) continue;
      draft->setDraftedFaces(draftToolSession_.faces());
      draft->setNeutralPlane(*draftToolSession_.neutralPlane());
      draft->setPullDirection(*draftToolSession_.pullDirection());
      draft->setAngleDeg(draftToolSession_.angleDeg());
      draft->setReversed(draftToolSession_.reversed());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<DraftFeature>(
        draftToolSession_.sourceFeatureId(), draftToolSession_.faces(),
        *draftToolSession_.neutralPlane(), *draftToolSession_.pullDirection(),
        draftToolSession_.angleDeg(), draftToolSession_.reversed(),
        "Уклон " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  cancelDraftTool();
  pushUndoAction([this, previous] {
    document_ = previous; refreshBodyViewFromDocument();
    rebuildFeatureTree(); rebuildHistoryPanel();
  });
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
}

void MainWindow::createSketchExtrude(std::size_t sketchIndex) {
  if (!ensureHistoryAtEnd()) return;
  if (sketchIndex >= sketchHistory_.size()) return;
  const SketchId sketchId = sketchHistory_[sketchIndex].documentSketchId;
  DocumentSketch* profile = document_.findSketch(sketchId);
  if (!profile) return;
  Body* activeBody = document_.activeBody();
  SketchProfileSelectionContext context;
  context.profile = *profile;
  std::optional<sketch::Sketch> profileOverride;
  const auto& pickedProfile = viewport_->extrusionCandidateSketch();
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
  partDesignTools_.activate(PartDesignToolKind::Extrude);
  faceExtrudeSession_.beginSketch(
      *profile, sketchId, capability->baseShape, 10.0,
      capability->operation, false, std::nullopt, std::move(profileOverride));
  faceExtrudeSession_.setOperationFollowsDirection(
      capability->operationFollowsDirection);
  viewport_->clearLegacyExtrusionPreview();
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Профиль"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
  toolParametersPanel_->setParameterValue(faceExtrudeSession_.lengthMm());
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::createFaceExtrude(const FaceReference& face) {
  // Native face extrusion shares the Extrude ribbon entry; picking a real
  // B-Rep body face (rather than a sketch contour) starts this face-source
  // session. Never interrupt a running revolve profile re-selection.
  if (revolveToolSession_.lifecycle() != ToolLifecycle::Inactive) return;
  Body* body = document_.findBody(face.bodyId);
  if (!body || !body->activeFeature() || !body->resultShape()) {
    statusBar()->showMessage(QString::fromUtf8("Сначала создайте тело."), 3000);
    return;
  }
  resetTransientModelingUi();
  partDesignTools_.activate(PartDesignToolKind::Extrude);
  faceExtrudeSession_.begin(body->id(), body->activeFeature()->id(),
                            body->resultShape(), face, 10.0,
                            ExtrudeOperation::Join, false);
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces({face});
  viewport_->clearLegacyExtrusionPreview();
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Грань"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  const auto parameters = faceExtrudeSession_.parameters();
  const double minimum = parameters.empty() ? 0.01 : parameters.front().minimum;
  const double maximum =
      parameters.empty() ? 100000.0 : parameters.front().maximum;
  toolParametersPanel_->setParameterRange(minimum, maximum, 2);
  toolParametersPanel_->setParameterValue(faceExtrudeSession_.lengthMm());
  toolParametersPanel_->configureOption(QString::fromUtf8("Вырезать"), false);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::updateFaceExtrudeToolPreview() {
  const auto state = faceExtrudeSession_.lifecycle();
  if (state == ToolLifecycle::Inactive) return;
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setSelectionCount(
      faceExtrudeSession_.isSketchSource() ||
              faceExtrudeSession_.face().bodyId != kInvalidBodyId
          ? 1
          : 0);
  toolParametersPanel_->setAcceptEnabled(valid);
  if (valid) {
    toolParametersPanel_->setStatus(QString::fromUtf8("Предпросмотр построен"),
                                    false);
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(faceExtrudeSession_.bodyId(),
                                   faceExtrudeSession_.sourceFeatureId(),
                                   faceExtrudeSession_.previewShape());
    viewport_->setToolCutPreviewShape(
        faceExtrudeSession_.subtractivePreviewShape());
  } else {
    viewport_->clearToolPreviewShape();
    toolParametersPanel_->setStatus(
        state == ToolLifecycle::PreviewInvalid
            ? localizedFaceToolError(faceExtrudeSession_.error())
            : QString::fromUtf8("Выберите грань тела"),
        state == ToolLifecycle::PreviewInvalid);
  }
  if (const auto manipulator = faceExtrudeSession_.manipulator())
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(localizedFaceToolError(faceExtrudeSession_.error()));
}

void MainWindow::acceptFaceExtrudeTool() {
  if (faceExtrudeSession_.lifecycle() != ToolLifecycle::PreviewValid) return;
  const Document previous = document_;
  if (faceExtrudeSession_.isSketchSource()) {
    Body* targetBody = nullptr;
    if (faceExtrudeSession_.operation() == ExtrudeOperation::NewBody) {
      targetBody = &document_.addBody();
    } else {
      targetBody = document_.activeBody();
      if (!targetBody) return;
    }
    auto feature = std::make_unique<ExtrudeFeature>(
        faceExtrudeSession_.profileSketchId(), faceExtrudeSession_.lengthMm(),
        "Extrude", faceExtrudeSession_.operation(),
        faceExtrudeSession_.reversed());
    feature->setProfileOverride(faceExtrudeSession_.profileOverride());
    targetBody->addFeature(std::move(feature));
    if (!document_.recompute()) {
      const QString error = QString::fromStdString(document_.rebuildError());
      document_ = previous;
      refreshBodyViewFromDocument();
      toolParametersPanel_->setStatus(error, true);
      return;
    }
    const Document next = document_;
    cancelFaceExtrudeTool();
    pushUndoRedoAction(
        [this, previous] {
          document_ = previous;
          refreshBodyViewFromDocument();
          rebuildFeatureTree();
          rebuildHistoryPanel();
        },
        [this, next] {
          document_ = next;
          refreshBodyViewFromDocument();
          rebuildFeatureTree();
          rebuildHistoryPanel();
        });
    modelRibbon_->clearActiveTool();
    rebuildFeatureTree();
    rebuildHistoryPanel();
    statusBar()->showMessage(QString::fromUtf8("Создано выдавливание профиля"), 3000);
    return;
  }
  Body* body = document_.findBody(faceExtrudeSession_.bodyId());
  if (!body) return;
  if (const auto editingId = faceExtrudeSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* extrude =
          dynamic_cast<ExtrudeFeature*>(body->features()[index].get());
      if (!extrude || extrude->id() != *editingId) continue;
      extrude->setLengthMm(faceExtrudeSession_.lengthMm());
      extrude->setOperation(faceExtrudeSession_.operation());
      extrude->setReversed(faceExtrudeSession_.reversed());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ExtrudeFeature>(
        faceExtrudeSession_.face(), faceExtrudeSession_.lengthMm(), "Extrude",
        faceExtrudeSession_.operation(), faceExtrudeSession_.reversed()));
  }
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  cancelFaceExtrudeTool();
  pushUndoAction([this, previous] {
    document_ = previous;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  statusBar()->showMessage(QString::fromUtf8("Создано выдавливание грани"), 3000);
}

void MainWindow::cancelFaceExtrudeTool() {
  if (faceExtrudeSession_.lifecycle() == ToolLifecycle::Inactive) return;
  faceExtrudeSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Extrude);
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
  viewport_->clearLegacyExtrusionPreview();
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyFaces({});
  toolParametersDock_->hide();
  refreshBodyViewFromDocument();
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
  partDesignTools_.activate(PartDesignToolKind::Extrude);
  viewport_->clearLegacyExtrusionPreview();
  faceExtrudeSession_.begin(body->id(), sourceFeatureId, baseShape, face,
                            extrude->lengthMm(), extrude->operation(),
                            extrude->reversed(), extrude->id());
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces({face});
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Грань"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  const auto parameters = faceExtrudeSession_.parameters();
  const double minimum = parameters.empty() ? 0.01 : parameters.front().minimum;
  const double maximum =
      parameters.empty() ? 100000.0 : parameters.front().maximum;
  toolParametersPanel_->setParameterRange(minimum, maximum, 2);
  toolParametersPanel_->setParameterValue(faceExtrudeSession_.lengthMm());
  toolParametersPanel_->configureOption(
      QString::fromUtf8("Вырезать"),
      faceExtrudeSession_.operation() == ExtrudeOperation::Cut);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
  statusBar()->showMessage(
      QString::fromUtf8("Редактирование выдавливания грани"), 3000);
}

void MainWindow::updateChamferToolPreview() {
  const auto state = chamferToolSession_.lifecycle();
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(chamferToolSession_.edges().size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  const auto maximum = chamferToolSession_.maximumValidDistanceMm();
  const QString error = maximum
                            ? QString::fromUtf8(
                                  "Размер фаски слишком велик. Максимально "
                                  "допустимое значение: %1 мм.")
                                  .arg(*maximum, 0, 'f', 2)
                            : localizedPartDesignError(
                                  PartDesignToolKind::Chamfer,
                                  chamferToolSession_.error());
  const QString limitStatus =
      maximum
          ? QString::fromUtf8("Достигнут предельный размер фаски: %1 мм")
                .arg(*maximum, 0, 'f', 2)
          : QString{};
  toolParametersPanel_->setParameterValue(chamferToolSession_.distanceMm());
  toolParametersPanel_->setStatus(
      valid ? chamferToolSession_.limitReached()
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Chamfer,
                                           ToolSelectionStage::SelectingInput)
                  : error,
      state == ToolLifecycle::PreviewInvalid);
  if (chamferToolSession_.previewShape())
    viewport_->setToolPreviewShape(chamferToolSession_.bodyId(),
                                   chamferToolSession_.sourceFeatureId(),
                                   chamferToolSession_.previewShape());
  else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = chamferToolSession_.manipulator())
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(error, 5000);
  else if (chamferToolSession_.limitReached())
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelChamferTool() {
  if (chamferToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  chamferToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Chamfer);
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
  viewport_->setEdgeMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyEdges({});
  toolParametersDock_->hide();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(QString::fromUtf8("Фаска отменена"), 2000);
}

void MainWindow::acceptChamferTool() {
  if (chamferToolSession_.lifecycle() != ToolLifecycle::PreviewValid) return;
  const Document previousDocument = document_;
  Body* body = document_.findBody(chamferToolSession_.bodyId());
  if (!body) return;
  if (const auto editingId = chamferToolSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* chamfer = dynamic_cast<ChamferFeature*>(body->features()[index].get());
      if (!chamfer || chamfer->id() != *editingId) continue;
      chamfer->setEdges(chamferToolSession_.edges());
      chamfer->setDistanceMm(chamferToolSession_.distanceMm());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ChamferFeature>(
        chamferToolSession_.edges(), chamferToolSession_.distanceMm(),
        "Фаска " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = localizedPartDesignError(
        PartDesignToolKind::Chamfer, document_.rebuildError());
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  const double distance = chamferToolSession_.distanceMm();
  chamferToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Chamfer);
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
  viewport_->setEdgeMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyEdges({});
  toolParametersDock_->hide();
  pushUndoAction([this, previousDocument] {
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  statusBar()->showMessage(
      QString::fromUtf8("Фаска применена: %1 мм").arg(distance), 3000);
}

void MainWindow::updateFilletToolPreview() {
  const auto state = filletToolSession_.lifecycle();
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(filletToolSession_.edges().size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  const auto maximum = filletToolSession_.maximumValidRadiusMm();
  const QString error = maximum
                            ? QString::fromUtf8(
                                  "Радиус скругления слишком велик. Максимально "
                                  "допустимое значение: %1 мм.")
                                  .arg(*maximum, 0, 'f', 2)
                            : localizedPartDesignError(
                                  PartDesignToolKind::Fillet,
                                  filletToolSession_.error());
  const QString limitStatus =
      maximum
          ? QString::fromUtf8("Достигнут предельный радиус: %1 мм")
                .arg(*maximum, 0, 'f', 2)
          : QString{};
  toolParametersPanel_->setParameterValue(filletToolSession_.radiusMm());
  toolParametersPanel_->setStatus(
      valid ? filletToolSession_.limitReached()
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Fillet,
                                           ToolSelectionStage::SelectingInput)
                  : error,
      state == ToolLifecycle::PreviewInvalid);
  if (filletToolSession_.previewShape())
    viewport_->setToolPreviewShape(filletToolSession_.bodyId(),
                                   filletToolSession_.sourceFeatureId(),
                                   filletToolSession_.previewShape());
  else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = filletToolSession_.manipulator())
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(error, 5000);
  else if (filletToolSession_.limitReached())
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelFilletTool() {
  if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  filletToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Fillet);
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
  viewport_->setEdgeMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyEdges({});
  toolParametersDock_->hide();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(QString::fromUtf8("Скругление отменено"), 2000);
}

void MainWindow::acceptFilletTool() {
  if (filletToolSession_.lifecycle() != ToolLifecycle::PreviewValid) return;
  const Document previousDocument = document_;
  Body* body = document_.findBody(filletToolSession_.bodyId());
  if (!body) return;
  if (const auto editingId = filletToolSession_.editingFeatureId()) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* fillet = dynamic_cast<FilletFeature*>(body->features()[index].get());
      if (!fillet || fillet->id() != *editingId) continue;
      fillet->setEdges(filletToolSession_.edges());
      fillet->setRadiusMm(filletToolSession_.radiusMm());
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<FilletFeature>(
        filletToolSession_.edges(), filletToolSession_.radiusMm(),
        "Скругление " + std::to_string(body->features().size())));
  }
  if (!document_.rebuild()) {
    const QString error = localizedPartDesignError(
        PartDesignToolKind::Fillet, document_.rebuildError());
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  const double radius = filletToolSession_.radiusMm();
  filletToolSession_.cancel();
  partDesignTools_.deactivate(PartDesignToolKind::Fillet);
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
  viewport_->setEdgeMultiSelectionMode(false);
  viewport_->setSelectionFilter(SelectionFilter::Any);
  viewport_->setSelectedBodyEdges({});
  toolParametersDock_->hide();
  pushUndoAction([this, previousDocument] {
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
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
      else editHistoryFeature(step.bodyId, step.featureId);
    });
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(button, &QToolButton::customContextMenuRequested, this,
            [this, button, step](const QPoint& point) {
      QMenu menu(button);
      QAction* edit = menu.addAction(QString::fromUtf8("Редактировать"));
      QAction* remove = menu.addAction(QString::fromUtf8("Удалить"));
      QAction* chosen = menu.exec(button->mapToGlobal(point));
      if (chosen == edit) {
        if (step.sketchId != kInvalidSketchId) editSketchById(step.sketchId);
        else editHistoryFeature(step.bodyId, step.featureId);
      } else if (chosen == remove) {
        removeHistoryStep(step);
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
  statusBar()->showMessage(QString::fromUtf8(
      "Вернитесь к последнему шагу истории, чтобы добавить новую операцию."),
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
  if (plan.empty()) return;
  QStringList dependents;
  for (const auto& candidate : historySteps_) {
    if (candidate.featureId != step.featureId &&
        std::find(plan.featureIds.begin(), plan.featureIds.end(),
                  candidate.featureId) != plan.featureIds.end())
      dependents << QString::fromUtf8("• ") + candidate.title;
    if (candidate.sketchId != step.sketchId &&
        std::find(plan.sketchIds.begin(), plan.sketchIds.end(),
                  candidate.sketchId) != plan.sketchIds.end())
      dependents << QString::fromUtf8("• ") + candidate.title;
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

  const Document previous = document_;
  const auto previousSketchHistory = sketchHistory_;
  std::string error;
  const bool removed = step.sketchId != kInvalidSketchId
      ? document_.removeSketchCascade(step.sketchId, &error)
      : document_.removeFeatureCascade(step.bodyId, step.featureId, &error);
  const bool recomputed = removed && document_.recompute();
  const std::string rebuildError = recomputed ? std::string{} : document_.rebuildError();
  if (!recomputed) {
    document_ = previous;
    QMessageBox::warning(this, QString::fromUtf8("Ошибка удаления"),
                         QString::fromStdString(
                             error.empty() ? rebuildError : error));
    return;
  }
  for (std::size_t index = sketchHistory_.size(); index-- > 0;)
    if (!document_.findSketch(sketchHistory_[index].documentSketchId)) {
      viewport_->removeSketch(index);
      sketchHistory_.erase(sketchHistory_.begin() +
                           static_cast<std::ptrdiff_t>(index));
    }
  pushUndoAction([this, previous, previousSketchHistory] {
    for (std::size_t index = sketchHistory_.size(); index-- > 0;)
      viewport_->removeSketch(index);
    document_ = previous;
    sketchHistory_ = previousSketchHistory;
    for (const auto& entry : sketchHistory_) {
      const auto* sketch = document_.findSketch(entry.documentSketchId);
      viewport_->addSketch(entry.geometry, entry.support,
                           sketch ? sketch->placement : SketchPlacement::xy());
    }
    refreshBodyViewFromDocument(); rebuildFeatureTree(); rebuildHistoryPanel();
  });
  partDesignTools_.cancelActive();
  viewport_->clearToolPreviewShape(); viewport_->clearToolManipulator();
  viewport_->setSelectedBodyEdges({}); viewport_->setSelectedBodyFaces({});
  toolParametersDock_->hide();
  revolveDock_->hide();
  mirrorDock_->hide();
  linearPatternDock_->hide();
  circularPatternDock_->hide();
  historyPosition_ = 1000000;
  refreshBodyViewFromDocument(); rebuildFeatureTree(); rebuildHistoryPanel();
}

void MainWindow::removeBody(BodyId bodyId) {
  if (!ensureHistoryAtEnd()) return;
  const Body* body = document_.findBody(bodyId);
  if (!body) return;
  QMessageBox box(
      QMessageBox::Warning, QString::fromUtf8("Удаление Body"),
      QString::fromUtf8("Удалить «%1» и зависимые эскизы/операции?")
          .arg(QString::fromStdString(body->name())),
      QMessageBox::Yes | QMessageBox::Cancel, this);
  box.button(QMessageBox::Yes)->setText(QString::fromUtf8("Удалить"));
  box.button(QMessageBox::Cancel)->setText(QString::fromUtf8("Отмена"));
  if (box.exec() != QMessageBox::Yes) return;

  resetTransientModelingUi();
  const Document previousDocument = document_;
  const auto previousSketchHistory = sketchHistory_;
  const auto previousExtrusionSource = extrusionSourceSketch_;
  const std::optional<SketchId> previousSourceSketchId =
      previousExtrusionSource &&
              *previousExtrusionSource < previousSketchHistory.size()
          ? std::optional<SketchId>(
                previousSketchHistory[*previousExtrusionSource].documentSketchId)
          : std::nullopt;
  std::string error;
  if (!document_.removeBodyCascade(bodyId, &error) || !document_.recompute()) {
    const std::string rebuildError = document_.rebuildError();
    document_ = previousDocument;
    QMessageBox::warning(
        this, QString::fromUtf8("Ошибка удаления"),
        QString::fromStdString(error.empty() ? rebuildError : error));
    return;
  }

  for (std::size_t index = sketchHistory_.size(); index-- > 0;)
    if (!document_.findSketch(sketchHistory_[index].documentSketchId)) {
      viewport_->removeSketch(index);
      sketchHistory_.erase(sketchHistory_.begin() +
                           static_cast<std::ptrdiff_t>(index));
    }
  sketchCount_ = sketchHistory_.size();
  extrusionSourceSketch_.reset();
  if (previousSourceSketchId)
    for (std::size_t index = 0; index < sketchHistory_.size(); ++index)
      if (sketchHistory_[index].documentSketchId == *previousSourceSketchId) {
        extrusionSourceSketch_ = index;
        break;
      }
  const Document removedDocument = document_;
  const auto removedSketchHistory = sketchHistory_;
  const auto removedExtrusionSource = extrusionSourceSketch_;
  const auto restoreSnapshot = [this](const Document& document,
                                      const auto& sketchHistory,
                                      std::optional<std::size_t> source) {
    for (std::size_t index = sketchHistory_.size(); index-- > 0;)
      viewport_->removeSketch(index);
    document_ = document;
    sketchHistory_ = sketchHistory;
    sketchCount_ = sketchHistory_.size();
    extrusionSourceSketch_ =
        source && *source < sketchHistory_.size() ? source : std::nullopt;
    for (const auto& entry : sketchHistory_) {
      const auto* sketch = document_.findSketch(entry.documentSketchId);
      viewport_->addSketch(entry.geometry, entry.support,
                           sketch ? sketch->placement : SketchPlacement::xy());
    }
    viewport_->setSelectedBodies({});
    viewport_->setSelectedBodyEdges({});
    viewport_->setSelectedBodyFaces({});
    historyPosition_ = 1000000;
    refreshBodyViewFromDocument();
    rebuildHistoryPanel();
    rebuildFeatureTree();
  };
  pushUndoRedoAction(
      [this, previousDocument, previousSketchHistory,
       previousExtrusionSource, restoreSnapshot] {
        restoreSnapshot(previousDocument, previousSketchHistory,
                        previousExtrusionSource);
      },
      [this, removedDocument, removedSketchHistory,
       removedExtrusionSource, restoreSnapshot] {
        restoreSnapshot(removedDocument, removedSketchHistory,
                        removedExtrusionSource);
      });
  restoreSnapshot(removedDocument, removedSketchHistory,
                  removedExtrusionSource);
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
      if (auto shape = body.resultShape())
        shapes.push_back({body.id(), body.activeFeature()->id(), std::move(shape)});
    }
    if (historyPosition_ > 0) {
      const HistoryStep& step = historySteps_[historyPosition_ - 1];
      if (step.shape)
        shapes.push_back({step.bodyId, step.featureId, step.shape});
    }
    viewport_->setBodyShapes(std::move(shapes));
    viewport_->setSolidVisible(historyPosition_ > 0);
  }
  for (std::size_t index = 0; index < sketchHistory_.size(); ++index) {
    const bool selectedSketch = historyPosition_ > 0 &&
        historySteps_[historyPosition_ - 1].sketchId ==
            sketchHistory_[index].documentSketchId;
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
  const auto resolved = resolveFacePlacement(*shape, reference.topology());
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
      {kInvalidSketchId, currentSketchPlacement_, shape, reference,
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
    if (body.id() == supportBodyId) continue;
    if (auto shape = body.resultShape()) bodyShapes.push_back(std::move(shape));
  }

  std::vector<SketchSceneReference> sketches;
  sketches.reserve(document_.sketches().size());
  for (const auto& documentSketch : document_.sketches()) {
    if (documentSketch.id == excludedSketchId) continue;
    sketches.push_back(
        {documentSketch.geometry, documentSketch.placement});
  }
  sketchCanvas_->setSceneReferences(currentSketchPlacement_, bodyShapes,
                                    std::move(sketches));
}

void MainWindow::editSketchStep(std::size_t index) {
  if (index >= sketchHistory_.size()) return;
  editingSketchIndex_ = index;
  currentSketchSupport_ = sketchHistory_[index].support;
  currentSketchFaceReference_.reset();
  if (const auto* modelSketch =
          document_.findSketch(sketchHistory_[index].documentSketchId)) {
    currentSketchPlacement_ = modelSketch->placement;
    if (modelSketch->support.type == SketchSupportType::Face)
      currentSketchFaceReference_ = modelSketch->support.face;
  }
  sketchCanvas_->clearSketchEditContext();
  if (currentSketchFaceReference_ && !configureSketchEditContext()) {
    editingSketchIndex_.reset();
    return;
  }
  sketchCanvas_->loadSketch(sketchHistory_[index].geometry);
  configureSketchSceneReferences(sketchHistory_[index].documentSketchId);
  sketchCanvas_->setReferenceBody(document_.box(), currentSketchSupport_,
                                  hasExtrusion_);
  workspaceStack_->setCurrentWidget(sketchCanvas_);
  ribbonStack_->setCurrentWidget(sketchRibbon_);
  statusBar()->showMessage(
      QString::fromUtf8("Редактируется эскиз %1 • Завершите эскиз для перестроения")
          .arg(index + 1));
}

void MainWindow::editSketchById(SketchId sketchId) {
  const auto found = std::find_if(
      sketchHistory_.begin(), sketchHistory_.end(), [sketchId](const auto& entry) {
        return entry.documentSketchId == sketchId;
      });
  if (found != sketchHistory_.end())
    editSketchStep(static_cast<std::size_t>(
        std::distance(sketchHistory_.begin(), found)));
}

void MainWindow::editHistoryFeature(BodyId bodyId, FeatureId featureId) {
  Body* body = document_.findBody(bodyId);
  if (!body) return;
  ShapeFeature* feature = findHistoryFeature(document_, bodyId, featureId);
  if (feature) resetTransientModelingUi();
  if (dynamic_cast<ExtrudeFeature*>(feature)) editExtrusionStep(bodyId, featureId);
  else if (dynamic_cast<PocketFeature*>(feature)) editPocketStep(bodyId, featureId);
  else if (dynamic_cast<FilletFeature*>(feature)) editFilletStep(bodyId, featureId);
  else if (dynamic_cast<ChamferFeature*>(feature)) editChamferStep(bodyId, featureId);
  else if (feature) editPatternFeature(featureId);
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
  if (dialog.exec() != QDialog::Accepted) return;
  const double height = distance->value();
  const Document previousDocument = document_;
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
  pushUndoAction([this, previousDocument] {
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
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
  if (!accepted) return;
  const Document previousDocument = document_;
  pocket->setDepthMm(depth);
  if (!document_.rebuild()) {
    const QString error = QString::fromStdString(pocket->error());
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    QMessageBox::warning(this, QString::fromUtf8("Ошибка кармана"), error);
    return;
  }
  pushUndoAction([this, previousDocument] {
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
  });
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  statusBar()->showMessage(
      QString::fromUtf8("Глубина кармана изменена: %1 мм").arg(depth), 3000);
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
  for (std::size_t index = 1; index < features.size(); ++index)
    if (features[index].get() == fillet) {
      upstream = features[index - 1]->shape();
      break;
    }
  if (!upstream) return;
  partDesignTools_.activate(PartDesignToolKind::Fillet);
  if (chamferToolSession_.lifecycle() != ToolLifecycle::Inactive)
    cancelChamferTool();
  filletToolSession_.begin(body->id(), fillet->edge().featureId, upstream,
                           fillet->edges(), fillet->radiusMm(), fillet->id());
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
  for (std::size_t index = 1; index < features.size(); ++index)
    if (features[index].get() == chamfer) {
      upstream = features[index - 1]->shape();
      break;
    }
  if (!upstream) return;
  partDesignTools_.activate(PartDesignToolKind::Chamfer);
  if (filletToolSession_.lifecycle() != ToolLifecycle::Inactive)
    cancelFilletTool();
  chamferToolSession_.begin(body->id(), chamfer->edge().featureId, upstream,
                            chamfer->edges(), chamfer->distanceMm(),
                            chamfer->id());
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
  // The rebuilt tree starts with unchecked base planes, so the viewport must
  // immediately use the same visibility state.
  for (int plane = 0; plane < 3; ++plane)
    viewport_->setBasePlaneVisible(plane, false);
  featureTree_->clear();
  auto* project = new QTreeWidgetItem(featureTree_, {QString::fromUtf8("Корпус детали")});
  auto* origin = new QTreeWidgetItem(project, {QString::fromUtf8("Начало координат")});
  origin->setData(0, Qt::UserRole, 1);
  origin->setFlags(origin->flags() | Qt::ItemIsUserCheckable);
  origin->setCheckState(0, Qt::Checked);
  for (int plane = 0; plane < 3; ++plane) {
    const QString name = plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ";
    auto* planeItem = new QTreeWidgetItem(
        origin, {QString::fromUtf8("Плоскость ") + name});
    planeItem->setData(0, Qt::UserRole, 10 + plane);
    planeItem->setFlags(planeItem->flags() | Qt::ItemIsUserCheckable);
    planeItem->setCheckState(0, Qt::Unchecked);
  }

  auto* sketches = new QTreeWidgetItem(project, {QString::fromUtf8("Эскизы")});
  const std::size_t activeSketchCount = std::min<std::size_t>(
      static_cast<std::size_t>(std::max(historyPosition_, 0)), sketchCount_);
  // During a just-committed direct operation rebuildFeatureTree() can run
  // before rebuildHistoryPanel() has appended the new history step. Treat the
  // previous end marker as "at end" so the newly created Body is not hidden.
  const bool historyAtEnd =
      historyPosition_ >= static_cast<int>(historySteps_.size());
  const bool activeExtrusion = hasExtrusion_ &&
      (historyAtEnd ||
       historyPosition_ > static_cast<int>(sketchHistory_.size()));
  if (activeSketchCount == 0) {
    new QTreeWidgetItem(sketches, {QString::fromUtf8("Эскизов нет")});
  } else {
    for (std::size_t index = 0; index < activeSketchCount; ++index) {
      auto* sketchItem = new QTreeWidgetItem(
          sketches, {QString::fromUtf8("⌞  Эскиз %1").arg(index + 1)});
      sketchItem->setData(0, Qt::UserRole, 20 + static_cast<int>(index));
      sketchItem->setFlags(sketchItem->flags() | Qt::ItemIsUserCheckable);
      const SketchId sketchId = sketchHistory_[index].documentSketchId;
      bool visible = true;
      if (historyAtEnd) {
        // Do not use legacy extrusionSourceSketch_ as the source of truth:
        // direct Join/Cut and multiple downstream Part Design features can
        // consume several different profile sketches in the same model.
        visible = !isSketchConsumedByPartDesign(document_, sketchId);
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
  if (!activeExtrusion || document_.bodies().empty()) {
    new QTreeWidgetItem(models, {QString::fromUtf8("Твёрдых тел нет")});
  } else {
    for (std::size_t bodyIndex = 0; bodyIndex < document_.bodies().size(); ++bodyIndex) {
      const Body& body = document_.bodies()[bodyIndex];
      auto* bodyItem = new QTreeWidgetItem(
          models, {QString::fromUtf8("▣  Body%1").arg(bodyIndex + 1, 3, 10,
                                                       QLatin1Char('0'))});
      bodyItem->setData(0, Qt::UserRole, 3);
      bodyItem->setData(0, Qt::UserRole + 2,
                        QVariant::fromValue<qulonglong>(body.id()));
      bodyItem->setFlags(bodyItem->flags() | Qt::ItemIsUserCheckable);
      bodyItem->setCheckState(0, Qt::Checked);
      for (const auto& feature : body.features()) {
        const QString state =
            feature->isValid() ? QStringLiteral("Valid")
            : feature->isDirty() ? QStringLiteral("Dirty")
                                 : QStringLiteral("Error");
        const QString name = QString::fromStdString(
            feature->name().empty() ? feature->typeName() : feature->name());
        auto* featureItem =
            new QTreeWidgetItem(bodyItem, {name + "  [" + state + "]"});
        featureItem->setData(
            0, Qt::UserRole + 1,
            QVariant::fromValue<qulonglong>(feature->id()));
        featureItem->setToolTip(
            0, feature->isFailed()
                   ? QString::fromStdString(feature->error())
                   : QString::fromUtf8("Состояние: ") + state);
        if (feature->isFailed()) featureItem->setForeground(0, QColor("#c62828"));
      }
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

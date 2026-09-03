#include "ui/MainWindow.h"

#include "model/ExtrudeFeature.h"
#include "model/ExtrudeOperationDetector.h"
#include "model/FilletFeature.h"
#include "ui/ToolParametersPanel.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"

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
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QButtonGroup>
#include <QLabel>
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
#include <QStackedWidget>
#include <QTreeWidget>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QToolButton>
#include <QTimer>
#include <QVariant>
#include <limits>

#include "drawing/EskdRenderer.h"
#include "io/StlExporter.h"
#include "project/ProjectFile.h"
#include "sketch/SketchRibbon.h"
#include "ui/DrawingSheetView.h"
#include "ui/ModelRibbon.h"
#include "ui/SketchCanvas.h"
#include "ui/Viewport.h"

namespace solidar {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  buildUi();
  buildMenus();
  resize(1200, 760);
  setWindowTitle(QString::fromUtf8("Солидарность CAD — Скетчер ЕСКД"));
}

void MainWindow::updateSketchConstraintPanel() {
  if (!sketchConstraintsList_ || !sketchCanvas_) return;

  const QSignalBlocker blocker(sketchConstraintsList_);
  sketchConstraintsList_->clear();

  const auto entries = sketchCanvas_->selectedConstraintPanelEntries();

  if (entries.empty()) {
    auto* item = new QTreeWidgetItem(sketchConstraintsList_);
    item->setText(0, QString::fromUtf8("Нет ограничений"));
    item->setForeground(0, QColor("#8795ac"));
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
  auto* exportMenu = fileMenu->addMenu(QString::fromUtf8("Экспорт"));
  auto* exportStlAction = exportMenu->addAction(QStringLiteral("STL (*.stl)"));

  auto* editMenu = bar->addMenu(QString::fromUtf8("Правка"));
  undoAction_ = editMenu->addAction(QString::fromUtf8("Отменить"));
  undoAction_->setShortcut(QKeySequence::Undo);
  undoAction_->setEnabled(false);

  layout->addWidget(bar);
  ribbonStack_->setParent(menuHost);
  layout->addWidget(ribbonStack_);
  setMenuWidget(menuHost);

  connect(createAction, &QAction::triggered, this, &MainWindow::createProject);
  connect(openAction, &QAction::triggered, this, &MainWindow::openProject);
  connect(saveAction, &QAction::triggered, this, &MainWindow::saveProject);
  connect(exportStlAction, &QAction::triggered, this, &MainWindow::exportStl);
  connect(undoAction_, &QAction::triggered, this, &MainWindow::undoLastAction);
  connect(sketchCanvas_, &SketchCanvas::undoAvailable, this,
          [this](bool) { updateUndoAvailability(); });
}

void MainWindow::pushUndoAction(std::function<void()> action) {
  if (applyingUndo_) return;
  modelUndoStack_.push_back(std::move(action));
  if (modelUndoStack_.size() > 100) modelUndoStack_.erase(modelUndoStack_.begin());
  updateUndoAvailability();
}

void MainWindow::updateUndoAvailability() {
  if (!undoAction_) return;
  const bool sketchUndo = workspaceStack_ &&
                          workspaceStack_->currentWidget() == sketchCanvas_ &&
                          sketchCanvas_->canUndo();
  undoAction_->setEnabled(sketchUndo || !modelUndoStack_.empty());
}

void MainWindow::undoLastAction() {
  if (workspaceStack_->currentWidget() == sketchCanvas_ &&
      sketchCanvas_->canUndo()) {
    sketchCanvas_->undo();
  } else if (!modelUndoStack_.empty()) {
    auto action = std::move(modelUndoStack_.back());
    modelUndoStack_.pop_back();
    applyingUndo_ = true;
    action();
    applyingUndo_ = false;
  }
  updateUndoAvailability();
}

void MainWindow::createProject() {
  QString path = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Создать проект"), QStringLiteral("Новый проект.solidar"),
      QString::fromUtf8("Проекты Солидарность CAD (*.solidar)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(QStringLiteral(".solidar"), Qt::CaseInsensitive))
    path += QStringLiteral(".solidar");
  QString error;
  if (!project::ProjectFile::create(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка создания"), error);
    return;
  }
  setProjectPath(path);
  sketchCanvas_->resetSketch();
  document_ = Document{};
  hasExtrusion_ = false;
  sketchCount_ = 0;
  extrusionSourceSketch_.reset();
  selectedExtrusionSurface_.clear();
  currentSketchSupport_ = QStringLiteral("XY");
  currentSketchPlacement_ = SketchPlacement::xy();
  currentSketchFaceReference_.reset();
  if (extrusionDock_) extrusionDock_->hide();
  viewport_->hideExtrusionManipulator();
  viewport_->resetScene();
  viewport_->setBox(document_.box());
  sketchHistory_.clear();
  modelUndoStack_.clear();
  historyPosition_ = 0;
  editingSketchIndex_.reset();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  workspaceStack_->setCurrentWidget(viewport_);
  statusBar()->showMessage(QString::fromUtf8("Создан новый проект"), 3000);
}

void MainWindow::openProject() {
  const QString path = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Открыть проект"), QString(),
      QString::fromUtf8("Проекты Солидарность CAD (*.solidar)"));
  if (path.isEmpty()) return;
  QString error;
  if (!loadProject(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка открытия"), error);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("Проект открыт"), 3000);
}

bool MainWindow::loadProject(const QString& path, QString* error) {
  project::ProjectData data;
  if (!project::ProjectFile::load(path, &data, error)) return false;
  Document restoredDocument;
  QString modelError;
  const bool hasParametricHistory = project::ProjectFile::loadDocument(
      path, &restoredDocument, &modelError);
  document_ = hasParametricHistory ? std::move(restoredDocument) : Document{};
  if (!hasParametricHistory) document_.setBox(data.box);
  sketchHistory_.clear();
  viewport_->resetScene();
  viewport_->setBox(document_.box());
  for (std::size_t index = 0; index < data.sketches.size(); ++index) {
    const auto& saved = data.sketches[index];
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
                                       : data.hasExtrusion;
  extrusionSourceSketch_ = data.extrusionSourceSketch;
  if (hasExtrusion_ && extrusionSourceSketch_ &&
      *extrusionSourceSketch_ < sketchHistory_.size()) {
    const auto& source = sketchHistory_[*extrusionSourceSketch_];
    viewport_->setSolidSketch(source.geometry);
    viewport_->setSolidSupport(source.support);
  }
  viewport_->setSolidVisible(hasExtrusion_);
  if (hasParametricHistory) refreshBodyViewFromDocument();
  historyPosition_ = static_cast<int>(sketchHistory_.size()) +
                     (hasExtrusion_ ? 1 : 0);
  editingSketchIndex_.reset();
  modelUndoStack_.clear();
  sketchCanvas_->resetSketch();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
  setProjectPath(path);
  return true;
}

void MainWindow::saveProject() {
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
  statusBar()->showMessage(QString::fromUtf8("Проект сохранён"), 3000);
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
  if (!io::exportAsciiStl(fileName, viewport_->solidSketch(),
                          viewport_->solidSupport(), document_.box(),
                          viewport_->bodyPosition(), viewport_->solidFeatures(),
                          &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка экспорта STL"), error);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("STL сохранён: ") + fileName, 5000);
}

void MainWindow::setProjectPath(const QString& path) {
  setWindowFilePath(path);
  setWindowTitle(QFileInfo(path).completeBaseName() +
                 QString::fromUtf8(" — Солидарность CAD"));
}

void MainWindow::buildUi() {
  workspaceStack_ = new QStackedWidget(this);
  sketchCanvas_ = new SketchCanvas(workspaceStack_);
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
  auto* extrusionHint = new QLabel(QString::fromUtf8(
      "Потяните синюю стрелку в 3D-виде или введите точное значение."),
      extrusionPanel);
  extrusionHint->setWordWrap(true);
  extrusionHint->setStyleSheet("color:#607493;");
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
  acceptExtrusion->setStyleSheet(
      "QPushButton{background:#0874f9;color:white;border:none;border-radius:6px;"
      "padding:8px 14px;font-weight:600;} QPushButton:hover{background:#0665dc;}");
  extrusionButtons->addWidget(cancelExtrusion);
  extrusionButtons->addWidget(acceptExtrusion);
  extrusionPanelLayout->addWidget(extrusionHint);
  extrusionPanelLayout->addLayout(extrusionForm);
  extrusionPanelLayout->addStretch();
  extrusionPanelLayout->addLayout(extrusionButtons);
  extrusionDock_->setWidget(extrusionPanel);
  extrusionDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, extrusionDock_);
  extrusionDock_->hide();

  revolveDock_ = new QDockWidget(QString::fromUtf8("Инструмент вращения"), this);
  revolveDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  revolveDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* revolvePanel = new QWidget(revolveDock_);
  auto* revolveLayout = new QVBoxLayout(revolvePanel);
  auto* revolveTitle = new QLabel(QString::fromUtf8("ИНСТРУМЕНТ ВРАЩЕНИЯ"), revolvePanel);
  QFont revolveTitleFont = revolveTitle->font();
  revolveTitleFont.setBold(true); revolveTitle->setFont(revolveTitleFont);
  auto* revolveForm = new QFormLayout;
  revolveProfileCombo_ = new QComboBox(revolvePanel);
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
  revolveForm->addRow(QString::fromUtf8("Профиль:"), revolveProfileCombo_);
  revolveForm->addRow(QString::fromUtf8("Ось:"), revolveAxisCombo_);
  revolveForm->addRow(QString::fromUtf8("Угол:"), revolveAngleSpin_);
  revolveForm->addRow(QString::fromUtf8("Операция:"), revolveOperationCombo_);
  revolveForm->addRow(QString(), revolveReverseCheck_);
  auto* revolveButtons = new QHBoxLayout;
  auto* cancelRevolve = new QPushButton(QString::fromUtf8("Отмена"), revolvePanel);
  revolveAcceptButton_ = new QPushButton(QStringLiteral("OK"), revolvePanel);
  revolveAcceptButton_->setEnabled(false);
  revolveButtons->addWidget(cancelRevolve);
  revolveButtons->addWidget(revolveAcceptButton_);
  revolveLayout->addWidget(revolveTitle);
  revolveLayout->addLayout(revolveForm);
  revolveLayout->addStretch();
  revolveLayout->addLayout(revolveButtons);
  revolveDock_->setWidget(revolvePanel);
  revolveDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, revolveDock_);
  revolveDock_->hide();
  connect(revolveProfileCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            rebuildRevolveAxisChoices();
            revolveToolSession_.clearAxis();
            const auto id = static_cast<SketchId>(revolveProfileCombo_->currentData().toULongLong());
            if (id == kInvalidSketchId) revolveToolSession_.clearProfile();
            else revolveToolSession_.setProfile(id);
            updateRevolveToolPreview();
          });
  connect(revolveAxisCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            if (revolveAxisCombo_->currentIndex() <= 0) {
              revolveToolSession_.clearAxis(); updateRevolveToolPreview(); return;
            }
            AxisReference reference;
            reference.sketchId = revolveToolSession_.profileSketchId();
            const qulonglong value = revolveAxisCombo_->currentData().toULongLong();
            if (value == 1) reference.type = AxisReferenceType::SketchHorizontalAxis;
            else if (value == 2) reference.type = AxisReferenceType::SketchVerticalAxis;
            else { reference.type = AxisReferenceType::SketchLine;
                   reference.lineId = static_cast<sketch::GeometryId>(value - 3); }
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
  connect(viewport_, &Viewport::angularToolManipulatorValueChanged, this,
          [this](double angle) {
            if (revolveToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
            revolveToolSession_.setAngleFromManipulator(angle);
            revolveAngleSpin_->setValue(revolveToolSession_.angleDeg());
            updateRevolveToolPreview();
          });

  toolParametersDock_ = new QDockWidget(QString::fromUtf8("Параметры инструмента"), this);
  toolParametersDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  toolParametersDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  toolParametersPanel_ = new ToolParametersPanel(toolParametersDock_);
  toolParametersPanel_->configure(QString::fromUtf8("СКРУГЛЕНИЕ"),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Радиус"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
  toolParametersDock_->setWidget(toolParametersPanel_);
  toolParametersDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, toolParametersDock_);
  toolParametersDock_->hide();
  connect(toolParametersPanel_, &ToolParametersPanel::parameterChanged, this,
          [this](double radius) {
            if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
            filletToolSession_.setRadiusFromPanel(radius);
            updateFilletToolPreview();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::accepted, this,
          &MainWindow::acceptFilletTool);
  connect(toolParametersPanel_, &ToolParametersPanel::cancelled, this,
          &MainWindow::cancelFilletTool);
  connect(toolParametersPanel_, &ToolParametersPanel::selectionRequested, this,
          [this] {
            if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
            statusBar()->showMessage(
                QString::fromUtf8("Выберите одно или несколько рёбер в viewport"));
            viewport_->setFocus();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::clearSelectionRequested,
          this, [this] {
            if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
            viewport_->setSelectedBodyEdges({});
            filletToolSession_.setEdges({});
            updateFilletToolPreview();
          });
  connect(viewport_, &Viewport::toolManipulatorValueChanged, this,
          [this](double radius) {
            if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
            filletToolSession_.setRadiusFromManipulator(radius);
            toolParametersPanel_->setParameterValue(filletToolSession_.radiusMm());
            updateFilletToolPreview();
          });
  connect(viewport_, &Viewport::bodyEdgeSelectionChanged, this, [this] {
    if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
    filletToolSession_.setEdges(viewport_->selectedBodyEdges());
    updateFilletToolPreview();
  });
  auto* acceptToolShortcut = new QShortcut(QKeySequence(Qt::Key_Return),
                                           toolParametersDock_);
  acceptToolShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(acceptToolShortcut, &QShortcut::activated, this,
          &MainWindow::acceptFilletTool);
  auto* cancelToolShortcut = new QShortcut(QKeySequence(Qt::Key_Escape),
                                           toolParametersDock_);
  cancelToolShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(cancelToolShortcut, &QShortcut::activated, this,
          &MainWindow::cancelFilletTool);
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
    if (extrusionDock_->isVisible()) extrudeSketch();
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
  snapCheck->setChecked(true);
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
  constraintsSection->setStyleSheet(
      "QFrame#constraintsSection{border-top:1px solid #d8e1ef;"
      "margin-top:8px;padding-top:10px;}");
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
  sketchConstraintsList_->setStyleSheet(
      "QTreeWidget{border:1px solid #d8e1ef;border-radius:6px;"
      "background:#fbfcff;color:#29466f;padding:2px;}"
      "QTreeWidget::item{padding:4px 5px;}"
      "QHeaderView::section{background:#f2f5fa;color:#66758c;"
      "border:none;border-bottom:1px solid #d8e1ef;padding:5px;}");

  constraintsSectionLayout->addWidget(constraintsTitle);
  constraintsSectionLayout->addWidget(sketchConstraintsList_);
  settingsLayout->addWidget(constraintsSection);

  auto* circlePropertiesSection = new QFrame(settingsPanel);
  circlePropertiesSection->setObjectName(QStringLiteral("circlePropertiesSection"));
  circlePropertiesSection->setStyleSheet(
      "QFrame#circlePropertiesSection{border-top:1px solid #d8e1ef;"
      "margin-top:8px;padding-top:10px;}");
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
    const char* icon;
    const char* tooltip;
  };
  const CircleModeButtonSpec circleModes[] = {
      {":/icons/sketch/circle/center-radius.png", "Из центра"},
      {":/icons/sketch/circle/two-points.png", "По двум точкам"},
      {":/icons/sketch/circle/three-points.png", "По трём точкам"},
      {":/icons/sketch/circle/three-tangents.png", "По трём прямым"},
      {":/icons/sketch/circle/two-tangents-radius.png",
       "По двум прямым и радиусу"},
  };
  for (int index = 0; index < 5; ++index) {
    auto* button = new QToolButton(circleModeButtons);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setIcon(QIcon(QString::fromUtf8(circleModes[index].icon)));
    button->setIconSize(QSize(26, 26));
    button->setFixedSize(36, 36);
    button->setToolTip(QString::fromUtf8(circleModes[index].tooltip));
    button->setStyleSheet(
        "QToolButton{border:1px solid transparent;border-radius:6px;"
        "background:transparent;padding:4px;}"
        "QToolButton:hover{background:#edf5ff;border-color:#9bc4ff;}"
        "QToolButton:checked{background:#dcecff;border:2px solid #0a72ff;}");
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
  rectanglePropertiesSection->setStyleSheet(
      "QFrame#rectanglePropertiesSection{border-top:1px solid #d8e1ef;"
      "margin-top:8px;padding-top:10px;}");
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
      {":/icons/sketch/rectangle/two-points.png", "По двум точкам"},
      {":/icons/sketch/rectangle/three-points.png", "По трём точкам"},
      {":/icons/sketch/rectangle/from-center.png", "Из центра"},
  };
  for (int index = 0; index < 3; ++index) {
    auto* button = new QToolButton(rectangleModeButtons);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setIcon(QIcon(QString::fromUtf8(rectangleModes[index].icon)));
    button->setIconSize(QSize(26, 26));
    button->setFixedSize(36, 36);
    button->setToolTip(QString::fromUtf8(rectangleModes[index].tooltip));
    button->setStyleSheet(
        "QToolButton{border:1px solid transparent;border-radius:6px;"
        "background:transparent;padding:4px;}"
        "QToolButton:hover{background:#edf5ff;border-color:#9bc4ff;}"
        "QToolButton:checked{background:#dcecff;border:2px solid #0a72ff;}");
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
  auto* drawingRibbonPlaceholder = new QWidget(ribbonStack_);
  drawingRibbonPlaceholder->setFixedHeight(124);
  ribbonStack_->addWidget(drawingRibbonPlaceholder);
  connect(modelRibbon_, &ModelRibbon::createSketchRequested, this,
          [this] {
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
            if (!document_.activeBody()) {
              QMessageBox::information(this, QString::fromUtf8("Вырезать"),
                  QString::fromUtf8("Сначала создайте Body."));
              return;
            }
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
  connect(modelRibbon_, &ModelRibbon::fitRequested, viewport_, &Viewport::fitAll);
  connect(modelRibbon_, &ModelRibbon::isoRequested, viewport_, &Viewport::viewIsometric);
  connect(viewport_, &Viewport::sketchPlanePicked, this,
          [this](const QString& plane) {
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
                QMessageBox::warning(this, QString::fromUtf8("Sketch on Face"),
                                     QStringLiteral("Sketch support face could not be resolved"));
                return;
              }
              const auto resolved =
                  resolveFacePlacement(*shape, faceReference->topology());
              if (!resolved.planar) {
                QMessageBox::information(
                    this, QString::fromUtf8("Sketch on Face"),
                    QStringLiteral("Sketch on curved surfaces is not supported yet"));
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
            if (currentSketchFaceReference_ && !configureSketchEditContext())
              return;
            workspaceStack_->setCurrentWidget(sketchCanvas_);
            ribbonStack_->setCurrentWidget(sketchRibbon_);
            statusBar()->showMessage(QString::fromUtf8("Рабочая плоскость: ") + plane);
          });
  connect(viewport_, &Viewport::extrusionSurfacePicked, this,
          [this](const QString& surface) {
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
  });
  workspaceStack_->setCurrentWidget(viewport_);
  ribbonStack_->setCurrentWidget(modelRibbon_);

  auto* modelDock = new QDockWidget(QString::fromUtf8("Дерево построений"), this);
  featureTree_ = new QTreeWidget(modelDock);
  featureTree_->setHeaderHidden(true);
  featureTree_->setAlternatingRowColors(true);
  rebuildFeatureTree();
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
  connect(viewport_, &Viewport::selectionChanged, this,
          [this](const QString& text) {
            if (text == QStringLiteral("__cancel_tools__")) {
              selectedExtrusionSurface_.clear();
              if (extrusionDock_) extrusionDock_->hide();
              modelRibbon_->clearActiveTool();
              statusBar()->showMessage(
                  QString::fromUtf8("Инструменты сброшены"), 2000);
              return;
            }
            statusBar()->showMessage(text.isEmpty()
                                         ? QString::fromUtf8("Выделение снято")
                                         : text);
          });
  setStyleSheet(R"(
    QMainWindow { background:#f7f9fc; }
    QDockWidget { color:#17356e; font-weight:600; }
    QDockWidget::title { background:#ffffff; padding:10px; border-bottom:1px solid #dce4f0; }
    QTreeWidget { background:#ffffff; border:none; color:#274875; padding:8px; }
    QTreeWidget::item { height:28px; border-radius:5px; }
    QTreeWidget::item:selected { background:#e5f0ff; color:#075fdd; }
    QStatusBar { background:#ffffff; color:#657a9b; border-top:1px solid #dce4f0; }
  )");
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
  auto* historyScroll = new QScrollArea(historyDock);
  historyScroll->setWidgetResizable(true);
  historyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  historyScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  historyScroll->setFrameShape(QFrame::NoFrame);
  historyContent_ = new QWidget(historyScroll);
  historyLayout_ = new QHBoxLayout(historyContent_);
  historyLayout_->setContentsMargins(12, 7, 12, 7);
  historyLayout_->setSpacing(8);
  historyLayout_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  historyScroll->setWidget(historyContent_);
  historySlider_ = new QSlider(Qt::Horizontal, historyHost);
  historySlider_->setRange(0, 0);
  historySlider_->setSingleStep(1);
  historySlider_->setPageStep(1);
  historySlider_->setTickPosition(QSlider::TicksBelow);
  historySlider_->setTickInterval(1);
  historySlider_->setFixedHeight(20);
  historySlider_->setToolTip(QString::fromUtf8(
      "Переместите маркер, чтобы откатить историю"));
  historySlider_->setStyleSheet(
      "QSlider::groove:horizontal{height:3px;background:#c8d5e8;border-radius:1px;}"
      "QSlider::sub-page:horizontal{background:#1671e8;}"
      "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:#fff;"
      "border:2px solid #1671e8;border-radius:6px;}");
  connect(historySlider_, &QSlider::valueChanged, this,
          &MainWindow::applyHistoryPosition);
  historyHostLayout->addWidget(historyScroll, 1);
  historyHostLayout->addWidget(historySlider_);
  historyDock->setWidget(historyHost);
  addDockWidget(Qt::BottomDockWidgetArea, historyDock);
  rebuildHistoryPanel();

  statusBar()->showMessage(
      QString::fromUtf8("Готово — профиль ЕСКД, лист A4"));
}

void MainWindow::updateFromSketch(double widthMm, double heightMm) {
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
  if (sketch.lines().empty() && sketch.circles().empty()) {
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
    viewport_->updateSketch(index, sketch, currentSketchSupport_,
                            currentSketchPlacement_);
    if (hasExtrusion_ && extrusionSourceSketch_ == index)
      viewport_->setSolidSketch(sketch);
    statusBar()->showMessage(
        QString::fromUtf8("Эскиз %1 обновлён").arg(index + 1), 3000);
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
      historyPosition_ = static_cast<int>(sketchHistory_.size()) +
                         (hasExtrusion_ ? 1 : 0);
      rebuildFeatureTree();
      rebuildHistoryPanel();
    });
    viewport_->addSketch(sketch, currentSketchSupport_,
                         currentSketchPlacement_);
    sketchHistory_.push_back(
        {sketch, currentSketchSupport_, modelSketchId});
    ++sketchCount_;
  }
  editingSketchIndex_.reset();
  historyPosition_ = static_cast<int>(sketchHistory_.size()) +
                     (hasExtrusion_ ? 1 : 0);
  rebuildFeatureTree();
  rebuildHistoryPanel();
  workspaceStack_->setCurrentWidget(viewport_);
  statusBar()->showMessage(
      QString::fromUtf8("Эскиз завершён — модель перестроена"), 3000);
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
  if (profile && targetShape)
    operation = detectExtrudeOperation(
        *profile, extrusionLengthSpin_->value(),
        extrusionReverseCheck_->isChecked(), targetShape.get(),
        profile->support.type == SketchSupportType::Face);
  const QSignalBlocker blocker(extrusionOperationCombo_);
  extrusionOperationCombo_->setCurrentIndex(static_cast<int>(operation));
}

void MainWindow::extrudeSketch() {
  const bool fromBodyFace = selectedExtrusionSurface_.startsWith(
      QString::fromUtf8("Грань тела"));
  const auto& pickedSketch = viewport_->extrusionCandidateSketch();
  // Keep an owned snapshot. Rebuild/viewport refresh mutates several Sketch
  // caches and must not invalidate the profile while the operation is using it.
  const sketch::Sketch sketch =
      fromBodyFace
          ? viewport_->solidSketch()
          : (pickedSketch.lines().empty() && pickedSketch.circles().empty()
                 ? sketchCanvas_->sketch()
                 : pickedSketch);
  if (!fromBodyFace && sketch.lines().empty() && sketch.circles().empty()) {
    QMessageBox::information(this, QString::fromUtf8("Выдавливание"),
                             QString::fromUtf8("Сначала создайте замкнутый контур эскиза."));
    return;
  }
  if (!fromBodyFace && !sketch.lines().empty() && !sketch.isClosed()) {
    QMessageBox::warning(this, QString::fromUtf8("Контур не замкнут"),
                         QString::fromUtf8("Соедините конечные точки линий замкнутого контура."));
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
  if (!modelSketch) {
    modelSketch = &document_.addSketch(
        kInvalidSketchId,
        "Extrude profile " + std::to_string(document_.sketches().size() + 1),
        sketch);
    modelSketch->placement = currentSketchPlacement_;
  } else {
    modelSketch->geometry = sketch;
  }
  Body* modelBody = operation == ExtrudeOperation::NewBody
                        ? &document_.addBody()
                        : document_.activeBody();
  modelBody->addFeature(std::make_unique<ExtrudeFeature>(
      modelSketch->id, height,
      "Extrude " + std::to_string(modelBody->features().size() + 1),
      operation, reversed));
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
    historyPosition_ = static_cast<int>(sketchHistory_.size()) +
                       (hasExtrusion_ ? 1 : 0);
    rebuildFeatureTree();
    rebuildHistoryPanel();
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
  historyPosition_ = static_cast<int>(sketchHistory_.size()) + 1;
  rebuildFeatureTree();
  rebuildHistoryPanel();
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
  revolveAxisCombo_->clear();
  revolveAxisCombo_->addItem(QString::fromUtf8("Выбрать ось"), 0);
  revolveAngleSpin_->setValue(360.0);
  revolveOperationCombo_->setCurrentIndex(0);
  revolveToolSession_.setOperation(ExtrudeOperation::NewBody);
  revolveReverseCheck_->setChecked(false);
  revolveDock_->show(); revolveDock_->raise();
  updateRevolveToolPreview();
  statusBar()->showMessage(
      QString::fromUtf8("Выберите профиль и ось в правой панели"));
}

void MainWindow::rebuildRevolveAxisChoices() {
  const auto sketchId = static_cast<SketchId>(
      revolveProfileCombo_->currentData().toULongLong());
  const QSignalBlocker blocker(revolveAxisCombo_);
  revolveAxisCombo_->clear();
  revolveAxisCombo_->addItem(QString::fromUtf8("Выбрать ось"), 0);
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
  if (valid)
    viewport_->setToolPreviewShape(revolveToolSession_.bodyId(),
        revolveToolSession_.sourceFeatureId(), revolveToolSession_.previewShape());
  else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = revolveToolSession_.manipulator())
    viewport_->setAngularToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (revolveToolSession_.lifecycle() == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(QString::fromStdString(revolveToolSession_.error()));
}

void MainWindow::cancelRevolveTool() {
  revolveToolSession_.cancel();
  viewport_->clearToolPreviewShape(); viewport_->clearToolManipulator();
  revolveDock_->hide(); modelRibbon_->clearActiveTool();
  refreshBodyViewFromDocument();
  statusBar()->showMessage(QString::fromUtf8("Инструмент вращения отменён"), 2000);
}

void MainWindow::acceptRevolveTool() {
  if (revolveToolSession_.lifecycle() != ToolLifecycle::PreviewValid ||
      !revolveToolSession_.axis()) return;
  const Document previous = document_;
  Body* body = revolveToolSession_.operation() == ExtrudeOperation::NewBody
                   ? &document_.addBody() : document_.activeBody();
  if (!body) return;
  body->addFeature(std::make_unique<RevolveFeature>(
      revolveToolSession_.profileSketchId(), *revolveToolSession_.axis(),
      revolveToolSession_.angleDeg(),
      "Revolve " + std::to_string(body->features().size() + 1),
      revolveToolSession_.operation(), revolveToolSession_.reversed()));
  if (!document_.recompute()) {
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previous; refreshBodyViewFromDocument();
    statusBar()->showMessage(error); return;
  }
  revolveToolSession_.cancel(); viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator(); revolveDock_->hide();
  pushUndoAction([this, previous] { document_ = previous; refreshBodyViewFromDocument();
                                    rebuildFeatureTree(); rebuildHistoryPanel(); });
  refreshBodyViewFromDocument(); rebuildFeatureTree(); rebuildHistoryPanel();
  modelRibbon_->clearActiveTool();
}

void MainWindow::createPocket() {
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
                           body->resultShape(), edges, 5.0);
  toolParametersPanel_->setParameterValue(5.0);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFilletToolPreview();
  if (edges.empty())
    statusBar()->showMessage(
        QString::fromUtf8("Нажмите «Выбрать» и укажите рёбра в viewport"));
}

void MainWindow::updateFilletToolPreview() {
  const auto state = filletToolSession_.lifecycle();
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(filletToolSession_.edges().size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  toolParametersPanel_->setStatus(
      valid ? QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::Editing
                  ? QString::fromUtf8("Выберите рёбра для скругления")
                  : QString::fromStdString(filletToolSession_.error()),
      state == ToolLifecycle::PreviewInvalid);
  if (valid)
    viewport_->setToolPreviewShape(filletToolSession_.bodyId(),
                                   filletToolSession_.sourceFeatureId(),
                                   filletToolSession_.previewShape());
  else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = filletToolSession_.manipulator())
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
}

void MainWindow::cancelFilletTool() {
  if (filletToolSession_.lifecycle() == ToolLifecycle::Inactive) return;
  filletToolSession_.cancel();
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
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
    const QString error = QString::fromStdString(document_.rebuildError());
    document_ = previousDocument;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  const double radius = filletToolSession_.radiusMm();
  filletToolSession_.cancel();
  viewport_->clearToolPreviewShape();
  viewport_->clearToolManipulator();
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
  while (QLayoutItem* item = historyLayout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  auto addStep = [this](const QIcon& icon, const QString& title,
                        const QString& tooltip, auto action) {
    auto* button = new QToolButton(historyContent_);
    button->setIcon(icon);
    button->setIconSize(QSize(6, 6));
    button->setText(title);
    button->setToolTip(tooltip);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setFixedSize(74, 28);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(
        "QToolButton{background:#fff;border:1px solid #cfdaea;border-radius:8px;"
        "color:#173f7d;padding:2px;font-size:11px;}"
        "QToolButton:hover{background:#e8f2ff;border:2px solid #6fa8f7;}"
        "QToolButton:pressed{background:#d7e9ff;}");
    connect(button, &QToolButton::clicked, this, action);
    historyLayout_->addWidget(button);
  };
  for (std::size_t index = 0; index < sketchHistory_.size(); ++index) {
    addStep(QIcon(QStringLiteral(":/icons/create-sketch.png")),
            QString::fromUtf8("Эскиз %1").arg(index + 1),
            QString::fromUtf8("Вернуться к эскизу и изменить его"),
            [this, index] { editSketchStep(index); });
  }
  if (hasExtrusion_) {
    addStep(QIcon(QStringLiteral(":/icons/extrude.png")),
            QString::fromUtf8("Выдавливание"),
            QString::fromUtf8("Изменить глубину выдавливания"),
            [this] { editExtrusionStep(); });
  }
  if (const Body* body = document_.activeBody();
      body && dynamic_cast<const PocketFeature*>(body->activeFeature())) {
    addStep(QIcon(QStringLiteral(":/icons/extrude.png")),
            QString::fromUtf8("Карман"),
            QString::fromUtf8("Изменить глубину кармана"),
            [this] { editPocketStep(); });
  }
  if (const Body* body = document_.activeBody();
      body && dynamic_cast<const FilletFeature*>(body->activeFeature())) {
    addStep(QIcon(), QString::fromUtf8("Скругление"),
            QString::fromUtf8("Изменить радиус скругления"),
            [this] { editFilletStep(); });
  }
  if (historySlider_) {
    const QSignalBlocker blocker(historySlider_);
    const int lastPosition = static_cast<int>(sketchHistory_.size()) +
                             (hasExtrusion_ ? 1 : 0);
    historySlider_->setRange(0, lastPosition);
    historySlider_->setFixedWidth(std::max(40, lastPosition * 82));
    historyPosition_ = std::clamp(historyPosition_, 0, lastPosition);
    historySlider_->setValue(historyPosition_);
  }
}

void MainWindow::applyHistoryPosition(int position) {
  const int lastPosition = static_cast<int>(sketchHistory_.size()) +
                           (hasExtrusion_ ? 1 : 0);
  historyPosition_ = std::clamp(position, 0, lastPosition);
  const std::size_t activeSketches = std::min<std::size_t>(
      static_cast<std::size_t>(historyPosition_), sketchHistory_.size());
  const bool solidActive = hasExtrusion_ &&
                           historyPosition_ > static_cast<int>(sketchHistory_.size());
  viewport_->setSolidVisible(solidActive);
  for (std::size_t index = 0; index < sketchHistory_.size(); ++index) {
    const bool consumedBySolid = solidActive && extrusionSourceSketch_ == index;
    viewport_->setSketchVisible(index, index < activeSketches && !consumedBySolid);
  }
  rebuildFeatureTree();
  statusBar()->showMessage(
      historyPosition_ == lastPosition
          ? QString::fromUtf8("Активно последнее состояние модели")
          : QString::fromUtf8("История откачена до шага %1").arg(historyPosition_),
      2500);
}

bool MainWindow::configureSketchEditContext() {
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
    QMessageBox::warning(this, QString::fromUtf8("Sketch on Face"),
                         QStringLiteral("Sketch support face could not be resolved"));
    return false;
  }
  const auto resolved = resolveFacePlacement(*shape, reference.topology());
  if (!resolved.planar) {
    QMessageBox::information(
        this, QString::fromUtf8("Sketch on Face"),
        QStringLiteral("Sketch on curved surfaces is not supported yet"));
    return false;
  }
  currentSketchPlacement_ = resolved.placement;
  sketchCanvas_->setSketchEditContext(
      {kInvalidSketchId, currentSketchPlacement_, shape, reference});
  return true;
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
  sketchCanvas_->setReferenceBody(document_.box(), currentSketchSupport_,
                                  hasExtrusion_);
  workspaceStack_->setCurrentWidget(sketchCanvas_);
  ribbonStack_->setCurrentWidget(sketchRibbon_);
  statusBar()->showMessage(
      QString::fromUtf8("Редактируется эскиз %1 • Завершите эскиз для перестроения")
          .arg(index + 1));
}

void MainWindow::editExtrusionStep() {
  if (!hasExtrusion_) return;
  Body* body = document_.activeBody();
  ExtrudeFeature* extrude = nullptr;
  std::size_t extrudeIndex = 0;
  if (body)
    for (std::size_t index = body->features().size(); index-- > 0;)
      if (auto* candidate =
              dynamic_cast<ExtrudeFeature*>(body->features()[index].get())) {
        extrude = candidate;
        extrudeIndex = index;
        break;
      }
  if (!extrude) return;
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
  statusBar()->showMessage(
      QString::fromUtf8("Выдавливание изменено: %1 мм").arg(height), 3000);
}

void MainWindow::editPocketStep() {
  Body* body = document_.activeBody();
  auto* pocket = body
                     ? dynamic_cast<PocketFeature*>(body->activeFeature())
                     : nullptr;
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

void MainWindow::editFilletStep() {
  Body* body = document_.activeBody();
  auto* fillet = body
                     ? dynamic_cast<FilletFeature*>(body->activeFeature())
                     : nullptr;
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
  filletToolSession_.begin(body->id(), fillet->edge().featureId, upstream,
                           fillet->edges(), fillet->radiusMm(), fillet->id());
  toolParametersPanel_->setParameterValue(fillet->radiusMm());
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFilletToolPreview();
  viewport_->setSelectedBodyEdges(fillet->edges());
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

  auto* drawings = new QTreeWidgetItem(project, {QString::fromUtf8("Чертежи")});
  const std::size_t activeSketchCount = std::min<std::size_t>(
      static_cast<std::size_t>(std::max(historyPosition_, 0)), sketchCount_);
  const bool activeExtrusion = hasExtrusion_ &&
      historyPosition_ > static_cast<int>(sketchHistory_.size());
  if (activeSketchCount == 0) {
    new QTreeWidgetItem(drawings, {QString::fromUtf8("Эскизов нет")});
  } else {
    for (std::size_t index = 0; index < activeSketchCount; ++index) {
      auto* sketchItem = new QTreeWidgetItem(
          drawings, {QString::fromUtf8("⌞  Эскиз %1").arg(index + 1)});
      sketchItem->setData(0, Qt::UserRole, 20 + static_cast<int>(index));
      sketchItem->setFlags(sketchItem->flags() | Qt::ItemIsUserCheckable);
      const bool visible = !activeExtrusion || !extrusionSourceSketch_.has_value() ||
                           index != *extrusionSourceSketch_;
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
      bodyItem->setFlags(bodyItem->flags() | Qt::ItemIsUserCheckable);
      bodyItem->setCheckState(0, Qt::Checked);
      for (const auto& feature : body.features())
        new QTreeWidgetItem(
            bodyItem,
            {QString::fromStdString(feature->name().empty()
                                        ? feature->typeName()
                                        : feature->name())});
    }
  }
  auto* components = new QTreeWidgetItem(project, {QString::fromUtf8("Компоненты")});
  new QTreeWidgetItem(components, {QString::fromUtf8("Компоненты отсутствуют")});
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
                                  sketchCanvas_->sketch());
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
                                  sketchCanvas_->sketch());
}

}  // namespace solidar

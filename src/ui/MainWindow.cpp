#include "ui/MainWindow.h"

#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QAction>
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

#include "drawing/EskdRenderer.h"
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
  connect(undoAction_, &QAction::triggered, sketchCanvas_, &SketchCanvas::undo);
  connect(sketchCanvas_, &SketchCanvas::undoAvailable, undoAction_,
          &QAction::setEnabled);
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
  viewport_->resetScene();
  viewport_->setBox(document_.box());
  rebuildFeatureTree();
  workspaceStack_->setCurrentWidget(viewport_);
  statusBar()->showMessage(QString::fromUtf8("Создан новый проект"), 3000);
}

void MainWindow::openProject() {
  const QString path = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Открыть проект"), QString(),
      QString::fromUtf8("Проекты Солидарность CAD (*.solidar)"));
  if (path.isEmpty()) return;
  QString error;
  if (!project::ProjectFile::validate(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка открытия"), error);
    return;
  }
  setProjectPath(path);
  statusBar()->showMessage(QString::fromUtf8("Проект открыт"), 3000);
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
  if (!project::ProjectFile::create(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка сохранения"), error);
    return;
  }
  setProjectPath(path);
  statusBar()->showMessage(QString::fromUtf8("Проект сохранён"), 3000);
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
  viewport_->setSketch(sketchCanvas_->sketch());
  drawingSheet_->setRectangle(document_.box().widthMm, document_.box().depthMm);
  workspaceStack_->addWidget(viewport_);
  workspaceStack_->addWidget(sketchCanvas_);
  setCentralWidget(workspaceStack_);

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
            statusBar()->showMessage(
                QString::fromUtf8("Выберите замкнутый контур или грань тела"));
            viewport_->beginExtrusionSurfaceSelection();
          });
  connect(viewport_, &Viewport::sketchPlanePicked, this,
          [this](const QString& plane) {
            modelRibbon_->clearActiveTool();
            currentSketchSupport_ = plane;
            sketchCanvas_->resetSketch();
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
            workspaceStack_->setCurrentWidget(sketchCanvas_);
            ribbonStack_->setCurrentWidget(sketchRibbon_);
            statusBar()->showMessage(QString::fromUtf8("Рабочая плоскость: ") + plane);
          });
  connect(viewport_, &Viewport::extrusionSurfacePicked, this,
          [this](const QString& surface) {
            modelRibbon_->clearActiveTool();
            selectedExtrusionSurface_ = surface;
            statusBar()->showMessage(QString::fromUtf8("Поверхность: ") + surface);
            extrudeSketch();
          });
  connect(sketchRibbon_, &SketchRibbon::finishRequested, this,
          &MainWindow::finishSketch);
  connect(sketchCanvas_, &SketchCanvas::geometryChanged, this,
          &MainWindow::updateFromSketch);
  connect(workspaceStack_, &QStackedWidget::currentChanged, this, [this](int index) {
    ribbonStack_->setCurrentWidget(workspaceStack_->widget(index) == viewport_
                                       ? static_cast<QWidget*>(modelRibbon_)
                                       : static_cast<QWidget*>(sketchRibbon_));
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
  viewport_->setSketch(sketchCanvas_->sketch());
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
  viewport_->addSketch(sketch, currentSketchSupport_);
  ++sketchCount_;
  rebuildFeatureTree();
  workspaceStack_->setCurrentWidget(viewport_);
  statusBar()->showMessage(
      QString::fromUtf8("Эскиз завершён — модель перестроена"), 3000);
}

void MainWindow::extrudeSketch() {
  const auto& pickedSketch = viewport_->extrusionCandidateSketch();
  const auto& sketch = (pickedSketch.lines().empty() && pickedSketch.circles().empty())
                           ? sketchCanvas_->sketch()
                           : pickedSketch;
  const bool fromBodyFace = selectedExtrusionSurface_.startsWith(
      QString::fromUtf8("Грань тела"));
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

  bool accepted = false;
  const double height = QInputDialog::getDouble(
      this, QString::fromUtf8("Выдавливание"),
      QString::fromUtf8("Расстояние, мм:"), document_.box().heightMm,
      0.01, 100000.0, 2, &accepted);
  if (!accepted) return;

  document_.setBox({fromBodyFace ? document_.box().widthMm : sketch.widthMm(),
                    fromBodyFace ? document_.box().depthMm : sketch.heightMm(),
                    height});
  viewport_->setSketch(sketch);
  viewport_->setSolidSketch(sketch);
  const QString pickedSupport = viewport_->extrusionCandidateSupport();
  viewport_->setSolidSupport(fromBodyFace ? selectedExtrusionSurface_
                                          : pickedSupport.isEmpty()
                                                ? currentSketchSupport_
                                                : pickedSupport);
  viewport_->setBox(document_.box());
  viewport_->setSolidVisible(true);
  hasExtrusion_ = true;
  const std::size_t pickedIndex = viewport_->extrusionCandidateSketchIndex();
  extrusionSourceSketch_ = pickedIndex != static_cast<std::size_t>(-1)
                               ? std::optional<std::size_t>(pickedIndex)
                               : sketchCount_ > 0
                                     ? std::optional<std::size_t>(sketchCount_ - 1)
                                     : std::nullopt;
  selectedExtrusionSurface_.clear();
  rebuildFeatureTree();
  workspaceStack_->setCurrentWidget(viewport_);
  statusBar()->showMessage(
      QString::fromUtf8("Создано твёрдое тело: выдавливание %1 мм").arg(height), 4000);
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
  if (sketchCount_ == 0) {
    new QTreeWidgetItem(drawings, {QString::fromUtf8("Эскизов нет")});
  } else {
    for (std::size_t index = 0; index < sketchCount_; ++index) {
      auto* sketchItem = new QTreeWidgetItem(
          drawings, {QString::fromUtf8("⌞  Эскиз %1").arg(index + 1)});
      sketchItem->setData(0, Qt::UserRole, 20 + static_cast<int>(index));
      sketchItem->setFlags(sketchItem->flags() | Qt::ItemIsUserCheckable);
      const bool visible = !extrusionSourceSketch_.has_value() ||
                           index != *extrusionSourceSketch_;
      sketchItem->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      viewport_->setSketchVisible(index, visible);
    }
  }
  auto* models = new QTreeWidgetItem(project, {QString::fromUtf8("Модели")});
  auto* bodyItem = new QTreeWidgetItem(
      models, {hasExtrusion_ ? QString::fromUtf8("▣  Выдавливание 1")
                             : QString::fromUtf8("Твёрдых тел нет")});
  if (hasExtrusion_) {
    bodyItem->setData(0, Qt::UserRole, 3);
    bodyItem->setFlags(bodyItem->flags() | Qt::ItemIsUserCheckable);
    bodyItem->setCheckState(0, Qt::Checked);
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

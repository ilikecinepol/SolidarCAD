#include "ui/ModelRibbon.h"
#include "ui/PartDesignToolHelp.h"

#include <QFrame>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

namespace solidar {
namespace {

constexpr auto kSourceCommandProperty = "sourceCommandObjectName";

QString compactCommandTitle(QString title) {
  // QToolButton does not word-wrap ToolButtonTextUnderIcon labels.  At the
  // 125–150% scaling commonly used on Windows, long Russian captions used to
  // paint hard against their neighbours and the checked frame appeared to
  // cover the next command.  An explicit line break keeps every caption
  // inside the same predictable command tile.
  if (title.size() > 11) {
    const qsizetype separator = title.indexOf(QLatin1Char(' '));
    if (separator > 0) title[separator] = QLatin1Char('\n');
  }
  return title;
}

QToolButton* commandButton(const QString& commandId, QWidget* parent) {
  auto* button = new QToolButton(parent);
  const auto* help = modelCommandHelp(commandId);
  Q_ASSERT(help);
  button->setText(compactCommandTitle(help->title));
  button->setProperty("commandTitle", help->title);
  button->setToolTip(help->detailedDescription);
  button->setAccessibleName(help->title);
  button->setAccessibleDescription(help->detailedDescription);
  button->setProperty("helpId", commandId);
  button->setProperty("uiRole", "modelCommand");
  button->setIcon(modelCommandIcon(commandId));
  button->setIconSize(QSize(42, 42));
  button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  button->setObjectName("modelCommand");
  button->setFixedWidth(104);
  button->setMinimumHeight(78);
  button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  button->setCursor(Qt::PointingHandCursor);
  button->setFocusPolicy(Qt::NoFocus);
  return button;
}

QWidget* group(const QString& title, QLayout* commands, QWidget* parent,
               const QList<QToolButton*>& menuButtons) {
  auto* widget = new QWidget(parent);
  widget->setObjectName("modelToolGroup");
  widget->setProperty("groupTitle", title);
  auto* layout = new QVBoxLayout(widget);
  layout->setContentsMargins(0, 3, 0, 2);
  layout->setSpacing(2);
  layout->addLayout(commands);
  auto* caption = new QToolButton(widget);
  caption->setObjectName("modelGroupMenuButton");
  caption->setText(title + QStringLiteral("  ▾"));
  caption->setPopupMode(QToolButton::InstantPopup);
  caption->setCursor(Qt::PointingHandCursor);
  caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* menu = new QMenu(caption);
  menu->setObjectName("modelGroupMenu");
  for (auto* button : menuButtons) {
    const QString commandTitle =
        button->property("commandTitle").toString().isEmpty()
            ? button->text()
            : button->property("commandTitle").toString();
    if (button->menu()) {
      button->menu()->setTitle(commandTitle);
      auto* action = menu->addMenu(button->menu());
      action->setProperty(kSourceCommandProperty, button->objectName());
      continue;
    }
    auto* action = menu->addAction(button->icon(), commandTitle);
    action->setObjectName(button->objectName() + QStringLiteral("Action"));
    action->setProperty(kSourceCommandProperty, button->objectName());
    action->setEnabled(button->isEnabled());
    QObject::connect(action, &QAction::triggered, button,
                     [button] { button->click(); });
  }
  caption->setMenu(menu);
  layout->addWidget(caption, 0, Qt::AlignCenter);
  return widget;
}

QFrame* separator(QWidget* parent) {
  auto* line = new QFrame(parent);
  line->setFrameShape(QFrame::VLine);
  line->setObjectName("modelSeparator");
  return line;
}

}  // namespace

ModelRibbon::ModelRibbon(QWidget* parent) : QWidget(parent) {
  setObjectName("modelRibbon");
  setFixedHeight(124);
  auto* root = new QHBoxLayout(this);
  root->setContentsMargins(4, 6, 4, 6);
  root->setSpacing(0);

  auto* createSketch = commandButton(QStringLiteral("createSketch"), this);
  auto* referenceImage =
      commandButton(QStringLiteral("referenceImage"), this);
  auto* extrude = commandButton(QStringLiteral("extrude"), this);
  auto* pocket = commandButton(QStringLiteral("pocket"), this);
  auto* revolve = commandButton(QStringLiteral("revolve"), this);
  createSketch->setObjectName("createSketchCommand");
  referenceImage->setObjectName("referenceImageCommand");
  extrude->setObjectName("extrudeCommand");
  pocket->setObjectName("pocketCommand");
  revolve->setObjectName("revolveCommand");
  createSketch->setCheckable(true);
  referenceImage->setCheckable(true);
  extrude->setCheckable(true);
  pocket->setCheckable(true);
  revolve->setCheckable(true);
  toolGroup_ = new QButtonGroup(this);
  toolGroup_->setExclusive(true);
  toolGroup_->addButton(createSketch);
  toolGroup_->addButton(referenceImage);
  toolGroup_->addButton(extrude);
  toolGroup_->addButton(pocket);
  toolGroup_->addButton(revolve);
  auto* creation = new QHBoxLayout;
  creation->setSpacing(0);
  creation->addWidget(createSketch);
  creation->addWidget(extrude);
  creation->addWidget(pocket);
  creation->addWidget(revolve);
  root->addWidget(group(QString::fromUtf8("СОЗДАНИЕ"), creation, this,
                        {createSketch, extrude, pocket, revolve}));
  root->addWidget(separator(this));

  auto* fillet = commandButton(QStringLiteral("fillet"), this);
  fillet->setObjectName("filletCommand");
  fillet->setCheckable(true);
  toolGroup_->addButton(fillet);
  auto* chamfer = commandButton(QStringLiteral("chamfer"), this);
  chamfer->setObjectName("chamferCommand");
  chamfer->setCheckable(true);
  toolGroup_->addButton(chamfer);
  auto* move = commandButton(QStringLiteral("move"), this);
  move->setObjectName("moveCommand");
  move->setCheckable(true);
  toolGroup_->addButton(move);
  auto* joinBodies = commandButton(QStringLiteral("joinBodies"), this);
  joinBodies->setObjectName("joinBodiesCommand");
  joinBodies->setCheckable(true);
  toolGroup_->addButton(joinBodies);
  auto* shell = commandButton(QStringLiteral("shell"), this);
  auto* draft = commandButton(QStringLiteral("draft"), this);
  shell->setObjectName("shellCommand");
  draft->setObjectName("draftCommand");
  shell->setCheckable(true);
  draft->setCheckable(true);
  toolGroup_->addButton(shell);
  toolGroup_->addButton(draft);
  auto* mirror = commandButton(QStringLiteral("mirror"), this);
  auto* linearPattern =
      commandButton(QStringLiteral("linearPattern"), this);
  auto* circularPattern =
      commandButton(QStringLiteral("circularPattern"), this);
  mirror->setObjectName("mirrorCommand");
  linearPattern->setObjectName("linearPatternCommand");
  circularPattern->setObjectName("circularPatternCommand");
  mirror->setCheckable(true);
  linearPattern->setCheckable(true);
  circularPattern->setCheckable(true);
  toolGroup_->addButton(mirror);
  toolGroup_->addButton(linearPattern);
  toolGroup_->addButton(circularPattern);
  auto* editing = new QHBoxLayout;
  editing->setSpacing(0);
  editing->addWidget(fillet);
  editing->addWidget(chamfer);
  editing->addWidget(joinBodies);
  editing->addWidget(move);
  editing->addWidget(shell);
  editing->addWidget(draft);
  editing->addWidget(mirror);
  editing->addWidget(linearPattern);
  editing->addWidget(circularPattern);
  root->addWidget(group(QString::fromUtf8("РЕДАКТИРОВАНИЕ"), editing, this,
                        {fillet, chamfer, joinBodies, move, shell, draft, mirror, linearPattern,
                         circularPattern}));
  root->addWidget(separator(this));

  createComponentFromBodyButton_ = new QToolButton(this);
  createComponentFromBodyButton_->setObjectName(
      QStringLiteral("createComponentFromBodyCommand"));
  createComponentFromBodyButton_->setText(
      QString::fromUtf8("Создать компонент\nиз тела"));
  createComponentFromBodyButton_->setProperty(
      "commandTitle", QString::fromUtf8("Создать компонент из тела"));
  createComponentFromBodyButton_->setToolTip(QString::fromUtf8(
      "Перенести выбранное тело в новый вложенный компонент"));
  createComponentFromBodyButton_->setAccessibleName(
      QString::fromUtf8("Создать компонент из тела"));
  createComponentFromBodyButton_->setProperty("uiRole", "modelCommand");
  createComponentFromBodyButton_->setIcon(
      modelCommandIcon(QStringLiteral("joinBodies")));
  createComponentFromBodyButton_->setIconSize(QSize(42, 42));
  createComponentFromBodyButton_->setToolButtonStyle(
      Qt::ToolButtonTextUnderIcon);
  createComponentFromBodyButton_->setFixedWidth(104);
  createComponentFromBodyButton_->setMinimumHeight(78);
  createComponentFromBodyButton_->setSizePolicy(QSizePolicy::Fixed,
                                                 QSizePolicy::Preferred);
  createComponentFromBodyButton_->setCursor(Qt::PointingHandCursor);
  createComponentFromBodyButton_->setFocusPolicy(Qt::NoFocus);
  createComponentFromBodyButton_->setCheckable(false);
  createComponentFromBodyButton_->setEnabled(false);
  const auto homePlacementButton = [this](const QString& objectName,
                                          const QString& text,
                                          const QString& title,
                                          const QString& tooltip,
                                          const QString& iconId) {
    auto* button = new QToolButton(this);
    button->setObjectName(objectName);
    button->setText(text);
    button->setProperty("commandTitle", title);
    button->setToolTip(tooltip);
    button->setAccessibleName(title);
    button->setAccessibleDescription(tooltip);
    button->setProperty("uiRole", "modelCommand");
    button->setIcon(modelCommandIcon(iconId));
    button->setIconSize(QSize(42, 42));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setFixedWidth(104);
    button->setMinimumHeight(78);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCheckable(false);
    button->setEnabled(false);
    return button;
  };
  setComponentHomePlacementButton_ = homePlacementButton(
      QStringLiteral("componentSetHomePlacementCommand"),
      QString::fromUtf8("Закрепить\nположение"),
      QString::fromUtf8("Закрепить положение"),
      QString::fromUtf8("Сохранить текущее положение как исходное. "
                        "Компонент останется перемещаемым."),
      QStringLiteral("move"));
  restoreComponentHomePlacementButton_ = homePlacementButton(
      QStringLiteral("componentRestoreHomePlacementCommand"),
      QString::fromUtf8("Вернуть исходное\nположение"),
      QString::fromUtf8("Вернуть исходное положение"),
      QString::fromUtf8(
          "Вернуть все компоненты в сохранённые исходные положения."),
      QStringLiteral("joinBodies"));
  auto* assemblyCommands = new QHBoxLayout;
  assemblyCommands->setSpacing(5);
  assemblyCommands->addWidget(createComponentFromBodyButton_);
  assemblyCommands->addWidget(setComponentHomePlacementButton_);
  assemblyCommands->addWidget(restoreComponentHomePlacementButton_);
  root->addWidget(group(QString::fromUtf8("СБОРКИ"), assemblyCommands, this,
                        {createComponentFromBodyButton_,
                         setComponentHomePlacementButton_,
                         restoreComponentHomePlacementButton_}));
  root->addWidget(separator(this));

  auto* views = new QHBoxLayout;
  views->setSpacing(3);
  rulerButton_ = commandButton(QStringLiteral("ruler"), this);
  rulerButton_->setObjectName("rulerCommand");
  rulerButton_->setCheckable(true);
  auto* displayMode = new QToolButton(this);
  displayMode->setObjectName("displayModeCommand");
  displayMode->setText(QString::fromUtf8("Затенённый\nс рёбрами"));
  displayMode->setToolTip(QString::fromUtf8("Режим отображения тела"));
  displayMode->setPopupMode(QToolButton::InstantPopup);
  displayMode->setToolButtonStyle(Qt::ToolButtonTextOnly);
  displayMode->setMinimumSize(104, 56);
  auto* displayMenu = new QMenu(displayMode);
  const auto addDisplayMode = [&](const QString& text, int mode) {
    auto* action = displayMenu->addAction(text);
    connect(action, &QAction::triggered, this,
            [this, displayMode, text, mode] {
              displayMode->setText(text);
              emit displayModeRequested(mode);
            });
  };
  addDisplayMode(QString::fromUtf8("Затенённый"), 0);
  addDisplayMode(QString::fromUtf8("Затенённый с рёбрами"), 1);
  addDisplayMode(QString::fromUtf8("Каркас"), 2);
  displayMode->setMenu(displayMenu);

  meshQualityButton_ = new QToolButton(this);
  meshQualityButton_->setObjectName("meshQualityCommand");
  meshQualityButton_->setText(QString::fromUtf8("Качество:\nОбычное"));
  meshQualityButton_->setToolTip(
      QString::fromUtf8("Качество сетки отображения"));
  meshQualityButton_->setPopupMode(QToolButton::InstantPopup);
  meshQualityButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  meshQualityButton_->setMinimumSize(92, 56);
  auto* qualityMenu = new QMenu(meshQualityButton_);
  auto* normalQuality = qualityMenu->addAction(QString::fromUtf8("Обычное"));
  auto* highQuality = qualityMenu->addAction(QString::fromUtf8("Высокое"));
  connect(normalQuality, &QAction::triggered, this, [this] {
    meshQualityButton_->setText(QString::fromUtf8("Качество:\nОбычное"));
    emit meshQualityRequested(0);
  });
  connect(highQuality, &QAction::triggered, this, [this] {
    meshQualityButton_->setText(QString::fromUtf8("Качество:\nВысокое"));
    emit meshQualityRequested(1);
  });
  meshQualityButton_->setMenu(qualityMenu);
  views->addWidget(rulerButton_);
  views->addWidget(referenceImage);
  views->addWidget(displayMode);
  views->addWidget(meshQualityButton_);
  root->addWidget(group(QString::fromUtf8("ОТОБРАЖЕНИЕ"), views, this,
                        {rulerButton_, referenceImage, displayMode,
                         meshQualityButton_}), 1);

  connect(createSketch, &QToolButton::clicked, this,
          &ModelRibbon::createSketchRequested);
  connect(referenceImage, &QToolButton::clicked, this,
          &ModelRibbon::referenceImageRequested);
  connect(extrude, &QToolButton::clicked, this,
          &ModelRibbon::extrudeRequested);
  connect(pocket, &QToolButton::clicked, this,
          &ModelRibbon::pocketRequested);
  connect(revolve, &QToolButton::clicked, this,
          &ModelRibbon::revolveRequested);
  connect(fillet, &QToolButton::clicked, this,
          &ModelRibbon::filletRequested);
  connect(chamfer, &QToolButton::clicked, this,
          &ModelRibbon::chamferRequested);
  connect(joinBodies, &QToolButton::clicked, this,
          &ModelRibbon::joinBodiesRequested);
  connect(move, &QToolButton::clicked, this, &ModelRibbon::moveRequested);
  connect(shell, &QToolButton::clicked, this, &ModelRibbon::shellRequested);
  connect(draft, &QToolButton::clicked, this, &ModelRibbon::draftRequested);
  connect(mirror, &QToolButton::clicked, this, &ModelRibbon::mirrorRequested);
  connect(linearPattern, &QToolButton::clicked, this,
          &ModelRibbon::linearPatternRequested);
  connect(circularPattern, &QToolButton::clicked, this,
          &ModelRibbon::circularPatternRequested);
  connect(createComponentFromBodyButton_, &QToolButton::clicked, this,
          &ModelRibbon::createComponentFromBodyRequested);
  connect(setComponentHomePlacementButton_, &QToolButton::clicked, this,
          &ModelRibbon::setComponentHomePlacementRequested);
  connect(restoreComponentHomePlacementButton_, &QToolButton::clicked, this,
          &ModelRibbon::restoreComponentHomePlacementRequested);
  setComponentEditing(false);
  connect(rulerButton_, &QToolButton::toggled, this,
          &ModelRibbon::rulerToggled);
}

void ModelRibbon::clearActiveTool() {
  toolGroup_->setExclusive(false);
  for (auto* button : toolGroup_->buttons()) button->setChecked(false);
  toolGroup_->setExclusive(true);
}

void ModelRibbon::setRulerActive(bool active) {
  const QSignalBlocker blocker(rulerButton_);
  rulerButton_->setChecked(active);
}

void ModelRibbon::setAuthoringEnabled(bool enabled, bool moveOnly) {
  for (auto* button : toolGroup_->buttons()) {
    const bool commandEnabled =
        enabled || (moveOnly && button->objectName() ==
                                    QStringLiteral("moveCommand"));
    button->setEnabled(commandEnabled);
    for (auto* action : findChildren<QAction*>())
      if (action->property(kSourceCommandProperty).toString() ==
          button->objectName())
        action->setEnabled(commandEnabled);
  }
}

void ModelRibbon::setMeshQualityControlEnabled(bool enabled) {
  meshQualityButton_->setEnabled(enabled);
  for (auto* action : meshQualityButton_->menu()->actions())
    action->setEnabled(enabled);
  for (auto* action : findChildren<QAction*>())
    if (action->property(kSourceCommandProperty).toString() ==
        meshQualityButton_->objectName())
      action->setEnabled(enabled);
}

void ModelRibbon::setCreateComponentFromBodyEnabled(bool enabled) {
  createComponentFromBodyButton_->setEnabled(enabled);
  for (auto* action : findChildren<QAction*>())
    if (action->property(kSourceCommandProperty).toString() ==
        createComponentFromBodyButton_->objectName())
      action->setEnabled(enabled);
}

void ModelRibbon::setComponentHomePlacementEnabled(bool adoptEnabled,
                                                   bool restoreEnabled) {
  const auto update = [this](QToolButton* button, bool enabled) {
    button->setEnabled(enabled);
    for (auto* action : findChildren<QAction*>())
      if (action->property(kSourceCommandProperty).toString() ==
          button->objectName())
        action->setEnabled(enabled);
  };
  update(setComponentHomePlacementButton_, adoptEnabled);
  update(restoreComponentHomePlacementButton_, restoreEnabled);
}

void ModelRibbon::setComponentEditing(bool active, bool rootComponent) {
  static_cast<void>(active);
  static_cast<void>(rootComponent);
  const bool componentExtractionVisible = true;
  createComponentFromBodyButton_->setVisible(componentExtractionVisible);
  for (auto* action : findChildren<QAction*>()) {
    const QString source =
        action->property(kSourceCommandProperty).toString();
    if (source == createComponentFromBodyButton_->objectName())
      action->setVisible(componentExtractionVisible);
  }
}

}  // namespace solidar

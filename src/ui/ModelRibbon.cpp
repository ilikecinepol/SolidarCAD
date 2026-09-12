#include "ui/ModelRibbon.h"
#include "ui/PartDesignToolHelp.h"

#include <QFrame>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

namespace solidar {
namespace {

QToolButton* commandButton(const QString& commandId, QWidget* parent) {
  auto* button = new QToolButton(parent);
  const auto* help = modelCommandHelp(commandId);
  Q_ASSERT(help);
  button->setText(help->title);
  button->setToolTip(help->detailedDescription);
  button->setAccessibleName(help->title);
  button->setAccessibleDescription(help->detailedDescription);
  button->setProperty("helpId", commandId);
  button->setProperty("uiRole", "modelCommand");
  button->setIcon(modelCommandIcon(commandId));
  button->setIconSize(QSize(42, 42));
  button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  button->setObjectName("modelCommand");
  button->setMinimumSize(118, 70);
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
  layout->setContentsMargins(12, 3, 12, 2);
  layout->setSpacing(2);
  layout->addLayout(commands);
  auto* caption = new QToolButton(widget);
  caption->setObjectName("modelGroupMenuButton");
  caption->setText(title + QStringLiteral("  ▾"));
  caption->setPopupMode(QToolButton::InstantPopup);
  caption->setCursor(Qt::PointingHandCursor);
  auto* menu = new QMenu(caption);
  menu->setObjectName("modelGroupMenu");
  for (auto* button : menuButtons) {
    if (button->menu()) {
      button->menu()->setTitle(button->text());
      menu->addMenu(button->menu());
      continue;
    }
    auto* action = menu->addAction(button->icon(), button->text());
    action->setObjectName(button->objectName() + QStringLiteral("Action"));
    action->setEnabled(button->isEnabled());
    if (button->isEnabled())
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
  root->setContentsMargins(18, 6, 18, 6);
  root->setSpacing(8);

  auto* createSketch = commandButton(QStringLiteral("createSketch"), this);
  auto* extrude = commandButton(QStringLiteral("extrude"), this);
  auto* revolve = commandButton(QStringLiteral("revolve"), this);
  createSketch->setObjectName("createSketchCommand");
  extrude->setObjectName("extrudeCommand");
  revolve->setObjectName("revolveCommand");
  createSketch->setCheckable(true);
  extrude->setCheckable(true);
  revolve->setCheckable(true);
  toolGroup_ = new QButtonGroup(this);
  toolGroup_->setExclusive(true);
  toolGroup_->addButton(createSketch);
  toolGroup_->addButton(extrude);
  toolGroup_->addButton(revolve);
  auto* creation = new QHBoxLayout;
  creation->setSpacing(5);
  creation->addWidget(createSketch);
  creation->addWidget(extrude);
  creation->addWidget(revolve);
  root->addWidget(group(QString::fromUtf8("СОЗДАНИЕ"), creation, this,
                        {createSketch, extrude, revolve}));
  root->addWidget(separator(this));

  auto* fillet = commandButton(QStringLiteral("fillet"), this);
  fillet->setObjectName("filletCommand");
  fillet->setCheckable(true);
  toolGroup_->addButton(fillet);
  auto* chamfer = commandButton(QStringLiteral("chamfer"), this);
  chamfer->setObjectName("chamferCommand");
  chamfer->setCheckable(true);
  toolGroup_->addButton(chamfer);
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
  editing->setSpacing(5);
  editing->addWidget(fillet);
  editing->addWidget(chamfer);
  editing->addWidget(shell);
  editing->addWidget(draft);
  editing->addWidget(mirror);
  editing->addWidget(linearPattern);
  editing->addWidget(circularPattern);
  root->addWidget(group(QString::fromUtf8("РЕДАКТИРОВАНИЕ"), editing, this,
                        {fillet, chamfer, shell, draft, mirror, linearPattern,
                         circularPattern}));
  root->addWidget(separator(this));

  auto* views = new QHBoxLayout;
  views->setSpacing(3);
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

  auto* quality = new QToolButton(this);
  quality->setObjectName("meshQualityCommand");
  quality->setText(QString::fromUtf8("Качество:\nОбычное"));
  quality->setToolTip(QString::fromUtf8("Качество сетки отображения"));
  quality->setPopupMode(QToolButton::InstantPopup);
  quality->setToolButtonStyle(Qt::ToolButtonTextOnly);
  quality->setMinimumSize(92, 56);
  auto* qualityMenu = new QMenu(quality);
  auto* normalQuality = qualityMenu->addAction(QString::fromUtf8("Обычное"));
  auto* highQuality = qualityMenu->addAction(QString::fromUtf8("Высокое"));
  connect(normalQuality, &QAction::triggered, this, [this, quality] {
    quality->setText(QString::fromUtf8("Качество:\nОбычное"));
    emit meshQualityRequested(0);
  });
  connect(highQuality, &QAction::triggered, this, [this, quality] {
    quality->setText(QString::fromUtf8("Качество:\nВысокое"));
    emit meshQualityRequested(1);
  });
  quality->setMenu(qualityMenu);
  views->addWidget(displayMode);
  views->addWidget(quality);
  root->addWidget(group(QString::fromUtf8("ОТОБРАЖЕНИЕ"), views, this,
                        {displayMode, quality}), 1);

  connect(createSketch, &QToolButton::clicked, this,
          &ModelRibbon::createSketchRequested);
  connect(extrude, &QToolButton::clicked, this,
          &ModelRibbon::extrudeRequested);
  connect(revolve, &QToolButton::clicked, this,
          &ModelRibbon::revolveRequested);
  connect(fillet, &QToolButton::clicked, this,
          &ModelRibbon::filletRequested);
  connect(chamfer, &QToolButton::clicked, this,
          &ModelRibbon::chamferRequested);
  connect(shell, &QToolButton::clicked, this, &ModelRibbon::shellRequested);
  connect(draft, &QToolButton::clicked, this, &ModelRibbon::draftRequested);
  connect(mirror, &QToolButton::clicked, this, &ModelRibbon::mirrorRequested);
  connect(linearPattern, &QToolButton::clicked, this,
          &ModelRibbon::linearPatternRequested);
  connect(circularPattern, &QToolButton::clicked, this,
          &ModelRibbon::circularPatternRequested);
}

void ModelRibbon::clearActiveTool() {
  toolGroup_->setExclusive(false);
  for (auto* button : toolGroup_->buttons()) button->setChecked(false);
  toolGroup_->setExclusive(true);
}

}  // namespace solidar

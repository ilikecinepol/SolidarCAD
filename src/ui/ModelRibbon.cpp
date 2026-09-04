#include "ui/ModelRibbon.h"

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

QToolButton* commandButton(const QString& iconPath, const QString& text,
                           QWidget* parent) {
  auto* button = new QToolButton(parent);
  button->setText(text);
  button->setIcon(QIcon(iconPath));
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

  auto* createSketch = commandButton(QStringLiteral(":/icons/create-sketch.png"),
                                     QString::fromUtf8("Создать эскиз"), this);
  auto* extrude = commandButton(QStringLiteral(":/icons/extrude.png"),
                                QString::fromUtf8("Выдавливание"), this);
  auto* revolve = commandButton(QStringLiteral(":/icons/revolve.png"),
                                QString::fromUtf8("Инструмент вращения"), this);
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

  auto* fillet = commandButton(QStringLiteral(":/icons/fillet.png"),
                               QString::fromUtf8("Скругление"), this);
  fillet->setObjectName("filletCommand");
  fillet->setCheckable(true);
  toolGroup_->addButton(fillet);
  auto* chamfer = commandButton(QStringLiteral(":/icons/fillet.png"),
                                QString::fromUtf8("Фаска"), this);
  chamfer->setObjectName("chamferCommand");
  chamfer->setCheckable(true);
  toolGroup_->addButton(chamfer);
  auto* editing = new QHBoxLayout;
  editing->setSpacing(5);
  editing->addWidget(fillet);
  editing->addWidget(chamfer);
  root->addWidget(group(QString::fromUtf8("РЕДАКТИРОВАНИЕ"), editing, this,
                        {fillet, chamfer}));
  root->addWidget(separator(this));

  auto* views = new QHBoxLayout;
  views->setSpacing(3);
  auto* fit = commandButton({}, QStringLiteral("Fit"), this);
  auto* iso = commandButton({}, QStringLiteral("ISO"), this);
  fit->setObjectName("fitCommand");
  iso->setObjectName("isoCommand");
  fit->setMinimumSize(58, 56);
  iso->setMinimumSize(58, 56);
  views->addWidget(fit);
  views->addWidget(iso);
  root->addWidget(group(QString::fromUtf8("ВИД"), views, this, {fit, iso}), 1);

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
  connect(fit, &QToolButton::clicked, this, &ModelRibbon::fitRequested);
  connect(iso, &QToolButton::clicked, this, &ModelRibbon::isoRequested);

  setStyleSheet(R"(
    QWidget#modelRibbon { background:#ffffff; border-bottom:1px solid #d8e1ef; }
    QToolButton#modelCommand { background:transparent; color:#17356e;
      border:1px solid transparent; border-radius:8px; font-size:13px; padding:5px 10px; }
    QToolButton#modelCommand:hover { background:#edf5ff; border-color:#a9cbff; }
    QToolButton#modelCommand:checked { background:#dcecff; color:#0068e8;
      border:2px solid #78adf8; font-weight:600; }
    QToolButton#modelGroupMenuButton { color:#526d98; background:transparent;
      border:none; font-size:10px; font-weight:700; padding:2px 10px; }
    QToolButton#modelGroupMenuButton:hover { color:#0868e8; background:#edf5ff;
      border-radius:5px; }
    QMenu { background:#ffffff; color:#17356e; border:1px solid #cfdbee;
      padding:5px; }
    QMenu::item { padding:7px 24px 7px 8px; border-radius:4px; }
    QMenu::item:selected { background:#e8f2ff; color:#0868e8; }
    QLabel#modelPlaceholder { color:#a8b7cd; font-size:12px; }
    QFrame#modelSeparator { color:#d8e1ef; margin:4px 7px; }
  )");
}

void ModelRibbon::clearActiveTool() {
  toolGroup_->setExclusive(false);
  for (auto* button : toolGroup_->buttons()) button->setChecked(false);
  toolGroup_->setExclusive(true);
}

}  // namespace solidar

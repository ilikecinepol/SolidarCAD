#include "ui/ModelRibbon.h"

#include <QFrame>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>

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

QWidget* group(const QString& title, QLayout* commands, QWidget* parent) {
  auto* widget = new QWidget(parent);
  auto* layout = new QVBoxLayout(widget);
  layout->setContentsMargins(12, 3, 12, 2);
  layout->setSpacing(2);
  layout->addLayout(commands);
  auto* caption = new QLabel(title, widget);
  caption->setObjectName("modelGroupCaption");
  caption->setAlignment(Qt::AlignCenter);
  layout->addWidget(caption);
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
  auto* pocket = commandButton(QStringLiteral(":/icons/extrude.png"),
                               QString::fromUtf8("Карман"), this);
  createSketch->setCheckable(true);
  extrude->setCheckable(true);
  toolGroup_ = new QButtonGroup(this);
  toolGroup_->setExclusive(true);
  toolGroup_->addButton(createSketch);
  toolGroup_->addButton(extrude);
  toolGroup_->addButton(pocket);
  auto* creation = new QHBoxLayout;
  creation->setSpacing(5);
  creation->addWidget(createSketch);
  creation->addWidget(extrude);
  creation->addWidget(pocket);
  root->addWidget(group(QString::fromUtf8("СОЗДАНИЕ"), creation, this));
  root->addWidget(separator(this));

  auto placeholderGroup = [this](const QString& title) {
    auto* layout = new QHBoxLayout;
    auto* label = new QLabel(QString::fromUtf8("Инструменты появятся позже"), this);
    label->setObjectName("modelPlaceholder");
    label->setMinimumWidth(260);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    return group(title, layout, this);
  };
  auto* views = new QHBoxLayout;
  views->setSpacing(3);
  const std::array<const char*, 5> viewNames{{"Fit", "ISO", "Top", "Front", "Right"}};
  std::array<QToolButton*, 5> viewButtons{};
  for (std::size_t index = 0; index < viewNames.size(); ++index) {
    viewButtons[index] = commandButton({}, viewNames[index], this);
    viewButtons[index]->setMinimumSize(58, 56);
    views->addWidget(viewButtons[index]);
  }
  root->addWidget(group(QString::fromUtf8("ВИД"), views, this), 1);
  root->addWidget(separator(this));
  root->addWidget(placeholderGroup(QString::fromUtf8("РАСШИРЕННЫЕ")), 1);

  connect(createSketch, &QToolButton::clicked, this,
          &ModelRibbon::createSketchRequested);
  connect(extrude, &QToolButton::clicked, this,
          &ModelRibbon::extrudeRequested);
  connect(pocket, &QToolButton::clicked, this,
          &ModelRibbon::pocketRequested);
  connect(viewButtons[0], &QToolButton::clicked, this, &ModelRibbon::fitRequested);
  connect(viewButtons[1], &QToolButton::clicked, this, &ModelRibbon::isoRequested);
  connect(viewButtons[2], &QToolButton::clicked, this, &ModelRibbon::topRequested);
  connect(viewButtons[3], &QToolButton::clicked, this, &ModelRibbon::frontRequested);
  connect(viewButtons[4], &QToolButton::clicked, this, &ModelRibbon::rightRequested);

  setStyleSheet(R"(
    QWidget#modelRibbon { background:#ffffff; border-bottom:1px solid #d8e1ef; }
    QToolButton#modelCommand { background:transparent; color:#17356e;
      border:1px solid transparent; border-radius:8px; font-size:13px; padding:5px 10px; }
    QToolButton#modelCommand:hover { background:#edf5ff; border-color:#a9cbff; }
    QToolButton#modelCommand:checked { background:#dcecff; color:#0068e8;
      border:2px solid #78adf8; font-weight:600; }
    QLabel#modelGroupCaption { color:#657a9b; font-size:10px; font-weight:600; }
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

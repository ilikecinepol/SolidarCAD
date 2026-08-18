#include "sketch/SketchRibbon.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/SketchCanvas.h"

namespace solidar {
namespace {

QPushButton* toolButton(const QString& iconPath, const QString& title,
                        QWidget* parent, bool enabled = true) {
  auto* button = new QPushButton(parent);
  button->setIcon(QIcon(iconPath));
  button->setIconSize(QSize(30, 30));
  button->setObjectName("toolButton");
  button->setCheckable(true);
  button->setFixedSize(54, 54);
  button->setEnabled(enabled);
  button->setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(enabled
                         ? title
                         : title + QString::fromUtf8(" — инструмент появится позже"));
  return button;
}

QWidget* groupWidget(const QString& title, QLayout* tools, QWidget* parent,
                     const QList<QPushButton*>& menuButtons = {}) {
  auto* group = new QWidget(parent);
  auto* layout = new QVBoxLayout(group);
  layout->setContentsMargins(8, 4, 8, 2);
  layout->setSpacing(2);
  layout->addLayout(tools);
  if (menuButtons.isEmpty()) {
    auto* caption = new QLabel(title, group);
    caption->setObjectName("groupCaption");
    caption->setAlignment(Qt::AlignCenter);
    layout->addWidget(caption);
  } else {
    auto* caption = new QToolButton(group);
    caption->setObjectName("groupMenuButton");
    caption->setText(title + QStringLiteral("  ▾"));
    caption->setPopupMode(QToolButton::InstantPopup);
    caption->setCursor(Qt::PointingHandCursor);
    auto* menu = new QMenu(caption);
    for (auto* button : menuButtons) {
      auto* action = menu->addAction(button->icon(), button->toolTip());
      action->setEnabled(button->isEnabled());
      if (button->isEnabled())
        QObject::connect(action, &QAction::triggered, button,
                         [button] { button->click(); });
    }
    caption->setMenu(menu);
    layout->addWidget(caption, 0, Qt::AlignCenter);
  }
  return group;
}

}  // namespace

SketchRibbon::SketchRibbon(SketchCanvas* canvas, QWidget* parent)
    : QWidget(parent) {
  setObjectName("sketchRibbon");
  setFixedHeight(124);
  auto* root = new QHBoxLayout(this);
  root->setContentsMargins(14, 6, 14, 6);
  root->setSpacing(5);

  auto* line = toolButton(":/icons/sketch/line.png",
                          QString::fromUtf8("Линия"), this);
  auto* rectangle = toolButton(":/icons/sketch/rectangle.png",
                               QString::fromUtf8("Прямоугольник"), this);
  auto* circle = toolButton(":/icons/sketch/circle.png",
                            QString::fromUtf8("Окружность"), this);
  auto* arc = toolButton(":/icons/sketch/arc.png",
                         QString::fromUtf8("Дуга"), this, false);
  auto* polygon = toolButton(":/icons/sketch/polygon.png",
                             QString::fromUtf8("Полигон"), this, false);
  auto* slot = toolButton(":/icons/sketch/slot.png",
                          QString::fromUtf8("Слот"), this, false);
  auto* text = toolButton(":/icons/sketch/text.png",
                          QString::fromUtf8("Текст"), this, false);
  auto* toolGroup = new QButtonGroup(this);
  toolGroup->setExclusive(true);
  for (auto* button : {line, rectangle, circle})
    toolGroup->addButton(button);

  auto* creationLayout = new QHBoxLayout;
  creationLayout->setSpacing(3);
  creationLayout->addWidget(line);
  creationLayout->addWidget(rectangle);
  creationLayout->addWidget(circle);
  creationLayout->addWidget(arc);
  creationLayout->addWidget(polygon);
  creationLayout->addWidget(slot);
  creationLayout->addWidget(text);
  root->addWidget(groupWidget(
      QString::fromUtf8("СОЗДАНИЕ"), creationLayout, this,
      {line, rectangle, circle, arc, polygon, slot, text}));

  auto* separator1 = new QFrame(this);
  separator1->setFrameShape(QFrame::VLine);
  separator1->setObjectName("separator");
  root->addWidget(separator1);

  auto* mirror = toolButton(":/icons/sketch/mirror.png",
                            QString::fromUtf8("Зеркало"), this, false);
  auto* remove = new QPushButton(QString::fromUtf8("⌫"), this);
  remove->setObjectName("toolButton");
  remove->setFixedSize(54, 54);
  remove->setToolTip(QString::fromUtf8("Удалить"));
  remove->setFocusPolicy(Qt::NoFocus);
  remove->setCheckable(false);
  auto* clear = new QPushButton(QString::fromUtf8("×"), this);
  clear->setObjectName("toolButton");
  clear->setFixedSize(54, 54);
  clear->setToolTip(QString::fromUtf8("Очистить"));
  clear->setFocusPolicy(Qt::NoFocus);
  clear->setCheckable(false);
  auto* editingLayout = new QHBoxLayout;
  editingLayout->setSpacing(3);
  editingLayout->addWidget(mirror);
  editingLayout->addWidget(remove);
  editingLayout->addWidget(clear);
  root->addWidget(groupWidget(QString::fromUtf8("РЕДАКТИРОВАНИЕ"),
                              editingLayout, this));

  auto* separator2 = new QFrame(this);
  separator2->setFrameShape(QFrame::VLine);
  separator2->setObjectName("separator");
  root->addWidget(separator2);

  auto* constraintsLayout = new QHBoxLayout;
  auto* dimension = toolButton(":/icons/sketch/auto-dimension.png",
                               QString::fromUtf8("Авторазмер"), this);
  toolGroup->addButton(dimension);
  constraintsLayout->addWidget(dimension);
  auto* orthogonalTool = toolButton(
      ":/icons/sketch/orthogonal-constraint.png",
      QString::fromUtf8("Горизонтально/вертикально"), this);
  toolGroup->addButton(orthogonalTool);
  constraintsLayout->addWidget(orthogonalTool);
  auto* constraintLabels = new QVBoxLayout;
  auto* snap = new QLabel(QString::fromUtf8("●  Привязка к сетке"), this);
  snap->setObjectName("constraintReady");
  auto* orthogonal = new QLabel(
      QString::fromUtf8("●  Горизонт./вертик. — готово"), this);
  orthogonal->setObjectName("constraintReady");
  constraintLabels->addWidget(snap);
  constraintLabels->addWidget(orthogonal);
  constraintsLayout->addLayout(constraintLabels);
  root->addWidget(groupWidget(QString::fromUtf8("ОГРАНИЧЕНИЯ"),
                              constraintsLayout, this));
  root->addStretch();

  auto* finish = new QPushButton(QString::fromUtf8("✓  Завершить эскиз"), this);
  finish->setObjectName("finishButton");
  finish->setMinimumSize(190, 48);
  finish->setCursor(Qt::PointingHandCursor);
  finish->setFocusPolicy(Qt::NoFocus);
  root->addWidget(finish, 0, Qt::AlignVCenter);

  connect(line, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Line); });
  connect(rectangle, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Rectangle); });
  connect(circle, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Circle); });
  connect(dimension, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::AutoDimension); });
  connect(orthogonalTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::OrthogonalConstraint);
          });
  connect(canvas, &SketchCanvas::toolChanged, this,
          [toolGroup](SketchCanvas::Tool tool) {
            if (tool != SketchCanvas::Tool::Select) return;
            toolGroup->setExclusive(false);
            for (auto* button : toolGroup->buttons()) button->setChecked(false);
            toolGroup->setExclusive(true);
          });
  connect(remove, &QPushButton::clicked, canvas,
          &SketchCanvas::deleteSelection);
  connect(clear, &QPushButton::clicked, canvas, &SketchCanvas::clearSketch);
  connect(finish, &QPushButton::clicked, this, &SketchRibbon::finishRequested);

  setStyleSheet(R"(
    QWidget#sketchRibbon { background: #ffffff; border-bottom: 1px solid #d8e1ef; }
    QPushButton#toolButton { background: transparent; color: #17356e; border: none;
      border-radius: 7px; font-size: 13px; padding: 5px 8px; }
    QPushButton#toolButton:hover { background: #edf5ff; }
    QPushButton#toolButton:checked { background:#dcecff; color:#0068e8;
      border:2px solid #78adf8; font-weight:600; }
    QPushButton#toolButton:disabled { color:#9ca9bd; background:transparent; }
    QLabel#groupCaption { color: #6d7f9d; font-size: 10px; font-weight: 600; }
    QToolButton#groupMenuButton { color:#526d98; background:transparent;
      border:none; font-size:10px; font-weight:700; padding:2px 10px; }
    QToolButton#groupMenuButton:hover { color:#0868e8; background:#edf5ff;
      border-radius:5px; }
    QMenu { background:#ffffff; color:#17356e; border:1px solid #cfdbee;
      padding:5px; }
    QMenu::item { padding:7px 24px 7px 8px; border-radius:4px; }
    QMenu::item:selected { background:#e8f2ff; color:#0868e8; }
    QMenu::item:disabled { color:#9ca9bd; }
    QFrame#separator { color: #d8e1ef; margin: 4px 7px; }
    QLabel#constraintReady { color: #087449; font-size: 13px; }
    QLabel#constraintMuted { color: #8795ac; font-size: 12px; }
    QPushButton#finishButton { background: #0872f9; color: white; border: none;
      border-radius: 9px; font-size: 14px; font-weight: 650; padding: 0 18px; }
    QPushButton#finishButton:hover { background: #005ed8; }
  )");
}

}  // namespace solidar

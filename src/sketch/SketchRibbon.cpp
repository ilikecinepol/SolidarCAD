#include "sketch/SketchRibbon.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/SketchCanvas.h"
#include "ui/ToolIcon.h"

namespace solidar {
namespace {

QPushButton* toolButton(ToolIconKind icon, const QString& title,
                        QWidget* parent, bool enabled = true) {
  auto* button = new QPushButton(parent);
  button->setIcon(toolIcon(icon));
  button->setIconSize(QSize(30, 30));
  button->setObjectName("toolButton");
  button->setCheckable(true);
  button->setFixedSize(54, 54);
  button->setEnabled(enabled);
  button->setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
  button->setFocusPolicy(Qt::NoFocus);
  button->setAccessibleName(title);
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

  auto* line = toolButton(ToolIconKind::Line,
                          QString::fromUtf8("Линия"), this);
  auto* rectangle = toolButton(ToolIconKind::Rectangle,
                               QString::fromUtf8("Прямоугольник"), this);
  auto* circle = toolButton(ToolIconKind::Circle,
                            QString::fromUtf8("Окружность"), this);
  auto* projection = toolButton(
      ToolIconKind::Projection, QString::fromUtf8("Проекция"), this);
  projection->setToolTip(
      QString::fromUtf8("Проекция существующего ребра в эскиз"));
  auto* arc = toolButton(ToolIconKind::Arc,
                         QString::fromUtf8("Дуга по 3 точкам"), this);
  auto* bezier = toolButton(ToolIconKind::Bezier,
                            QString::fromUtf8("Кривая Безье"), this);
  bezier->setToolTip(QString::fromUtf8(
      "Кривая Безье по 4 точкам: начало, две опорные, конец"));
  auto* toolGroup = new QButtonGroup(this);
  toolGroup->setExclusive(true);
  for (auto* button : {line, rectangle, circle, arc, bezier, projection})
    toolGroup->addButton(button);

  auto* creationLayout = new QHBoxLayout;
  creationLayout->setSpacing(3);
  creationLayout->addWidget(line);
  creationLayout->addWidget(rectangle);
  creationLayout->addWidget(circle);
  creationLayout->addWidget(projection);
  creationLayout->addWidget(arc);
  creationLayout->addWidget(bezier);
  root->addWidget(groupWidget(
      QString::fromUtf8("СОЗДАНИЕ"), creationLayout, this,
      {line, rectangle, circle, projection, arc, bezier}));

  auto* separator1 = new QFrame(this);
  separator1->setFrameShape(QFrame::VLine);
  separator1->setObjectName("separator");
  root->addWidget(separator1);

  auto* remove = new QPushButton(this);
  remove->setIcon(toolIcon(ToolIconKind::Delete));
  remove->setIconSize(QSize(30, 30));
  remove->setObjectName("toolButton");
  remove->setFixedSize(54, 54);
  remove->setToolTip(QString::fromUtf8("Удалить"));
  remove->setAccessibleName(remove->toolTip());
  remove->setFocusPolicy(Qt::NoFocus);
  remove->setCheckable(false);
  auto* clear = new QPushButton(this);
  clear->setIcon(toolIcon(ToolIconKind::Clear));
  clear->setIconSize(QSize(30, 30));
  clear->setObjectName("toolButton");
  clear->setFixedSize(54, 54);
  clear->setToolTip(QString::fromUtf8("Очистить"));
  clear->setAccessibleName(clear->toolTip());
  clear->setFocusPolicy(Qt::NoFocus);
  clear->setCheckable(false);
  auto* mirror = toolButton(ToolIconKind::SketchMirror,
                            QString::fromUtf8("Зеркало"), this);
  auto* trim = toolButton(ToolIconKind::Trim,
                          QString::fromUtf8("Ножницы"), this);
  toolGroup->addButton(mirror);
  toolGroup->addButton(trim);
  auto* editingLayout = new QHBoxLayout;
  editingLayout->setSpacing(3);
  editingLayout->addWidget(mirror);
  editingLayout->addWidget(trim);
  editingLayout->addWidget(remove);
  editingLayout->addWidget(clear);
  root->addWidget(groupWidget(QString::fromUtf8("РЕДАКТИРОВАНИЕ"),
                              editingLayout, this));

  auto* separator2 = new QFrame(this);
  separator2->setFrameShape(QFrame::VLine);
  separator2->setObjectName("separator");
  root->addWidget(separator2);

  auto* constraintsLayout = new QHBoxLayout;
  auto* dimension = toolButton(ToolIconKind::AutoDimension,
                               QString::fromUtf8("Авторазмер"), this);
  toolGroup->addButton(dimension);
  constraintsLayout->addWidget(dimension);
  auto* orthogonalTool = toolButton(
      ToolIconKind::OrthogonalConstraint,
      QString::fromUtf8("Горизонтально/вертикально"), this);
  toolGroup->addButton(orthogonalTool);
  constraintsLayout->addWidget(orthogonalTool);

  auto* coincidentTool = toolButton(
      ToolIconKind::CoincidentConstraint,
      QString::fromUtf8("Совпадение / Принадлежность"), this);
  toolGroup->addButton(coincidentTool);
  constraintsLayout->addWidget(coincidentTool);

  auto* perpendicularTool = toolButton(
      ToolIconKind::PerpendicularConstraint,
      QString::fromUtf8("Перпендикулярность"), this);
  toolGroup->addButton(perpendicularTool);
  constraintsLayout->addWidget(perpendicularTool);

  auto* parallelTool = toolButton(
      ToolIconKind::ParallelConstraint,
      QString::fromUtf8("Параллельность"), this);
  toolGroup->addButton(parallelTool);
  constraintsLayout->addWidget(parallelTool);

  auto* equalTool = toolButton(
      ToolIconKind::EqualConstraint,
      QString::fromUtf8("Эквивалентность"), this);
  toolGroup->addButton(equalTool);
  constraintsLayout->addWidget(equalTool);

  auto* tangentTool = toolButton(
      ToolIconKind::TangentConstraint,
      QString::fromUtf8("Касательная к окружности"), this);
  toolGroup->addButton(tangentTool);
  constraintsLayout->addWidget(tangentTool);

  auto* lockTool = toolButton(
      ToolIconKind::LockConstraint, QString::fromUtf8("Замок"), this);
  lockTool->setToolTip(
      QString::fromUtf8("Зафиксировать объект"));
  toolGroup->addButton(lockTool);
  constraintsLayout->addWidget(lockTool);

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
  connect(arc, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Arc); });
  connect(bezier, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Bezier); });
  connect(projection, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Projection); });
  connect(mirror, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Mirror); });
  connect(trim, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::Trim); });
  connect(dimension, &QPushButton::clicked, canvas,
          [canvas] { canvas->setTool(SketchCanvas::Tool::AutoDimension); });
  connect(orthogonalTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::OrthogonalConstraint);
          });
  connect(coincidentTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::CoincidentConstraint);
          });
  connect(perpendicularTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::PerpendicularConstraint);
          });
  connect(parallelTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::ParallelConstraint);
          });
  connect(equalTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::EqualConstraint);
          });
  connect(tangentTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::TangentConstraint);
          });
  connect(lockTool, &QPushButton::clicked, canvas,
          [canvas] {
            canvas->setTool(SketchCanvas::Tool::LockConstraint);
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
}

}  // namespace solidar

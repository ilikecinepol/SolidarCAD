#include "sketch/SketchRibbon.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/SketchCanvas.h"

namespace solidar {
namespace {

QPushButton* toolButton(const QString& symbol, const QString& title,
                        QWidget* parent) {
  auto* button = new QPushButton(symbol + "\n" + title, parent);
  button->setObjectName("toolButton");
  button->setCheckable(true);
  button->setMinimumSize(82, 68);
  button->setCursor(Qt::PointingHandCursor);
  button->setFocusPolicy(Qt::NoFocus);
  return button;
}

QWidget* groupWidget(const QString& title, QLayout* tools, QWidget* parent) {
  auto* group = new QWidget(parent);
  auto* layout = new QVBoxLayout(group);
  layout->setContentsMargins(8, 4, 8, 2);
  layout->setSpacing(2);
  layout->addLayout(tools);
  auto* caption = new QLabel(title, group);
  caption->setObjectName("groupCaption");
  caption->setAlignment(Qt::AlignCenter);
  layout->addWidget(caption);
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

  auto* line = toolButton("╱", QString::fromUtf8("Линия"), this);
  auto* rectangle = toolButton("▭", QString::fromUtf8("Прямоугольник"), this);
  auto* circle = toolButton("○", QString::fromUtf8("Окружность"), this);
  auto* toolGroup = new QButtonGroup(this);
  toolGroup->setExclusive(true);
  for (auto* button : {line, rectangle, circle})
    toolGroup->addButton(button);

  auto* creationLayout = new QHBoxLayout;
  creationLayout->setSpacing(3);
  creationLayout->addWidget(line);
  creationLayout->addWidget(rectangle);
  creationLayout->addWidget(circle);
  root->addWidget(groupWidget(QString::fromUtf8("СОЗДАНИЕ"), creationLayout,
                              this));

  auto* separator1 = new QFrame(this);
  separator1->setFrameShape(QFrame::VLine);
  separator1->setObjectName("separator");
  root->addWidget(separator1);

  auto* remove = toolButton("⌫", QString::fromUtf8("Удалить"), this);
  remove->setCheckable(false);
  auto* clear = toolButton("×", QString::fromUtf8("Очистить"), this);
  clear->setCheckable(false);
  auto* editingLayout = new QHBoxLayout;
  editingLayout->setSpacing(3);
  editingLayout->addWidget(remove);
  editingLayout->addWidget(clear);
  root->addWidget(groupWidget(QString::fromUtf8("РЕДАКТИРОВАНИЕ"),
                              editingLayout, this));

  auto* separator2 = new QFrame(this);
  separator2->setFrameShape(QFrame::VLine);
  separator2->setObjectName("separator");
  root->addWidget(separator2);

  auto* constraintsLayout = new QVBoxLayout;
  auto* snap = new QLabel(QString::fromUtf8("●  Привязка к сетке"), this);
  snap->setObjectName("constraintReady");
  auto* orthogonal = new QLabel(QString::fromUtf8("○  Автоограничения — далее"), this);
  orthogonal->setObjectName("constraintMuted");
  constraintsLayout->addWidget(snap);
  constraintsLayout->addWidget(orthogonal);
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
    QLabel#groupCaption { color: #6d7f9d; font-size: 10px; font-weight: 600; }
    QFrame#separator { color: #d8e1ef; margin: 4px 7px; }
    QLabel#constraintReady { color: #087449; font-size: 13px; }
    QLabel#constraintMuted { color: #8795ac; font-size: 12px; }
    QPushButton#finishButton { background: #0872f9; color: white; border: none;
      border-radius: 9px; font-size: 14px; font-weight: 650; padding: 0 18px; }
    QPushButton#finishButton:hover { background: #005ed8; }
  )");
}

}  // namespace solidar

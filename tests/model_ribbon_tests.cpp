#include <QApplication>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QToolButton>

#include <cstdlib>
#include <iostream>
#include <vector>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "ui/ModelRibbon.h"
#include "ui/Viewport.h"
#include "ui/ToolParametersPanel.h"
#include "ui/PartDesignToolHelp.h"
#include "ui/PartDesignHistory.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  solidar::ModelRibbon ribbon;
  ribbon.resize(1920, 124);
  ribbon.show();
  QApplication::processEvents();
  CHECK(ribbon.minimumSizeHint().width() <= 1920);

  const auto groups = ribbon.findChildren<QWidget*>("modelToolGroup");
  assert(groups.size() == 3);

  QWidget* creation = nullptr;
  QWidget* editing = nullptr;
  QWidget* view = nullptr;
  for (auto* group : groups) {
    const auto title = group->property("groupTitle").toString();
    if (title == QString::fromUtf8("СОЗДАНИЕ")) creation = group;
    if (title == QString::fromUtf8("РЕДАКТИРОВАНИЕ")) editing = group;
    if (title == QString::fromUtf8("ОТОБРАЖЕНИЕ")) view = group;
    assert(title != QString::fromUtf8("ВИД"));
    auto* menuButton = group->findChild<QToolButton*>("modelGroupMenuButton");
    assert(menuButton);
    assert(menuButton->menu());
  }
  assert(creation && editing && view);

  assert(creation->findChild<QToolButton*>("createSketchCommand"));
  assert(creation->findChild<QToolButton*>("extrudeCommand"));
  assert(creation->findChild<QToolButton*>("revolveCommand"));
  assert(editing->findChild<QToolButton*>("filletCommand"));
  assert(editing->findChild<QToolButton*>("chamferCommand"));
  assert(editing->findChild<QToolButton*>("joinBodiesCommand"));
  assert(editing->findChild<QToolButton*>("moveCommand"));
  assert(view->findChild<QToolButton*>("rulerCommand"));
  assert(!creation->findChild<QToolButton*>("filletCommand"));

  const auto commands = ribbon.findChildren<QToolButton*>();
  int documentedCommands = 0;
  std::vector<QRect> commandRects;
  for (const auto* command : commands) {
    assert(command->text() != QStringLiteral("Top"));
    assert(command->text() != QStringLiteral("Front"));
    assert(command->text() != QStringLiteral("Right"));
    assert(command->text() != QStringLiteral("Bottom"));
    assert(command->text() != QStringLiteral("Back"));
    assert(command->text() != QStringLiteral("Left"));
    if (!command->property("helpId").isValid()) continue;
    ++documentedCommands;
    assert(!command->toolTip().isEmpty());
    assert(!command->accessibleName().isEmpty());
    assert(!command->accessibleDescription().isEmpty());
    CHECK(command->width() == 104);
    CHECK(!command->property("commandTitle").toString().contains(
        QLatin1Char('\n')));
    commandRects.push_back(
        QRect(command->mapTo(&ribbon, QPoint{}), command->size()));
  }
  assert(documentedCommands == 13);
  for (std::size_t first = 0; first < commandRects.size(); ++first)
    for (std::size_t second = first + 1; second < commandRects.size(); ++second)
      CHECK(!commandRects[first].intersects(commandRects[second]));
  CHECK(editing->findChild<QToolButton*>("joinBodiesCommand")
            ->text()
            .contains(QLatin1Char('\n')));
  assert(!ribbon.findChild<QToolButton*>("fitCommand"));
  assert(!ribbon.findChild<QToolButton*>("isoCommand"));
  const std::initializer_list<solidar::PartDesignToolKind> historyKinds{
      solidar::PartDesignToolKind::Extrude,
      solidar::PartDesignToolKind::Pocket,
      solidar::PartDesignToolKind::Revolve,
      solidar::PartDesignToolKind::Fillet,
      solidar::PartDesignToolKind::Chamfer,
      solidar::PartDesignToolKind::JoinBodies,
      solidar::PartDesignToolKind::Move,
      solidar::PartDesignToolKind::Mirror,
      solidar::PartDesignToolKind::LinearPattern,
      solidar::PartDesignToolKind::CircularPattern,
      solidar::PartDesignToolKind::Shell,
      solidar::PartDesignToolKind::Draft};
  for (const auto kind : historyKinds)
    assert(!solidar::partDesignToolIcon(kind).isNull());
  assert(!solidar::modelCommandIcon(QStringLiteral("createSketch")).isNull());
  assert(!solidar::modelCommandIcon(QStringLiteral("ruler")).isNull());
  solidar::HistoryStep compactStep;
  compactStep.title = QString::fromUtf8("Фаска 1");
  compactStep.tooltip = QString::fromUtf8("Фаска 1\nРазмер: 2 мм");
  compactStep.icon = solidar::partDesignToolIcon(
      solidar::PartDesignToolKind::Chamfer);
  QToolButton compactButton;
  solidar::configureHistoryButton(compactButton, compactStep, true);
  assert(compactButton.text().isEmpty());
  assert(compactButton.toolButtonStyle() == Qt::ToolButtonIconOnly);
  assert(compactButton.width() <= 32 && compactButton.height() <= 32);
  assert(compactButton.iconSize().width() >= 14);
  assert(!compactButton.toolTip().isEmpty() && compactButton.isChecked());

  assert(creation->findChild<QMenu*>("modelGroupMenu")->actions().size() == 3);
  assert(editing->findChild<QMenu*>("modelGroupMenu")->actions().size() == 9);
  assert(view->findChild<QMenu*>("modelGroupMenu")->actions().size() == 3);

  solidar::ToolParametersPanel panel;
  const auto* shellHelp = solidar::partDesignToolHelp(
      solidar::PartDesignToolKind::Shell);
  panel.configure(*shellHelp, QString::fromUtf8("Грани"),
                  QString::fromUtf8("Толщина"), QStringLiteral(" mm"));
  assert(panel.titleText() == shellHelp->title.toUpper());
  assert(panel.descriptionText() == shellHelp->shortDescription);
  panel.setStatus(QString::fromUtf8("Тестовая ошибка"), true);
  assert(panel.descriptionText() == shellHelp->shortDescription);
  const auto* joinHelp = solidar::partDesignToolHelp(
      solidar::PartDesignToolKind::JoinBodies);
  panel.configureSelectionOnly(*joinHelp, QString::fromUtf8("Тела"));
  auto* parameterInput =
      panel.findChild<QDoubleSpinBox*>("toolParameterInput");
  assert(parameterInput && parameterInput->isHidden());
  const auto* chamferHelp = solidar::partDesignToolHelp(
      solidar::PartDesignToolKind::Chamfer);
  panel.configure(*chamferHelp, QString::fromUtf8("Рёбра"),
                  QString::fromUtf8("Размер"), QStringLiteral(" mm"));
  assert(!parameterInput->isHidden());

  solidar::Viewport viewport;
  viewport.setSelectionFilter(solidar::SelectionFilter::Edge);
  assert(viewport.selectionFilter() == solidar::SelectionFilter::Edge);
  viewport.setSelectionFilter(solidar::SelectionFilter::Face);
  assert(viewport.selectionFilter() == solidar::SelectionFilter::Face);
  viewport.setSelectionFilter(solidar::SelectionFilter::Any);

  viewport.viewTop();
  assert(viewport.cameraYawDegrees() == 0.0F);
  assert(viewport.cameraPitchDegrees() == 0.0F);
  viewport.viewFront();
  assert(viewport.cameraPitchDegrees() == -90.0F);
  viewport.viewRight();
  assert(viewport.cameraYawDegrees() == 90.0F);
  assert(viewport.cameraPitchDegrees() == 90.0F);
  viewport.viewIsometric();
  assert(viewport.cameraYawDegrees() == -45.0F);
  assert(viewport.cameraPitchDegrees() == 30.0F);
  return 0;
}

#include <QAction>
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
#include "TestAssertions.h"

#include "ui/ModelRibbon.h"
#include "ui/Viewport.h"
#include "ui/ToolParametersPanel.h"
#include "ui/PartDesignToolHelp.h"
#include "ui/PartDesignHistory.h"

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  solidar::ModelRibbon ribbon;
  ribbon.resize(1920, 124);
  ribbon.show();
  QApplication::processEvents();
  // U7b adds two ordinary assembly tiles; the full uncollapsed ribbon remains
  // bounded for wide editor layouts while the existing group menus provide
  // compact access on narrower windows.
  CHECK(ribbon.minimumSizeHint().width() <= 2200);

  const auto groups = ribbon.findChildren<QWidget*>("modelToolGroup");
  CHECK(groups.size() == 4);

  QWidget* creation = nullptr;
  QWidget* editing = nullptr;
  QWidget* view = nullptr;
  QWidget* assemblies = nullptr;
  for (auto* group : groups) {
    const auto title = group->property("groupTitle").toString();
    if (title == QString::fromUtf8("СОЗДАНИЕ")) creation = group;
    if (title == QString::fromUtf8("РЕДАКТИРОВАНИЕ")) editing = group;
    if (title == QString::fromUtf8("ОТОБРАЖЕНИЕ")) view = group;
    if (title == QString::fromUtf8("СБОРКИ")) assemblies = group;
    CHECK(title != QString::fromUtf8("ВИД"));
    auto* menuButton = group->findChild<QToolButton*>("modelGroupMenuButton");
    CHECK(menuButton);
    CHECK(menuButton->menu());
  }
  CHECK(creation && editing && assemblies && view);
  auto* createComponent = assemblies->findChild<QToolButton*>(
      "createComponentFromBodyCommand");
  CHECK(createComponent);
  CHECK(!createComponent->isCheckable());
  CHECK(createComponent->text() ==
        QString::fromUtf8("Создать компонент\nиз тела"));
  auto* setHome = assemblies->findChild<QToolButton*>(
      "componentSetHomePlacementCommand");
  auto* restoreHome = assemblies->findChild<QToolButton*>(
      "componentRestoreHomePlacementCommand");
  CHECK(setHome && restoreHome);
  CHECK(!setHome->isCheckable() && !restoreHome->isCheckable());
  CHECK(setHome->text() == QString::fromUtf8("Закрепить\nположение"));
  CHECK(restoreHome->text() ==
        QString::fromUtf8("Вернуть исходное\nположение"));
  CHECK(setHome->toolTip() == QString::fromUtf8(
      "Сохранить текущее положение как исходное. Компонент останется перемещаемым."));
  CHECK(restoreHome->toolTip() == QString::fromUtf8(
      "Вернуть все компоненты в сохранённые исходные положения."));
  CHECK(!assemblies->findChild<QToolButton*>(
      "returnToComponentsCommand"));
  int componentRequests = 0;
  QObject::connect(
      &ribbon, &solidar::ModelRibbon::createComponentFromBodyRequested,
      [&componentRequests] { ++componentRequests; });
  createComponent->click();
  CHECK(componentRequests == 0);

  CHECK(creation->findChild<QToolButton*>("createSketchCommand"));
  CHECK(!creation->findChild<QToolButton*>("referenceImageCommand"));
  CHECK(creation->findChild<QToolButton*>("extrudeCommand"));
  auto* pocketCommand = creation->findChild<QToolButton*>("pocketCommand");
  CHECK(pocketCommand);
  int pocketRequests = 0;
  QObject::connect(&ribbon, &solidar::ModelRibbon::pocketRequested,
                   [&pocketRequests] { ++pocketRequests; });
  pocketCommand->click();
  CHECK(pocketRequests == 1);
  ribbon.clearActiveTool();
  CHECK(creation->findChild<QToolButton*>("revolveCommand"));
  CHECK(editing->findChild<QToolButton*>("filletCommand"));
  CHECK(editing->findChild<QToolButton*>("chamferCommand"));
  CHECK(editing->findChild<QToolButton*>("joinBodiesCommand"));
  CHECK(editing->findChild<QToolButton*>("moveCommand"));
  CHECK(view->findChild<QToolButton*>("rulerCommand"));
  CHECK(view->findChild<QToolButton*>("referenceImageCommand"));
  CHECK(!creation->findChild<QToolButton*>("filletCommand"));

  const auto commands = ribbon.findChildren<QToolButton*>();
  int documentedCommands = 0;
  std::vector<QRect> commandRects;
  for (const auto* command : commands) {
    CHECK(command->text() != QStringLiteral("Top"));
    CHECK(command->text() != QStringLiteral("Front"));
    CHECK(command->text() != QStringLiteral("Right"));
    CHECK(command->text() != QStringLiteral("Bottom"));
    CHECK(command->text() != QStringLiteral("Back"));
    CHECK(command->text() != QStringLiteral("Left"));
    if (!command->property("helpId").isValid()) continue;
    ++documentedCommands;
    CHECK(!command->toolTip().isEmpty());
    CHECK(!command->accessibleName().isEmpty());
    CHECK(!command->accessibleDescription().isEmpty());
    CHECK(command->width() == 104);
    CHECK(!command->property("commandTitle").toString().contains(
        QLatin1Char('\n')));
    commandRects.push_back(
        QRect(command->mapTo(&ribbon, QPoint{}), command->size()));
  }
  CHECK(documentedCommands == 15);
  for (std::size_t first = 0; first < commandRects.size(); ++first)
    for (std::size_t second = first + 1; second < commandRects.size(); ++second)
      CHECK(!commandRects[first].intersects(commandRects[second]));
  CHECK(editing->findChild<QToolButton*>("joinBodiesCommand")
            ->text()
            .contains(QLatin1Char('\n')));
  CHECK(!ribbon.findChild<QToolButton*>("fitCommand"));
  CHECK(!ribbon.findChild<QToolButton*>("isoCommand"));
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
    CHECK(!solidar::partDesignToolIcon(kind).isNull());
  CHECK(!solidar::modelCommandIcon(QStringLiteral("createSketch")).isNull());
  CHECK(!solidar::modelCommandIcon(QStringLiteral("referenceImage")).isNull());
  CHECK(!solidar::modelCommandIcon(QStringLiteral("ruler")).isNull());
  solidar::HistoryStep compactStep;
  compactStep.title = QString::fromUtf8("Фаска 1");
  compactStep.tooltip = QString::fromUtf8("Фаска 1\nРазмер: 2 мм");
  compactStep.icon = solidar::partDesignToolIcon(
      solidar::PartDesignToolKind::Chamfer);
  QToolButton compactButton;
  solidar::configureHistoryButton(compactButton, compactStep, true);
  CHECK(compactButton.text().isEmpty());
  CHECK(compactButton.toolButtonStyle() == Qt::ToolButtonIconOnly);
  CHECK(compactButton.width() <= 32 && compactButton.height() <= 32);
  CHECK(compactButton.iconSize().width() >= 14);
  CHECK(!compactButton.toolTip().isEmpty() && compactButton.isChecked());

  CHECK(creation->findChild<QMenu*>("modelGroupMenu")->actions().size() == 4);
  CHECK(editing->findChild<QMenu*>("modelGroupMenu")->actions().size() == 9);
  CHECK(view->findChild<QMenu*>("modelGroupMenu")->actions().size() == 4);
  CHECK(assemblies->findChild<QMenu*>("modelGroupMenu")->actions().size() ==
        3);

  const auto overflowFor = [&ribbon](const QString& command) {
    for (auto* action : ribbon.findChildren<QAction*>())
      if (action->property("sourceCommandObjectName").toString() == command)
        return action;
    return static_cast<QAction*>(nullptr);
  };
  auto* createSketch =
      ribbon.findChild<QToolButton*>("createSketchCommand");
  auto* move = ribbon.findChild<QToolButton*>("moveCommand");
  auto* quality = ribbon.findChild<QToolButton*>("meshQualityCommand");
  auto* createSketchOverflow = overflowFor(QStringLiteral("createSketchCommand"));
  auto* moveOverflow = overflowFor(QStringLiteral("moveCommand"));
  auto* qualityOverflow = overflowFor(QStringLiteral("meshQualityCommand"));
  auto* componentOverflow =
      overflowFor(QStringLiteral("createComponentFromBodyCommand"));
  auto* setHomeOverflow =
      overflowFor(QStringLiteral("componentSetHomePlacementCommand"));
  auto* restoreHomeOverflow =
      overflowFor(QStringLiteral("componentRestoreHomePlacementCommand"));
  CHECK(createSketch && move && quality && createSketchOverflow &&
        moveOverflow && qualityOverflow && componentOverflow &&
        setHomeOverflow && restoreHomeOverflow);
  CHECK(!setHomeOverflow->isEnabled() && !restoreHomeOverflow->isEnabled());
  int adoptRequests = 0;
  int restoreRequests = 0;
  QObject::connect(
      &ribbon, &solidar::ModelRibbon::setComponentHomePlacementRequested,
      [&adoptRequests] { ++adoptRequests; });
  QObject::connect(
      &ribbon, &solidar::ModelRibbon::restoreComponentHomePlacementRequested,
      [&restoreRequests] { ++restoreRequests; });
  ribbon.setComponentHomePlacementEnabled(true, false);
  CHECK(setHome->isEnabled() && setHomeOverflow->isEnabled());
  CHECK(!restoreHome->isEnabled() && !restoreHomeOverflow->isEnabled());
  setHomeOverflow->trigger();
  restoreHomeOverflow->trigger();
  CHECK(adoptRequests == 1 && restoreRequests == 0);
  ribbon.setComponentHomePlacementEnabled(false, true);
  restoreHomeOverflow->trigger();
  CHECK(adoptRequests == 1 && restoreRequests == 1);
  CHECK(!overflowFor(QStringLiteral("returnToComponentsCommand")));
  CHECK(!componentOverflow->isEnabled());
  ribbon.setCreateComponentFromBodyEnabled(true);
  CHECK(createComponent->isEnabled() && componentOverflow->isEnabled());
  componentOverflow->trigger();
  CHECK(componentRequests == 1);
  ribbon.setCreateComponentFromBodyEnabled(false);
  CHECK(!createComponent->isEnabled() && !componentOverflow->isEnabled());
  ribbon.setComponentEditing(true);
  CHECK(!createComponent->isHidden() && componentOverflow->isVisible());
  ribbon.setComponentEditing(true, true);
  CHECK(!createComponent->isHidden() && componentOverflow->isVisible());
  ribbon.setComponentEditing(false);
  CHECK(!createComponent->isHidden() && componentOverflow->isVisible());
  ribbon.setAuthoringEnabled(false);
  CHECK(!createSketch->isEnabled() && !move->isEnabled());
  CHECK(!createSketchOverflow->isEnabled() && !moveOverflow->isEnabled());
  ribbon.setAuthoringEnabled(false, true);
  CHECK(!createSketch->isEnabled() && move->isEnabled());
  CHECK(!createSketchOverflow->isEnabled() && moveOverflow->isEnabled());
  ribbon.setAuthoringEnabled(true);
  CHECK(createSketch->isEnabled() && move->isEnabled());
  CHECK(createSketchOverflow->isEnabled() && moveOverflow->isEnabled());

  int qualityRequests = 0;
  QObject::connect(&ribbon, &solidar::ModelRibbon::meshQualityRequested,
                   [&qualityRequests] { ++qualityRequests; });
  const QString qualityLabel = quality->text();
  ribbon.setMeshQualityControlEnabled(false);
  CHECK(!quality->isEnabled() && !qualityOverflow->isEnabled());
  CHECK(!quality->menu()->actions().at(1)->isEnabled());
  quality->menu()->actions().at(1)->trigger();
  CHECK(quality->text() == qualityLabel);
  CHECK(qualityRequests == 0);
  ribbon.setMeshQualityControlEnabled(true);
  CHECK(quality->isEnabled() && qualityOverflow->isEnabled());
  CHECK(quality->menu()->actions().at(1)->isEnabled());

  solidar::ToolParametersPanel panel;
  const auto* shellHelp = solidar::partDesignToolHelp(
      solidar::PartDesignToolKind::Shell);
  panel.configure(*shellHelp, QString::fromUtf8("Грани"),
                  QString::fromUtf8("Толщина"), QStringLiteral(" mm"));
  CHECK(panel.titleText() == shellHelp->title.toUpper());
  CHECK(panel.descriptionText() == shellHelp->shortDescription);
  panel.setStatus(QString::fromUtf8("Тестовая ошибка"), true);
  CHECK(panel.descriptionText() == shellHelp->shortDescription);
  const auto* joinHelp = solidar::partDesignToolHelp(
      solidar::PartDesignToolKind::JoinBodies);
  panel.configureSelectionOnly(*joinHelp, QString::fromUtf8("Тела"));
  auto* parameterInput =
      panel.findChild<QDoubleSpinBox*>("toolParameterInput");
  CHECK(parameterInput && parameterInput->isHidden());
  const auto* chamferHelp = solidar::partDesignToolHelp(
      solidar::PartDesignToolKind::Chamfer);
  panel.configure(*chamferHelp, QString::fromUtf8("Рёбра"),
                  QString::fromUtf8("Размер"), QStringLiteral(" mm"));
  CHECK(!parameterInput->isHidden());

  solidar::Viewport viewport;
  viewport.setSelectionFilter(solidar::SelectionFilter::Edge);
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Edge);
  viewport.setSelectionFilter(solidar::SelectionFilter::Face);
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Face);
  viewport.setSelectionFilter(solidar::SelectionFilter::Any);

  viewport.viewTop();
  CHECK(viewport.cameraYawDegrees() == 0.0F);
  CHECK(viewport.cameraPitchDegrees() == 0.0F);
  viewport.viewFront();
  CHECK(viewport.cameraPitchDegrees() == -90.0F);
  viewport.viewRight();
  CHECK(viewport.cameraYawDegrees() == 90.0F);
  CHECK(viewport.cameraPitchDegrees() == 90.0F);
  viewport.viewIsometric();
  CHECK(viewport.cameraYawDegrees() == -45.0F);
  CHECK(viewport.cameraPitchDegrees() == 30.0F);
  return 0;
}

#include <QApplication>
#include <QMenu>
#include <QToolButton>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "ui/ModelRibbon.h"
#include "ui/Viewport.h"
#include "ui/ToolParametersPanel.h"
#include "ui/PartDesignToolHelp.h"

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  solidar::ModelRibbon ribbon;

  const auto groups = ribbon.findChildren<QWidget*>("modelToolGroup");
  assert(groups.size() == 3);

  QWidget* creation = nullptr;
  QWidget* editing = nullptr;
  QWidget* view = nullptr;
  for (auto* group : groups) {
    const auto title = group->property("groupTitle").toString();
    if (title == QString::fromUtf8("СОЗДАНИЕ")) creation = group;
    if (title == QString::fromUtf8("РЕДАКТИРОВАНИЕ")) editing = group;
    if (title == QString::fromUtf8("ВИД")) view = group;
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
  assert(!creation->findChild<QToolButton*>("filletCommand"));

  const auto commands = ribbon.findChildren<QToolButton*>();
  int documentedCommands = 0;
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
  }
  assert(documentedCommands == 12);

  assert(creation->findChild<QMenu*>("modelGroupMenu")->actions().size() == 3);
  assert(editing->findChild<QMenu*>("modelGroupMenu")->actions().size() == 7);
  assert(view->findChild<QMenu*>("modelGroupMenu")->actions().size() == 2);

  solidar::ToolParametersPanel panel;
  const auto* shellHelp = solidar::partDesignToolHelp(
      solidar::PartDesignToolKind::Shell);
  panel.configure(*shellHelp, QString::fromUtf8("Грани"),
                  QString::fromUtf8("Толщина"), QStringLiteral(" mm"));
  assert(panel.titleText() == shellHelp->title.toUpper());
  assert(panel.descriptionText() == shellHelp->shortDescription);
  panel.setStatus(QString::fromUtf8("Тестовая ошибка"), true);
  assert(panel.descriptionText() == shellHelp->shortDescription);

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

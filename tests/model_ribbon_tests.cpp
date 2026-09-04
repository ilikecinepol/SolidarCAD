#include <QApplication>
#include <QMenu>
#include <QToolButton>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "ui/ModelRibbon.h"

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
  for (const auto* command : commands) {
    assert(command->text() != QStringLiteral("Top"));
    assert(command->text() != QStringLiteral("Front"));
    assert(command->text() != QStringLiteral("Right"));
    assert(command->text() != QStringLiteral("Bottom"));
    assert(command->text() != QStringLiteral("Back"));
    assert(command->text() != QStringLiteral("Left"));
  }

  assert(creation->findChild<QMenu*>("modelGroupMenu")->actions().size() == 3);
  assert(editing->findChild<QMenu*>("modelGroupMenu")->actions().size() == 2);
  assert(view->findChild<QMenu*>("modelGroupMenu")->actions().size() == 2);
  return 0;
}

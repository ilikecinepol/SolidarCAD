#include "ui/EditorFactory.h"

#include "ui/MainWindow.h"

namespace solidar {

QMainWindow* createEditorWindow(const QString& projectPath, QString* error) {
  auto* editor = new MainWindow;
  if (!projectPath.isEmpty() && !editor->loadProject(projectPath, error)) {
    delete editor;
    return nullptr;
  }
  return editor;
}

}  // namespace solidar

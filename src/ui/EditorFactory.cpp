#include "ui/EditorFactory.h"

#include "ui/MainWindow.h"

namespace solidar {

QMainWindow* createEditorWindow(const QString& projectPath, AppSettings& settings,
                                QString* error) {
  auto* editor = new MainWindow(settings);
  if (!projectPath.isEmpty() && !editor->loadProject(projectPath, error)) {
    delete editor;
    return nullptr;
  }
  return editor;
}

}  // namespace solidar

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cassert>
#include <cstdio>

#include "project/ProjectFile.h"
#include "ui/MainWindow.h"

// Headless document-replacement stress. Switching between projects drives the
// full MainWindow::loadProject teardown path (PartDesignToolController
// cancelActive, tool-session cancel, resetScene, history rebuild). Repeating it
// guards against transient state or stale B-Rep references surviving a
// document replacement. No file dialogs, no OpenGL surface, no platform code.
int main(int argc, char** argv) {
  QApplication application(argc, argv);

  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-switch-ui-tests-XXXXXX")));
  if (!directory.isValid()) {
    std::fprintf(stderr, "QTemporaryDir failed: %s\n",
                 directory.errorString().toUtf8().constData());
    return 1;
  }

  QString error;
  const QString pathA = directory.filePath(QStringLiteral("a.solidar"));
  const QString pathB = directory.filePath(QStringLiteral("b.solidar"));
  assert(solidar::project::ProjectFile::create(pathA, &error));
  assert(solidar::project::ProjectFile::create(pathB, &error));

  solidar::MainWindow editor;
  for (int cycle = 0; cycle < 6; ++cycle) {
    assert(editor.loadProject(pathA, &error));
    assert(editor.loadProject(pathB, &error));
  }

  return 0;
}

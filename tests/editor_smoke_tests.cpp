#include <QApplication>
#include <QTimer>

#include "home/HomeWindow.h"
#include "ui/MainWindow.h"

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  solidar::home::HomeWindow home;
  solidar::MainWindow* editor = nullptr;
  QObject::connect(&home, &solidar::home::HomeWindow::projectRequested,
                   &application, [&](const QString& path) {
    editor = new solidar::MainWindow;
    editor->setProjectPath(path);
    home.hide();
    // Construction/destruction is tested without exposing a native OpenGL
    // surface; rendering is covered by the application-level smoke launch.
  });
  home.show();
  emit home.projectRequested(QStringLiteral("C:/Temp/test.solidar"));
  QTimer::singleShot(700, &application, &QApplication::quit);
  const int result = application.exec();
  delete editor;
  return result;
}

#include <QApplication>
#include <QPointer>
#include <QStyleFactory>

#include "home/HomeWindow.h"
#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  QApplication::setApplicationName(QString::fromUtf8("Солидарность CAD"));
  QApplication::setOrganizationName("Solidar CAD");
  application.setStyle(QStyleFactory::create("Fusion"));

  if (argc > 1) {
    solidar::MainWindow editor;
    editor.setProjectPath(QString::fromLocal8Bit(argv[1]));
    editor.show();
    return application.exec();
  }

  solidar::home::HomeWindow home;
  QPointer<solidar::MainWindow> editor;
  QObject::connect(&home, &solidar::home::HomeWindow::projectRequested,
                   &application, [&](const QString& path) {
                     if (editor) {
                       editor->raise();
                       editor->activateWindow();
                       return;
                     }
                     editor = new solidar::MainWindow;
                     editor->setAttribute(Qt::WA_DeleteOnClose);
                     editor->setProjectPath(path);
                     QObject::connect(editor, &QObject::destroyed, &home, [&home] {
                       home.show();
                       home.raise();
                     });
                     home.hide();
                     editor->show();
                   });
  home.show();
  return application.exec();
}

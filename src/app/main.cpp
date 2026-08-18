#include <QApplication>
#include <QMessageBox>
#include <QPointer>

#include "home/HomeWindow.h"
#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  QApplication::setApplicationName(QString::fromUtf8("Солидарность CAD"));
  QApplication::setOrganizationName("Solidar CAD");

  if (argc > 1) {
    solidar::MainWindow editor;
    QString error;
    if (!editor.loadProject(QString::fromLocal8Bit(argv[1]), &error)) {
      QMessageBox::critical(nullptr, QString::fromUtf8("Ошибка открытия"), error);
      return 1;
    }
    editor.showMaximized();
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
                     QString error;
                     if (!editor->loadProject(path, &error)) {
                       QMessageBox::critical(&home,
                                             QString::fromUtf8("Ошибка открытия"),
                                             error);
                       delete editor.data();
                       return;
                     }
                     QObject::connect(editor, &QObject::destroyed, &home, [&home] {
                       home.showMaximized();
                       home.raise();
                     });
                     home.hide();
                     editor->showMaximized();
                   });
  home.showMaximized();
  return application.exec();
}

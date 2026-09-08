#include <QApplication>
#include <QMessageBox>
#include <QPointer>

#include "home/HomeWindow.h"
#include "ui/EditorFactory.h"

int main(int argc, char* argv[]) {
  // The 3D viewport is also used while the first editor is being created.
  // Prefer Qt's software OpenGL backend here: on affected Windows drivers the
  // hardware context may corrupt the process during that initial paint.
  QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
  QApplication application(argc, argv);
  QApplication::setApplicationName(QString::fromUtf8("Солидарность CAD"));
  QApplication::setOrganizationName("Solidar CAD");

  if (argc > 1) {
    QString error;
    QPointer<QMainWindow> editor(solidar::createEditorWindow(
        QString::fromLocal8Bit(argv[1]), &error));
    if (!editor) {
      QMessageBox::critical(nullptr, QString::fromUtf8("Ошибка открытия"), error);
      return 1;
    }
    editor->setAttribute(Qt::WA_DeleteOnClose);
    editor->showMaximized();
    return application.exec();
  }

  solidar::home::HomeWindow home;
  QPointer<QMainWindow> editor;
  QObject::connect(&home, &solidar::home::HomeWindow::projectRequested,
                   &application, [&](const QString& path) {
                     if (editor) {
                       editor->raise();
                       editor->activateWindow();
                       return;
                     }
                     QString error;
                     editor = solidar::createEditorWindow(path, &error);
                     if (!editor) {
                       QMessageBox::critical(&home,
                                             QString::fromUtf8("Ошибка открытия"),
                                             error);
                       return;
                     }
                     editor->setAttribute(Qt::WA_DeleteOnClose);
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

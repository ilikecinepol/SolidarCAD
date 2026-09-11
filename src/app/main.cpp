#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPointer>
#include <QStyleHints>

#include "app/AppSettings.h"
#include "home/HomeWindow.h"
#include "ui/EditorFactory.h"
#include "ui/ThemeManager.h"

namespace {

void applyTheme(const solidar::AppSettings& settings) {
  const auto resolved = solidar::resolveTheme(
      settings.theme(), solidar::ThemeManager::systemColorScheme());
  solidar::ThemeManager::instance().apply(resolved);
}

}  // namespace

int main(int argc, char* argv[]) {
  // The 3D viewport is also used while the first editor is being created.
  // Prefer Qt's software OpenGL backend here: on affected Windows drivers the
  // hardware context may corrupt the process during that initial paint.
  QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
  QApplication application(argc, argv);

  // Stable technical identifiers keep the QSettings namespace independent of
  // the display name and of theme/language changes.
  QCoreApplication::setOrganizationName(QStringLiteral("SolidarCAD"));
  QCoreApplication::setApplicationName(QStringLiteral("SolidarCAD"));
  QApplication::setApplicationDisplayName(
      QString::fromUtf8("Солидарность CAD"));

  // Load settings and resolve/apply the theme before any window is created so
  // the first frame is already correctly themed (no flash of the wrong theme).
  solidar::AppSettings settings;
  applyTheme(settings);

  // Apply the theme whenever the user commits a new preference.
  QObject::connect(&settings, &solidar::AppSettings::themeChanged, &application,
                   [&settings](solidar::AppTheme) { applyTheme(settings); });

  // React to the OS color scheme changing while "System" is selected.
  QObject::connect(QGuiApplication::styleHints(),
                   &QStyleHints::colorSchemeChanged, &application,
                   [&settings](Qt::ColorScheme) {
                     if (settings.theme() == solidar::AppTheme::System)
                       applyTheme(settings);
                   });

  if (argc > 1) {
    QString error;
    QPointer<QMainWindow> editor(solidar::createEditorWindow(
        QString::fromLocal8Bit(argv[1]), settings, &error));
    if (!editor) {
      QMessageBox::critical(nullptr, QString::fromUtf8("Ошибка открытия"), error);
      return 1;
    }
    editor->setAttribute(Qt::WA_DeleteOnClose);
    editor->showMaximized();
    return application.exec();
  }

  solidar::home::HomeWindow home(settings);
  QPointer<QMainWindow> editor;
  QObject::connect(&home, &solidar::home::HomeWindow::projectRequested,
                   &application, [&](const QString& path) {
                     if (editor) {
                       editor->raise();
                       editor->activateWindow();
                       return;
                     }
                     QString error;
                     editor = solidar::createEditorWindow(path, settings, &error);
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

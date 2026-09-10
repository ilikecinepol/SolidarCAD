#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#include "app/AppSettings.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory;
  CHECK(directory.isValid());

  // Default: no stored value resolves to System.
  {
    const QString path = directory.filePath(QStringLiteral("default.ini"));
    solidar::AppSettings settings(path);
    CHECK(settings.theme() == solidar::AppTheme::System);
  }

  // Round trip: each preference survives a fresh AppSettings instance.
  for (const auto theme :
       {solidar::AppTheme::Light, solidar::AppTheme::Dark, solidar::AppTheme::System}) {
    const QString path =
        directory.filePath(QStringLiteral("roundtrip-%1.ini")
                               .arg(static_cast<int>(theme)));
    {
      solidar::AppSettings settings(path);
      settings.setTheme(theme);
      CHECK(settings.theme() == theme);
    }
    solidar::AppSettings reloaded(path);
    CHECK(reloaded.theme() == theme);
  }

  // Invalid stored value falls back to System without failing.
  {
    const QString path = directory.filePath(QStringLiteral("invalid.ini"));
    QSettings raw(path, QSettings::IniFormat);
    raw.setValue(QStringLiteral("ui/theme"), QStringLiteral("garbage"));
    raw.sync();
    solidar::AppSettings settings(path);
    CHECK(settings.theme() == solidar::AppTheme::System);
  }

  // themeChanged fires on real changes, and not when the value is unchanged.
  {
    const QString path = directory.filePath(QStringLiteral("signal.ini"));
    solidar::AppSettings settings(path);
    int emitted = 0;
    QObject::connect(&settings, &solidar::AppSettings::themeChanged,
                     [&](solidar::AppTheme) { ++emitted; });
    settings.setTheme(solidar::AppTheme::Dark);
    CHECK(emitted == 1);
    settings.setTheme(solidar::AppTheme::Dark);  // no-op
    CHECK(emitted == 1);
    settings.setTheme(solidar::AppTheme::Light);
    CHECK(emitted == 2);
  }

  return EXIT_SUCCESS;
}

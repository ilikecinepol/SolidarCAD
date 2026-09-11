#include "app/AppSettings.h"

namespace solidar {
namespace {

constexpr char kThemeKey[] = "ui/theme";

QString themeToken(AppTheme theme) {
  switch (theme) {
    case AppTheme::Light: return QStringLiteral("light");
    case AppTheme::Dark: return QStringLiteral("dark");
    case AppTheme::System: return QStringLiteral("system");
  }
  return QStringLiteral("system");
}

AppTheme themeFromToken(const QString& token) {
  if (token == QLatin1String("light")) return AppTheme::Light;
  if (token == QLatin1String("dark")) return AppTheme::Dark;
  return AppTheme::System;  // unknown / missing / "system" all fall back
}

}  // namespace

AppSettings::AppSettings(QObject* parent) : QObject(parent) { load(); }

AppSettings::AppSettings(const QString& settingsPath, QObject* parent)
    : QObject(parent), settings_(settingsPath, QSettings::IniFormat) {
  load();
}

AppTheme AppSettings::theme() const noexcept { return theme_; }

void AppSettings::setTheme(AppTheme theme) {
  if (theme_ == theme) return;
  theme_ = theme;
  settings_.setValue(QString::fromLatin1(kThemeKey), themeToken(theme));
  // Write through immediately so an abrupt exit right after switching still
  // keeps the new value.
  settings_.sync();
  emit themeChanged(theme_);
}

void AppSettings::sync() { settings_.sync(); }

void AppSettings::load() {
  theme_ = themeFromToken(settings_.value(QString::fromLatin1(kThemeKey)).toString());
}

}  // namespace solidar

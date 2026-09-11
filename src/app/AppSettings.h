#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

#include "app/AppTheme.h"

namespace solidar {

// Single application-level source of truth for user preferences. Persistence
// uses Qt QSettings under the stable application namespace, so the setting
// survives localization changes and is shared by every window.
class AppSettings final : public QObject {
  Q_OBJECT

 public:
  // Uses the default QSettings() scope (organization/application names must be
  // set on QCoreApplication beforehand).
  explicit AppSettings(QObject* parent = nullptr);
  // Test-only: point persistence at an explicit settings file so tests never
  // touch the real user configuration.
  AppSettings(const QString& settingsPath, QObject* parent = nullptr);

  [[nodiscard]] AppTheme theme() const noexcept;
  void setTheme(AppTheme theme);

  void sync();

 signals:
  void themeChanged(AppTheme theme);

 private:
  void load();

  QSettings settings_;
  AppTheme theme_{AppTheme::System};
};

}  // namespace solidar

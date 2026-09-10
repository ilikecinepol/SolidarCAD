#pragma once

#include <QWidget>

#include "app/AppSettings.h"

class QComboBox;
class QLabel;

namespace solidar {

// Reusable settings control for the theme preference. It edits an AppSettings
// instance and reflects its changes; it does not own theme application, so the
// same widget works on the Home screen and inside the editor settings dialog.
class SettingsWidget final : public QWidget {
  Q_OBJECT

 public:
  explicit SettingsWidget(AppSettings& settings, QWidget* parent = nullptr);

 private:
  void updateDescription(AppTheme theme);

  AppSettings& settings_;
  QComboBox* themeCombo_{nullptr};
  QLabel* themeDescription_{nullptr};
};

}  // namespace solidar

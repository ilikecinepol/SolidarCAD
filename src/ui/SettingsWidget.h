#pragma once

#include <QWidget>

#include "app/AppSettings.h"

class QComboBox;
class QLabel;
class QPushButton;

namespace solidar {

// Reusable settings control for the theme preference. The combo holds a
// temporary, uncommitted selection; Apply persists and applies it, Cancel
// reverts the combo back to the saved value. It does not own theme application
// (see ThemeManager / main wiring), so the same widget works on the Home
// settings page and inside the editor settings dialog.
class SettingsWidget final : public QWidget {
  Q_OBJECT

 public:
  explicit SettingsWidget(AppSettings& settings, QWidget* parent = nullptr);

 private:
  [[nodiscard]] AppTheme selectedTheme() const;
  void syncComboToSettings();
  void applyChanges();
  void cancelChanges();
  void updateDescription(AppTheme theme);

  AppSettings& settings_;
  QComboBox* themeCombo_{nullptr};
  QLabel* themeDescription_{nullptr};
  QPushButton* applyButton_{nullptr};
  QPushButton* cancelButton_{nullptr};
};

}  // namespace solidar

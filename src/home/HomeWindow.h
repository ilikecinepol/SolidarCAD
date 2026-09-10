#pragma once

#include <QMainWindow>

#include "app/AppSettings.h"

class QPushButton;
class QStackedWidget;
class QWidget;

namespace solidar::home {

class HomeWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit HomeWindow(AppSettings& settings, QWidget* parent = nullptr);

 signals:
  void projectRequested(const QString& path);

 private:
  void buildUi();
  void createProject();
  void openProject();
  void showPage(int index);

  AppSettings& settings_;
  QStackedWidget* pages_{nullptr};
  QPushButton* overviewButton_{nullptr};
  QPushButton* settingsButton_{nullptr};
};

}  // namespace solidar::home

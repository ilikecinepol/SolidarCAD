#pragma once

#include <QMainWindow>

namespace solidar::home {

class HomeWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit HomeWindow(QWidget* parent = nullptr);

 signals:
  void projectRequested(const QString& path);

 private:
  void buildUi();
  void createProject();
  void openProject();
};

}  // namespace solidar::home

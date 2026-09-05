#pragma once

#include <QWidget>

#include <map>
#include <string>
#include <vector>

#include "model/PartDesignToolFramework.h"

class QDoubleSpinBox;
class QHBoxLayout;

namespace solidar {

// Reusable viewport HUD. Enter commits only the focused parameter; normal Qt
// focus traversal provides Tab/Shift+Tab for multi-parameter tools.
class ToolParameterHud final : public QWidget {
  Q_OBJECT
 public:
  explicit ToolParameterHud(QWidget* parent = nullptr);
  void setParameters(const std::vector<ToolParameterDescriptor>& parameters);
  void setValue(const std::string& id, double value);
  [[nodiscard]] double value(const std::string& id) const;

 signals:
  void valueChanged(const QString& id, double value);
  void valueCommitted(const QString& id, double value);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void clearEditors();
  QHBoxLayout* layout_{};
  std::map<std::string, QDoubleSpinBox*> editors_;
  std::map<QDoubleSpinBox*, double> committedValues_;
};

}  // namespace solidar

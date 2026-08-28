#pragma once

#include <QWidget>

class QLabel;
class QDoubleSpinBox;
class QPushButton;

namespace solidar {

// Reusable one-scalar property panel foundation for modeling tools. Future
// tools can configure labels/suffixes without duplicating accept/cancel UX.
class ToolParametersPanel final : public QWidget {
  Q_OBJECT
 public:
  explicit ToolParametersPanel(QWidget* parent = nullptr);
  void configure(const QString& title, const QString& selectionName,
                 const QString& parameterName, const QString& suffix);
  void setSelectionCount(std::size_t count);
  void setParameterRange(double minimum, double maximum, int decimals);
  void setParameterValue(double value);
  [[nodiscard]] double parameterValue() const;
  void setStatus(const QString& text, bool error = false);
  void setAcceptEnabled(bool enabled);

 signals:
  void parameterChanged(double value);
  void accepted();
  void cancelled();

 private:
  QLabel* title_{};
  QLabel* selectionCaption_{};
  QLabel* selectionValue_{};
  QLabel* parameterCaption_{};
  QLabel* status_{};
  QDoubleSpinBox* parameter_{};
  QPushButton* accept_{};
};

}  // namespace solidar

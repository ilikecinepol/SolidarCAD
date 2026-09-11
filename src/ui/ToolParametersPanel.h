#pragma once

#include <QWidget>

class QLabel;
class QDoubleSpinBox;
class QPushButton;
class QCheckBox;

namespace solidar {
struct PartDesignToolHelp;

// Reusable one-scalar property panel foundation for modeling tools. Future
// tools can configure labels/suffixes without duplicating accept/cancel UX.
class ToolParametersPanel final : public QWidget {
  Q_OBJECT
 public:
  explicit ToolParametersPanel(QWidget* parent = nullptr);
  void configure(const QString& title, const QString& selectionName,
                 const QString& parameterName, const QString& suffix);
  void configure(const PartDesignToolHelp& help, const QString& selectionName,
                 const QString& parameterName, const QString& suffix);
  void setSelectionCount(std::size_t count);
  void setParameterRange(double minimum, double maximum, int decimals);
  void setParameterRangeAndValue(double minimum, double maximum, int decimals,
                                 double value);
  void setParameterValue(double value);
  [[nodiscard]] double parameterValue() const;
  void setStatus(const QString& text, bool error = false);
  void setAcceptEnabled(bool enabled);
  void focusParameterInput();
  void setDescription(const QString& text);
  [[nodiscard]] QString titleText() const;
  [[nodiscard]] QString descriptionText() const;
  void configureOption(const QString& text, bool checked);
  void setOptionChecked(bool checked);

 signals:
  void parameterChanged(double value);
  void accepted();
  void cancelled();
  void selectionRequested();
  void clearSelectionRequested();
  void optionChanged(bool checked);

 private:
  QLabel* title_{};
  QLabel* description_{};
  QLabel* selectionCaption_{};
  QLabel* selectionValue_{};
  QLabel* parameterCaption_{};
  QLabel* status_{};
  QDoubleSpinBox* parameter_{};
  QPushButton* accept_{};
  QPushButton* select_{};
  QPushButton* clear_{};
  QCheckBox* option_{};
};

}  // namespace solidar

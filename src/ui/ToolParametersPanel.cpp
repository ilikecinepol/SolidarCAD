#include "ui/ToolParametersPanel.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace solidar {

ToolParametersPanel::ToolParametersPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(12);
  title_ = new QLabel(this);
  QFont titleFont = title_->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() + 2);
  title_->setFont(titleFont);
  auto* form = new QFormLayout;
  selectionCaption_ = new QLabel(this);
  selectionValue_ = new QLabel(this);
  auto* selectionControls = new QWidget(this);
  auto* selectionLayout = new QHBoxLayout(selectionControls);
  selectionLayout->setContentsMargins(0, 0, 0, 0);
  select_ = new QPushButton(QString::fromUtf8("Выбрать"), selectionControls);
  clear_ = new QPushButton(QString::fromUtf8("Очистить"), selectionControls);
  selectionLayout->addWidget(selectionValue_);
  selectionLayout->addWidget(select_);
  selectionLayout->addWidget(clear_);
  parameterCaption_ = new QLabel(this);
  parameter_ = new QDoubleSpinBox(this);
  form->addRow(selectionCaption_, selectionControls);
  form->addRow(parameterCaption_, parameter_);
  status_ = new QLabel(this);
  status_->setWordWrap(true);
  auto* buttons = new QHBoxLayout;
  auto* cancel = new QPushButton(QString::fromUtf8("Отмена"), this);
  accept_ = new QPushButton(QStringLiteral("OK"), this);
  accept_->setDefault(true);
  accept_->setStyleSheet(
      "QPushButton{background:#0874f9;color:white;border:none;border-radius:6px;"
      "padding:8px 14px;font-weight:600;} QPushButton:disabled{background:#9db4d1;}");
  buttons->addWidget(cancel);
  buttons->addWidget(accept_);
  layout->addWidget(title_);
  layout->addLayout(form);
  layout->addWidget(status_);
  layout->addStretch();
  layout->addLayout(buttons);
  connect(parameter_, &QDoubleSpinBox::valueChanged, this,
          &ToolParametersPanel::parameterChanged);
  connect(accept_, &QPushButton::clicked, this, &ToolParametersPanel::accepted);
  connect(cancel, &QPushButton::clicked, this, &ToolParametersPanel::cancelled);
  connect(select_, &QPushButton::clicked, this,
          &ToolParametersPanel::selectionRequested);
  connect(clear_, &QPushButton::clicked, this,
          &ToolParametersPanel::clearSelectionRequested);
}

void ToolParametersPanel::configure(const QString& title,
                                    const QString& selectionName,
                                    const QString& parameterName,
                                    const QString& suffix) {
  title_->setText(title);
  selectionCaption_->setText(selectionName + QStringLiteral(":"));
  parameterCaption_->setText(parameterName + QStringLiteral(":"));
  parameter_->setSuffix(suffix);
}
void ToolParametersPanel::setSelectionCount(std::size_t count) {
  selectionValue_->setText(QString::fromUtf8("%1 выбрано").arg(count));
}
void ToolParametersPanel::setParameterRange(double minimum, double maximum,
                                            int decimals) {
  parameter_->setRange(minimum, maximum);
  parameter_->setDecimals(decimals);
}
void ToolParametersPanel::setParameterValue(double value) {
  const QSignalBlocker blocker(parameter_);
  parameter_->setValue(value);
}
double ToolParametersPanel::parameterValue() const { return parameter_->value(); }
void ToolParametersPanel::setStatus(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(error ? QStringLiteral("color:#c62828;")
                               : QStringLiteral("color:#607493;"));
}
void ToolParametersPanel::setAcceptEnabled(bool enabled) {
  accept_->setEnabled(enabled);
}

}  // namespace solidar

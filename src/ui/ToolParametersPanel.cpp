#include "ui/ToolParametersPanel.h"
#include "ui/PartDesignToolHelp.h"

#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
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
  description_ = new QLabel(this);
  description_->setObjectName("toolDescription");
  description_->setWordWrap(true);
  description_->setProperty("uiRole", "secondaryText");
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
  option_ = new QCheckBox(this);
  option_->hide();
  form->addRow(QString{}, option_);
  status_ = new QLabel(this);
  status_->setWordWrap(true);
  auto* buttons = new QHBoxLayout;
  auto* cancel = new QPushButton(QString::fromUtf8("Отмена"), this);
  accept_ = new QPushButton(QStringLiteral("OK"), this);
  accept_->setDefault(true);
  accept_->setProperty("uiRole", "primaryAction");
  buttons->addWidget(cancel);
  buttons->addWidget(accept_);
  layout->addWidget(title_);
  layout->addWidget(description_);
  layout->addLayout(form);
  layout->addWidget(status_);
  layout->addStretch();
  layout->addLayout(buttons);
  connect(parameter_, &QDoubleSpinBox::valueChanged, this,
          &ToolParametersPanel::parameterChanged);
  if (auto* editor = parameter_->findChild<QLineEdit*>())
    connect(editor, &QLineEdit::returnPressed, this, [this] {
      if (accept_->isEnabled()) emit accepted();
    });
  connect(accept_, &QPushButton::clicked, this, &ToolParametersPanel::accepted);
  connect(cancel, &QPushButton::clicked, this, &ToolParametersPanel::cancelled);
  connect(select_, &QPushButton::clicked, this,
          &ToolParametersPanel::selectionRequested);
  connect(clear_, &QPushButton::clicked, this,
          &ToolParametersPanel::clearSelectionRequested);
  connect(option_, &QCheckBox::toggled, this,
          &ToolParametersPanel::optionChanged);
}

void ToolParametersPanel::configure(const QString& title,
                                    const QString& selectionName,
                                    const QString& parameterName,
                                    const QString& suffix) {
  title_->setText(title);
  selectionCaption_->setText(selectionName + QStringLiteral(":"));
  parameterCaption_->setText(parameterName + QStringLiteral(":"));
  parameter_->setSuffix(suffix);
  option_->hide();
}
void ToolParametersPanel::configure(const PartDesignToolHelp& help,
                                    const QString& selectionName,
                                    const QString& parameterName,
                                    const QString& suffix) {
  configure(help.title.toUpper(), selectionName, parameterName, suffix);
  setDescription(help.shortDescription);
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
  status_->setProperty("uiRole", error ? "danger" : "secondaryText");
  status_->style()->unpolish(status_);
  status_->style()->polish(status_);
}
void ToolParametersPanel::setAcceptEnabled(bool enabled) {
  accept_->setEnabled(enabled);
}
void ToolParametersPanel::focusParameterInput() {
  parameter_->setFocus(Qt::OtherFocusReason);
  parameter_->selectAll();
}
void ToolParametersPanel::setDescription(const QString& text) {
  description_->setText(text);
}
QString ToolParametersPanel::titleText() const { return title_->text(); }
QString ToolParametersPanel::descriptionText() const {
  return description_->text();
}
void ToolParametersPanel::configureOption(const QString& text, bool checked) {
  const QSignalBlocker blocker(option_);
  option_->setText(text);
  option_->setChecked(checked);
  option_->show();
}
void ToolParametersPanel::setOptionChecked(bool checked) {
  const QSignalBlocker blocker(option_);
  option_->setChecked(checked);
}

}  // namespace solidar

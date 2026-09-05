#include "ui/tools/ToolParameterHud.h"

#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QSignalBlocker>

namespace solidar {

ToolParameterHud::ToolParameterHud(QWidget* parent) : QWidget(parent) {
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(6);
  setStyleSheet(
      "QDoubleSpinBox{background:#fff;color:#20252c;border:1px solid #c8d1de;"
      "border-radius:8px;padding:6px 8px;font-size:16px;}"
      "QDoubleSpinBox:focus{border:2px solid #1477ed;}"
      "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:24px;"
      "border:none;background:transparent;}");
}

void ToolParameterHud::clearEditors() {
  while (auto* item = layout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  editors_.clear();
  committedValues_.clear();
}

void ToolParameterHud::setParameters(
    const std::vector<ToolParameterDescriptor>& parameters) {
  clearEditors();
  for (const auto& parameter : parameters) {
    if (!parameter.editableInHud ||
        (parameter.type != ToolParameterType::Distance &&
         parameter.type != ToolParameterType::Angle &&
         parameter.type != ToolParameterType::Integer))
      continue;
    auto* editor = new QDoubleSpinBox(this);
    editor->setObjectName(QString::fromStdString(parameter.id));
    editor->setRange(parameter.minimum, parameter.maximum);
    editor->setSingleStep(parameter.step);
    editor->setDecimals(parameter.type == ToolParameterType::Integer ? 0 : 2);
    editor->setSuffix(parameter.unit.empty()
                          ? QString{}
                          : QStringLiteral(" ") + QString::fromStdString(parameter.unit));
    const double initial = parameter.type == ToolParameterType::Integer
                               ? static_cast<double>(std::get<int>(parameter.value))
                               : std::get<double>(parameter.value);
    editor->setValue(initial);
    editor->setFixedHeight(40);
    editor->installEventFilter(this);
    editors_[parameter.id] = editor;
    committedValues_[editor] = initial;
    layout_->addWidget(editor);
    connect(editor, &QDoubleSpinBox::valueChanged, this,
            [this, id = QString::fromStdString(parameter.id)](double value) {
              emit valueChanged(id, value);
            });
    connect(editor, &QDoubleSpinBox::editingFinished, this,
            [this, editor, id = QString::fromStdString(parameter.id)] {
              committedValues_[editor] = editor->value();
              emit valueCommitted(id, editor->value());
            });
  }
  adjustSize();
}

void ToolParameterHud::setValue(const std::string& id, double value) {
  const auto found = editors_.find(id);
  if (found == editors_.end()) return;
  if (found->second->hasFocus()) return;
  const QSignalBlocker blocker(found->second);
  found->second->setValue(value);
  committedValues_[found->second] = value;
}

double ToolParameterHud::value(const std::string& id) const {
  const auto found = editors_.find(id);
  return found == editors_.end() ? 0.0 : found->second->value();
}

bool ToolParameterHud::eventFilter(QObject* watched, QEvent* event) {
  auto* editor = qobject_cast<QDoubleSpinBox*>(watched);
  if (editor && event->type() == QEvent::KeyPress) {
    const auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Escape) {
      const QSignalBlocker blocker(editor);
      editor->setValue(committedValues_[editor]);
      editor->clearFocus();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace solidar

#include "ui/tools/ToolParameterHud.h"

#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPointer>
#include <QSignalBlocker>

#include <algorithm>
#include <iterator>

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
  orderedEditors_.clear();
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
    editor->setDecimals(parameter.type == ToolParameterType::Integer
                            ? 0
                            : parameter.type == ToolParameterType::Angle ? 1 : 2);
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
    orderedEditors_.push_back(editor);
    committedValues_[editor] = initial;
    layout_->addWidget(editor);
    connect(editor, &QDoubleSpinBox::valueChanged, this,
            [this, id = QString::fromStdString(parameter.id)](double value) {
              emit valueChanged(id, value);
            });
  }
  adjustSize();
}

void ToolParameterHud::setValue(const std::string& id, double value) {
  const auto found = editors_.find(id);
  if (found == editors_.end()) return;
  if (!found->second->hasFocus()) {
    const QSignalBlocker blocker(found->second);
    found->second->setValue(value);
    value = found->second->value();
  }
  committedValues_[found->second] = value;
}

double ToolParameterHud::value(const std::string& id) const {
  const auto found = editors_.find(id);
  return found == editors_.end() ? 0.0 : found->second->value();
}

bool ToolParameterHud::eventFilter(QObject* watched, QEvent* event) {
  auto* editor = qobject_cast<QDoubleSpinBox*>(watched);
  if (!editor) return QWidget::eventFilter(watched, event);

  if (event->type() == QEvent::ShortcutOverride) {
    const auto* key = static_cast<QKeyEvent*>(event);
    // Own the numeric-policy keys while a HUD field has focus so a parent
    // Return/Escape/Tab shortcut cannot preempt the HUD. Standard line-edit
    // editing shortcuts (Ctrl+A/C/V/X/Z) are deliberately left to the editor.
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
        key->key() == Qt::Key_Escape || key->key() == Qt::Key_Tab ||
        key->key() == Qt::Key_Backtab) {
      event->accept();
      return true;
    }
    return QWidget::eventFilter(watched, event);
  }

  if (event->type() != QEvent::KeyPress)
    return QWidget::eventFilter(watched, event);

  const auto* key = static_cast<QKeyEvent*>(event);
  if (key->key() == Qt::Key_Escape) {
    const QString id = [&]() -> QString {
      const auto found = std::find_if(
          editors_.begin(), editors_.end(),
          [editor](const auto& entry) { return entry.second == editor; });
      return found == editors_.end() ? QString{}
                                     : QString::fromStdString(found->first);
    }();
    const double restored = committedValues_[editor];
    QPointer<QDoubleSpinBox> guard(editor);
    {
      const QSignalBlocker blocker(editor);
      editor->setValue(restored);
    }
    if (!guard) return true;
    editor->clearFocus();
    // Route the rollback through the normal value path so the session and
    // preview re-sync to the last committed value, not just the display.
    if (!id.isEmpty()) emit valueChanged(id, restored);
    return true;
  }

  if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
    if (orderedEditors_.empty()) return QWidget::eventFilter(watched, event);
    const auto current = std::find(orderedEditors_.begin(),
                                   orderedEditors_.end(), editor);
    if (current == orderedEditors_.end())
      return QWidget::eventFilter(watched, event);
    auto index = static_cast<std::size_t>(
        std::distance(orderedEditors_.begin(), current));
    if (orderedEditors_.size() > 1) {
      const bool backward = key->key() == Qt::Key_Backtab ||
                            key->modifiers().testFlag(Qt::ShiftModifier);
      index = backward
                  ? (index + orderedEditors_.size() - 1) % orderedEditors_.size()
                  : (index + 1) % orderedEditors_.size();
    }
    auto* target = orderedEditors_[index];
    target->setFocus(Qt::TabFocusReason);
    target->selectAll();
    return true;
  }

  if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
    if (key->isAutoRepeat()) return true;
    const auto found = std::find_if(
        editors_.begin(), editors_.end(),
        [editor](const auto& entry) { return entry.second == editor; });
    if (found == editors_.end()) return true;
    const QString id = QString::fromStdString(found->first);
    QPointer<QDoubleSpinBox> guard(editor);
    editor->interpretText();
    if (!guard) return true;
    const double value = editor->value();
    committedValues_[editor] = value;
    emit valueCommitted(id, value);
    return true;
  }

  return QWidget::eventFilter(watched, event);
}

}  // namespace solidar

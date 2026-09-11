#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QShortcut>
#include <QSignalSpy>
#include <QString>
#include <QTest>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "model/PartDesignToolFramework.h"
#include "ui/tools/ToolParameterHud.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {

void pressKey(QWidget* target, int key,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier,
              bool autorep = false) {
  QKeyEvent press(QEvent::KeyPress, key, modifiers, QString(), autorep);
  QApplication::sendEvent(target, &press);
}

void showAndFocus(QWidget& window, QWidget* focus) {
  window.show();
  window.activateWindow();
  QApplication::processEvents();
  if (focus) {
    focus->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
  }
}

bool nearly(double a, double b) { return std::abs(a - b) < 1e-9; }

QLineEdit* spinLineEdit(QDoubleSpinBox* spin) {
  return spin ? spin->findChild<QLineEdit*>() : nullptr;
}

solidar::ToolParameterDescriptor distanceParam(const char* id, double value) {
  return {id, "Distance", solidar::ToolParameterType::Distance, value, 0.01,
          1000.0, 0.1, "mm", true, solidar::ToolManipulatorType::Linear};
}

solidar::ToolParameterDescriptor countParam(int value) {
  return {"count", "Count", solidar::ToolParameterType::Integer, value, 2.0,
          100.0, 1.0, "", true, solidar::ToolManipulatorType::None};
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  // Test 1: single field — Return commits exactly once.
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);
    QSignalSpy spy(&hud, &solidar::ToolParameterHud::valueCommitted);

    pressKey(editor, Qt::Key_Return);
    CHECK(spy.count() == 1);
    CHECK(spy.at(0).at(0).toString() == QStringLiteral("distance"));
    CHECK(nearly(spy.at(0).at(1).toDouble(), 10.0));
  }

  // Test 2: keypad Enter is equivalent to Return.
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);
    QSignalSpy spy(&hud, &solidar::ToolParameterHud::valueCommitted);

    pressKey(editor, Qt::Key_Enter);
    CHECK(spy.count() == 1);
    CHECK(spy.at(0).at(0).toString() == QStringLiteral("distance"));
  }

  // Test 3 & 4: two-field Tab / Backtab / Shift+Tab traversal with selectAll.
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("spacing", 10.0), countParam(2)});
    auto* first = hud.findChild<QDoubleSpinBox*>("spacing");
    auto* second = hud.findChild<QDoubleSpinBox*>("count");
    CHECK(first != nullptr && second != nullptr);
    showAndFocus(hud, first);
    CHECK(QApplication::focusWidget() == first);

    pressKey(first, Qt::Key_Tab);
    CHECK(QApplication::focusWidget() == second);
    auto* secondLine = spinLineEdit(second);
    CHECK(secondLine != nullptr);
    CHECK(secondLine->hasSelectedText());

    pressKey(second, Qt::Key_Backtab);
    CHECK(QApplication::focusWidget() == first);
    auto* firstLine = spinLineEdit(first);
    CHECK(firstLine != nullptr);
    CHECK(firstLine->hasSelectedText());

    // Shift+Tab (delivered as Key_Tab + ShiftModifier) also moves backward.
    pressKey(first, Qt::Key_Tab, Qt::ShiftModifier);
    CHECK(QApplication::focusWidget() == second);

    // Forward Tab from the last editor wraps back to the first.
    pressKey(second, Qt::Key_Tab);
    CHECK(QApplication::focusWidget() == first);
  }

  // Test 5: Escape restores the last committed value and clears focus, with no
  // valueCommitted emission. Also covers the hasFocus staleness fix in setValue.
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);
    QSignalSpy spy(&hud, &solidar::ToolParameterHud::valueCommitted);
    showAndFocus(hud, editor);
    CHECK(editor->hasFocus());

    editor->setValue(25.0);
    CHECK(nearly(editor->value(), 25.0));
    pressKey(editor, Qt::Key_Escape);
    CHECK(spy.count() == 0);
    CHECK(nearly(editor->value(), 10.0));
    CHECK(!editor->hasFocus());

    // setValue while focused must update the committed value without clobbering
    // the in-progress text.
    editor->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    CHECK(editor->hasFocus());
    editor->setValue(7.0);
    hud.setValue("distance", 12.0);
    CHECK(nearly(editor->value(), 7.0));
    pressKey(editor, Qt::Key_Escape);
    CHECK(nearly(editor->value(), 12.0));
  }

  // Test 6: recreating editors (setParameters twice) drops the deleted
  // editor's callbacks; no commit references the removed id afterwards.
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* firstEditor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(firstEditor != nullptr);
    QSignalSpy spy(&hud, &solidar::ToolParameterHud::valueCommitted);
    pressKey(firstEditor, Qt::Key_Return);
    CHECK(spy.count() == 1);
    CHECK(spy.at(0).at(0).toString() == QStringLiteral("distance"));

    hud.setParameters({distanceParam("radius", 20.0)});
    CHECK(hud.findChild<QDoubleSpinBox*>("distance") == nullptr);
    auto* newEditor = hud.findChild<QDoubleSpinBox*>("radius");
    CHECK(newEditor != nullptr);
    pressKey(newEditor, Qt::Key_Return);
    CHECK(spy.count() == 2);
    CHECK(spy.at(1).at(0).toString() == QStringLiteral("radius"));
    for (int i = 1; i < spy.count(); ++i)
      CHECK(spy.at(i).at(0).toString() != QStringLiteral("distance"));
  }

  // Test 7: descriptor-driven traversal for LinearPattern {spacing, count} and
  // CircularPattern {angle, count}; Tab traversal + a single commit per Enter.
  {
    const auto& definitions = solidar::standardPartDesignToolDefinitions();
    const solidar::PartDesignToolDefinition* linear = nullptr;
    const solidar::PartDesignToolDefinition* circular = nullptr;
    for (const auto& definition : definitions) {
      if (definition.kind == solidar::PartDesignToolKind::LinearPattern)
        linear = &definition;
      if (definition.kind == solidar::PartDesignToolKind::CircularPattern)
        circular = &definition;
    }
    CHECK(linear != nullptr && circular != nullptr);

    {
      solidar::ToolParameterHud hud;
      hud.setParameters(linear->parameters);
      auto* spacing = hud.findChild<QDoubleSpinBox*>("spacing");
      auto* count = hud.findChild<QDoubleSpinBox*>("count");
      CHECK(spacing != nullptr && count != nullptr);
      showAndFocus(hud, spacing);
      QSignalSpy linearSpy(&hud, &solidar::ToolParameterHud::valueCommitted);
      pressKey(spacing, Qt::Key_Tab);
      CHECK(QApplication::focusWidget() == count);
      pressKey(count, Qt::Key_Return);
      CHECK(linearSpy.count() == 1);
      CHECK(linearSpy.at(0).at(0).toString() == QStringLiteral("count"));
    }

    {
      solidar::ToolParameterHud hud;
      hud.setParameters(circular->parameters);
      auto* angle = hud.findChild<QDoubleSpinBox*>("angle");
      auto* circularCount = hud.findChild<QDoubleSpinBox*>("count");
      CHECK(angle != nullptr && circularCount != nullptr);
      showAndFocus(hud, angle);
      QSignalSpy circularSpy(&hud, &solidar::ToolParameterHud::valueCommitted);
      pressKey(angle, Qt::Key_Tab);
      CHECK(QApplication::focusWidget() == circularCount);
      pressKey(circularCount, Qt::Key_Enter);
      CHECK(circularSpy.count() == 1);
      CHECK(circularSpy.at(0).at(0).toString() == QStringLiteral("count"));
    }
  }

  // Test 8: Return interprets pending text (value() != typed text) and commits
  // the typed value exactly once; an auto-repeat Return is consumed without a
  // second valueCommitted.
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);
    editor->setLocale(QLocale::c());
    editor->setKeyboardTracking(false);
    showAndFocus(hud, editor);
    auto* line = spinLineEdit(editor);
    CHECK(line != nullptr);
    line->setText(QStringLiteral("42.0"));
    CHECK(nearly(editor->value(), 10.0));
    QSignalSpy spy(&hud, &solidar::ToolParameterHud::valueCommitted);

    pressKey(editor, Qt::Key_Return);
    CHECK(spy.count() == 1);
    CHECK(spy.at(0).at(0).toString() == QStringLiteral("distance"));
    CHECK(nearly(spy.at(0).at(1).toDouble(), 42.0));

    pressKey(editor, Qt::Key_Return, Qt::NoModifier, /*autorep=*/true);
    CHECK(spy.count() == 1);
  }

  // Test 9: Escape restores the committed value, clears focus and re-emits
  // valueChanged with the restored value so a connected listener observes the
  // rollback (session/preview path).
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);
    showAndFocus(hud, editor);
    CHECK(editor->hasFocus());

    QSignalSpy changed(&hud, &solidar::ToolParameterHud::valueChanged);
    editor->setValue(25.0);
    CHECK(nearly(editor->value(), 25.0));
    changed.clear();

    pressKey(editor, Qt::Key_Escape);
    CHECK(nearly(editor->value(), 10.0));
    CHECK(!editor->hasFocus());
    CHECK(changed.count() == 1);
    CHECK(changed.at(0).at(0).toString() == QStringLiteral("distance"));
    CHECK(nearly(changed.at(0).at(1).toDouble(), 10.0));
  }

  // Test 10: a competing parent Return/Enter shortcut does not fire while the
  // HUD editor has focus because the HUD accepts the ShortcutOverride.
  {
    QWidget parent;
    solidar::ToolParameterHud hud(&parent);
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);

    int shortcutFires = 0;
    QShortcut returnShortcut(QKeySequence(Qt::Key_Return), &parent);
    returnShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    returnShortcut.setAutoRepeat(false);
    QObject::connect(&returnShortcut, &QShortcut::activated,
                     [&] { ++shortcutFires; });
    QShortcut keypadShortcut(QKeySequence(Qt::Key_Enter), &parent);
    keypadShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    keypadShortcut.setAutoRepeat(false);
    QObject::connect(&keypadShortcut, &QShortcut::activated,
                     [&] { ++shortcutFires; });

    QSignalSpy commitSpy(&hud, &solidar::ToolParameterHud::valueCommitted);

    parent.resize(320, 240);
    parent.show();
    parent.activateWindow();
    QApplication::processEvents();
    editor->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    CHECK(editor->hasFocus());

    QTest::keyClick(editor, Qt::Key_Return);
    CHECK(shortcutFires == 0);
    CHECK(commitSpy.count() == 1);

    QTest::keyClick(editor, Qt::Key_Enter);
    CHECK(shortcutFires == 0);
    CHECK(commitSpy.count() == 2);
  }

  // Test 11: a synchronous setParameters() rebuild triggered by valueCommitted
  // does not crash and leaves no stale editor/id behind. (A rebuild triggered
  // from valueChanged inside interpretText() hard-deletes the active spinbox on
  // its own stack, which is UB under Qt; the commit path is the safe reentrancy
  // boundary, so the rebuild is triggered there instead.)
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);
    editor->setLocale(QLocale::c());
    editor->setKeyboardTracking(false);
    auto* line = spinLineEdit(editor);
    CHECK(line != nullptr);

    int rebuilds = 0;
    QObject::connect(&hud, &solidar::ToolParameterHud::valueCommitted,
                     [&](const QString& id, double) {
                       if (id == QStringLiteral("distance")) {
                         ++rebuilds;
                         hud.setParameters({distanceParam("radius", 20.0)});
                       }
                     });
    QSignalSpy commitSpy(&hud, &solidar::ToolParameterHud::valueCommitted);

    line->setText(QStringLiteral("42.0"));
    pressKey(editor, Qt::Key_Return);

    CHECK(rebuilds == 1);
    CHECK(hud.findChild<QDoubleSpinBox*>("distance") == nullptr);
    // The commit fired exactly once for "distance" before the rebuild deleted
    // the editor; no stale emit for the deleted id occurred afterwards.
    CHECK(commitSpy.count() == 1);
    CHECK(commitSpy.at(0).at(0).toString() == QStringLiteral("distance"));
  }

  // Test 12: public focus/introspection API — hasEditableParameters /
  // hasFieldFocus / focusFirstField / focusLastField / focusNextField.
  {
    solidar::ToolParameterHud hud;
    CHECK(!hud.hasEditableParameters());
    CHECK(!hud.hasFieldFocus());

    hud.setParameters({distanceParam("spacing", 10.0), countParam(2)});
    CHECK(hud.hasEditableParameters());
    CHECK(!hud.hasFieldFocus());
    auto* first = hud.findChild<QDoubleSpinBox*>("spacing");
    auto* second = hud.findChild<QDoubleSpinBox*>("count");
    CHECK(first != nullptr && second != nullptr);

    showAndFocus(hud, first);
    CHECK(hud.hasFieldFocus());
    CHECK(QApplication::focusWidget() == first);

    // focusFirstField focuses the first editor and selects its text.
    hud.focusFirstField();
    CHECK(QApplication::focusWidget() == first);
    auto* firstLine = spinLineEdit(first);
    CHECK(firstLine != nullptr);
    CHECK(firstLine->hasSelectedText());

    // focusLastField focuses the last editor and selects its text.
    hud.focusLastField();
    CHECK(QApplication::focusWidget() == second);
    auto* secondLine = spinLineEdit(second);
    CHECK(secondLine != nullptr);
    CHECK(secondLine->hasSelectedText());

    // focusNextField(false) from the last wraps forward to the first.
    hud.focusNextField(false);
    CHECK(QApplication::focusWidget() == first);
    CHECK(spinLineEdit(first)->hasSelectedText());

    // focusNextField(true) from the first wraps backward to the last.
    hud.focusNextField(true);
    CHECK(QApplication::focusWidget() == second);
    CHECK(spinLineEdit(second)->hasSelectedText());

    // Forward from first advances to second.
    hud.focusFirstField();
    hud.focusNextField(false);
    CHECK(QApplication::focusWidget() == second);
    CHECK(spinLineEdit(second)->hasSelectedText());

    // Backward from second returns to first.
    hud.focusNextField(true);
    CHECK(QApplication::focusWidget() == first);
    CHECK(spinLineEdit(first)->hasSelectedText());
  }

  // Test 13: single parameter — focusFirstField focuses it and selects text;
  // focusLastField focuses the same (only) field.
  {
    solidar::ToolParameterHud hud;
    hud.setParameters({distanceParam("distance", 10.0)});
    CHECK(hud.hasEditableParameters());
    auto* editor = hud.findChild<QDoubleSpinBox*>("distance");
    CHECK(editor != nullptr);

    showAndFocus(hud, nullptr);
    hud.focusFirstField();
    CHECK(QApplication::focusWidget() == editor);
    auto* line = spinLineEdit(editor);
    CHECK(line != nullptr);
    CHECK(line->hasSelectedText());
    CHECK(hud.hasFieldFocus());

    hud.focusLastField();
    CHECK(QApplication::focusWidget() == editor);
    CHECK(spinLineEdit(editor)->hasSelectedText());

    // focusNextField on a single field re-focuses the same field.
    hud.focusNextField(false);
    CHECK(QApplication::focusWidget() == editor);
    hud.focusNextField(true);
    CHECK(QApplication::focusWidget() == editor);
  }

  return EXIT_SUCCESS;
}

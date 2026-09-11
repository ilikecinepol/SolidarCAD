#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeySequence>
#include <QLineEdit>
#include <QLocale>
#include <QShortcut>
#include <QTest>
#include <QWidget>

#include <cmath>
#include <cstdlib>
#include <iostream>

#include "ui/ToolParametersPanel.h"
#include "ui/tools/PartDesignToolController.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
class Session final : public solidar::ToolSession {
 public:
  solidar::ToolLifecycle lifecycle() const noexcept override { return state; }
  std::shared_ptr<const TopoDS_Shape> previewShape() const override { return {}; }
  const std::string& error() const noexcept override { return message; }
  bool updatePreview() override { state = solidar::ToolLifecycle::PreviewValid; return true; }
  void cancel() noexcept override { ++cancels; state = solidar::ToolLifecycle::Inactive; }
  solidar::ToolLifecycle state{solidar::ToolLifecycle::SelectingInput};
  int cancels{};
  std::string message;
};

bool nearly(double a, double b) { return std::abs(a - b) < 1e-9; }
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  // Controller lifecycle: switching tools and escaping must tear every session
  // down exactly once, with no surviving active tool or reselection state.
  {
    solidar::PartDesignToolController controller;
    Session extrude, revolve, mirror, circular;
    int presentationClears = 0;
    const auto add = [&](solidar::PartDesignToolKind kind, Session& session) {
      controller.registerTool(kind, {&session, [&session] { session.cancel(); },
                                     [&] { ++presentationClears; }});
    };
    add(solidar::PartDesignToolKind::Extrude, extrude);
    add(solidar::PartDesignToolKind::Revolve, revolve);
    add(solidar::PartDesignToolKind::Mirror, mirror);
    add(solidar::PartDesignToolKind::CircularPattern, circular);
    add(solidar::PartDesignToolKind::Extrude, extrude);
    CHECK(controller.registrationCount() == 4);

    for (int pass = 0; pass < 20; ++pass) {
      controller.activate(solidar::PartDesignToolKind::Extrude);
      CHECK(controller.activeTool() == solidar::PartDesignToolKind::Extrude);
      controller.activate(solidar::PartDesignToolKind::Revolve);
      CHECK(controller.activeTool() == solidar::PartDesignToolKind::Revolve);
      controller.beginReselection(solidar::ToolSelectionStage::SelectingReference);
      CHECK(controller.handleEscape());
      controller.activate(solidar::PartDesignToolKind::Mirror);
      controller.activate(solidar::PartDesignToolKind::CircularPattern);
      controller.cancelActive();
      CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);
      CHECK(!controller.isReselecting());
    }
    CHECK(extrude.cancels == 20);
    CHECK(revolve.cancels == 20);
    CHECK(mirror.cancels == 20);
    CHECK(circular.cancels == 20);
    CHECK(presentationClears == 80);
  }

  // Test 8: panel + accept shortcut — Return on the focused spinbox produces
  // exactly one accept, not two (shortcut + removed returnPressed path).
  {
    int shortcutFires = 0;
    int panelAccepts = 0;
    int totalAccepts = 0;
    QWidget window;
    solidar::ToolParametersPanel panel(&window);
    panel.setAcceptEnabled(true);
    QShortcut returnShortcut(QKeySequence(Qt::Key_Return), &window);
    returnShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    returnShortcut.setAutoRepeat(false);
    QObject::connect(&returnShortcut, &QShortcut::activated, [&] {
      ++shortcutFires;
      ++totalAccepts;
    });
    QShortcut keypadShortcut(QKeySequence(Qt::Key_Enter), &window);
    keypadShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    keypadShortcut.setAutoRepeat(false);
    QObject::connect(&keypadShortcut, &QShortcut::activated, [&] {
      ++shortcutFires;
      ++totalAccepts;
    });
    QObject::connect(&panel, &solidar::ToolParametersPanel::accepted, [&] {
      ++panelAccepts;
      ++totalAccepts;
    });

    window.resize(320, 240);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    panel.focusParameterInput();
    QApplication::processEvents();
    QWidget* focus = QApplication::focusWidget();
    CHECK(focus != nullptr);

    QTest::keyClick(focus, Qt::Key_Return);
    CHECK(shortcutFires == 1);
    CHECK(panelAccepts == 0);
    CHECK(totalAccepts == 1);

    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Enter);
    CHECK(shortcutFires == 2);
    CHECK(panelAccepts == 0);
    CHECK(totalAccepts == 2);
  }

  // Test 9: disabled/invalid tool — Return does not complete the tool.
  {
    int completes = 0;
    QWidget window;
    solidar::ToolParametersPanel panel(&window);
    QShortcut returnShortcut(QKeySequence(Qt::Key_Return), &window);
    returnShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    returnShortcut.setAutoRepeat(false);
    QObject::connect(&returnShortcut, &QShortcut::activated, [&] {
      if (!panel.acceptEnabled()) return;
      ++completes;
    });

    panel.setAcceptEnabled(false);
    window.resize(320, 240);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    panel.focusParameterInput();
    QApplication::processEvents();
    CHECK(QApplication::focusWidget() != nullptr);

    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    CHECK(completes == 0);

    panel.setAcceptEnabled(true);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    CHECK(completes == 1);
  }

  // Test 10: legacy Extrude — Return and keypad Enter each fire exactly once,
  // and the visibility guard suppresses activation when the dock is hidden.
  {
    int extrudes = 0;
    QWidget dock;
    dock.setFocusPolicy(Qt::StrongFocus);
    dock.resize(320, 240);
    dock.show();
    dock.activateWindow();
    QApplication::processEvents();
    dock.setFocus();
    QApplication::processEvents();
    const auto applyExtrusionOnEnter = [&] {
      if (dock.isVisible()) ++extrudes;
    };
    QShortcut returnShortcut(QKeySequence(Qt::Key_Return), &dock);
    returnShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    returnShortcut.setAutoRepeat(false);
    QObject::connect(&returnShortcut, &QShortcut::activated,
                     applyExtrusionOnEnter);
    QShortcut keypadEnterShortcut(QKeySequence(Qt::Key_Enter), &dock);
    keypadEnterShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    keypadEnterShortcut.setAutoRepeat(false);
    QObject::connect(&keypadEnterShortcut, &QShortcut::activated,
                     applyExtrusionOnEnter);

    QTest::keyClick(&dock, Qt::Key_Return);
    QTest::keyClick(&dock, Qt::Key_Enter);
    CHECK(extrudes == 2);

    dock.hide();
    QApplication::processEvents();
    QTest::keyClick(&dock, Qt::Key_Return);
    CHECK(extrudes == 2);
  }

  // Test 11: project/tool switch leaves no stale accept and no dangling
  // callbacks after the dock/panel teardown.
  {
    int accepts = 0;
    auto* dock = new QWidget;
    auto* panel = new solidar::ToolParametersPanel(dock);
    dock->resize(320, 240);
    dock->show();
    dock->activateWindow();
    QApplication::processEvents();
    auto* returnShortcut = new QShortcut(QKeySequence(Qt::Key_Return), dock);
    returnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    returnShortcut->setAutoRepeat(false);
    QObject::connect(returnShortcut, &QShortcut::activated, [&] {
      if (panel->acceptEnabled()) ++accepts;
    });

    panel->setAcceptEnabled(true);
    panel->focusParameterInput();
    QApplication::processEvents();
    QWidget* focus = QApplication::focusWidget();
    CHECK(focus != nullptr);
    QTest::keyClick(focus, Qt::Key_Return);
    CHECK(accepts == 1);

    delete dock;  // tears down the panel, spinbox and the shortcut together
    QApplication::processEvents();

    QWidget fresh;
    fresh.show();
    fresh.activateWindow();
    QApplication::processEvents();
    QTest::keyClick(&fresh, Qt::Key_Return);
    CHECK(accepts == 1);
  }

  // Test 12: dock-style accept — interpretParameterText() commits pending
  // typed text before the accept handler reads the value. A full MainWindow
  // dock fixture is not constructed here (heavy); the viewport HUD exercises
  // the same interpretText-before-commit contract in tool_parameter_hud_tests.
  {
    QWidget window;
    solidar::ToolParametersPanel panel(&window);
    panel.setParameterRange(0.0, 100000.0, 2);
    panel.setParameterValue(10.0);
    panel.setAcceptEnabled(true);

    double acceptedValue = -1.0;
    QShortcut returnShortcut(QKeySequence(Qt::Key_Return), &window);
    returnShortcut.setContext(Qt::WidgetWithChildrenShortcut);
    returnShortcut.setAutoRepeat(false);
    QObject::connect(&returnShortcut, &QShortcut::activated, [&] {
      panel.interpretParameterText();
      if (panel.acceptEnabled()) acceptedValue = panel.parameterValue();
    });

    window.resize(320, 240);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    panel.focusParameterInput();
    QApplication::processEvents();
    QWidget* focus = QApplication::focusWidget();
    CHECK(focus != nullptr);

    auto* parameterSpin = panel.findChild<QDoubleSpinBox*>();
    CHECK(parameterSpin != nullptr);
    parameterSpin->setLocale(QLocale::c());
    parameterSpin->setKeyboardTracking(false);

    auto* line = panel.findChild<QLineEdit*>();
    CHECK(line != nullptr);
    line->setText(QStringLiteral("42.5"));
    CHECK(nearly(panel.parameterValue(), 10.0));

    QTest::keyClick(focus, Qt::Key_Return);
    CHECK(nearly(panel.parameterValue(), 42.5));
    CHECK(nearly(acceptedValue, 42.5));
  }

  return EXIT_SUCCESS;
}

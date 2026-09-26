#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QAction>
#include <QDockWidget>
#include <QDir>
#include <QMessageBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <cstdlib>
#include <iostream>
#include <memory>

#include "app/AppSettings.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletToolSession.h"
#include "project/ProjectFile.h"
#include "ui/MainWindow.h"
#include "ui/SketchCanvas.h"
#include "ui/Viewport.h"
#include "ui/tools/PartDesignToolController.h"

namespace solidar {

class MainWindowUndoTestAccess {
 public:
  static void reset(MainWindow& window) {
    window.modelUndoStack_.clear();
    window.modelRedoStack_.clear();
    window.updateUndoAvailability();
  }
  static void pushUndo(MainWindow& window, std::function<void()> undo) {
    window.pushUndoAction(std::move(undo));
  }
  static void pushUndoRedo(MainWindow& window, std::function<void()> undo,
                           std::function<void()> redo) {
    window.pushUndoRedoAction(std::move(undo), std::move(redo));
  }
  static std::size_t redoCount(const MainWindow& window) {
    return window.modelRedoStack_.size();
  }
  static QAction* undoAction(MainWindow& window) { return window.undoAction_; }
  static QAction* redoAction(MainWindow& window) { return window.redoAction_; }
  static QStackedWidget* workspace(MainWindow& window) {
    return window.workspaceStack_;
  }
  static SketchCanvas* sketch(MainWindow& window) { return window.sketchCanvas_; }
  static Viewport* viewport(MainWindow& window) { return window.viewport_; }
  static void removeBody(MainWindow& window, BodyId id) {
    window.removeBody(id);
  }
  static std::size_t bodyCount(const MainWindow& window) {
    return window.document_.bodies().size();
  }
  static std::size_t sketchHistoryCount(const MainWindow& window) {
    return window.sketchHistory_.size();
  }
  static std::size_t sketchCount(const MainWindow& window) {
    return window.sketchCount_;
  }
  static bool extrusionSourceValid(const MainWindow& window) {
    return !window.extrusionSourceSketch_ ||
           *window.extrusionSourceSketch_ < window.sketchHistory_.size();
  }
};

}  // namespace solidar

// Every critical check uses CHECK (not assert) so it remains active in the
// Release CI build where NDEBUG is defined.
#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-switch-ui-tests-XXXXXX")));
  CHECK(directory.isValid());

  // Project A carries a real body so the replacement tears down actual B-Rep
  // state, not an empty shell.
  {
    solidar::Document document;
    auto& baseSketch = document.addSketch("Base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    auto& body = document.addBody("Body");
    body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 15.0, "Extrude"));
    CHECK(document.recompute());
    QString error;
    CHECK(solidar::project::ProjectFile::saveDocument(
        directory.filePath(QStringLiteral("a.solidar")), document, &error));
  }

  QString error;
  const QString pathA = directory.filePath(QStringLiteral("a.solidar"));
  const QString pathB = directory.filePath(QStringLiteral("b.solidar"));
  CHECK(solidar::project::ProjectFile::create(pathB, &error));

  // Headless document replacement: repeated loadProject drives the full
  // teardown path (PartDesignToolController cancelActive, session cancel,
  // resetScene, history rebuild). No file dialogs, no OpenGL surface, no
  // platform-specific code.
  solidar::AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
  solidar::MainWindow editor(settings);
  for (int cycle = 0; cycle < 6; ++cycle) {
    CHECK(editor.loadProject(pathA, &error));
    CHECK(editor.loadProject(pathB, &error));
  }

  // Switching between the legacy extrusion picker, the dedicated Revolve
  // panel and the shared Part Design panel must leave exactly one tool UI.
  // This used to reproduce after a longer session because these three paths
  // had independent teardown code.
  CHECK(editor.loadProject(pathA, &error));

  // Model and Sketch histories are separate domains. A new committed model
  // action invalidates a stale model redo, and switching workspaces exposes
  // only the active domain's availability.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    Access::reset(editor);
    int modelState = 1;
    Access::pushUndoRedo(editor, [&] { modelState = 0; },
                         [&] { modelState = 1; });
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());
    Access::undoAction(editor)->trigger();
    CHECK(modelState == 0);
    CHECK(Access::redoCount(editor) == 1);
    CHECK(Access::redoAction(editor)->isEnabled());

    modelState = 2;
    Access::pushUndo(editor, [&] { modelState = 0; });
    CHECK(Access::redoCount(editor) == 0);
    CHECK(!Access::redoAction(editor)->isEnabled());

    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    Access::workspace(editor)->setCurrentWidget(canvas);
    QApplication::processEvents();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());

    canvas->setRectangle(12.0, 8.0);
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());
    Access::undoAction(editor)->trigger();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(Access::redoAction(editor)->isEnabled());

    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
    QApplication::processEvents();
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());

    Access::workspace(editor)->setCurrentWidget(canvas);
    QApplication::processEvents();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(Access::redoAction(editor)->isEnabled());
    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
  }

  auto* filletButton = editor.findChild<QToolButton*>(
      QStringLiteral("filletCommand"));
  auto* extrudeButton = editor.findChild<QToolButton*>(
      QStringLiteral("extrudeCommand"));
  auto* revolveButton = editor.findChild<QToolButton*>(
      QStringLiteral("revolveCommand"));
  auto* partDesignDock = editor.findChild<QDockWidget*>(
      QStringLiteral("partDesignParametersDock"));
  auto* extrusionDock = editor.findChild<QDockWidget*>(
      QStringLiteral("extrusionParametersDock"));
  auto* revolveDock = editor.findChild<QDockWidget*>(
      QStringLiteral("revolveParametersDock"));
  CHECK(filletButton != nullptr);
  CHECK(extrudeButton != nullptr);
  CHECK(revolveButton != nullptr);
  CHECK(partDesignDock != nullptr);
  CHECK(extrusionDock != nullptr);
  CHECK(revolveDock != nullptr);

  filletButton->click();
  QApplication::processEvents();
  CHECK(!partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(revolveDock->isHidden());

  extrudeButton->click();
  QApplication::processEvents();
  CHECK(partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());  // still selecting an input profile
  CHECK(revolveDock->isHidden());

  revolveButton->click();
  QApplication::processEvents();
  CHECK(partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(!revolveDock->isHidden());

  filletButton->click();
  QApplication::processEvents();
  CHECK(!partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(revolveDock->isHidden());

  // Active-tool teardown at the controller/session layer. A live Fillet
  // session holds B-Rep references and a preview; the same cancelActive +
  // cancel sequence MainWindow::loadProject performs must leave the controller
  // and session fully inactive, with no surviving preview or selection state.
  {
    solidar::Document document;
    auto& baseSketch = document.addSketch("Base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    auto& body = document.addBody("Body");
    auto extrude = std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 15.0, "Extrude");
    const solidar::FeatureId featureId = extrude->id();
    body.addFeature(std::move(extrude));
    CHECK(document.recompute());
    const auto shape = body.resultShape();
    CHECK(shape != nullptr);

    solidar::PartDesignToolController controller;
    solidar::FilletToolSession fillet;
    controller.registerTool(solidar::PartDesignToolKind::Fillet,
                            {&fillet, [&] { fillet.cancel(); }, {}});
    controller.activate(solidar::PartDesignToolKind::Fillet);
    CHECK(controller.activeTool() == solidar::PartDesignToolKind::Fillet);
    CHECK(controller.activeSession() == &fillet);

    // Zero radius keeps the preview equal to the base shape, so the session is
    // deterministically active and holding B-Rep without depending on whether
    // a particular edge is fillet-able.
    fillet.begin(body.id(), featureId, shape,
                 {solidar::EdgeReference{body.id(), featureId, 0}}, 0.0);
    CHECK(fillet.lifecycle() != solidar::ToolLifecycle::Inactive);
    CHECK(fillet.previewShape() != nullptr);

    // Mirror the teardown order of MainWindow::loadProject.
    controller.cancelActive();
    fillet.cancel();

    CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);
    CHECK(controller.activeSession() == nullptr);
    CHECK(fillet.lifecycle() == solidar::ToolLifecycle::Inactive);
    CHECK(fillet.previewShape() == nullptr);
    CHECK(fillet.edges().empty());
  }

  // Removing a Body must synchronize the legacy sketch count/source index
  // before rebuilding the tree. Exercise the real confirmation plus Undo/Redo;
  // stale sketchCount_ previously caused an out-of-bounds tree rebuild here.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document twoBodies;
    auto& firstSketch = twoBodies.addSketch("First profile");
    firstSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& firstBody = twoBodies.addBody("First body");
    const auto removedBodyId = firstBody.id();
    firstBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        firstSketch.id, 5.0));
    auto& secondSketch = twoBodies.addSketch("Second profile");
    secondSketch.geometry.addRectangle({30.0, 0.0}, {42.0, 10.0});
    auto& secondBody = twoBodies.addBody("Second body");
    secondBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        secondSketch.id, 7.0));
    CHECK(twoBodies.recompute());
    const QString twoBodiesPath =
        directory.filePath(QStringLiteral("two-bodies-delete.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        twoBodiesPath, twoBodies, &error));
    CHECK(editor.loadProject(twoBodiesPath, &error));
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::sketchCount(editor) == 2);

    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, removedBodyId);
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::sketchHistoryCount(editor) == 2);
    CHECK(Access::sketchCount(editor) == 2);
    CHECK(Access::extrusionSourceValid(editor));

    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::sketchHistoryCount(editor) == 2);
    CHECK(Access::sketchCount(editor) == 2);
    CHECK(Access::extrusionSourceValid(editor));

    Access::redoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::sketchHistoryCount(editor) == 2);
    CHECK(Access::sketchCount(editor) == 2);
    CHECK(Access::extrusionSourceValid(editor));
  }

  return EXIT_SUCCESS;
}

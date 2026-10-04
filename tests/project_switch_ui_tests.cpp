#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QAction>
#include <QDockWidget>
#include <QDir>
#include <QKeyEvent>
#include <QMessageBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <cstdlib>
#include <iostream>
#include <memory>

#include "TestGeometryUtils.h"
#include "app/AppSettings.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletToolSession.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"
#include "ui/MainWindow.h"
#include "ui/SketchCanvas.h"
#include "ui/ToolParametersPanel.h"
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
  static bool modified(const MainWindow& window) {
    return window.isWindowModified();
  }
  static void save(MainWindow& window) { window.saveProject(); }
  static void startSketchExtrude(MainWindow& window, std::size_t index) {
    window.createSketchExtrude(index);
  }
  static bool startTopFaceExtrude(MainWindow& window, double zMm) {
    Body* body = window.document_.activeBody();
    if (!body || !body->activeFeature() || !body->resultShape()) return false;
    const auto faceIndex = test::topPlanarFace(*body->resultShape(), zMm);
    if (!faceIndex) return false;
    window.createFaceExtrude(makeFaceReference(
        *body->resultShape(), body->id(), body->activeFeature()->id(),
        *faceIndex));
    return true;
  }
  static void dragDirectExtrude(MainWindow& window, double length) {
    emit window.viewport_->toolManipulatorValueChanged(length);
    QApplication::processEvents();
  }
  static bool pressDirectExtrudeKey(MainWindow& window, Qt::Key key) {
    window.toolParametersPanel_->focusParameterInput();
    QApplication::processEvents();
    QWidget* focus = QApplication::focusWidget();
    if (!focus) return false;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(focus, &press);
    QApplication::sendEvent(focus, &release);
    QApplication::processEvents();
    return true;
  }
  static void commitDirectExtrudeFromViewport(MainWindow& window) {
    emit window.viewport_->toolParameterCommitted();
    QApplication::processEvents();
  }
  static ToolLifecycle directExtrudeLifecycle(const MainWindow& window) {
    return window.faceExtrudeSession_.lifecycle();
  }
  static ExtrudeOperation directExtrudeOperation(const MainWindow& window) {
    return window.faceExtrudeSession_.operation();
  }
  static bool directExtrudeReversed(const MainWindow& window) {
    return window.faceExtrudeSession_.reversed();
  }
  static ShapeFeature::ShapePtr directExtrudePreview(const MainWindow& window) {
    return window.faceExtrudeSession_.previewShape();
  }
  static PartDesignToolKind activeTool(const MainWindow& window) {
    return window.partDesignTools_.activeTool();
  }
  static std::size_t undoCount(const MainWindow& window) {
    return window.modelUndoStack_.size();
  }
  static std::size_t bodyFeatureCount(const MainWindow& window) {
    const Body* body = window.document_.activeBody();
    return body ? body->features().size() : 0;
  }
  static ShapeFeature::ShapePtr bodyShape(const MainWindow& window) {
    const Body* body = window.document_.activeBody();
    return body ? body->resultShape() : ShapeFeature::ShapePtr{};
  }
  static ShapeFeature::ShapePtr displayedBodyShape(const MainWindow& window) {
    return window.viewport_->bodyShape_;
  }
  static bool viewportHasToolPreview(const MainWindow& window) {
    return (window.viewport_->toolPreviewShape_ &&
            !window.viewport_->toolPreviewShape_->IsNull()) ||
           (window.viewport_->toolCutPreviewShape_ &&
            !window.viewport_->toolCutPreviewShape_->IsNull());
  }
  static std::size_t historyStepCount(const MainWindow& window) {
    return window.historySteps_.size();
  }
  static void applyHistoryPosition(MainWindow& window, int position) {
    window.applyHistoryPosition(position);
  }
  static void moveHistoryToEnd(MainWindow& window) {
    window.moveHistoryToEnd();
    window.applyHistoryPosition(window.historyPosition_);
  }
  static bool historyAtEnd(const MainWindow& window) {
    return window.isHistoryAtEnd();
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

  // Save must commit the sketcher's working copy before serializing.  Without
  // this, the UI reported success while the visible sketch was absent from
  // the file on disk.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    canvas->setRectangle(12.0, 8.0);
    CHECK(Access::modified(editor));
    Access::workspace(editor)->setCurrentWidget(canvas);
    Access::save(editor);
    CHECK(!Access::modified(editor));

    solidar::Document restored;
    CHECK(solidar::project::ProjectFile::loadDocument(pathB, &restored,
                                                       &error));
    CHECK(restored.sketches().size() == 1);
    CHECK(!restored.sketches().front().geometry.lines().empty());
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
    CHECK(Access::modified(editor));
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

  // End-to-end direct Sketch-on-Face Extrude: signed drag chooses outward
  // Join or inward Cut, commit creates one history entry, transient state is
  // cleared, and Undo/Redo plus history scrubbing preserve an editable model.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document document;
    auto& baseSketch = document.addSketch("Direct Extrude base");
    baseSketch.geometry.addRectangle({-20.0, -15.0}, {20.0, 15.0});
    auto& body = document.addBody("Direct Extrude body");
    auto base = std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 20.0, "Extrude");
    const auto baseId = base->id();
    body.addFeature(std::move(base));
    CHECK(document.recompute());
    const auto topFace =
        solidar::test::topPlanarFace(*body.resultShape(), 20.0);
    CHECK(topFace);
    const auto support = solidar::makeFaceReference(
        *body.resultShape(), body.id(), baseId, *topFace);
    auto& faceSketch = document.addSketch("Direct Extrude face profile");
    CHECK(document.attachSketchToFace(faceSketch.id, support));
    faceSketch.geometry.addRectangle({-5.0, -5.0}, {5.0, 5.0});
    CHECK(document.recompute());

    const QString directPath =
        directory.filePath(QStringLiteral("direct-face-extrude.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        directPath, document, &error));
    solidar::MainWindow directEditor(settings);
    CHECK(directEditor.loadProject(directPath, &error));
    directEditor.show();
    directEditor.activateWindow();
    QApplication::processEvents();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    const auto baseShape = Access::bodyShape(directEditor);
    CHECK(baseShape);
    const double baseVolume = solidar::test::volumeOf(*baseShape);

    Access::startSketchExtrude(directEditor, 1);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudeOperation(directEditor) ==
          solidar::ExtrudeOperation::Join);
    CHECK(!Access::directExtrudeReversed(directEditor));
    CHECK(Access::directExtrudePreview(directEditor));
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::activeTool(directEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Escape));
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::Inactive);
    CHECK(!Access::directExtrudePreview(directEditor));
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::undoCount(directEditor) == 0);

    Access::startSketchExtrude(directEditor, 1);
    Access::dragDirectExtrude(directEditor, 8.0);
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Return));
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 0);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::Inactive);
    CHECK(!Access::directExtrudePreview(directEditor));
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(Access::activeTool(directEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(Access::viewport(directEditor)->selectedBodyFaces().empty());
    CHECK(Access::viewport(directEditor)->selectionFilter() ==
          solidar::SelectionFilter::Any);
    CHECK(!Access::viewport(directEditor)->linearToolManipulator());
    const auto joinedShape = Access::bodyShape(directEditor);
    CHECK(joinedShape);
    CHECK(solidar::test::solidCount(*joinedShape) == 1);
    const double joinedVolume = solidar::test::volumeOf(*joinedShape);
    CHECK(joinedVolume > baseVolume);

    // A second Enter/Apply after completion is a no-op, not a duplicate node.
    Access::commitDirectExtrudeFromViewport(directEditor);
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);

    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 1);
    Access::redoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::bodyShape(directEditor)),
        joinedVolume, 1e-4));

    CHECK(Access::historyStepCount(directEditor) == 4);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        joinedVolume, 1e-4));
    Access::applyHistoryPosition(directEditor, 2);
    CHECK(!Access::historyAtEnd(directEditor));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        baseVolume, 1e-4));
    Access::moveHistoryToEnd(directEditor);
    CHECK(Access::historyAtEnd(directEditor));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        joinedVolume, 1e-4));

    // Returning to the tip must restore an immediately editable model.
    CHECK(Access::startTopFaceExtrude(directEditor, 28.0));
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Escape));
    CHECK(!Access::viewportHasToolPreview(directEditor));

    // Return to the base and create the inward Cut through the same UI path.
    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    Access::startSketchExtrude(directEditor, 1);
    Access::dragDirectExtrude(directEditor, -6.0);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudeOperation(directEditor) ==
          solidar::ExtrudeOperation::Cut);
    CHECK(Access::directExtrudeReversed(directEditor));
    CHECK(solidar::test::volumeOf(
              *Access::directExtrudePreview(directEditor)) < baseVolume);
    CHECK(Access::viewportHasToolPreview(directEditor));
    Access::commitDirectExtrudeFromViewport(directEditor);
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 0);
    const auto cutShape = Access::bodyShape(directEditor);
    CHECK(cutShape);
    CHECK(solidar::test::solidCount(*cutShape) == 1);
    const double cutVolume = solidar::test::volumeOf(*cutShape);
    CHECK(cutVolume < baseVolume);
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(!Access::viewport(directEditor)->linearToolManipulator());
    CHECK(Access::viewport(directEditor)->selectedBodyFaces().empty());

    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    Access::redoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::bodyShape(directEditor)), cutVolume,
        1e-4));
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

  // Closing a modified project must never discard work without an explicit
  // choice. Cancel keeps the editor alive; Discard permits the close.
  editor.show();
  QApplication::processEvents();
  CHECK(solidar::MainWindowUndoTestAccess::modified(editor));
  QTimer::singleShot(0, [] {
    for (QWidget* widget : QApplication::topLevelWidgets())
      if (auto* box = qobject_cast<QMessageBox*>(widget))
        box->done(QMessageBox::Cancel);
  });
  CHECK(!editor.close());
  CHECK(editor.isVisible());

  QTimer::singleShot(0, [] {
    for (QWidget* widget : QApplication::topLevelWidgets())
      if (auto* box = qobject_cast<QMessageBox*>(widget))
        box->done(QMessageBox::Discard);
  });
  CHECK(editor.close());

  return EXIT_SUCCESS;
}

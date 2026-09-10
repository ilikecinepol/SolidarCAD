#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

#include "model/ExtrudeFeature.h"
#include "model/FilletToolSession.h"
#include "project/ProjectFile.h"
#include "ui/MainWindow.h"
#include "ui/tools/PartDesignToolController.h"

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
  solidar::MainWindow editor;
  for (int cycle = 0; cycle < 6; ++cycle) {
    CHECK(editor.loadProject(pathA, &error));
    CHECK(editor.loadProject(pathB, &error));
  }

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

  return EXIT_SUCCESS;
}

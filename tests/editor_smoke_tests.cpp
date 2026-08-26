#include <QApplication>
#include <QPixmap>
#include <QTimer>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <memory>
#include <optional>

#include "home/HomeWindow.h"
#include "model/ExtrudeFeature.h"
#include "ui/MainWindow.h"
#include "ui/SketchCanvas.h"

int main(int argc, char** argv) {
  QApplication application(argc, argv);

  // Regression: a Sketch-on-Face canvas receives the actual circular support
  // feature and its real topological boundary, not BoxParameters.
  solidar::Document document;
  auto& profile = document.addSketch("Circular profile");
  profile.geometry.addCircle({0.0, 0.0}, 20.0);
  auto& body = document.addBody();
  auto extrusion = std::make_unique<solidar::ExtrudeFeature>(
      profile.id, 30.0, "Cylinder");
  const auto featureId = extrusion->id();
  body.addFeature(std::move(extrusion));
  assert(document.rebuild());
  std::optional<std::size_t> topFace;
  for (std::size_t index = 0; index < 8; ++index) {
    const auto resolved =
        solidar::resolveFacePlacement(*body.resultShape(), index);
    if (resolved.planar && resolved.placement.normal().z > 0.9 &&
        std::abs(resolved.placement.origin.z - 30.0) < 1e-6) {
      topFace = index;
      break;
    }
  }
  assert(topFace);
  const auto placement =
      solidar::resolveFacePlacement(*body.resultShape(), *topFace).placement;
  solidar::SketchCanvas canvas;
  canvas.resize(900, 650);
  canvas.setSketchEditContext(
      {solidar::kInvalidSketchId, placement, body.resultShape(),
       solidar::FaceReference{body.id(), featureId, *topFace}});
  assert(canvas.hasRealReferenceBody());
  assert(canvas.referenceFaceEdgeCount() >= 1);
  assert(canvas.referenceFaceEdgeCount() <= 2);
  const QPixmap renderedCanvas = canvas.grab();
  assert(!renderedCanvas.isNull());

  solidar::home::HomeWindow home;
  solidar::MainWindow* editor = nullptr;
  QObject::connect(&home, &solidar::home::HomeWindow::projectRequested,
                   &application, [&](const QString& path) {
    editor = new solidar::MainWindow;
    editor->setProjectPath(path);
    home.hide();
    // Construction/destruction is tested without exposing a native OpenGL
    // surface; rendering is covered by the application-level smoke launch.
  });
  home.show();
  emit home.projectRequested(QStringLiteral("C:/Temp/test.solidar"));
  QTimer::singleShot(700, &application, &QApplication::quit);
  const int result = application.exec();
  delete editor;
  return result;
}

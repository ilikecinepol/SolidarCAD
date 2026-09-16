#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QTemporaryDir>
#include <QTimer>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "TestGeometryUtils.h"
#include "app/AppSettings.h"
#include "home/HomeWindow.h"
#include "model/ExtrudeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "ui/MainWindow.h"
#include "ui/SketchCanvas.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition))                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                        \
                        std::to_string(__LINE__) + ": " #condition);         \
  } while (false)

solidar::SketchEditContext faceContext(const TopoDS_Shape& shape,
                                       double topZ, bool autoProject) {
  const auto face = solidar::test::topPlanarFace(shape, topZ);
  CHECK(face);
  const auto placement = solidar::resolveFacePlacement(shape, *face).placement;
  return {solidar::kInvalidSketchId, placement,
          std::make_shared<TopoDS_Shape>(shape),
          solidar::makeFaceReference(shape, 1, 1, *face), autoProject};
}

std::set<std::size_t> projectedElements(const solidar::SketchCanvas& canvas) {
  std::set<std::size_t> result;
  const auto& sketch = canvas.sketch();
  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    const auto& line = sketch.lines()[index];
    if (!line.dashed) continue;
    result.insert(line.elementId);
    CHECK(sketch.isGeometryLocked(sketch.lineId(index)));
  }
  return result;
}

void autoProjectionRegressionTests() {
  // A/C: only a new face-supported sketch receives the rectangular boundary.
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
  solidar::SketchCanvas rectangular;
  rectangular.resize(900, 650);
  rectangular.setSketchEditContext(faceContext(box, 20.0, true));
  CHECK(rectangular.hasRealReferenceBody());
  CHECK(rectangular.referenceFaceEdgeCount() == 4);
  CHECK(rectangular.sketch().lines().size() == 4);
  CHECK(projectedElements(rectangular).size() == 4);
  CHECK(rectangular.sketch().constraints().size() == 4);
  CHECK(!rectangular.canUndo());
  const QPixmap renderedCanvas = rectangular.grab();
  CHECK(!renderedCanvas.isNull());

  const auto automaticLineCount = rectangular.sketch().lines().size();
  for (std::size_t edge = 0; edge < rectangular.referenceBodyEdgeCount();
       ++edge)
    CHECK(!rectangular.projectReferenceEdge(edge));
  CHECK(rectangular.sketch().lines().size() == automaticLineCount);
  CHECK(projectedElements(rectangular).size() == 4);
  CHECK(!rectangular.canUndo());

  solidar::SketchCanvas datum;
  datum.setSketchEditContext({solidar::kInvalidSketchId,
                              solidar::SketchPlacement::xy(),
                              std::make_shared<TopoDS_Shape>(box),
                              std::nullopt, true});
  CHECK(datum.sketch().lines().empty());
  CHECK(datum.sketch().constraints().empty());

  // B: every edge of both the outer wire and the circular inner wire is used.
  const TopoDS_Shape cylinder =
      BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20.0, 15.0, 0.0),
                                      gp_Dir(0.0, 0.0, 1.0)),
                               5.0, 20.0)
          .Shape();
  BRepAlgoAPI_Cut cut(box, cylinder);
  cut.Build();
  CHECK(cut.IsDone());
  solidar::SketchCanvas withHole;
  withHole.setSketchEditContext(faceContext(cut.Shape(), 20.0, true));
  CHECK(withHole.referenceFaceEdgeCount() == 5);
  CHECK(projectedElements(withHole).size() == 5);
  CHECK(withHole.sketch().constraints().size() == 5);
  CHECK(!withHole.canUndo());

  // D: configuring an existing face sketch does not add another boundary.
  solidar::sketch::Sketch saved = rectangular.sketch();
  saved.addLine({5.0, 5.0}, {10.0, 5.0});
  const auto savedLineCount = saved.lines().size();
  const auto savedConstraintCount = saved.constraints().size();
  rectangular.setSketchEditContext(faceContext(box, 20.0, false));
  rectangular.loadSketch(saved);
  CHECK(rectangular.sketch().lines().size() == savedLineCount);
  CHECK(rectangular.sketch().constraints().size() == savedConstraintCount);
  CHECK(projectedElements(rectangular).size() == 4);

  // E/F: support edges remain duplicate-guarded, while another body edge is
  // still manually projectable and creates exactly one undo entry.
  const TopoDS_Shape lowerExtension =
      BRepPrimAPI_MakeBox(gp_Pnt(45.0, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
  BRepAlgoAPI_Fuse fuse(box, lowerExtension);
  fuse.Build();
  CHECK(fuse.IsDone());
  solidar::SketchCanvas manual;
  manual.setSketchEditContext(faceContext(fuse.Shape(), 20.0, true));
  const auto automaticElements = projectedElements(manual);
  const auto initialManualLineCount = manual.sketch().lines().size();
  bool externalProjected = false;
  for (std::size_t edge = 0; edge < manual.referenceBodyEdgeCount(); ++edge) {
    if (!manual.projectReferenceEdge(edge)) continue;
    externalProjected = true;
    break;
  }
  CHECK(externalProjected);
  CHECK(projectedElements(manual).size() == automaticElements.size() + 1);
  CHECK(manual.sketch().lines().size() > initialManualLineCount);
  CHECK(manual.canUndo());
  manual.undo();
  CHECK(projectedElements(manual) == automaticElements);
  CHECK(manual.sketch().lines().size() == initialManualLineCount);
  CHECK(!manual.canUndo());
}

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  QTemporaryDir tempDir;
  CHECK(tempDir.isValid());
  solidar::AppSettings settings(tempDir.filePath("settings.ini"));

  autoProjectionRegressionTests();

  solidar::home::HomeWindow home(settings);
  solidar::MainWindow* editor = nullptr;
  QObject::connect(&home, &solidar::home::HomeWindow::projectRequested,
                   &application, [&](const QString& path) {
    editor = new solidar::MainWindow(settings);
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

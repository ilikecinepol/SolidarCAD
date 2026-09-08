#include <BRepPrimAPI_MakeBox.hxx>
#include <QApplication>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "ui/Viewport.h"
#include "ui/ViewportCamera.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {

void mouse(solidar::Viewport& view, QEvent::Type type, QPointF position,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
  QApplication::sendEvent(&view, &event);
}

// Replicates Viewport::fitAll() over the 8 box corners so the test camera
// matches the internal projection (yaw/pitch from the accessors, zoom/pan
// computed the same way, center = mesh bbox center).
solidar::ViewportCameraState fitCamera(const solidar::Viewport& view,
                                       const solidar::Point3d& center) {
  const float yaw = view.cameraYawDegrees();
  const float pitch = view.cameraPitchDegrees();
  const QSize size = view.size();
  solidar::ViewportCameraState probe{yaw, pitch, 1.0F, {}, size, 1.0F, center,
                                     1.0};
  double minX = std::numeric_limits<double>::max();
  double minY = minX;
  double maxX = -minX;
  double maxY = -minX;
  for (double x : {0.0, 40.0})
    for (double y : {0.0, 30.0})
      for (double z : {0.0, 20.0}) {
        const QPointF p = probe.worldToScreen({x, y, z});
        minX = std::min(minX, p.x());
        minY = std::min(minY, p.y());
        maxX = std::max(maxX, p.x());
        maxY = std::max(maxY, p.y());
      }
  const double projectedWidth = std::max(1.0, maxX - minX);
  const double projectedHeight = std::max(1.0, maxY - minY);
  const double zoom = std::clamp(
      0.88 * std::min(size.width() / projectedWidth,
                      size.height() / projectedHeight),
      0.02, 100.0);
  const QPointF viewportCenter(size.width() * 0.5, size.height() * 0.52);
  const QPointF boundsCenter((minX + maxX) * 0.5, (minY + maxY) * 0.5);
  const QPointF pan =
      viewportCenter - (viewportCenter + (boundsCenter - viewportCenter) * zoom);
  return {static_cast<float>(yaw), static_cast<float>(pitch),
          static_cast<float>(zoom), pan, size, 1.0F, center, 1.0};
}

constexpr solidar::BodyId kBodyId = 41;
constexpr solidar::FeatureId kSourceFeatureId = 73;
constexpr solidar::FeatureId kPreviewFeatureId = 99;

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  const auto source = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
  const auto previewTall = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(40.0, 30.0, 30.0).Shape());
  const auto previewWide = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(46.0, 30.0, 20.0).Shape());
  const solidar::Point3d center{20.0, 15.0, 10.0};

  auto makeViewport = [&] {
    auto* view = new solidar::Viewport;
    view->resize(800, 600);
    view->setBodyShape(source, kBodyId, kSourceFeatureId);
    view->setSolidVisible(true);
    view->viewTop();
    return view;
  };

  // 1+2: hover finds a visible top edge; clicking selects it (source ref).
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF side = camera.worldToScreen({40.0, 15.0, 20.0});
    const QPointF front = camera.worldToScreen({20.0, 0.0, 20.0});
    view.beginEdgeSelection();
    mouse(view, QEvent::MouseMove, side, Qt::NoButton, Qt::NoButton);
    CHECK(view.hoveredBodyEdgeIndex() != static_cast<std::size_t>(-1));
    mouse(view, QEvent::MouseButtonPress, side, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(view.selectedBodyEdges().size() == 1);
    CHECK(view.selectedBodyEdges().front().bodyId == kBodyId);
    CHECK(view.selectedBodyEdges().front().featureId == kSourceFeatureId);
    // Second click (multi-select kept) adds a different edge.
    mouse(view, QEvent::MouseButtonPress, front, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(view.selectedBodyEdges().size() == 2);
    for (const auto& edge : view.selectedBodyEdges()) {
      CHECK(edge.bodyId == kBodyId);
      CHECK(edge.featureId == kSourceFeatureId);
    }
  }

  // 3: hover + click are unaffected by an active tool preview (source topology),
  // and the source reference is never replaced by the preview feature id.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF side = camera.worldToScreen({40.0, 15.0, 20.0});
    view.setToolPreviewShape(kBodyId, kPreviewFeatureId, previewTall);
    view.beginEdgeSelection();
    mouse(view, QEvent::MouseMove, side, Qt::NoButton, Qt::NoButton);
    CHECK(view.hoveredBodyEdgeIndex() != static_cast<std::size_t>(-1));
    mouse(view, QEvent::MouseButtonPress, side, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(view.selectedBodyEdges().size() == 1);
    CHECK(view.selectedBodyEdges().front().featureId == kSourceFeatureId);
    CHECK(view.selectedBodyEdges().front().featureId != kPreviewFeatureId);
  }

  // 4: repeated preview rebuilds keep the selected source edges.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF front = camera.worldToScreen({20.0, 0.0, 20.0});
    view.beginEdgeSelection();
    mouse(view, QEvent::MouseMove, front, Qt::NoButton, Qt::NoButton);
    mouse(view, QEvent::MouseButtonPress, front, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(view.selectedBodyEdges().size() == 1);
    view.setToolPreviewShape(kBodyId, kPreviewFeatureId, previewTall);
    view.setToolPreviewShape(kBodyId, kPreviewFeatureId, previewWide);
    view.setToolPreviewShape(kBodyId, kPreviewFeatureId, previewTall);
    CHECK(view.selectedBodyEdges().size() == 1);
    CHECK(view.selectedBodyEdges().front().featureId == kSourceFeatureId);
  }

  // 5: a stale ExtrusionSurface pick mode must never block edge selection.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF front = camera.worldToScreen({20.0, 0.0, 20.0});
    view.beginExtrusionSurfaceSelection();
    CHECK(view.selectionFilter() == solidar::SelectionFilter::Any);
    view.beginEdgeSelection();
    mouse(view, QEvent::MouseMove, front, Qt::NoButton, Qt::NoButton);
    mouse(view, QEvent::MouseButtonPress, front, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(view.selectedBodyEdges().size() == 1);
    CHECK(view.selectedBodyEdges().front().featureId == kSourceFeatureId);
  }

  // 6: a stale SketchPlane pick mode must not block edge selection and the
  // base-plane overlay must not leak into the Edge context.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF front = camera.worldToScreen({20.0, 0.0, 20.0});
    int planePicks = 0;
    QObject::connect(&view, &solidar::Viewport::sketchPlanePicked, &view,
                     [&](const QString&) { ++planePicks; });
    view.beginSketchPlaneSelection();
    view.beginEdgeSelection();
    CHECK(view.selectionFilter() == solidar::SelectionFilter::Edge);
    CHECK(view.edgeMultiSelectionMode());
    CHECK(!view.faceMultiSelectionMode());
    mouse(view, QEvent::MouseButtonPress, front, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(planePicks == 0);
    CHECK(view.selectedBodyEdges().size() == 1);
    CHECK(view.selectedBodyEdges().front().featureId == kSourceFeatureId);
  }

  // 7: a stale RevolveAxis pick mode must not block edge selection.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF front = camera.worldToScreen({20.0, 0.0, 20.0});
    view.beginRevolveAxisSelection(0);
    view.beginEdgeSelection();
    mouse(view, QEvent::MouseButtonPress, front, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(view.selectedBodyEdges().size() == 1);
  }

  // 8: switching tools wipes the previous interaction context (selected
  // edges, filter, multi-select modes) instead of leaking it forward.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF front = camera.worldToScreen({20.0, 0.0, 20.0});
    view.beginEdgeSelection();
    mouse(view, QEvent::MouseButtonPress, front, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(view.selectedBodyEdges().size() == 1);
    view.beginExtrusionSurfaceSelection();
    CHECK(view.selectionFilter() == solidar::SelectionFilter::Any);
    CHECK(!view.edgeMultiSelectionMode());
    CHECK(view.selectedBodyEdges().empty());
    view.beginEdgeSelection();
    CHECK(view.selectionFilter() == solidar::SelectionFilter::Edge);
    CHECK(view.edgeMultiSelectionMode());
    CHECK(view.selectedBodyEdges().empty());
  }

  // Face context: Face filter hovers faces (not edges) and clicking a face
  // centre selects a source-face reference.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(source, kBodyId, kSourceFeatureId);
    view.setSolidVisible(true);
    view.viewTop();
    const auto camera = fitCamera(view, center);
    const QPointF top = camera.worldToScreen({20.0, 15.0, 20.0});
    view.beginFaceSelection();
    mouse(view, QEvent::MouseMove, top, Qt::NoButton, Qt::NoButton);
    CHECK(view.hoveredBodyEdgeIndex() == static_cast<std::size_t>(-1));
    CHECK(view.hoveredBodyFaceIndex() != static_cast<std::size_t>(-1));
    mouse(view, QEvent::MouseButtonPress, top, Qt::LeftButton, Qt::LeftButton);
    CHECK(view.selectedBodyFaces().size() == 1);
    CHECK(view.selectedBodyFaces().front().bodyId == kBodyId);
    CHECK(view.selectedBodyFaces().front().featureId == kSourceFeatureId);
  }

  return EXIT_SUCCESS;
}
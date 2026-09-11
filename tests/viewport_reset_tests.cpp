#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QMouseEvent>

#include <cstdlib>
#include <iostream>
#include <memory>

#include "ui/Viewport.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

// Targeted regression for Viewport::resetScene. The scene is populated through
// the public API, reset, and the observable state is verified clean. The
// QOpenGLWidget is never shown, so no OpenGL context or surface is required.
int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  solidar::Viewport viewport;
  viewport.resize(800, 600);

  const auto box = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape());
  const solidar::BodyId bodyId = 1;
  const solidar::FeatureId featureId = 2;
  viewport.setBodyShape(box, bodyId, featureId);

  solidar::sketch::Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {40.0, 20.0});

  // Populate transient interaction state through the public API. The face
  // selection is set after the Edge filter because switching the selection
  // context to edges clears the now-incompatible face selection.
  viewport.setSelectedBodyEdges({solidar::EdgeReference{bodyId, featureId, 0}});
  viewport.setFaceMultiSelectionMode(true);
  viewport.setEdgeMultiSelectionMode(true);
  viewport.setSelectionFilter(solidar::SelectionFilter::Edge);
  viewport.setSelectedBodyFaces({solidar::FaceReference{bodyId, featureId, 0}});
  viewport.setToolManipulator(solidar::LinearToolManipulator{});
  viewport.setAngularToolManipulator(solidar::AngularToolManipulator{});
  viewport.commitAdditiveExtrusion(sketch, QStringLiteral("XY"), 0.0, 10.0);

  // Sanity: state is actually populated before the reset.
  CHECK(viewport.selectedBodyFaces().size() == 1);
  CHECK(viewport.selectedBodyEdges().size() == 1);
  CHECK(viewport.faceMultiSelectionMode());
  CHECK(viewport.edgeMultiSelectionMode());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Edge);
  CHECK(viewport.angularToolManipulator().has_value());
  CHECK(!viewport.solidFeatures().empty());

  viewport.resetScene();

  // After reset, all transient interaction state must be cleared.
  CHECK(viewport.selectedBodyFaces().empty());
  CHECK(viewport.selectedBodyEdges().empty());
  CHECK(!viewport.faceMultiSelectionMode());
  CHECK(!viewport.edgeMultiSelectionMode());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);
  CHECK(!viewport.angularToolManipulator().has_value());
  CHECK(viewport.solidFeatures().empty());

  // The viewport remains reusable: repopulate and reset again. The edge
  // selection is set after the Face filter because switching to faces clears
  // the now-incompatible edge selection.
  viewport.setBodyShape(box, bodyId, featureId);
  viewport.setSelectionFilter(solidar::SelectionFilter::Face);
  viewport.setSelectedBodyEdges({solidar::EdgeReference{bodyId, featureId, 0}});
  CHECK(viewport.selectedBodyEdges().size() == 1);
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Face);
  viewport.resetScene();
  CHECK(viewport.selectedBodyEdges().empty());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);

  // resetScene clears the transient marquee and leaves the viewport reusable.
  {
    solidar::Viewport marqueeView;
    marqueeView.resize(800, 600);
    const auto marqueeBox = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
    marqueeView.setBodyShape(marqueeBox, bodyId, featureId);
    marqueeView.setSolidVisible(true);
    const auto mouse = [](solidar::Viewport& view, QEvent::Type type,
                          QPointF position, Qt::MouseButton button,
                          Qt::MouseButtons buttons) {
      QMouseEvent event(type, position, position, button, buttons,
                        Qt::NoModifier);
      QApplication::sendEvent(&view, &event);
    };
    // Press on empty area begins a marquee.
    mouse(marqueeView, QEvent::MouseButtonPress, {1.0, 1.0}, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(marqueeView.marqueeActive());
    marqueeView.resetScene();
    CHECK(!marqueeView.marqueeActive());
    // The viewport is reusable: repopulate and start another marquee.
    marqueeView.setBodyShape(marqueeBox, bodyId, featureId);
    marqueeView.setSolidVisible(true);
    mouse(marqueeView, QEvent::MouseButtonPress, {1.0, 1.0}, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(marqueeView.marqueeActive());
    mouse(marqueeView, QEvent::MouseButtonRelease, {1.0, 1.0}, Qt::LeftButton,
          Qt::NoButton);
    CHECK(!marqueeView.marqueeActive());
  }

  return EXIT_SUCCESS;
}

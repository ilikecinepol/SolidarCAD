#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QThread>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include "ui/Viewport.h"

// Targeted regression for Viewport::resetScene. The scene is populated through
// the public API, reset, and the observable state is verified clean. The
// QOpenGLWidget is never shown, so no OpenGL context or surface is required.
int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  solidar::Viewport viewport;
  viewport.resize(800, 600);
  std::vector<solidar::ViewportCancelReason> cancelReasons;
  std::vector<QString> selectionDescriptions;
  QObject::connect(&viewport, &solidar::Viewport::interactionCancelled,
                   &viewport,
                   [&](solidar::ViewportCancelReason reason) {
                     cancelReasons.push_back(reason);
                   });
  QObject::connect(&viewport, &solidar::Viewport::selectionChanged, &viewport,
                   [&](const QString& text) {
                     selectionDescriptions.push_back(text);
                   });

  const auto box = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape());
  const solidar::BodyId bodyId = 1;
  const solidar::FeatureId featureId = 2;
  viewport.setBodyShape(box, bodyId, featureId);

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

  // Sanity: state is actually populated before the reset.
  CHECK(viewport.selectedBodyFaces().size() == 1);
  CHECK(viewport.selectedBodyEdges().size() == 1);
  CHECK(viewport.faceMultiSelectionMode());
  CHECK(viewport.edgeMultiSelectionMode());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Edge);
  CHECK(viewport.angularToolManipulator().has_value());

  // Populate the whole-body selection last (it clears the face/edge selection
  // cross-type), then confirm resetScene clears it.
  viewport.setSelectedBodies({bodyId});
  CHECK(viewport.selectedBodies() == std::vector<solidar::BodyId>{bodyId});

  viewport.resetScene();

  // After reset, all transient interaction state must be cleared.
  CHECK(viewport.selectedBodyFaces().empty());
  CHECK(viewport.selectedBodyEdges().empty());
  CHECK(viewport.selectedBodies().empty());
  CHECK(!viewport.faceMultiSelectionMode());
  CHECK(!viewport.edgeMultiSelectionMode());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);

  // Create Sketch cancellation is a full state transition: its temporary
  // plane picker and all three forced-visible base planes disappear.
  viewport.beginSketchPlaneSelection();
  CHECK(viewport.sketchPlaneSelectionActive());
  CHECK(viewport.basePlaneVisible(0));
  CHECK(viewport.basePlaneVisible(1));
  CHECK(viewport.basePlaneVisible(2));
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&viewport, &escape);
  CHECK(cancelReasons.size() == 1);
  CHECK(cancelReasons.back() ==
        solidar::ViewportCancelReason::SketchPlaneSelection);
  CHECK(std::none_of(selectionDescriptions.begin(),
                     selectionDescriptions.end(), [](const QString& text) {
                       return text.startsWith(QStringLiteral("__"));
                     }));
  CHECK(!viewport.sketchPlaneSelectionActive());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);
  CHECK(!viewport.basePlaneVisible(0));
  CHECK(!viewport.basePlaneVisible(1));
  CHECK(!viewport.basePlaneVisible(2));
  viewport.beginSketchPlaneSelection();
  CHECK(viewport.sketchPlaneSelectionActive());
  viewport.resetToolInteraction();
  CHECK(!viewport.sketchPlaneSelectionActive());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);
  CHECK(!viewport.basePlaneVisible(0));
  CHECK(!viewport.angularToolManipulator().has_value());

  // Mirror owns two explicit viewport pick modes. Body selection persists
  // while choosing a Plane; reset/cancel removes both the mode and the forced
  // construction-plane visibility.
  viewport.setBodyShape(box, bodyId, featureId);
  viewport.setSolidVisible(true);
  viewport.beginMirrorBodySelection();
  CHECK(viewport.mirrorBodySelectionActive());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Face);
  QApplication::sendEvent(&viewport, &escape);
  CHECK(cancelReasons.size() == 2);
  CHECK(cancelReasons.back() ==
        solidar::ViewportCancelReason::NestedReselection);
  viewport.beginMirrorBodySelection();
  viewport.setSelectedBodies({bodyId});
  viewport.beginMirrorPlaneSelection();
  CHECK(viewport.mirrorPlaneSelectionActive());
  CHECK(viewport.selectedBodies() == std::vector<solidar::BodyId>{bodyId});
  CHECK(viewport.basePlaneVisible(0));
  CHECK(viewport.basePlaneVisible(1));
  CHECK(viewport.basePlaneVisible(2));
  viewport.showMirrorPlaneSelection(1);
  CHECK(!viewport.mirrorPlaneSelectionActive());
  viewport.resetToolInteraction();
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);
  CHECK(!viewport.basePlaneVisible(0));
  CHECK(!viewport.basePlaneVisible(1));
  CHECK(!viewport.basePlaneVisible(2));

  viewport.setBodyShape(box, bodyId, featureId);
  viewport.setSolidVisible(true);
  viewport.beginMoveBodySelection();
  CHECK(viewport.moveBodySelectionActive());
  viewport.setSelectedBodies({bodyId});
  viewport.showMovePreview();
  viewport.setTranslationToolManipulator(
      {{5.0, 10.0, 15.0}, {1.0, 2.0, 3.0}, -100000.0, 100000.0});
  CHECK(viewport.translationToolManipulator().has_value());
  viewport.resetToolInteraction();
  CHECK(!viewport.moveBodySelectionActive());
  CHECK(!viewport.translationToolManipulator().has_value());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);

  viewport.setBodyShape(box, bodyId, featureId);
  viewport.setSolidVisible(true);
  viewport.beginLinearPatternBodySelection();
  CHECK(viewport.linearPatternBodySelectionActive());
  viewport.setSelectedBodies({bodyId});
  viewport.beginLinearPatternAxisSelection();
  CHECK(viewport.linearPatternAxisSelectionActive());
  CHECK(viewport.selectedBodies() == std::vector<solidar::BodyId>{bodyId});
  viewport.showLinearPatternAxisSelection(0);
  CHECK(!viewport.linearPatternAxisSelectionActive());
  viewport.setToolManipulator({{}, {1.0, 0.0, 0.0}, 30.0, 0.01,
                                   100000.0});
  viewport.resetToolInteraction();
  CHECK(!viewport.linearPatternBodySelectionActive());
  CHECK(!viewport.linearPatternAxisSelectionActive());
  CHECK(!viewport.linearToolManipulator().has_value());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);

  viewport.setBodyShape(box, bodyId, featureId);
  viewport.setSolidVisible(true);
  viewport.beginCircularPatternBodySelection();
  CHECK(viewport.circularPatternBodySelectionActive());
  viewport.setSelectedBodies({bodyId});
  viewport.beginCircularPatternAxisSelection();
  CHECK(viewport.circularPatternAxisSelectionActive());
  CHECK(viewport.selectedBodies() == std::vector<solidar::BodyId>{bodyId});
  viewport.showCircularPatternAxisSelection(2);
  CHECK(!viewport.circularPatternAxisSelectionActive());
  viewport.setAngularToolManipulator(
      {{}, {0.0, 0.0, 1.0}, 30.0, 180.0});
  viewport.resetToolInteraction();
  CHECK(!viewport.circularPatternBodySelectionActive());
  CHECK(!viewport.circularPatternAxisSelectionActive());
  CHECK(!viewport.angularToolManipulator().has_value());
  CHECK(viewport.selectionFilter() == solidar::SelectionFilter::Any);

  viewport.beginRulerMeasurement();
  CHECK(viewport.rulerMeasurementActive());
  viewport.resetToolInteraction();
  CHECK(!viewport.rulerMeasurementActive());
  viewport.beginRulerMeasurement();
  CHECK(viewport.rulerMeasurementActive());
  viewport.resetScene();
  CHECK(!viewport.rulerMeasurementActive());

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

  // A queued hover belongs to the scene generation that scheduled it. Scene
  // replacement must stop the timer so it cannot republish old geometry.
  solidar::sketch::Sketch hoverProfile;
  hoverProfile.addRectangle({-10.0, -10.0}, {10.0, 10.0});
  viewport.addSketch(77, hoverProfile, QStringLiteral("display only"),
                     solidar::SketchPlacement::xy());
  viewport.beginExtrusionSurfaceSelection();
  QMouseEvent pendingMove(QEvent::MouseMove, QPointF{400.0, 312.0},
                          QPointF{400.0, 312.0}, Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
  QApplication::sendEvent(&viewport, &pendingMove);
  viewport.resetScene();
  QThread::msleep(25);
  QApplication::processEvents();
  CHECK(viewport.extrusionHoverBounds().isEmpty());

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

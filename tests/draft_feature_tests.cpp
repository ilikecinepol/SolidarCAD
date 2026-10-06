#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomAbs_CurveType.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <memory>
#include <cstdlib>
#include <iostream>

#include "model/Document.h"
#include "model/DraftBuilder.h"
#include "model/DraftFeature.h"
#include "model/DraftToolSession.h"
#include "model/ExtrudeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
double volume(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  solidar::Document document;
  auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  auto& body = document.addBody();
  auto& source = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, 20.0, "Box", solidar::ExtrudeOperation::NewBody));
  assert(document.recompute());

  // Choose a vertical planar face accepted by OCCT for the XY neutral plane.
  std::size_t chosen = static_cast<std::size_t>(-1);
  std::size_t index = 0;
  for (TopExp_Explorer faces(*source.shape(), TopAbs_FACE); faces.More();
       faces.Next(), ++index) {
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if (surface.GetType() != GeomAbs_Plane ||
        std::abs(surface.Plane().Axis().Direction().Z()) > 0.1)
      continue;
    std::string ignored;
    if (solidar::buildDraftShape(*source.shape(), {index},
          gp_Pln(gp_Pnt(0,0,0), gp_Dir(0,0,1)), gp_Dir(0,0,1),
          5.0, false, &ignored)) { chosen = index; break; }
  }
  assert(chosen != static_cast<std::size_t>(-1));
  const auto face = solidar::makeFaceReference(*source.shape(), body.id(),
                                                source.id(), chosen);
  solidar::PlaneReference plane{solidar::NeutralPlaneType::GlobalXY};
  solidar::AxisReference direction{solidar::AxisReferenceType::GlobalZ,
                                    solidar::kInvalidSketchId,
                                    solidar::sketch::kInvalidGeometryId};
  auto draft = std::make_unique<solidar::DraftFeature>(
      source.id(), std::vector{face}, plane, direction, 5.0, false, "Draft 1");
  auto* draftPtr = draft.get(); const auto draftId = draftPtr->id();
  body.addFeature(std::move(draft));
  assert(document.recompute());
  const double firstVolume = volume(*draftPtr->shape());
  assert(std::abs(firstVolume - volume(*source.shape())) > 0.01);

  draftPtr->setAngleDeg(8.0);
  assert(document.recompute());
  assert(std::abs(volume(*draftPtr->shape()) - firstVolume) > 0.01);
  draftPtr->setReversed(true);
  assert(document.recompute());
  draftPtr->setAngleDeg(0.0);
  CHECK(document.recompute());
  CHECK(!draftPtr->isFailed());
  CHECK(std::abs(volume(*draftPtr->shape()) - volume(*source.shape())) < 0.01);
  draftPtr->setAngleDeg(5.0);
  assert(document.recompute() && draftPtr->id() == draftId);

  solidar::DraftToolSession session;
  session.begin(document, body.id(), source.id(), source.shape(), {face}, plane,
                direction, 5.0, false, draftId);
  assert(session.previewShape() && session.manipulator());
  session.clearPrincipalAxis();
  assert(session.selectionRequirement()->type == solidar::SelectionType::Axis);
  assert(session.faces().size() == 1 && session.angleDeg() == 5.0);
  assert(session.setPrincipalAxis(2));
  assert(session.principalAxisIndex() == 2);
  assert(session.neutralPlane()->type == solidar::NeutralPlaneType::GlobalXY);
  assert(session.pullDirection()->type ==
         solidar::AxisReferenceType::GlobalZ);
  assert(session.previewShape() && session.editingFeatureId() == draftId);
  CHECK(session.parameters().size() == 1);
  CHECK(session.parameters().front().minimum == -89.99);
  CHECK(session.parameters().front().maximum == 89.99);
  session.setAngleFromManipulator(-5.0);
  CHECK(session.angleDeg() == -5.0);
  CHECK(session.previewShape() && session.manipulator());
  CHECK(session.manipulator()->angleDeg == -5.0);
  CHECK(session.manipulator()->minimumDeg == -89.99);
  CHECK(session.manipulator()->maximumDeg == 89.99);
  session.setAngleFromManipulator(0.0);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  session.setAngleFromManipulator(5.0);

  solidar::DraftToolSession legacyReversedSession;
  legacyReversedSession.begin(document, body.id(), source.id(), source.shape(),
                              {face}, plane, direction, 5.0, true);
  CHECK(legacyReversedSession.angleDeg() == -5.0);
  CHECK(legacyReversedSession.previewShape());

  // Parameter-domain failure keeps the last valid visual preview, disables
  // commit through PreviewInvalid and recovers in the same session when the
  // angle returns to a buildable value.
  bool foundInvalidAngle = false;
  double invalidAngle = 0.0;
  for (double angle = 10.0; angle <= 88.0; angle += 2.0) {
    const auto lastValidPreview = session.previewShape();
    session.setAngleFromPanel(angle);
    if (session.lifecycle() != solidar::ToolLifecycle::PreviewInvalid) continue;
    foundInvalidAngle = true;
    invalidAngle = angle;
    assert(!session.error().empty());
    assert(session.previewShape() == lastValidPreview);
    break;
  }
  assert(foundInvalidAngle);
  // Last-valid geometry belongs only to the same faces/reference/reversed
  // context. Changing a structural input at an invalid angle must not surface
  // a preview built for the previous context.
  session.clearNeutralPlane();
  assert(!session.previewShape());
  session.setNeutralPlane(plane);
  assert(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  assert(session.angleDeg() == invalidAngle);
  assert(!session.previewShape());
  session.setAngleFromPanel(5.0);
  assert(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  assert(session.previewShape());
  assert(session.editingFeatureId() == draftId);

  // A straight edge of the selected face is a first-class rotation axis. Its
  // persistent EdgeReference must drive both the live preview and the saved
  // feature, rather than relying on an unstable OCCT edge ordinal.
  std::optional<solidar::EdgeReference> adjacentEdge;
  const auto resolvedChosenFace =
      solidar::resolveFaceReference(*source.shape(), face.topology());
  CHECK(resolvedChosenFace);
  std::size_t edgeIndex = 0;
  for (TopExp_Explorer edges(*source.shape(), TopAbs_EDGE); edges.More();
       edges.Next(), ++edgeIndex) {
    BRepAdaptor_Curve curve(TopoDS::Edge(edges.Current()));
    if (curve.GetType() != GeomAbs_Line) continue;
    bool belongsToFace = false;
    for (TopExp_Explorer faceEdges(*resolvedChosenFace.subshape, TopAbs_EDGE);
         faceEdges.More(); faceEdges.Next()) {
      if (TopoDS::Edge(faceEdges.Current()).IsSame(
              TopoDS::Edge(edges.Current()))) {
        belongsToFace = true;
        break;
      }
    }
    if (!belongsToFace) continue;
    auto candidate = solidar::makeEdgeReference(
        *source.shape(), body.id(), source.id(), edgeIndex);
    gp_Pln candidatePlane;
    gp_Dir candidateDirection;
    std::string candidateError;
    if (!solidar::resolveDraftEdgeAxis(
            *source.shape(), face, candidate, &candidatePlane,
            &candidateDirection, &candidateError))
      continue;
    if (solidar::buildDraftShape(*source.shape(), {chosen}, candidatePlane,
                                 candidateDirection, 5.0, false,
                                 &candidateError)) {
      adjacentEdge = std::move(candidate);
      break;
    }
  }
  CHECK(adjacentEdge && adjacentEdge->signature);
  CHECK(session.setRotationEdge(*adjacentEdge));
  CHECK(session.rotationEdge() == adjacentEdge);
  CHECK(!session.principalAxisIndex());
  CHECK(session.previewShape() && session.manipulator());

  draftPtr->setReversed(false);
  draftPtr->setRotationEdge(adjacentEdge);
  CHECK(document.recompute());
  CHECK(draftPtr->shape() && !draftPtr->shape()->IsNull());
  auto* sourceExtrude = dynamic_cast<solidar::ExtrudeFeature*>(&source);
  CHECK(sourceExtrude);
  sourceExtrude->setLengthMm(22.0);
  CHECK(document.recompute());
  CHECK(draftPtr->shape() && !draftPtr->shape()->IsNull());

  QTemporaryDir temporary(QDir::current().filePath(
      QStringLiteral("draft-feature-tests-XXXXXX")));
  assert(temporary.isValid());
  const QString path = temporary.filePath("draft.solidar"); QString error;
  assert(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document loaded;
  assert(solidar::project::ProjectFile::loadDocument(path, &loaded, &error));
  assert(loaded.recompute());
  const auto* loadedDraft = dynamic_cast<const solidar::DraftFeature*>(
      loaded.activeBody()->activeFeature());
  assert(loadedDraft && loadedDraft->id() == draftId);
  assert(loadedDraft->draftedFaces().front().signature);
  CHECK(loadedDraft->rotationEdge());
  CHECK(loadedDraft->rotationEdge()->signature);
  CHECK(loadedDraft->rotationEdge()->bodyId == body.id());
  CHECK(loadedDraft->rotationEdge()->featureId == source.id());
  assert(loadedDraft->pullDirection().type == solidar::AxisReferenceType::GlobalZ);
  return 0;
}

#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <memory>

#include "model/Document.h"
#include "model/DraftBuilder.h"
#include "model/DraftFeature.h"
#include "model/DraftToolSession.h"
#include "model/ExtrudeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

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
  assert(!document.recompute() && draftPtr->isFailed());
  draftPtr->setAngleDeg(5.0);
  assert(document.recompute() && draftPtr->id() == draftId);

  solidar::DraftToolSession session;
  session.begin(document, body.id(), source.id(), source.shape(), {face}, plane,
                direction, 5.0, false, draftId);
  assert(session.previewShape() && session.manipulator());
  session.clearNeutralPlane();
  assert(session.selectionRequirement()->type == solidar::SelectionType::Plane);
  assert(session.faces().size() == 1 && session.angleDeg() == 5.0);
  session.setNeutralPlane(plane);
  assert(session.previewShape() && session.editingFeatureId() == draftId);

  QTemporaryDir temporary; assert(temporary.isValid());
  const QString path = temporary.filePath("draft.solidar"); QString error;
  assert(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document loaded;
  assert(solidar::project::ProjectFile::loadDocument(path, &loaded, &error));
  assert(loaded.recompute());
  const auto* loadedDraft = dynamic_cast<const solidar::DraftFeature*>(
      loaded.activeBody()->activeFeature());
  assert(loadedDraft && loadedDraft->id() == draftId);
  assert(loadedDraft->draftedFaces().front().signature);
  assert(loadedDraft->pullDirection().type == solidar::AxisReferenceType::GlobalZ);
  return 0;
}

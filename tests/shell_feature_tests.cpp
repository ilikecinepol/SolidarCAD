#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
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
#include "model/ExtrudeFeature.h"
#include "model/ShellFeature.h"
#include "model/ShellToolSession.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

namespace {
double volume(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

std::size_t topFace(const TopoDS_Shape& shape) {
  std::size_t index = 0;
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More();
       faces.Next(), ++index) {
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if (surface.GetType() == GeomAbs_Plane &&
        std::abs(surface.Plane().Axis().Direction().Z()) > 0.99 &&
        std::abs(surface.Plane().Location().Z() - 20.0) < 1e-6)
      return index;
  }
  return static_cast<std::size_t>(-1);
}

solidar::Document boxDocument(solidar::FeatureId* sourceId,
                              solidar::FaceReference* opening) {
  solidar::Document document;
  auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  auto& body = document.addBody();
  auto& source = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, 20.0, "Box", solidar::ExtrudeOperation::NewBody));
  assert(document.recompute());
  *sourceId = source.id();
  const auto index = topFace(*source.shape());
  assert(index != static_cast<std::size_t>(-1));
  *opening = solidar::makeFaceReference(*source.shape(), body.id(), source.id(), index);
  return document;
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  solidar::FeatureId sourceId;
  solidar::FaceReference opening;
  auto document = boxDocument(&sourceId, &opening);
  auto* body = document.activeBody();
  assert(body);
  auto shell = std::make_unique<solidar::ShellFeature>(
      sourceId, std::vector{opening}, 2.0, false, "Shell 1");
  auto* shellPtr = shell.get();
  const auto shellId = shellPtr->id();
  body->addFeature(std::move(shell));
  assert(document.recompute());
  assert(shellPtr->isValid() && shellPtr->shape());
  const double inwardVolume = volume(*shellPtr->shape());
  assert(inwardVolume > 0.0 && inwardVolume < 24000.0);

  shellPtr->setThicknessMm(4.0);
  assert(document.recompute());
  assert(volume(*shellPtr->shape()) > inwardVolume);
  shellPtr->setOutside(true);
  assert(document.recompute());
  assert(std::abs(volume(*shellPtr->shape()) - inwardVolume) > 1.0);

  // Invalid parameters retain identity and recover without recreating history.
  shellPtr->setThicknessMm(1000.0);
  assert(!document.recompute() && shellPtr->isFailed());
  shellPtr->setThicknessMm(2.0);
  shellPtr->setOutside(false);
  assert(document.recompute() && shellPtr->id() == shellId);

  solidar::ShellToolSession session;
  session.begin(body->id(), sourceId, body->features().front()->shape(),
                {opening}, 2.0, false, shellId);
  assert(session.previewShape() && session.manipulator());
  session.setThicknessFromManipulator(3.0);
  assert(session.thicknessMm() == 3.0 && session.previewShape());
  session.setOutside(true);
  assert(session.outside() && session.editingFeatureId() == shellId);

  QTemporaryDir temporary;
  assert(temporary.isValid());
  const QString path = temporary.filePath("shell.solidar");
  QString error;
  assert(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document loaded;
  assert(solidar::project::ProjectFile::loadDocument(path, &loaded, &error));
  assert(loaded.recompute());
  const auto* loadedShell = dynamic_cast<const solidar::ShellFeature*>(
      loaded.activeBody()->activeFeature());
  assert(loadedShell && loadedShell->id() == shellId);
  assert(loadedShell->removedFaces().front().signature);
  assert(std::abs(loadedShell->thicknessMm() - 2.0) < 1e-9);
  return 0;
}

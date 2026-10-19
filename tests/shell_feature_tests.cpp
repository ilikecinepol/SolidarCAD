#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "TestAssertions.h"
#include <cmath>
#include <iostream>
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
  double bestZ = -1e100;
  std::size_t best = static_cast<std::size_t>(-1);
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More();
       faces.Next(), ++index) {
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if (surface.GetType() == GeomAbs_Plane &&
        std::abs(surface.Plane().Axis().Direction().Z()) > 0.99 &&
        surface.Plane().Location().Z() > bestZ) {
      bestZ = surface.Plane().Location().Z();
      best = index;
    }
  }
  return best;
}

solidar::Document boxDocument(solidar::FeatureId* sourceId,
                              solidar::FaceReference* opening) {
  solidar::Document document;
  auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  auto& body = document.addBody();
  auto& source = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, 20.0, "Box", solidar::ExtrudeOperation::NewBody));
  CHECK(document.recompute());
  *sourceId = source.id();
  const auto index = topFace(*source.shape());
  CHECK(index != static_cast<std::size_t>(-1));
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
  CHECK(body);
  auto shell = std::make_unique<solidar::ShellFeature>(
      sourceId, std::vector{opening}, 2.0, false, "Shell 1");
  auto* shellPtr = shell.get();
  const auto shellId = shellPtr->id();
  body->addFeature(std::move(shell));
  CHECK(document.recompute());
  CHECK(shellPtr->isValid() && shellPtr->shape());
  const double inwardVolume = volume(*shellPtr->shape());
  CHECK(inwardVolume > 0.0 && inwardVolume < 24000.0);

  shellPtr->setThicknessMm(4.0);
  CHECK(document.recompute());
  CHECK(volume(*shellPtr->shape()) > inwardVolume);
  shellPtr->setOutside(true);
  CHECK(document.recompute());
  CHECK(std::abs(volume(*shellPtr->shape()) - inwardVolume) > 1.0);

  // Invalid parameters retain identity and recover without recreating history.
  shellPtr->setThicknessMm(1000.0);
  CHECK(!document.recompute() && shellPtr->isFailed());
  shellPtr->setThicknessMm(2.0);
  shellPtr->setOutside(false);
  CHECK(document.recompute() && shellPtr->id() == shellId);

  solidar::ShellToolSession session;
  session.begin(body->id(), sourceId, body->features().front()->shape(),
                {opening}, 2.0, false, shellId);
  CHECK(session.previewShape() && session.manipulator());
  session.setThicknessFromManipulator(3.0);
  CHECK(session.thicknessMm() == 3.0 && session.previewShape());
  session.setOutside(true);
  CHECK(session.outside() && session.editingFeatureId() == shellId);

  // An excessive interactive value keeps a valid preview and stops at the
  // last buildable wall thickness instead of replacing the model with an
  // error state.
  auto shallowShape = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(40.0, 30.0, 3.0).Shape());
  const auto shallowTop = topFace(*shallowShape);
  CHECK(shallowTop != static_cast<std::size_t>(-1));
  const auto shallowOpening = solidar::makeFaceReference(
      *shallowShape, 77, 88, shallowTop);
  solidar::ShellToolSession boundedSession;
  boundedSession.begin(77, 88, shallowShape, {shallowOpening}, 1.0, false);
  CHECK(boundedSession.previewShape());
  CHECK(boundedSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  const auto buildsBeforeDrag = boundedSession.previewBuildAttemptCount();
  boundedSession.setThicknessFromManipulator(4.0);
  CHECK(boundedSession.previewBuildAttemptCount() == buildsBeforeDrag + 1);
  CHECK(boundedSession.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!boundedSession.limitReached());
  const auto buildsBeforeBoundary = boundedSession.previewBuildAttemptCount();
  CHECK(boundedSession.refineThicknessToBoundary(
      boundedSession.thicknessMm()));
  CHECK(boundedSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(boundedSession.limitReached());
  CHECK(boundedSession.maximumValidThicknessMm());
  CHECK(boundedSession.thicknessMm() >= 0.01);
  CHECK(boundedSession.thicknessMm() < 3.0);
  CHECK(boundedSession.previewBuildAttemptCount() > buildsBeforeBoundary);
  CHECK(boundedSession.previewBuildAttemptCount() <=
        buildsBeforeBoundary + 27);

  // V2 faces are resolved as a batch against the owning source index. A
  // single missing member invalidates the preview before any shell build.
  solidar::FeatureId v2SourceId;
  solidar::FaceReference ignoredOpening;
  auto v2Document = boxDocument(&v2SourceId, &ignoredOpening);
  auto* v2Body = v2Document.activeBody();
  CHECK(v2Body && v2Body->activeFeature());
  const auto v2SourceShape = v2Body->resultShape();
  const auto v2Opening = v2Document.createFaceReferenceV2(
      v2Body->id(), v2SourceId, topFace(*v2SourceShape));
  CHECK(v2Opening);
  const auto v2Topology = v2Body->activeFeature()->topologyIndex();
  CHECK(v2Topology);
  solidar::ShellToolSession v2Batch;
  auto missingV2 = *v2Opening.reference.v2();
  missingV2.provenance.localName += ":missing";
  v2Batch.begin(
      v2Body->id(), v2SourceId, v2SourceShape,
      {v2Opening.reference,
       solidar::FaceReference::fromV2(std::move(missingV2))},
      2.0, false, std::nullopt, v2Topology);
  CHECK(v2Batch.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!v2Batch.previewShape());
  CHECK(v2Batch.previewBuildAttemptCount() == 0);
  v2Batch.setRemovedFaces({v2Opening.reference});
  CHECK(v2Batch.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(v2Batch.manipulator());
  auto wrongBodyV2 = *v2Opening.reference.v2();
  wrongBodyV2.bodyId += 1000;
  v2Batch.setRemovedFaces(
      {solidar::FaceReference::fromV2(std::move(wrongBodyV2))});
  CHECK(v2Batch.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!v2Batch.manipulator());
  v2Batch.setRemovedFaces({v2Opening.reference});
  CHECK(v2Batch.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  auto v2Shell = std::make_unique<solidar::ShellFeature>(
      v2SourceId, std::vector{v2Opening.reference}, 2.0, false, "v2 shell");
  auto* v2ShellPtr = v2Shell.get();
  v2Body->addFeature(std::move(v2Shell));
  CHECK(v2Document.recompute());
  CHECK(v2ShellPtr->removedFaces() ==
        std::vector<solidar::FaceReference>{v2Opening.reference});

  QTemporaryDir temporary(QDir::current().filePath(
      QStringLiteral("shell-feature-tests-XXXXXX")));
  CHECK(temporary.isValid());
  const QString path = temporary.filePath("shell.solidar");
  QString error;
  CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document loaded;
  CHECK(solidar::project::ProjectFile::loadDocument(path, &loaded, &error));
  CHECK(loaded.recompute());
  const auto* loadedShell = dynamic_cast<const solidar::ShellFeature*>(
      loaded.activeBody()->activeFeature());
  CHECK(loadedShell && loadedShell->id() == shellId);
  CHECK(loadedShell->removedFaces().front().signature());
  CHECK(std::abs(loadedShell->thicknessMm() - 2.0) < 1e-9);
  return 0;
}

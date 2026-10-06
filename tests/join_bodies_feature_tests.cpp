#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "model/Document.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/JoinBodiesToolSession.h"
#include "model/MoveFeature.h"
#include "project/ProjectFile.h"

namespace {

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

solidar::ShapeFeature::ShapePtr boxAt(double x) {
  gp_Trsf translation;
  translation.SetTranslation(gp_Vec(x, 0.0, 0.0));
  return std::make_shared<TopoDS_Shape>(BRepBuilderAPI_Transform(
      BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), translation, true)
                                            .Shape());
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);

  // Session preview accepts a real union and keeps invalid/disconnected input
  // non-committable without modifying either source.
  solidar::JoinBodiesToolSession session;
  session.begin();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  session.setBodies({{1, 11, boxAt(0.0)}, {2, 22, boxAt(5.0)}});
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  CHECK(std::abs(volumeOf(*session.previewShape()) - 1500.0) < 1e-5);
  session.setBodies({{1, 11, boxAt(0.0)}, {2, 22, boxAt(20.0)}});
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!session.previewShape());
  CHECK(!session.error().empty());
  session.cancel();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);

  // A JoinBodies feature is a cross-Body dependency. The result rebuilds when
  // either source Feature changes and stores no raw B-Rep in the document.
  solidar::Document document;
  auto& firstBody = document.addBody("First");
  auto& firstFeature = firstBody.addFeature(
      std::make_unique<solidar::ImportedShapeFeature>(boxAt(0.0), "Box A"));
  const auto firstBodyId = firstBody.id();
  const auto firstFeatureId = firstFeature.id();

  auto& secondBody = document.addBody("Second");
  auto& secondBase = secondBody.addFeature(
      std::make_unique<solidar::ImportedShapeFeature>(boxAt(0.0), "Box B"));
  auto& moved = secondBody.addFeature(std::make_unique<solidar::MoveFeature>(
      secondBase.id(), solidar::Vector3d{5.0, 0.0, 0.0}, "Move B"));
  const auto secondBodyId = secondBody.id();
  const auto movedId = moved.id();

  auto& resultBody = document.addBody("Joined");
  auto& joined = resultBody.addFeature(
      std::make_unique<solidar::JoinBodiesFeature>(
          firstBodyId, firstFeatureId, secondBodyId, movedId, "Join"));
  const auto resultBodyId = resultBody.id();
  const auto joinedId = joined.id();
  document.findBody(firstBodyId)->setVisible(false);
  document.findBody(secondBodyId)->setVisible(false);
  CHECK(document.recompute());
  CHECK(joined.isValid() && joined.hasShape());
  CHECK(std::abs(volumeOf(*joined.shape()) - 1500.0) < 1e-5);

  auto* move = dynamic_cast<solidar::MoveFeature*>(
      document.findBody(secondBodyId)->features().back().get());
  CHECK(move);
  move->setOffsetMm({4.0, 0.0, 0.0});
  CHECK(document.recomputeFrom(move->id()));
  const auto* rebuilt = document.findBody(resultBodyId)->activeFeature();
  CHECK(rebuilt && rebuilt->id() == joinedId && rebuilt->isValid());
  CHECK(std::abs(volumeOf(*rebuilt->shape()) - 1400.0) < 1e-5);

  // Save/load preserves dependency IDs and consumed-body visibility.
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("join-bodies-tests-XXXXXX")));
  CHECK(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("joined.solidar"));
  QString error;
  CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document restored;
  CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
  CHECK(restored.bodies().size() == 3);
  CHECK(!restored.findBody(firstBodyId)->visible());
  CHECK(!restored.findBody(secondBodyId)->visible());
  CHECK(restored.findBody(resultBodyId)->visible());
  const auto* restoredJoin = dynamic_cast<const solidar::JoinBodiesFeature*>(
      restored.findBody(resultBodyId)->activeFeature());
  CHECK(restoredJoin && restoredJoin->id() == joinedId);
  CHECK(restoredJoin->firstBodyId() == firstBodyId);
  CHECK(restoredJoin->secondBodyId() == secondBodyId);
  CHECK(restoredJoin->firstFeatureId() == firstFeatureId);
  CHECK(restoredJoin->secondFeatureId() == movedId);
  CHECK(restoredJoin->isValid() && restoredJoin->hasShape());
  CHECK(std::abs(volumeOf(*restoredJoin->shape()) - 1400.0) < 1e-5);

  return EXIT_SUCCESS;
}

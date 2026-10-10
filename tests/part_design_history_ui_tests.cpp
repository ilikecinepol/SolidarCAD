#include "TestAssertions.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <TopoDS_Shape.hxx>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"
#include "project/ProjectFile.h"
#include "ui/PartDesignHistory.h"
#include "ui/FeatureUiRegistry.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  using Kind = solidar::FeatureKind;
  using Route = solidar::FeatureEditorRoute;
  using StepType = solidar::HistoryStepType;
  struct ExpectedDescriptor {
    Kind kind;
    StepType stepType;
    bool editable;
    Route route;
  };
  constexpr std::array<ExpectedDescriptor, 13> kExpectedDescriptors{{
      {Kind::ImportedShape, StepType::ImportedShape, false, Route::None},
      {Kind::Extrude, StepType::Extrude, true, Route::Extrude},
      {Kind::Revolve, StepType::Revolve, true, Route::Revolve},
      {Kind::Pocket, StepType::Pocket, true, Route::Pocket},
      {Kind::Fillet, StepType::Fillet, true, Route::Fillet},
      {Kind::Chamfer, StepType::Chamfer, true, Route::Chamfer},
      {Kind::Mirror, StepType::Mirror, true, Route::Mirror},
      {Kind::Move, StepType::Move, true, Route::Move},
      {Kind::LinearPattern, StepType::LinearPattern, true,
       Route::LinearPattern},
      {Kind::CircularPattern, StepType::CircularPattern, true,
       Route::CircularPattern},
      {Kind::JoinBodies, StepType::JoinBodies, true, Route::JoinBodies},
      {Kind::Shell, StepType::Shell, true, Route::Shell},
      {Kind::Draft, StepType::Draft, true, Route::Draft},
  }};
  const auto descriptors = solidar::featureUiDescriptors();
  CHECK(descriptors.size() == kExpectedDescriptors.size());
  std::set<Kind> registeredKinds;
  std::set<Route> registeredRoutes;
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const auto& descriptor = descriptors[index];
    const auto& expected = kExpectedDescriptors[index];
    CHECK(descriptor.kind == expected.kind);
    CHECK(descriptor.historyType == expected.stepType);
    CHECK(descriptor.editable == expected.editable);
    CHECK(descriptor.editorRoute == expected.route);
    CHECK(descriptor.titleUtf8 != nullptr);
    CHECK(!descriptor.title().isEmpty());
    CHECK(descriptor.formatParameters != nullptr);
    CHECK(descriptor.consumesSketch != nullptr);
    CHECK(registeredKinds.insert(descriptor.kind).second);
    CHECK(registeredRoutes.insert(descriptor.editorRoute).second ||
          descriptor.editorRoute == Route::None);
    CHECK(solidar::featureUiDescriptor(descriptor.kind) == &descriptor);
  }

  solidar::Document importedDocument;
  auto& importedBody = importedDocument.addBody("Imported body");
  const auto importedId = importedBody
                              .addFeature(std::make_unique<
                                          solidar::ImportedShapeFeature>(
                                  std::make_shared<const TopoDS_Shape>(),
                                  "Imported source"))
                              .id();
  const auto importedSteps =
      solidar::buildPartDesignHistory(importedDocument, importedBody);
  CHECK(importedSteps.size() == 1);
  CHECK(importedSteps.front().featureId == importedId);
  CHECK(importedSteps.front().type == StepType::ImportedShape);
  CHECK(!importedSteps.front().editable);
  CHECK(!importedSteps.front().icon.isNull());
  CHECK(importedSteps.front().title.contains(QString::fromUtf8("Импорт")));
  CHECK(!importedSteps.front().title.contains(QString::fromUtf8("Эскиз")));

  solidar::Document sketchOnlyDocument;
  const auto sketchOnlyId = sketchOnlyDocument.addSketch("Standalone").id;
  const auto sketchOnlySteps = solidar::buildPartDesignHistory(
      sketchOnlyDocument, static_cast<const solidar::Body*>(nullptr));
  CHECK(sketchOnlySteps.size() == 1);
  CHECK(sketchOnlySteps.front().sketchId == sketchOnlyId);
  CHECK(!solidar::isSketchConsumedByPartDesign(sketchOnlyDocument,
                                               sketchOnlyId));

  solidar::Document document;
  auto& sketch1 = document.addSketch("Sketch 1");
  const auto sketch1Id = sketch1.id;
  auto& body = document.addBody("Body");
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(sketch1Id, 30.0);
  const auto extrudeId = extrude->id(); body.addFeature(std::move(extrude));
  solidar::EdgeReference edge{body.id(), extrudeId, 0};
  auto fillet = std::make_unique<solidar::FilletFeature>(edge, 4.0);
  const auto filletId = fillet->id(); body.addFeature(std::move(fillet));
  auto& sketch2 = document.addSketch("Sketch on Face");
  const auto sketch2Id = sketch2.id;
  sketch2.support.type = solidar::SketchSupportType::Face;
  sketch2.support.face = {body.id(), filletId, 0};
  auto pocket = std::make_unique<solidar::PocketFeature>(sketch2Id, 10.0);
  const auto pocketId = pocket->id(); body.addFeature(std::move(pocket));
  solidar::EdgeReference pocketEdge{body.id(), pocketId, 0};
  auto chamfer = std::make_unique<solidar::ChamferFeature>(pocketEdge, 2.0);
  const auto chamferId = chamfer->id(); body.addFeature(std::move(chamfer));
  auto shell = std::make_unique<solidar::ShellFeature>(
      chamferId, std::vector<solidar::FaceReference>{{body.id(), chamferId, 0}}, 2.0);
  const auto shellId = shell->id(); body.addFeature(std::move(shell));
  auto draft = std::make_unique<solidar::DraftFeature>(
      shellId, std::vector<solidar::FaceReference>{{body.id(), shellId, 0}},
      solidar::PlaneReference{}, solidar::AxisReference{solidar::AxisReferenceType::GlobalZ}, 5.0);
  const auto draftId = draft->id(); body.addFeature(std::move(draft));
  auto mirror = std::make_unique<solidar::MirrorFeature>(draftId, solidar::MirrorPlane::YZ);
  const auto mirrorId = mirror->id(); body.addFeature(std::move(mirror));
  auto linear = std::make_unique<solidar::LinearPatternFeature>(
      mirrorId, solidar::PrincipalAxis::X, 4, 20.0);
  const auto linearId = linear->id(); body.addFeature(std::move(linear));
  auto circular = std::make_unique<solidar::CircularPatternFeature>(
      linearId, solidar::PrincipalAxis::Z, 6, 360.0);
  const auto circularId = circular->id(); body.addFeature(std::move(circular));

  const auto steps = solidar::buildPartDesignHistory(document, body);
  CHECK(steps.size() == 11);
  CHECK(steps[0].sketchId == sketch1Id);
  CHECK(steps[1].featureId == extrudeId);
  CHECK(steps[2].featureId == filletId);
  CHECK(steps[3].sketchId == sketch2Id);
  CHECK(steps[4].featureId == pocketId);
  CHECK(steps[5].featureId == chamferId);
  CHECK(steps[6].featureId == shellId);
  CHECK(steps[7].featureId == draftId);
  CHECK(steps[8].featureId == mirrorId);
  CHECK(steps[9].featureId == linearId);
  CHECK(steps[10].featureId == circularId);
  CHECK(solidar::isSketchConsumedByPartDesign(document, sketch1Id));
  CHECK(solidar::isSketchConsumedByPartDesign(document, sketch2Id));

  std::set<solidar::FeatureId> ids;
  for (const auto& step : steps) {
    CHECK(!step.icon.isNull()); CHECK(!step.tooltip.isEmpty());
    if (step.featureId != solidar::kInvalidFeatureId) CHECK(ids.insert(step.featureId).second);
  }
  CHECK(solidar::findHistoryFeature(document, body.id(), filletId) ==
        body.features()[1].get());
  CHECK(solidar::findHistoryFeature(document, body.id(), chamferId) ==
        body.features()[3].get());
  body.features()[1]->markBlocked("test failure");
  const auto failedSteps = solidar::buildPartDesignHistory(document, body);
  CHECK(failedSteps.size() == steps.size());
  CHECK(failedSteps[2].featureId == filletId);
  CHECK(failedSteps[2].state == solidar::FeatureState::Error);
  CHECK(failedSteps[5].featureId == chamferId);
  body.features()[1]->setDirty();
  const auto recoveredSteps = solidar::buildPartDesignHistory(document, body);
  CHECK(recoveredSteps[2].featureId == filletId);
  CHECK(recoveredSteps[2].state == solidar::FeatureState::Dirty);

  auto& revolveSketch = document.addSketch("Revolve sketch");
  const auto revolveSketchId = revolveSketch.id;
  auto& revolveBody = document.addBody("Revolve body");
  auto revolve = std::make_unique<solidar::RevolveFeature>(
      revolveSketchId, solidar::AxisReference{}, 180.0);
  const auto revolveId = revolve->id(); revolveBody.addFeature(std::move(revolve));
  const auto revolveSteps = solidar::buildPartDesignHistory(document, revolveBody);
  CHECK(revolveSteps.size() == 2);
  CHECK(revolveSteps[0].sketchId == revolveSketchId);
  CHECK(revolveSteps[1].featureId == revolveId);
  CHECK(solidar::isSketchConsumedByPartDesign(document, revolveSketchId));

  solidar::Document persisted;
  auto& persistedSketch = persisted.addSketch();
  persistedSketch.geometry.addRectangle({10, 0}, {20, 5});
  auto& persistedBody = persisted.addBody();
  auto persistedExtrude = std::make_unique<solidar::ExtrudeFeature>(
      persistedSketch.id, 5.0);
  const auto persistedExtrudeId = persistedExtrude->id();
  persistedBody.addFeature(std::move(persistedExtrude));
  auto persistedMove = std::make_unique<solidar::MoveFeature>(
      persistedExtrudeId, solidar::Vector3d{7.0, -3.0, 2.0});
  const auto persistedMoveId = persistedMove->id();
  persistedBody.addFeature(std::move(persistedMove));
  auto persistedMirror = std::make_unique<solidar::MirrorFeature>(
      persistedMoveId, solidar::MirrorPlane::YZ);
  const auto persistedMirrorId = persistedMirror->id();
  persistedBody.addFeature(std::move(persistedMirror));
  auto persistedLinear = std::make_unique<solidar::LinearPatternFeature>(
      persistedMirrorId, solidar::PrincipalAxis::Y, 3, 40.0);
  const auto persistedLinearId = persistedLinear->id();
  persistedBody.addFeature(std::move(persistedLinear));
  CHECK(persisted.recompute());
  const auto persistedSteps = solidar::buildPartDesignHistory(persisted, persistedBody);
  CHECK(persistedSteps.size() == 5);
  CHECK(persistedSteps[2].type == solidar::HistoryStepType::Move);
  CHECK(persistedSteps[2].featureId == persistedMoveId);
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("part-design-history-ui-tests-XXXXXX")));
  CHECK(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("history.solidar"));
  QString error;
  CHECK(solidar::project::ProjectFile::saveDocument(path, persisted, &error));
  solidar::Document restored;
  CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
  CHECK(restored.bodies().size() == 1);
  const auto restoredSteps = solidar::buildPartDesignHistory(restored, restored.bodies()[0]);
  CHECK(restoredSteps.size() == persistedSteps.size());
  for (std::size_t i = 0; i < persistedSteps.size(); ++i) {
    CHECK(restoredSteps[i].type == persistedSteps[i].type);
    CHECK(restoredSteps[i].featureId == persistedSteps[i].featureId);
    CHECK(restoredSteps[i].sketchId == persistedSteps[i].sketchId);
  }
  return EXIT_SUCCESS;
}

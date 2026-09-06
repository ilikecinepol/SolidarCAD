#include <QApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"
#include "project/ProjectFile.h"
#include "ui/PartDesignHistory.h"

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  solidar::Document sketchOnlyDocument;
  const auto sketchOnlyId = sketchOnlyDocument.addSketch("Standalone").id;
  const auto sketchOnlySteps = solidar::buildPartDesignHistory(
      sketchOnlyDocument, static_cast<const solidar::Body*>(nullptr));
  CHECK(sketchOnlySteps.size() == 1);
  CHECK(sketchOnlySteps.front().sketchId == sketchOnlyId);
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

  solidar::Document persisted;
  auto& persistedSketch = persisted.addSketch();
  persistedSketch.geometry.addRectangle({10, 0}, {20, 5});
  auto& persistedBody = persisted.addBody();
  auto persistedExtrude = std::make_unique<solidar::ExtrudeFeature>(
      persistedSketch.id, 5.0);
  const auto persistedExtrudeId = persistedExtrude->id();
  persistedBody.addFeature(std::move(persistedExtrude));
  auto persistedMirror = std::make_unique<solidar::MirrorFeature>(
      persistedExtrudeId, solidar::MirrorPlane::YZ);
  const auto persistedMirrorId = persistedMirror->id();
  persistedBody.addFeature(std::move(persistedMirror));
  auto persistedLinear = std::make_unique<solidar::LinearPatternFeature>(
      persistedMirrorId, solidar::PrincipalAxis::Y, 3, 40.0);
  const auto persistedLinearId = persistedLinear->id();
  persistedBody.addFeature(std::move(persistedLinear));
  CHECK(persisted.recompute());
  const auto persistedSteps = solidar::buildPartDesignHistory(persisted, persistedBody);
  QTemporaryDir directory; CHECK(directory.isValid());
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

#include <cstdlib>
#include <iostream>
#include <memory>

#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/ShellFeature.h"

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)

int main() {
  solidar::Document document;
  const auto sketchId = document.addSketch().id;
  auto& body = document.addBody();
  const auto bodyId = body.id();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(sketchId, 20.0);
  const auto extrudeId = extrude->id(); body.addFeature(std::move(extrude));
  auto chamfer = std::make_unique<solidar::ChamferFeature>(
      solidar::EdgeReference{bodyId, extrudeId, 0}, 2.0);
  const auto chamferId = chamfer->id(); body.addFeature(std::move(chamfer));
  auto shell = std::make_unique<solidar::ShellFeature>(
      chamferId, std::vector<solidar::FaceReference>{{bodyId, chamferId, 0}}, 1.0);
  const auto shellId = shell->id(); body.addFeature(std::move(shell));
  const solidar::Document undoSnapshot = document;

  auto lastPlan = document.planFeatureRemoval(bodyId, shellId);
  CHECK(lastPlan.featureIds.size() == 1 && lastPlan.featureIds[0] == shellId);
  std::string error;
  CHECK(document.removeFeatureCascade(bodyId, shellId, &error));
  CHECK(document.activeBody()->features().size() == 2);
  document = undoSnapshot;
  CHECK(document.activeBody()->features().size() == 3);
  CHECK(document.activeBody()->features()[2]->id() == shellId);

  const auto middlePlan = document.planFeatureRemoval(bodyId, chamferId);
  CHECK(middlePlan.featureIds.size() == 2);
  CHECK(middlePlan.featureIds[0] == chamferId && middlePlan.featureIds[1] == shellId);
  CHECK(document.removeFeatureCascade(bodyId, chamferId, &error));
  CHECK(document.activeBody()->features().size() == 1);

  document = undoSnapshot;
  auto sketchPlan = document.planSketchRemoval(sketchId);
  CHECK(sketchPlan.featureIds.size() == 3);
  CHECK(document.removeSketchCascade(sketchId, &error));
  CHECK(document.sketches().empty());
  CHECK(document.activeBody()->features().empty());

  solidar::Document faceDocument;
  const auto baseSketchId = faceDocument.addSketch().id;
  auto& faceBody = faceDocument.addBody();
  auto base = std::make_unique<solidar::ExtrudeFeature>(baseSketchId, 20.0);
  const auto baseId = base->id(); faceBody.addFeature(std::move(base));
  auto edgeFeature = std::make_unique<solidar::ChamferFeature>(
      solidar::EdgeReference{faceBody.id(), baseId, 0}, 2.0);
  const auto edgeFeatureId = edgeFeature->id();
  faceBody.addFeature(std::move(edgeFeature));
  auto& faceSketch = faceDocument.addSketch();
  const auto faceSketchId = faceSketch.id;
  faceSketch.support = {solidar::SketchSupportType::Face,
                        {faceBody.id(), edgeFeatureId, 0}};
  faceBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(faceSketchId, 5.0));
  const auto facePlan = faceDocument.planFeatureRemoval(faceBody.id(), edgeFeatureId);
  CHECK(facePlan.featureIds.size() == 2);
  CHECK(facePlan.sketchIds.size() == 1 && facePlan.sketchIds[0] == faceSketchId);
  return EXIT_SUCCESS;
}

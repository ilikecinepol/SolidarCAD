#include <cstdlib>
#include <iostream>
#include <memory>

#include "TestGeometryUtils.h"
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

  // Body removal owns its features and face-supported sketches. A base-plane
  // sketch is a reusable datum and must survive even when one consumer Body is
  // removed. Undo/redo snapshots retain stable BodyIds and shapes.
  solidar::Document bodyDocument;
  auto& firstSketch = bodyDocument.addSketch("First body sketch");
  const auto firstSketchId = firstSketch.id;
  firstSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& firstBody = bodyDocument.addBody("First body");
  const auto firstBodyId = firstBody.id();
  firstBody.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(firstSketchId, 10.0));
  auto& secondSketch = bodyDocument.addSketch("Second body sketch");
  const auto secondSketchId = secondSketch.id;
  secondSketch.geometry.addRectangle({30.0, 0.0}, {40.0, 10.0});
  auto& secondBody = bodyDocument.addBody("Second body");
  const auto secondBodyId = secondBody.id();
  secondBody.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(secondSketchId, 8.0));
  CHECK(bodyDocument.recompute());
  const solidar::Document beforeBodyRemoval = bodyDocument;
  CHECK(bodyDocument.removeBodyCascade(firstBodyId, &error));
  CHECK(bodyDocument.recompute());
  CHECK(bodyDocument.findBody(firstBodyId) == nullptr);
  CHECK(bodyDocument.findSketch(firstSketchId) != nullptr);
  CHECK(bodyDocument.findBody(secondBodyId) != nullptr);
  CHECK(bodyDocument.findSketch(secondSketchId) != nullptr);
  CHECK(bodyDocument.findBody(secondBodyId)->resultShape());
  const solidar::Document afterBodyRemoval = bodyDocument;
  bodyDocument = beforeBodyRemoval;  // UI Undo snapshot.
  CHECK(bodyDocument.findBody(firstBodyId) != nullptr);
  CHECK(bodyDocument.findBody(secondBodyId) != nullptr);
  bodyDocument = afterBodyRemoval;  // UI Redo snapshot.
  CHECK(bodyDocument.findBody(firstBodyId) == nullptr);
  CHECK(bodyDocument.findBody(secondBodyId) != nullptr);

  // Two Bodies may intentionally consume the same base-plane sketch. Removing
  // either Body must not delete shared input or invalidate the survivor.
  solidar::Document sharedDocument;
  auto& sharedSketch = sharedDocument.addSketch("Shared profile");
  const auto sharedSketchId = sharedSketch.id;
  sharedSketch.geometry.addRectangle({0.0, 0.0}, {12.0, 8.0});
  auto& sharedFirstBody = sharedDocument.addBody("Shared consumer one");
  const auto sharedFirstBodyId = sharedFirstBody.id();
  sharedFirstBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      sharedSketchId, 4.0));
  auto& sharedSecondBody = sharedDocument.addBody("Shared consumer two");
  const auto sharedSecondBodyId = sharedSecondBody.id();
  sharedSecondBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      sharedSketchId, 9.0));
  CHECK(sharedDocument.recompute());
  CHECK(sharedDocument.removeBodyCascade(sharedFirstBodyId, &error));
  CHECK(sharedDocument.recompute());
  CHECK(sharedDocument.findSketch(sharedSketchId));
  CHECK(sharedDocument.findBody(sharedFirstBodyId) == nullptr);
  CHECK(sharedDocument.findBody(sharedSecondBodyId));
  CHECK(sharedDocument.findBody(sharedSecondBodyId)->features().size() == 1);
  CHECK(sharedDocument.findBody(sharedSecondBodyId)->resultShape());

  // A sketch supported by the removed Body may be consumed by another Body.
  // Cascade that dependent feature instead of leaving a dangling FaceReference.
  solidar::Document dependentDocument;
  auto& dependentBaseSketch = dependentDocument.addSketch("Support owner");
  dependentBaseSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
  auto& supportBody = dependentDocument.addBody("Support body");
  const auto supportBodyId = supportBody.id();
  auto supportExtrude = std::make_unique<solidar::ExtrudeFeature>(
      dependentBaseSketch.id, 10.0);
  const auto supportFeatureId = supportExtrude->id();
  supportBody.addFeature(std::move(supportExtrude));
  CHECK(dependentDocument.recompute());
  const auto supportFace =
      solidar::test::topPlanarFace(*supportBody.resultShape(), 10.0);
  CHECK(supportFace);
  auto& dependentSketch = dependentDocument.addSketch("Dependent sketch");
  const auto dependentSketchId = dependentSketch.id;
  dependentSketch.geometry.addRectangle({2.0, 2.0}, {6.0, 6.0});
  CHECK(dependentDocument.attachSketchToFace(
      dependentSketchId,
      {supportBodyId, supportFeatureId, *supportFace}));
  auto& dependentBody = dependentDocument.addBody("Dependent body");
  const auto dependentBodyId = dependentBody.id();
  dependentBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      dependentSketchId, 3.0));
  CHECK(dependentDocument.recompute());
  CHECK(dependentDocument.removeBodyCascade(supportBodyId, &error));
  CHECK(dependentDocument.recompute());
  CHECK(dependentDocument.findSketch(dependentSketchId) == nullptr);
  CHECK(dependentDocument.findBody(dependentBodyId));
  CHECK(dependentDocument.findBody(dependentBodyId)->features().empty());
  return EXIT_SUCCESS;
}

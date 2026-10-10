#include "TestAssertions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "TestGeometryUtils.h"
#include "model/CircularPatternFeature.h"
#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/ShellFeature.h"

namespace {

const solidar::BodyFeatureRemovalRange* findBodyRange(
    const solidar::FeatureRemovalPlan& plan, solidar::BodyId bodyId) {
  const auto found = std::find_if(
      plan.bodyRanges.begin(), plan.bodyRanges.end(),
      [bodyId](const solidar::BodyFeatureRemovalRange& range) {
        return range.bodyId == bodyId;
      });
  return found == plan.bodyRanges.end() ? nullptr : &*found;
}

solidar::Document makeCyclicPatternDocument(bool reverseBodyOrder) {
  constexpr solidar::BodyId kFirstBodyId = 71001;
  constexpr solidar::BodyId kSecondBodyId = 71002;
  constexpr solidar::FeatureId kFirstFeatureId = 72001;
  constexpr solidar::FeatureId kSecondFeatureId = 72002;

  solidar::Document document;
  const auto addFirst = [&] {
    document.addBody(kFirstBodyId, "Cycle body A")
        .addFeature(std::make_unique<solidar::LinearPatternFeature>(
            kFirstFeatureId, kSecondBodyId, kSecondFeatureId,
            solidar::PrincipalAxis::X, 2, 10.0,
            solidar::PatternOperation::NewBody, "Cycle feature A"));
  };
  const auto addSecond = [&] {
    document.addBody(kSecondBodyId, "Cycle body B")
        .addFeature(std::make_unique<solidar::LinearPatternFeature>(
            kSecondFeatureId, kFirstBodyId, kFirstFeatureId,
            solidar::PrincipalAxis::Y, 2, 10.0,
            solidar::PatternOperation::NewBody, "Cycle feature B"));
  };
  if (reverseBodyOrder) {
    addSecond();
    addFirst();
  } else {
    addFirst();
    addSecond();
  }
  return document;
}

bool mentionsDependencyCycle(const std::string& diagnostic) {
  return diagnostic.find("cycle") != std::string::npos ||
         diagnostic.find("Cycle") != std::string::npos ||
         diagnostic.find("cyclic") != std::string::npos ||
         diagnostic.find("Cyclic") != std::string::npos;
}

}  // namespace

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

  // Removing a shared sketch is one atomic multi-Body operation. The plan
  // retains a distinct, deterministic suffix for every consumer Body.
  solidar::Document sharedSketchDocument;
  auto& multiBodySketch = sharedSketchDocument.addSketch("Shared input");
  const auto multiBodySketchId = multiBodySketch.id;
  multiBodySketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& multiBodyFirst = sharedSketchDocument.addBody("Consumer A");
  const auto multiBodyFirstId = multiBodyFirst.id();
  auto& multiBodyFirstFeature = multiBodyFirst.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(multiBodySketchId, 4.0));
  const auto multiBodyFirstFeatureId = multiBodyFirstFeature.id();
  auto& multiBodySecond = sharedSketchDocument.addBody("Consumer B");
  const auto multiBodySecondId = multiBodySecond.id();
  auto& multiBodySecondFeature = multiBodySecond.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(multiBodySketchId, 8.0));
  const auto multiBodySecondFeatureId = multiBodySecondFeature.id();
  CHECK(sharedSketchDocument.recompute());

  const auto sharedSketchPlan =
      sharedSketchDocument.planSketchRemoval(multiBodySketchId);
  CHECK(sharedSketchPlan.applicable);
  CHECK(sharedSketchPlan.diagnostic.empty());
  CHECK(sharedSketchPlan.bodyRanges.size() == 2);
  CHECK(sharedSketchPlan.featureIds.size() == 2);
  CHECK(sharedSketchPlan.sketchIds.size() == 1);
  CHECK(sharedSketchPlan.sketchIds[0] == multiBodySketchId);
  const auto* multiBodyFirstRange =
      findBodyRange(sharedSketchPlan, multiBodyFirstId);
  const auto* multiBodySecondRange =
      findBodyRange(sharedSketchPlan, multiBodySecondId);
  CHECK(multiBodyFirstRange);
  CHECK(multiBodyFirstRange->firstFeatureIndex == 0);
  CHECK(multiBodyFirstRange->featureIds.size() == 1);
  CHECK(multiBodyFirstRange->featureIds[0] == multiBodyFirstFeatureId);
  CHECK(multiBodySecondRange);
  CHECK(multiBodySecondRange->firstFeatureIndex == 0);
  CHECK(multiBodySecondRange->featureIds.size() == 1);
  CHECK(multiBodySecondRange->featureIds[0] == multiBodySecondFeatureId);
  std::string stage2Error;
  CHECK(sharedSketchDocument.applyRemovalPlan(sharedSketchPlan, &stage2Error));
  CHECK(stage2Error.empty());
  CHECK(sharedSketchDocument.findSketch(multiBodySketchId) == nullptr);
  CHECK(sharedSketchDocument.findBody(multiBodyFirstId));
  CHECK(sharedSketchDocument.findBody(multiBodyFirstId)->features().empty());
  CHECK(sharedSketchDocument.findBody(multiBodySecondId));
  CHECK(sharedSketchDocument.findBody(multiBodySecondId)->features().empty());

  // Dependencies are transitive across Bodies: A and B feed Join Bodies C,
  // which feeds two NewBody patterns. Removing A preserves independent B but
  // clears every feature that can no longer resolve its source.
  solidar::Document crossBodyDocument;
  auto& crossSketchA = crossBodyDocument.addSketch("Cross-body A");
  const auto crossSketchAId = crossSketchA.id;
  crossSketchA.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
  auto& crossBodyA = crossBodyDocument.addBody("Cross-body A");
  const auto crossBodyAId = crossBodyA.id();
  auto& crossFeatureA = crossBodyA.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(crossSketchAId, 10.0));
  const auto crossFeatureAId = crossFeatureA.id();
  auto& crossSketchB = crossBodyDocument.addSketch("Cross-body B");
  const auto crossSketchBId = crossSketchB.id;
  crossSketchB.geometry.addRectangle({10.0, 0.0}, {30.0, 20.0});
  auto& crossBodyB = crossBodyDocument.addBody("Cross-body B");
  const auto crossBodyBId = crossBodyB.id();
  auto& crossFeatureB = crossBodyB.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(crossSketchBId, 10.0));
  const auto crossFeatureBId = crossFeatureB.id();
  auto& crossBodyC = crossBodyDocument.addBody("Join result");
  const auto crossBodyCId = crossBodyC.id();
  auto& crossJoin = crossBodyC.addFeature(
      std::make_unique<solidar::JoinBodiesFeature>(
          crossBodyAId, crossFeatureAId, crossBodyBId, crossFeatureBId,
          "Join A and B"));
  const auto crossJoinId = crossJoin.id();
  auto& crossBodyD = crossBodyDocument.addBody("Linear result");
  const auto crossBodyDId = crossBodyD.id();
  auto& crossLinear = crossBodyD.addFeature(
      std::make_unique<solidar::LinearPatternFeature>(
          crossBodyCId, crossJoinId, solidar::PrincipalAxis::X, 2, 40.0,
          solidar::PatternOperation::NewBody, "Linear from join"));
  const auto crossLinearId = crossLinear.id();
  auto& crossBodyE = crossBodyDocument.addBody("Circular result");
  const auto crossBodyEId = crossBodyE.id();
  auto& crossCircular = crossBodyE.addFeature(
      std::make_unique<solidar::CircularPatternFeature>(
          crossBodyDId, crossLinearId, solidar::PrincipalAxis::Z, 2, 180.0,
          solidar::PatternOperation::NewBody, "Circular from linear"));
  const auto crossCircularId = crossCircular.id();
  CHECK(crossBodyDocument.recompute());
  const solidar::Document crossBodySnapshot = crossBodyDocument;

  const auto crossPlan =
      crossBodyDocument.planFeatureRemoval(crossBodyAId, crossFeatureAId);
  CHECK(crossPlan.applicable);
  CHECK(crossPlan.bodyRanges.size() == 4);
  CHECK(crossPlan.featureIds.size() == 4);
  CHECK(findBodyRange(crossPlan, crossBodyAId));
  CHECK(findBodyRange(crossPlan, crossBodyAId)->featureIds[0] ==
        crossFeatureAId);
  CHECK(findBodyRange(crossPlan, crossBodyBId) == nullptr);
  CHECK(findBodyRange(crossPlan, crossBodyCId));
  CHECK(findBodyRange(crossPlan, crossBodyCId)->featureIds[0] == crossJoinId);
  CHECK(findBodyRange(crossPlan, crossBodyDId));
  CHECK(findBodyRange(crossPlan, crossBodyDId)->featureIds[0] == crossLinearId);
  CHECK(findBodyRange(crossPlan, crossBodyEId));
  CHECK(findBodyRange(crossPlan, crossBodyEId)->featureIds[0] ==
        crossCircularId);
  CHECK(crossBodyDocument.removeFeatureCascade(
      crossBodyAId, crossFeatureAId, &stage2Error));
  CHECK(crossBodyDocument.findBody(crossBodyAId)->features().empty());
  CHECK(crossBodyDocument.findBody(crossBodyBId)->features().size() == 1);
  CHECK(crossBodyDocument.findBody(crossBodyBId)->features()[0]->id() ==
        crossFeatureBId);
  CHECK(crossBodyDocument.findBody(crossBodyCId)->features().empty());
  CHECK(crossBodyDocument.findBody(crossBodyDId)->features().empty());
  CHECK(crossBodyDocument.findBody(crossBodyEId)->features().empty());
  CHECK(crossBodyDocument.findSketch(crossSketchAId));
  CHECK(crossBodyDocument.findSketch(crossSketchBId));
  CHECK(crossBodyDocument.recompute());
  CHECK(crossBodyDocument.findBody(crossBodyBId)->resultShape());

  // Body removal uses the same graph and per-Body ranges. The selected Body
  // disappears, its reusable base-plane Sketch survives, and all transitive
  // dependents are removed without touching the independent source Body.
  solidar::Document crossBodyRemoval = crossBodySnapshot;
  const auto bodyCascadePlan = crossBodyRemoval.planBodyRemoval(crossBodyAId);
  CHECK(bodyCascadePlan.applicable);
  CHECK(bodyCascadePlan.bodyIds.size() == 1);
  CHECK(bodyCascadePlan.bodyIds[0] == crossBodyAId);
  CHECK(findBodyRange(bodyCascadePlan, crossBodyBId) == nullptr);
  CHECK(findBodyRange(bodyCascadePlan, crossBodyCId));
  CHECK(findBodyRange(bodyCascadePlan, crossBodyDId));
  CHECK(findBodyRange(bodyCascadePlan, crossBodyEId));
  CHECK(crossBodyRemoval.applyRemovalPlan(bodyCascadePlan, &stage2Error));
  CHECK(crossBodyRemoval.findBody(crossBodyAId) == nullptr);
  CHECK(crossBodyRemoval.findSketch(crossSketchAId));
  CHECK(crossBodyRemoval.findBody(crossBodyBId));
  CHECK(crossBodyRemoval.findBody(crossBodyBId)->features().size() == 1);
  CHECK(crossBodyRemoval.findBody(crossBodyCId)->features().empty());
  CHECK(crossBodyRemoval.findBody(crossBodyDId)->features().empty());
  CHECK(crossBodyRemoval.findBody(crossBodyEId)->features().empty());
  CHECK(crossBodyRemoval.recompute());

  // A face-supported Sketch is itself a dependency node. Removing its support
  // Feature removes the Sketch, its cross-Body Extrude, and a downstream
  // NewBody pattern in one closure.
  solidar::Document faceCascadeDocument;
  auto& faceCascadeBaseSketch =
      faceCascadeDocument.addSketch("Face cascade base");
  const auto faceCascadeBaseSketchId = faceCascadeBaseSketch.id;
  faceCascadeBaseSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
  auto& faceCascadeSupportBody =
      faceCascadeDocument.addBody("Face support owner");
  const auto faceCascadeSupportBodyId = faceCascadeSupportBody.id();
  auto& faceCascadeSupportFeature = faceCascadeSupportBody.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(faceCascadeBaseSketchId,
                                               12.0));
  const auto faceCascadeSupportFeatureId = faceCascadeSupportFeature.id();
  CHECK(faceCascadeDocument.recompute());
  const auto faceCascadeTop = solidar::test::topPlanarFace(
      *faceCascadeSupportBody.resultShape(), 12.0);
  CHECK(faceCascadeTop);
  auto& faceCascadeSketch =
      faceCascadeDocument.addSketch("Face-supported dependency");
  const auto faceCascadeSketchId = faceCascadeSketch.id;
  faceCascadeSketch.geometry.addRectangle({2.0, 2.0}, {8.0, 8.0});
  CHECK(faceCascadeDocument.attachSketchToFace(
      faceCascadeSketchId,
      {faceCascadeSupportBodyId, faceCascadeSupportFeatureId,
       *faceCascadeTop}));
  auto& faceCascadeConsumerBody =
      faceCascadeDocument.addBody("Face sketch consumer");
  const auto faceCascadeConsumerBodyId = faceCascadeConsumerBody.id();
  auto& faceCascadeConsumer = faceCascadeConsumerBody.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(faceCascadeSketchId, 5.0));
  const auto faceCascadeConsumerId = faceCascadeConsumer.id();
  auto& faceCascadePatternBody =
      faceCascadeDocument.addBody("Face cascade pattern");
  const auto faceCascadePatternBodyId = faceCascadePatternBody.id();
  auto& faceCascadePattern = faceCascadePatternBody.addFeature(
      std::make_unique<solidar::LinearPatternFeature>(
          faceCascadeConsumerBodyId, faceCascadeConsumerId,
          solidar::PrincipalAxis::X, 2, 30.0,
          solidar::PatternOperation::NewBody, "Pattern from face sketch"));
  const auto faceCascadePatternId = faceCascadePattern.id();
  CHECK(faceCascadeDocument.recompute());

  // Dirty propagation follows the same support path. Moving the support face
  // must rebuild both the cross-Body consumer and its downstream pattern in a
  // single recompute, without relying on Body storage order.
  const auto consumerShapeBefore = faceCascadeConsumer.shape();
  const auto patternShapeBefore = faceCascadePattern.shape();
  CHECK(consumerShapeBefore && patternShapeBefore);
  const double consumerMinZBefore =
      solidar::test::boundsOf(*consumerShapeBefore).minZ;
  auto* faceSupportExtrude = dynamic_cast<solidar::ExtrudeFeature*>(
      &faceCascadeSupportFeature);
  CHECK(faceSupportExtrude);
  faceSupportExtrude->setLengthMm(18.0);
  CHECK(faceCascadeDocument.recompute());
  CHECK(faceCascadeConsumer.shape() != consumerShapeBefore);
  CHECK(faceCascadePattern.shape() != patternShapeBefore);
  CHECK(std::abs(solidar::test::boundsOf(*faceCascadeConsumer.shape()).minZ -
                 consumerMinZBefore) > 1.0e-4);

  const auto faceCascadePlan = faceCascadeDocument.planFeatureRemoval(
      faceCascadeSupportBodyId, faceCascadeSupportFeatureId);
  CHECK(faceCascadePlan.applicable);
  CHECK(faceCascadePlan.bodyRanges.size() == 3);
  CHECK(faceCascadePlan.featureIds.size() == 3);
  CHECK(faceCascadePlan.sketchIds.size() == 1);
  CHECK(faceCascadePlan.sketchIds[0] == faceCascadeSketchId);
  CHECK(findBodyRange(faceCascadePlan, faceCascadeSupportBodyId));
  CHECK(findBodyRange(faceCascadePlan, faceCascadeSupportBodyId)
            ->featureIds[0] == faceCascadeSupportFeatureId);
  CHECK(findBodyRange(faceCascadePlan, faceCascadeConsumerBodyId));
  CHECK(findBodyRange(faceCascadePlan, faceCascadeConsumerBodyId)
            ->featureIds[0] == faceCascadeConsumerId);
  CHECK(findBodyRange(faceCascadePlan, faceCascadePatternBodyId));
  CHECK(findBodyRange(faceCascadePlan, faceCascadePatternBodyId)
            ->featureIds[0] == faceCascadePatternId);
  CHECK(faceCascadeDocument.removeFeatureCascade(
      faceCascadeSupportBodyId, faceCascadeSupportFeatureId, &stage2Error));
  CHECK(faceCascadeDocument.findSketch(faceCascadeBaseSketchId));
  CHECK(faceCascadeDocument.findSketch(faceCascadeSketchId) == nullptr);
  CHECK(faceCascadeDocument.findBody(faceCascadeSupportBodyId)
            ->features()
            .empty());
  CHECK(faceCascadeDocument.findBody(faceCascadeConsumerBodyId)
            ->features()
            .empty());
  CHECK(faceCascadeDocument.findBody(faceCascadePatternBodyId)
            ->features()
            .empty());
  CHECK(faceCascadeDocument.recompute());

  // Applying a previewed plan after the history changes must fail before any
  // mutation. A freshly planned equivalent operation remains applicable.
  solidar::Document stalePlanDocument;
  auto& staleSketch = stalePlanDocument.addSketch("Stale plan sketch");
  const auto staleSketchId = staleSketch.id;
  staleSketch.geometry.addRectangle({0.0, 0.0}, {8.0, 8.0});
  auto& staleBody = stalePlanDocument.addBody("Stale plan body");
  const auto staleBodyId = staleBody.id();
  auto& staleExtrude = staleBody.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(staleSketchId, 6.0));
  const auto staleExtrudeId = staleExtrude.id();
  const auto stalePlan =
      stalePlanDocument.planFeatureRemoval(staleBodyId, staleExtrudeId);
  CHECK(stalePlan.applicable);
  CHECK(stalePlan.diagnostic.empty());
  CHECK(stalePlan.bodyRanges.size() == 1);
  CHECK(stalePlan.bodyRanges[0].featureIds.size() == 1);
  auto& lateFeature = staleBody.addFeature(
      std::make_unique<solidar::LinearPatternFeature>(
          staleExtrudeId, solidar::PrincipalAxis::X, 2, 20.0,
          "Added after planning"));
  const auto lateFeatureId = lateFeature.id();
  stage2Error.clear();
  CHECK(!stalePlanDocument.applyRemovalPlan(stalePlan, &stage2Error));
  CHECK(!stage2Error.empty());
  CHECK(stalePlanDocument.findSketch(staleSketchId));
  CHECK(stalePlanDocument.findBody(staleBodyId)->features().size() == 2);
  CHECK(stalePlanDocument.findBody(staleBodyId)->features()[0]->id() ==
        staleExtrudeId);
  CHECK(stalePlanDocument.findBody(staleBodyId)->features()[1]->id() ==
        lateFeatureId);
  const auto refreshedPlan =
      stalePlanDocument.planFeatureRemoval(staleBodyId, staleExtrudeId);
  CHECK(refreshedPlan.applicable);
  CHECK(refreshedPlan.bodyRanges.size() == 1);
  CHECK(refreshedPlan.bodyRanges[0].featureIds.size() == 2);
  CHECK(stalePlanDocument.applyRemovalPlan(refreshedPlan, &stage2Error));
  CHECK(stalePlanDocument.findBody(staleBodyId)->features().empty());
  const auto missingPlan = stalePlanDocument.planFeatureRemoval(
      staleBodyId, static_cast<solidar::FeatureId>(999999999));
  CHECK(!missingPlan.applicable);
  CHECK(!missingPlan.diagnostic.empty());

  // Cycle diagnostics are deterministic and identify every participating
  // Feature, regardless of the storage order of their Bodies.
  auto orderedCycle = makeCyclicPatternDocument(false);
  CHECK(!orderedCycle.recompute());
  const std::string orderedCycleError = orderedCycle.rebuildError();
  CHECK(mentionsDependencyCycle(orderedCycleError));
  CHECK(orderedCycleError.find("72001") != std::string::npos);
  CHECK(orderedCycleError.find("72002") != std::string::npos);
  auto reversedCycle = makeCyclicPatternDocument(true);
  CHECK(!reversedCycle.recompute());
  const std::string reversedCycleError = reversedCycle.rebuildError();
  CHECK(mentionsDependencyCycle(reversedCycleError));
  CHECK(reversedCycleError.find("72001") != std::string::npos);
  CHECK(reversedCycleError.find("72002") != std::string::npos);
  CHECK(reversedCycleError == orderedCycleError);
  return EXIT_SUCCESS;
}

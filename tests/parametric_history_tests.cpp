#include "TestAssertions.h"
#include "TestGeometryUtils.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>

#include "model/Document.h"
#include "model/TopologyReferenceResolver.h"

namespace {
class CountingFeature final : public solidar::ShapeFeature {
 public:
  CountingFeature(std::string name,
                  solidar::SketchId sketchId = solidar::kInvalidSketchId,
                  solidar::FeatureId featureId = solidar::kInvalidFeatureId)
      : ShapeFeature(std::move(name)),
        sketchId_(sketchId),
        featureId_(featureId) {}
  solidar::FeatureDependencies dependencies() const override {
    ++dependencyDeclarationCount;
    solidar::FeatureDependencies result;
    if (sketchId_ != solidar::kInvalidSketchId)
      result.sketchIds.push_back(sketchId_);
    if (featureId_ != solidar::kInvalidFeatureId)
      result.featureIds.push_back(featureId_);
    return result;
  }
 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    ++rebuildCount;
    clearShape();
    if (fail_) {
      markError("Intentional upstream history failure");
      return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0 + rebuildCount, 10.0, 10.0).Shape()));
    markValid();
    return true;
  }

 public:
  std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<CountingFeature>(*this);
  }
  void setFail(bool fail) { fail_ = fail; setDirty(); }
  void resetDependencyDeclarationCount() const noexcept {
    dependencyDeclarationCount = 0;
  }
  int rebuildCount{};
  mutable int dependencyDeclarationCount{};
 private:
  solidar::SketchId sketchId_{solidar::kInvalidSketchId};
  solidar::FeatureId featureId_{solidar::kInvalidFeatureId};
  bool fail_{};
};
}  // namespace

int main() {
  solidar::Document document;
  auto& rootSketch = document.addSketch("Root sketch");
  const auto rootSketchId = rootSketch.id;
  rootSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
  auto& body = document.addBody("Dependent body");
  const auto bodyId = body.id();
  auto root = std::make_unique<CountingFeature>("Root", rootSketchId);
  auto child = std::make_unique<CountingFeature>("Child");
  auto tail = std::make_unique<CountingFeature>("Tail");
  auto* rootPtr = root.get();
  auto* childPtr = child.get();
  auto* tailPtr = tail.get();
  const auto rootId = rootPtr->id();
  const auto childId = childPtr->id();
  const auto tailId = tailPtr->id();
  body.addFeature(std::move(root));
  body.addFeature(std::move(child));
  body.addFeature(std::move(tail));
  auto& otherBody = document.addBody("Independent body");
  auto other = std::make_unique<CountingFeature>("Independent");
  auto* otherPtr = other.get();
  otherBody.addFeature(std::move(other));
  auto* dependentBody = document.findBody(bodyId);
  CHECK(dependentBody != nullptr);

  CHECK(document.recompute());
  CHECK(rootPtr->rebuildCount == 1 && childPtr->rebuildCount == 1);
  CHECK(tailPtr->rebuildCount == 1 && otherPtr->rebuildCount == 1);
  const std::set<solidar::ShapeRevision> initialRevisions{
      rootPtr->shapeRevision(), childPtr->shapeRevision(),
      tailPtr->shapeRevision(), otherPtr->shapeRevision()};
  CHECK(initialRevisions.size() == 4);
  CHECK(!initialRevisions.contains(solidar::kInvalidShapeRevision));
  std::string topologyError;
  const auto topologyBuildsBeforeFirst =
      solidar::TopologyIndex::buildAttemptCount();
  const auto initialRootTopology = rootPtr->topologyIndex(&topologyError);
  CHECK(initialRootTopology != nullptr);
  CHECK(solidar::TopologyIndex::buildAttemptCount() ==
        topologyBuildsBeforeFirst + 1);
  CHECK(topologyError.empty());
  CHECK(initialRootTopology->revision() == rootPtr->shapeRevision());
  CHECK(rootPtr->topologyIndex() == initialRootTopology);
  CHECK(solidar::TopologyIndex::buildAttemptCount() ==
        topologyBuildsBeforeFirst + 1);
  const auto initialRootRevision = rootPtr->shapeRevision();

  // Each recompute builds one operation-local O(N+E) index. Every Feature
  // declares its dependencies exactly once; the scheduler performs no old
  // all-pairs dependsOnFeature/dependsOnSketch virtual probes.
  rootPtr->resetDependencyDeclarationCount();
  childPtr->resetDependencyDeclarationCount();
  tailPtr->resetDependencyDeclarationCount();
  otherPtr->resetDependencyDeclarationCount();
  CHECK(document.recompute());
  CHECK(rootPtr->dependencyDeclarationCount == 1);
  CHECK(childPtr->dependencyDeclarationCount == 1);
  CHECK(tailPtr->dependencyDeclarationCount == 1);
  CHECK(otherPtr->dependencyDeclarationCount == 1);
  CHECK(rootPtr->rebuildCount == 1 && childPtr->rebuildCount == 1);
  CHECK(tailPtr->rebuildCount == 1 && otherPtr->rebuildCount == 1);
  CHECK(rootPtr->shapeRevision() == initialRootRevision);
  CHECK(rootPtr->topologyIndex() == initialRootTopology);
  solidar::sketch::Sketch replacement;
  replacement.addRectangle({0.0, 0.0}, {25.0, 12.0});
  CHECK(document.replaceSketchGeometry(rootSketchId, replacement));
  CHECK(rootPtr->isDirty() && childPtr->isDirty() && tailPtr->isDirty());
  CHECK(otherPtr->isValid());
  CHECK(document.recompute());
  CHECK(rootPtr->rebuildCount == 2 && childPtr->rebuildCount == 2);
  CHECK(tailPtr->rebuildCount == 2 && otherPtr->rebuildCount == 1);
  CHECK(rootPtr->id() == rootId && childPtr->id() == childId &&
        tailPtr->id() == tailId);
  CHECK(rootPtr->shapeRevision() != initialRootRevision);
  const auto topologyBuildsBeforeRebuilt =
      solidar::TopologyIndex::buildAttemptCount();
  const auto rebuiltRootTopology = rootPtr->topologyIndex(&topologyError);
  CHECK(rebuiltRootTopology != nullptr);
  CHECK(topologyError.empty());
  CHECK(rebuiltRootTopology != initialRootTopology);
  CHECK(rebuiltRootTopology->revision() == rootPtr->shapeRevision());
  CHECK(solidar::TopologyIndex::buildAttemptCount() ==
        topologyBuildsBeforeRebuilt + 1);
  CHECK(rootPtr->topologyIndex() == rebuiltRootTopology);
  CHECK(solidar::TopologyIndex::buildAttemptCount() ==
        topologyBuildsBeforeRebuilt + 1);

  const auto rootLastValid = rootPtr->shape();
  const auto childLastValid = childPtr->shape();
  const auto tailLastValid = tailPtr->shape();
  const auto rootLastValidRevision = rootPtr->shapeRevision();
  rootPtr->setFail(true);
  CHECK(!document.recompute());
  CHECK(rootPtr->isFailed() && !rootPtr->shape() &&
        rootPtr->lastValidShape() == rootLastValid &&
        !rootPtr->error().empty());
  CHECK(childPtr->isFailed() && tailPtr->isFailed());
  CHECK(!childPtr->shape() && !tailPtr->shape());
  CHECK(childPtr->lastValidShape() == childLastValid &&
        tailPtr->lastValidShape() == tailLastValid);
  CHECK(!dependentBody->resultShape());
  CHECK(dependentBody->lastValidResultShape() == tailLastValid);
  CHECK(childPtr->error().find("Blocked by invalid upstream") != std::string::npos);
  CHECK(tailPtr->error().find("Blocked by invalid upstream") != std::string::npos);
  CHECK(otherPtr->isValid() && otherPtr->rebuildCount == 1);
  CHECK(rootPtr->shapeRevision() == rootLastValidRevision);
  CHECK(rootPtr->topologyIndex(&topologyError) == nullptr);
  CHECK(!topologyError.empty());
  topologyError.clear();
  CHECK(rootPtr->lastValidTopologyIndex(&topologyError) ==
        rebuiltRootTopology);
  CHECK(topologyError.empty());

  rootPtr->setFail(false);
  CHECK(document.recompute());
  CHECK(rootPtr->isValid() && childPtr->isValid() && tailPtr->isValid());
  CHECK(rootPtr->id() == rootId && childPtr->id() == childId &&
        tailPtr->id() == tailId);
  CHECK(otherPtr->rebuildCount == 1);
  CHECK(rootPtr->shapeRevision() != rootLastValidRevision);
  CHECK(rootPtr->topologyIndex() != rebuiltRootTopology);
  const int rootCount = rootPtr->rebuildCount;
  const int childCount = childPtr->rebuildCount;
  CHECK(document.recomputeFrom(tailId));
  CHECK(rootPtr->rebuildCount == rootCount && childPtr->rebuildCount == childCount);
  CHECK(tailPtr->rebuildCount == 4);

  // A missing cross-Body Feature dependency invalidates its direct consumer
  // and the active history tail without evaluating either closure or an
  // independent Dirty Body. Restoring the same ID recovers the graph.
  {
    solidar::Document missingFeatureDocument;
    auto& sourceBody = missingFeatureDocument.addBody("Feature source body");
    const auto sourceBodyId = sourceBody.id();
    auto source = std::make_unique<CountingFeature>("Feature source");
    auto* sourcePtr = source.get();
    const auto sourceId = sourcePtr->id();
    sourceBody.addFeature(std::move(source));

    auto& consumerBody =
        missingFeatureDocument.addBody("Feature consumer body");
    const auto consumerBodyId = consumerBody.id();
    auto direct = std::make_unique<CountingFeature>(
        "Missing Feature consumer", solidar::kInvalidSketchId, sourceId);
    auto tail = std::make_unique<CountingFeature>("Feature consumer tail");
    auto* directPtr = direct.get();
    auto* tailPtr = tail.get();
    const auto directId = directPtr->id();
    consumerBody.addFeature(std::move(direct));
    consumerBody.addFeature(std::move(tail));

    auto& independentBody =
        missingFeatureDocument.addBody("Independent Feature body");
    auto independent = std::make_unique<CountingFeature>("Independent Feature");
    auto* independentPtr = independent.get();
    independentBody.addFeature(std::move(independent));

    CHECK(missingFeatureDocument.recompute());
    const auto directLastValid = directPtr->shape();
    const auto tailLastValid = tailPtr->shape();
    const auto independentLastValid = independentPtr->shape();
    const auto directRevision = directPtr->shapeRevision();
    const auto tailRevision = tailPtr->shapeRevision();
    const auto independentRevision = independentPtr->shapeRevision();
    const int directCount = directPtr->rebuildCount;
    const int tailCount = tailPtr->rebuildCount;
    const int independentCount = independentPtr->rebuildCount;
    CountingFeature sourceSnapshot = *sourcePtr;
    independentPtr->setFail(false);
    CHECK(independentPtr->isDirty());
    CHECK(missingFeatureDocument.applyFeatureSlice(sourceBodyId, 0, nullptr));

    const std::string rootDiagnostic =
        "Feature #" + std::to_string(directId) +
        " references missing Feature #" + std::to_string(sourceId);
    CHECK(!missingFeatureDocument.recompute());
    CHECK(missingFeatureDocument.rebuildError() == rootDiagnostic);
    CHECK(directPtr->isFailed() && directPtr->error() == rootDiagnostic);
    CHECK(tailPtr->isFailed());
    CHECK(tailPtr->error() ==
          "Blocked by invalid upstream dependency 'Body #" +
              std::to_string(consumerBodyId) + " / Feature #" +
              std::to_string(directId) + "'");
    CHECK(!directPtr->shape() && !tailPtr->shape());
    CHECK(directPtr->lastValidShape() == directLastValid);
    CHECK(tailPtr->lastValidShape() == tailLastValid);
    CHECK(directPtr->shapeRevision() == directRevision);
    CHECK(tailPtr->shapeRevision() == tailRevision);
    CHECK(directPtr->rebuildCount == directCount);
    CHECK(tailPtr->rebuildCount == tailCount);
    CHECK(!missingFeatureDocument.findBody(consumerBodyId)->resultShape());
    CHECK(missingFeatureDocument.findBody(consumerBodyId)
              ->lastValidResultShape() == tailLastValid);
    CHECK(independentPtr->isDirty());
    CHECK(independentPtr->error().empty() && !independentPtr->shape());
    CHECK(independentPtr->lastValidShape() == independentLastValid);
    CHECK(independentPtr->shapeRevision() == independentRevision);
    CHECK(independentPtr->rebuildCount == independentCount);

    CHECK(missingFeatureDocument.applyFeatureSlice(sourceBodyId, 0,
                                                    &sourceSnapshot));
    CHECK(missingFeatureDocument.recompute());
    CHECK(missingFeatureDocument.findFeature(sourceId));
    CHECK(directPtr->isValid() && tailPtr->isValid());
    CHECK(missingFeatureDocument.findBody(consumerBodyId)->resultShape());
    CHECK(independentPtr->isValid());
  }

  // A missing declared Sketch has the same atomic abort and recovery contract.
  {
    solidar::Document missingSketchDocument;
    auto& profile = missingSketchDocument.addSketch("Missing profile");
    const auto profileId = profile.id;
    profile.geometry.addRectangle({0.0, 0.0}, {8.0, 6.0});
    auto& consumerBody = missingSketchDocument.addBody("Sketch consumer body");
    const auto consumerBodyId = consumerBody.id();
    auto direct = std::make_unique<CountingFeature>("Missing Sketch consumer",
                                                    profileId);
    auto tail = std::make_unique<CountingFeature>("Sketch consumer tail");
    auto* directPtr = direct.get();
    auto* tailPtr = tail.get();
    const auto directId = directPtr->id();
    consumerBody.addFeature(std::move(direct));
    consumerBody.addFeature(std::move(tail));
    auto& independentBody =
        missingSketchDocument.addBody("Independent Sketch body");
    auto independent = std::make_unique<CountingFeature>("Independent Sketch");
    auto* independentPtr = independent.get();
    independentBody.addFeature(std::move(independent));

    CHECK(missingSketchDocument.recompute());
    const auto directLastValid = directPtr->shape();
    const auto tailLastValid = tailPtr->shape();
    const auto independentLastValid = independentPtr->shape();
    const auto directRevision = directPtr->shapeRevision();
    const auto tailRevision = tailPtr->shapeRevision();
    const auto independentRevision = independentPtr->shapeRevision();
    const int directCount = directPtr->rebuildCount;
    const int tailCount = tailPtr->rebuildCount;
    const int independentCount = independentPtr->rebuildCount;
    const solidar::DocumentSketch profileSnapshot =
        *missingSketchDocument.findSketch(profileId);
    independentPtr->setFail(false);
    CHECK(missingSketchDocument.applySketchSlice(0, std::nullopt));

    const std::string rootDiagnostic =
        "Feature #" + std::to_string(directId) +
        " references missing Sketch #" + std::to_string(profileId);
    CHECK(!missingSketchDocument.recompute());
    CHECK(missingSketchDocument.rebuildError() == rootDiagnostic);
    CHECK(directPtr->isFailed() && directPtr->error() == rootDiagnostic);
    CHECK(tailPtr->isFailed());
    CHECK(tailPtr->error() ==
          "Blocked by invalid upstream dependency 'Body #" +
              std::to_string(consumerBodyId) + " / Feature #" +
              std::to_string(directId) + "'");
    CHECK(!directPtr->shape() && !tailPtr->shape());
    CHECK(directPtr->lastValidShape() == directLastValid);
    CHECK(tailPtr->lastValidShape() == tailLastValid);
    CHECK(directPtr->shapeRevision() == directRevision);
    CHECK(tailPtr->shapeRevision() == tailRevision);
    CHECK(directPtr->rebuildCount == directCount);
    CHECK(tailPtr->rebuildCount == tailCount);
    CHECK(!missingSketchDocument.findBody(consumerBodyId)->resultShape());
    CHECK(missingSketchDocument.findBody(consumerBodyId)
              ->lastValidResultShape() == tailLastValid);
    CHECK(independentPtr->isDirty());
    CHECK(independentPtr->error().empty() && !independentPtr->shape());
    CHECK(independentPtr->lastValidShape() == independentLastValid);
    CHECK(independentPtr->shapeRevision() == independentRevision);
    CHECK(independentPtr->rebuildCount == independentCount);

    CHECK(missingSketchDocument.applySketchSlice(0, profileSnapshot));
    CHECK(missingSketchDocument.recompute());
    CHECK(missingSketchDocument.findSketch(profileId));
    CHECK(directPtr->isValid() && tailPtr->isValid());
    CHECK(missingSketchDocument.findBody(consumerBodyId)->resultShape());
    CHECK(independentPtr->isValid());
  }

  // A face-support owner mismatch invalidates the Sketch node first. Its
  // consumers are blocked, cached geometry is retained, and the support flags
  // force reconciliation after restoring the same support reference.
  {
    solidar::Document supportOwnerDocument;
    auto& supportBody = supportOwnerDocument.addBody("Support owner body");
    const auto supportBodyId = supportBody.id();
    auto support = std::make_unique<CountingFeature>("Support owner Feature");
    auto* supportPtr = support.get();
    const auto supportId = supportPtr->id();
    supportBody.addFeature(std::move(support));
    CHECK(supportOwnerDocument.recompute());
    const auto topFace =
        solidar::test::topPlanarFace(*supportPtr->shape(), 10.0);
    CHECK(topFace);

    auto& supportedSketch =
        supportOwnerDocument.addSketch("Face-supported dependency");
    const auto supportedSketchId = supportedSketch.id;
    supportedSketch.geometry.addRectangle({1.0, 1.0}, {4.0, 4.0});
    const auto supportReference = solidar::makeFaceReference(
        *supportPtr->shape(), supportBodyId, supportId, *topFace);
    CHECK(supportOwnerDocument.attachSketchToFace(supportedSketchId,
                                                  supportReference));

    auto& consumerBody =
        supportOwnerDocument.addBody("Supported consumer body");
    const auto consumerBodyId = consumerBody.id();
    auto direct = std::make_unique<CountingFeature>(
        "Supported Sketch consumer", supportedSketchId);
    auto tail = std::make_unique<CountingFeature>("Supported consumer tail");
    auto* directPtr = direct.get();
    auto* tailPtr = tail.get();
    const auto directId = directPtr->id();
    consumerBody.addFeature(std::move(direct));
    consumerBody.addFeature(std::move(tail));
    auto& independentBody =
        supportOwnerDocument.addBody("Independent support body");
    const auto independentBodyId = independentBody.id();
    auto independent = std::make_unique<CountingFeature>("Independent support");
    auto* independentPtr = independent.get();
    independentBody.addFeature(std::move(independent));

    CHECK(supportOwnerDocument.recompute());
    const auto directLastValid = directPtr->shape();
    const auto tailLastValid = tailPtr->shape();
    const auto independentLastValid = independentPtr->shape();
    const auto directRevision = directPtr->shapeRevision();
    const auto tailRevision = tailPtr->shapeRevision();
    const auto independentRevision = independentPtr->shapeRevision();
    const int supportCount = supportPtr->rebuildCount;
    const int directCount = directPtr->rebuildCount;
    const int tailCount = tailPtr->rebuildCount;
    const int independentCount = independentPtr->rebuildCount;
    const solidar::DocumentSketch supportSnapshot =
        *supportOwnerDocument.findSketch(supportedSketchId);
    const auto placementRevision = supportSnapshot.placementRevision;
    auto invalidSupport = supportSnapshot;
    auto invalidSupportLegacy = *invalidSupport.support.face.v1();
    invalidSupportLegacy.bodyId = independentBodyId;
    invalidSupport.support.face =
        solidar::FaceReference::fromV1(std::move(invalidSupportLegacy));
    independentPtr->setFail(false);
    CHECK(supportOwnerDocument.applySketchSlice(0, invalidSupport));

    const std::string rootDiagnostic =
        "Sketch #" + std::to_string(supportedSketchId) +
        " support owner Body does not match Feature #" +
        std::to_string(supportId);
    CHECK(!supportOwnerDocument.recompute());
    CHECK(supportOwnerDocument.rebuildError() == rootDiagnostic);
    const auto* failedSupport =
        supportOwnerDocument.findSketch(supportedSketchId);
    CHECK(failedSupport);
    CHECK(!failedSupport->supportResolved && failedSupport->placementDirty);
    CHECK(failedSupport->placementRevision == placementRevision);
    CHECK(directPtr->isFailed());
    CHECK(directPtr->error() ==
          "Blocked by invalid upstream dependency 'Sketch #" +
              std::to_string(supportedSketchId) + "'");
    CHECK(tailPtr->isFailed());
    CHECK(tailPtr->error() ==
          "Blocked by invalid upstream dependency 'Body #" +
              std::to_string(consumerBodyId) + " / Feature #" +
              std::to_string(directId) + "'");
    CHECK(!directPtr->shape() && !tailPtr->shape());
    CHECK(directPtr->lastValidShape() == directLastValid);
    CHECK(tailPtr->lastValidShape() == tailLastValid);
    CHECK(directPtr->shapeRevision() == directRevision);
    CHECK(tailPtr->shapeRevision() == tailRevision);
    CHECK(supportPtr->isValid() && supportPtr->rebuildCount == supportCount);
    CHECK(directPtr->rebuildCount == directCount);
    CHECK(tailPtr->rebuildCount == tailCount);
    CHECK(!supportOwnerDocument.findBody(consumerBodyId)->resultShape());
    CHECK(supportOwnerDocument.findBody(consumerBodyId)
              ->lastValidResultShape() == tailLastValid);
    CHECK(independentPtr->isDirty());
    CHECK(independentPtr->error().empty() && !independentPtr->shape());
    CHECK(independentPtr->lastValidShape() == independentLastValid);
    CHECK(independentPtr->shapeRevision() == independentRevision);
    CHECK(independentPtr->rebuildCount == independentCount);

    CHECK(supportOwnerDocument.applySketchSlice(0, supportSnapshot));
    CHECK(supportOwnerDocument.recompute());
    const auto* restoredSupport =
        supportOwnerDocument.findSketch(supportedSketchId);
    CHECK(restoredSupport && restoredSupport->supportResolved);
    CHECK(directPtr->isValid() && tailPtr->isValid());
    CHECK(supportOwnerDocument.findBody(consumerBodyId)->resultShape());
    CHECK(independentPtr->isValid());
  }
  return EXIT_SUCCESS;
}

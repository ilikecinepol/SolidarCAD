#include "TestAssertions.h"

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
                  solidar::SketchId sketchId = solidar::kInvalidSketchId)
      : ShapeFeature(std::move(name)), sketchId_(sketchId) {}
  solidar::FeatureDependencies dependencies() const override {
    ++dependencyDeclarationCount;
    solidar::FeatureDependencies result;
    if (sketchId_ != solidar::kInvalidSketchId)
      result.sketchIds.push_back(sketchId_);
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
  return EXIT_SUCCESS;
}

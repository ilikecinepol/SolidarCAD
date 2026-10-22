#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/MirrorFeature.h"
#include "model/ShellFeature.h"
#include "model/TopologyLineage.h"
#include "model/TopologyReferenceResolver.h"

namespace {

using solidar::OperationFailureCode;

class DiagnosticFeature final : public solidar::ShapeFeature {
 public:
  DiagnosticFeature(solidar::FeatureId id, OperationFailureCode code,
                    std::string name,
                    solidar::SketchId dependsOn = solidar::kInvalidSketchId)
      : ShapeFeature(id, std::move(name)), code_(code), dependsOn_(dependsOn) {}

  void setCode(OperationFailureCode code) {
    code_ = code;
    setDirty();
  }

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<DiagnosticFeature>(*this);
  }

  [[nodiscard]] solidar::FeatureDependencies dependencies() const override {
    solidar::FeatureDependencies result;
    if (dependsOn_ != solidar::kInvalidSketchId)
      result.sketchIds.push_back(dependsOn_);
    return result;
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    ++rebuildCount;
    if (code_ != OperationFailureCode::None) {
      markError(code_, "synthetic diagnostic root");
      return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(8.0, 7.0, 6.0).Shape()));
    markValid();
    return true;
  }

 public:
  int rebuildCount{};

 private:
  OperationFailureCode code_{OperationFailureCode::None};
  solidar::SketchId dependsOn_{solidar::kInvalidSketchId};
};

class LegacyDiagnosticFeature final : public solidar::ShapeFeature {
 public:
  explicit LegacyDiagnosticFeature(solidar::FeatureId id)
      : ShapeFeature(id, "legacy diagnostic") {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<LegacyDiagnosticFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    markError("legacy string-only failure");
    return false;
  }
};

class AmbiguousTopologyFeature final : public solidar::ShapeFeature {
 public:
  explicit AmbiguousTopologyFeature(solidar::FeatureId id)
      : ShapeFeature(id, "ambiguous topology source") {}

  [[nodiscard]] const solidar::TopologyProvenanceKey& ambiguousFaceKey()
      const {
    return ambiguousFaceKey_;
  }
  [[nodiscard]] const solidar::TopologyProvenanceKey& validFaceKey() const {
    return validFaceKey_;
  }
  [[nodiscard]] const solidar::TopologyProvenanceKey& ambiguousEdgeKey()
      const {
    return ambiguousEdgeKey_;
  }
  [[nodiscard]] const solidar::TopologyProvenanceKey& validEdgeKey() const {
    return validEdgeKey_;
  }

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<AmbiguousTopologyFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0, 9.0, 8.0).Shape());
    ambiguousFaceKey_ = {id(), solidar::TopologyKind::Face,
                         solidar::TopologyProvenanceOperation::Generated,
                         solidar::TopologySemanticRole::Custom,
                         "ambiguous-face"};
    ambiguousEdgeKey_ = {id(), solidar::TopologyKind::Edge,
                         solidar::TopologyProvenanceOperation::Generated,
                         solidar::TopologySemanticRole::Custom,
                         "ambiguous-edge"};
    std::vector<solidar::TopologyLineageBinding> bindings;
    std::size_t faceIndex = 0;
    for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
         faces.Next(), ++faceIndex) {
      auto key = ambiguousFaceKey_;
      if (faceIndex >= 2) key.localName += ":" + std::to_string(faceIndex);
      if (faceIndex == 2) validFaceKey_ = key;
      bindings.push_back(
          {std::move(key), std::make_shared<TopoDS_Shape>(faces.Current())});
    }
    CHECK(bindings.size() >= 2);
    std::size_t edgeIndex = 0;
    for (TopExp_Explorer edges(*shape, TopAbs_EDGE); edges.More();
         edges.Next(), ++edgeIndex) {
      auto key = ambiguousEdgeKey_;
      if (edgeIndex >= 2) key.localName += ":" + std::to_string(edgeIndex);
      if (edgeIndex == 2) validEdgeKey_ = key;
      bindings.push_back(
          {std::move(key), std::make_shared<TopoDS_Shape>(edges.Current())});
    }
    CHECK(edgeIndex >= 3);
    std::string error;
    auto lineage = solidar::TopologyLineage::create(
        std::move(bindings),
        solidar::TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage);
    CHECK(error.empty());
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  solidar::TopologyProvenanceKey ambiguousFaceKey_;
  solidar::TopologyProvenanceKey validFaceKey_;
  solidar::TopologyProvenanceKey ambiguousEdgeKey_;
  solidar::TopologyProvenanceKey validEdgeKey_;
};

struct SourceState {
  solidar::Body* body{};
  solidar::FeatureId featureId{solidar::kInvalidFeatureId};
  solidar::FaceReference face;
  solidar::EdgeReference edge;
};

struct AmbiguousSourceState {
  solidar::Body* body{};
  solidar::FeatureId featureId{solidar::kInvalidFeatureId};
  solidar::FaceReference ambiguousFace;
  solidar::FaceReference validFace;
  solidar::EdgeReference ambiguousEdge;
  solidar::EdgeReference validEdge;
};

SourceState addSimpleSource(solidar::Document& document) {
  auto& body = document.addBody("diagnostic body");
  auto source = std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 18.0, 16.0).Shape()),
      "diagnostic source");
  const auto sourceId = source->id();
  body.addFeature(std::move(source));
  CHECK(document.recompute());
  const auto* sourceFeature = document.findFeature(body.id(), sourceId);
  CHECK(sourceFeature);
  const auto topology = sourceFeature->topologyIndex();
  CHECK(topology);
  const auto face =
      topology->createFaceReference(body.id(), sourceId, 0);
  const auto edge =
      topology->createEdgeReference(body.id(), sourceId, 0);
  CHECK(face);
  CHECK(edge);
  return {&body, sourceId, face.reference, edge.reference};
}

AmbiguousSourceState addAmbiguousSource(solidar::Document& document,
                                        solidar::BodyId bodyId,
                                        solidar::FeatureId featureId) {
  auto& body = document.addBody(bodyId, "ambiguous diagnostic body");
  auto source = std::make_unique<AmbiguousTopologyFeature>(featureId);
  auto* sourcePtr = source.get();
  body.addFeature(std::move(source));
  CHECK(document.recompute());

  AmbiguousSourceState result;
  result.body = &body;
  result.featureId = featureId;
  result.ambiguousFace = solidar::FaceReference::fromV2(
      {body.id(), sourcePtr->ambiguousFaceKey()});
  result.validFace = solidar::FaceReference::fromV2(
      {body.id(), sourcePtr->validFaceKey()});
  result.ambiguousEdge = solidar::EdgeReference::fromV2(
      {body.id(), sourcePtr->ambiguousEdgeKey()});
  result.validEdge = solidar::EdgeReference::fromV2(
      {body.id(), sourcePtr->validEdgeKey()});

  const auto topology = sourcePtr->topologyIndex();
  CHECK(topology);
  CHECK(topology->resolveFace(result.ambiguousFace).failure ==
        solidar::TopologyResolutionFailure::Ambiguous);
  CHECK(topology->resolveFace(result.validFace));
  CHECK(topology->resolveEdge(result.ambiguousEdge).failure ==
        solidar::TopologyResolutionFailure::Ambiguous);
  CHECK(topology->resolveEdge(result.validEdge));
  return result;
}

solidar::FaceReference missingFace(const SourceState& source) {
  return {source.body->id(), source.featureId, 0,
          "runtime-diagnostic:missing-face"};
}

solidar::EdgeReference missingEdge(const SourceState& source) {
  return {source.body->id(), source.featureId, 0,
          "runtime-diagnostic:missing-edge"};
}

void checkSingleFeatureFailure(solidar::Document& document,
                               solidar::BodyId bodyId,
                               solidar::FeatureId featureId,
                               OperationFailureCode code) {
  const auto result = document.recomputeDetailed();
  CHECK(!result.success);
  CHECK(result.primaryFailures.size() == 1);
  CHECK(result.primaryFailures[0].site ==
        solidar::RecomputeFailureSite::Feature);
  CHECK(result.primaryFailures[0].bodyId == bodyId);
  CHECK(result.primaryFailures[0].featureId == featureId);
  CHECK(result.primaryFailures[0].failure.code == code);
  const auto* feature = document.findFeature(bodyId, featureId);
  CHECK(feature);
  CHECK(feature->errorCode() == code);
}

}  // namespace

int main() {
  // The scheduler publishes only primary roots, while every blocked
  // descendant retains the root's stable code. Recovery clears both states.
  {
    solidar::Document document;
    auto& body = document.addBody(810001, "root propagation");
    auto root = std::make_unique<DiagnosticFeature>(
        810002, OperationFailureCode::TopologyReferenceMissing, "root");
    auto* rootPtr = root.get();
    auto child = std::make_unique<DiagnosticFeature>(
        810003, OperationFailureCode::None, "child");
    auto* childPtr = child.get();
    body.addFeature(std::move(root));
    body.addFeature(std::move(child));

    const auto failed = document.recomputeDetailed();
    CHECK(!failed.success);
    CHECK(failed.primaryFailures.size() == 1);
    CHECK(failed.primaryFailures[0].featureId == rootPtr->id());
    CHECK(failed.primaryFailures[0].failure.code ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(childPtr->errorCode() ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(childPtr->rebuildCount == 0);

    rootPtr->setCode(OperationFailureCode::None);
    const auto recovered = document.recomputeDetailed();
    CHECK(recovered.success);
    CHECK(recovered.primaryFailures.empty());
    CHECK(rootPtr->errorCode() == OperationFailureCode::None);
    CHECK(childPtr->errorCode() == OperationFailureCode::None);
    CHECK(childPtr->rebuildCount == 1);

    rootPtr->setCode(OperationFailureCode::GeometryOperationFailed);
    const auto hard = document.recomputeDetailed();
    CHECK(!hard.success);
    CHECK(hard.primaryFailures.size() == 1);
    CHECK(hard.primaryFailures[0].failure.code ==
          OperationFailureCode::GeometryOperationFailed);
    CHECK(childPtr->errorCode() ==
          OperationFailureCode::GeometryOperationFailed);
  }

  // Legacy string-only diagnostics are still hard failures. They never
  // acquire a topology-tolerable code through the compatibility overload.
  {
    solidar::Document document;
    auto& body = document.addBody(815001, "legacy diagnostic body");
    body.addFeature(std::make_unique<LegacyDiagnosticFeature>(815002));
    const auto result = document.recomputeDetailed();
    CHECK(!result.success);
    CHECK(result.primaryFailures.size() == 1);
    CHECK(result.primaryFailures[0].failure.code ==
          OperationFailureCode::Unknown);
    CHECK(result.primaryFailures[0].failure.code !=
          OperationFailureCode::None);
    CHECK(result.primaryFailures[0].failure.code !=
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(result.primaryFailures[0].failure.code !=
          OperationFailureCode::TopologyReferenceAmbiguous);
  }

  // Independent topology and hard-geometry roots are emitted once and in
  // deterministic node order; a topology failure never masks a hard root.
  {
    solidar::Document document;
    auto& laterBody = document.addBody(820001, "later body");
    laterBody.addFeature(std::make_unique<DiagnosticFeature>(
        820020, OperationFailureCode::TopologyReferenceAmbiguous, "later"));
    auto& earlierBody = document.addBody(820002, "earlier body");
    earlierBody.addFeature(std::make_unique<DiagnosticFeature>(
        820010, OperationFailureCode::TopologyReferenceMissing, "earlier"));
    auto& middleBody = document.addBody(820003, "middle body");
    middleBody.addFeature(std::make_unique<DiagnosticFeature>(
        820015, OperationFailureCode::GeometryOperationFailed, "middle"));
    const auto result = document.recomputeDetailed();
    CHECK(!result.success);
    CHECK(result.primaryFailures.size() == 3);
    CHECK(result.primaryFailures[0].featureId == 820010);
    CHECK(result.primaryFailures[0].failure.code ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(result.primaryFailures[1].featureId == 820015);
    CHECK(result.primaryFailures[1].failure.code ==
          OperationFailureCode::GeometryOperationFailed);
    CHECK(result.primaryFailures[2].featureId == 820020);
    CHECK(result.primaryFailures[2].failure.code ==
          OperationFailureCode::TopologyReferenceAmbiguous);
  }

  // Sketch support is the sole primary root for its dependent Feature. The
  // dependent inherits the code without rebuilding; repairing the authored
  // support clears the aggregate and both runtime failures.
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    auto& sketch = document.addSketch("missing support");
    const auto sketchId = sketch.id;
    sketch.support = {solidar::SketchSupportType::Face, missingFace(source)};
    auto dependent = std::make_unique<DiagnosticFeature>(
        825001, OperationFailureCode::None, "support dependent", sketchId);
    auto* dependentPtr = dependent.get();
    source.body->addFeature(std::move(dependent));
    const auto result = document.recomputeDetailed();
    CHECK(!result.success);
    CHECK(result.primaryFailures.size() == 1);
    CHECK(result.primaryFailures[0].site ==
          solidar::RecomputeFailureSite::SketchSupport);
    CHECK(result.primaryFailures[0].sketchId == sketchId);
    CHECK(result.primaryFailures[0].failure.code ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(sketch.supportFailure.code ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(dependentPtr->errorCode() ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(dependentPtr->rebuildCount == 0);
    CHECK(document.rebuildError() == sketch.supportFailure.detail);

    const auto authoredValidSupport = source.face;
    sketch.support.face = authoredValidSupport;
    sketch.placementDirty = true;
    const auto recovered = document.recomputeDetailed();
    CHECK(recovered.success);
    CHECK(recovered.primaryFailures.empty());
    CHECK(sketch.supportResolved);
    CHECK(sketch.supportFailure.code == OperationFailureCode::None);
    CHECK(sketch.supportFailure.detail.empty());
    CHECK(sketch.support.face == authoredValidSupport);
    CHECK(dependentPtr->errorCode() == OperationFailureCode::None);
    CHECK(dependentPtr->rebuildCount == 1);
  }

  // A root Feature can transit its exact cause through a supported Sketch to
  // a downstream Feature without creating extra primary failures.
  {
    solidar::Document document;
    auto& rootBody = document.addBody(826001, "feature root body");
    auto root = std::make_unique<DiagnosticFeature>(
        826002, OperationFailureCode::TopologyReferenceMissing,
        "feature root");
    auto* rootPtr = root.get();
    rootBody.addFeature(std::move(root));
    auto& sketch = document.addSketch("transitive supported sketch");
    sketch.support = {
        solidar::SketchSupportType::Face,
        solidar::FaceReference(rootBody.id(), rootPtr->id(), 0)};
    auto& childBody = document.addBody(826003, "transitive child body");
    auto child = std::make_unique<DiagnosticFeature>(
        826004, OperationFailureCode::None, "transitive child", sketch.id);
    auto* childPtr = child.get();
    childBody.addFeature(std::move(child));

    const auto result = document.recomputeDetailed();
    CHECK(!result.success);
    CHECK(result.primaryFailures.size() == 1);
    CHECK(result.primaryFailures[0].site ==
          solidar::RecomputeFailureSite::Feature);
    CHECK(result.primaryFailures[0].featureId == rootPtr->id());
    CHECK(result.primaryFailures[0].failure.code ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(sketch.supportFailure.code ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(childPtr->errorCode() ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(childPtr->rebuildCount == 0);
  }

  // Every model-side topology consumer preserves resolver failure codes.
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    auto feature = std::make_unique<solidar::FilletFeature>(
        missingEdge(source), 1.0, "missing fillet edge");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(document, source.body->id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    auto feature = std::make_unique<solidar::ChamferFeature>(
        missingEdge(source), 1.0, "missing chamfer edge");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(document, source.body->id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    auto feature = std::make_unique<solidar::MirrorFeature>(
        source.featureId, missingFace(source), "missing mirror face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(document, source.body->id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    auto feature = std::make_unique<solidar::ShellFeature>(
        source.featureId, std::vector{missingFace(source)}, 1.0, false,
        "missing shell face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(document, source.body->id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    const solidar::PlaneReference plane{
        solidar::NeutralPlaneType::GlobalXY};
    const solidar::AxisReference direction{
        solidar::AxisReferenceType::GlobalZ};
    auto feature = std::make_unique<solidar::DraftFeature>(
        source.featureId, std::vector{missingFace(source)}, plane, direction,
        5.0, false, "missing drafted face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(document, source.body->id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    const solidar::PlaneReference plane{
        solidar::NeutralPlaneType::BodyFace, missingFace(source)};
    const solidar::AxisReference direction{
        solidar::AxisReferenceType::GlobalZ};
    auto feature = std::make_unique<solidar::DraftFeature>(
        source.featureId, std::vector{source.face}, plane, direction, 5.0,
        false, "missing neutral face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(document, source.body->id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    const solidar::PlaneReference plane{
        solidar::NeutralPlaneType::GlobalXY};
    const solidar::AxisReference direction{
        solidar::AxisReferenceType::GlobalZ};
    auto feature = std::make_unique<solidar::DraftFeature>(
        source.featureId, std::vector{source.face}, plane, direction, 5.0,
        false, "missing rotation edge", missingEdge(source));
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(document, source.body->id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }

  // Duplicate provenance gives each remaining site a deterministic
  // Ambiguous oracle. Valid companion references ensure Draft reaches the
  // intended slot instead of failing earlier.
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840001, 840002);
    auto& sketch = document.addSketch("ambiguous support");
    sketch.support = {solidar::SketchSupportType::Face,
                      source.ambiguousFace};
    const auto result = document.recomputeDetailed();
    CHECK(!result.success);
    CHECK(result.primaryFailures.size() == 1);
    CHECK(result.primaryFailures[0].site ==
          solidar::RecomputeFailureSite::SketchSupport);
    CHECK(result.primaryFailures[0].failure.code ==
          OperationFailureCode::TopologyReferenceAmbiguous);
    CHECK(sketch.supportFailure.code ==
          OperationFailureCode::TopologyReferenceAmbiguous);
  }
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840011, 840012);
    auto feature = std::make_unique<solidar::FilletFeature>(
        source.ambiguousEdge, 1.0, "ambiguous fillet edge");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(
        document, source.body->id(), id,
        OperationFailureCode::TopologyReferenceAmbiguous);
  }
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840021, 840022);
    auto feature = std::make_unique<solidar::ChamferFeature>(
        source.ambiguousEdge, 1.0, "ambiguous chamfer edge");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(
        document, source.body->id(), id,
        OperationFailureCode::TopologyReferenceAmbiguous);
  }
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840031, 840032);
    auto feature = std::make_unique<solidar::MirrorFeature>(
        source.featureId, source.ambiguousFace, "ambiguous mirror face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(
        document, source.body->id(), id,
        OperationFailureCode::TopologyReferenceAmbiguous);
  }
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840041, 840042);
    auto feature = std::make_unique<solidar::ShellFeature>(
        source.featureId, std::vector{source.ambiguousFace}, 1.0, false,
        "ambiguous shell face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(
        document, source.body->id(), id,
        OperationFailureCode::TopologyReferenceAmbiguous);
  }
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840051, 840052);
    const solidar::PlaneReference plane{
        solidar::NeutralPlaneType::GlobalXY};
    const solidar::AxisReference direction{
        solidar::AxisReferenceType::GlobalZ};
    auto feature = std::make_unique<solidar::DraftFeature>(
        source.featureId, std::vector{source.ambiguousFace}, plane, direction,
        5.0, false, "ambiguous drafted face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(
        document, source.body->id(), id,
        OperationFailureCode::TopologyReferenceAmbiguous);
  }
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840061, 840062);
    const solidar::PlaneReference plane{
        solidar::NeutralPlaneType::BodyFace, source.ambiguousFace};
    const solidar::AxisReference direction{
        solidar::AxisReferenceType::GlobalZ};
    auto feature = std::make_unique<solidar::DraftFeature>(
        source.featureId, std::vector{source.validFace}, plane, direction, 5.0,
        false, "ambiguous neutral face");
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(
        document, source.body->id(), id,
        OperationFailureCode::TopologyReferenceAmbiguous);
  }
  {
    solidar::Document document;
    const auto source = addAmbiguousSource(document, 840071, 840072);
    const solidar::PlaneReference plane{
        solidar::NeutralPlaneType::GlobalXY};
    const solidar::AxisReference direction{
        solidar::AxisReferenceType::GlobalZ};
    auto feature = std::make_unique<solidar::DraftFeature>(
        source.featureId, std::vector{source.validFace}, plane, direction, 5.0,
        false, "ambiguous rotation edge", source.ambiguousEdge);
    const auto id = feature->id();
    source.body->addFeature(std::move(feature));
    checkSingleFeatureFailure(
        document, source.body->id(), id,
        OperationFailureCode::TopologyReferenceAmbiguous);
  }

  // A real consumer rollback retains the exact last valid geometry snapshot,
  // revision and topology cache. Recompute does not rewrite the newly authored
  // broken carrier while reporting its typed failure.
  {
    solidar::Document document;
    const auto source = addSimpleSource(document);
    auto feature = std::make_unique<solidar::FilletFeature>(
        source.edge, 1.0, "rollback fillet");
    auto* featurePtr = feature.get();
    source.body->addFeature(std::move(feature));
    CHECK(document.recompute());
    const auto lastShape = featurePtr->lastValidShape();
    const auto lastRevision = featurePtr->shapeRevision();
    const auto lastTopology = featurePtr->lastValidTopologyIndex();
    CHECK(lastShape);
    CHECK(lastTopology);

    const auto broken = missingEdge(source);
    featurePtr->setEdge(broken);
    const auto result = document.recomputeDetailed();
    CHECK(!result.success);
    CHECK(result.primaryFailures.size() == 1);
    CHECK(result.primaryFailures[0].failure.code ==
          OperationFailureCode::TopologyReferenceMissing);
    CHECK(featurePtr->isFailed());
    CHECK(!featurePtr->shape());
    CHECK(featurePtr->lastValidShape().get() == lastShape.get());
    CHECK(featurePtr->shapeRevision() == lastRevision);
    CHECK(featurePtr->lastValidTopologyIndex().get() == lastTopology.get());
    CHECK(featurePtr->edge() == broken);
  }

  // Strict v2 face extrusion classifies authored provenance before boundary
  // capture, preserving both Missing and Ambiguous exactly.
  {
    solidar::Document document;
    auto& profile = document.addSketch("strict source profile");
    profile.geometry.addRectangle({0.0, 0.0}, {12.0, 10.0});
    auto& body = document.addBody("strict missing body");
    auto source = std::make_unique<solidar::ExtrudeFeature>(
        profile.id, 8.0, "strict source");
    const auto sourceId = source->id();
    body.addFeature(std::move(source));
    CHECK(document.recompute());
    const auto created = document.createFaceReferenceV2(body.id(), sourceId, 0);
    CHECK(created);
    auto missing = *created.reference.v2();
    missing.provenance.localName += ":missing";
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        solidar::FaceReference::fromV2(std::move(missing)), 2.0,
        "strict missing", solidar::ExtrudeOperation::Join, false);
    const auto id = feature->id();
    body.addFeature(std::move(feature));
    checkSingleFeatureFailure(document, body.id(), id,
                              OperationFailureCode::TopologyReferenceMissing);
  }
  {
    solidar::Document document;
    auto& body = document.addBody(830001, "strict ambiguous body");
    auto source = std::make_unique<AmbiguousTopologyFeature>(830002);
    auto* sourcePtr = source.get();
    body.addFeature(std::move(source));
    CHECK(document.recompute());
    const auto ambiguous = solidar::FaceReference::fromV2(
        {body.id(), sourcePtr->ambiguousFaceKey()});
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        830003, ambiguous, 2.0, "strict ambiguous",
        solidar::ExtrudeOperation::Join, false);
    body.addFeature(std::move(feature));
    checkSingleFeatureFailure(document, body.id(), 830003,
                              OperationFailureCode::TopologyReferenceAmbiguous);
  }

  return 0;
}

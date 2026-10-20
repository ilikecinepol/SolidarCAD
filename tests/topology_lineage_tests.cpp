#include "TestAssertions.h"

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/ShapeFeature.h"
#include "model/TopologyLineage.h"
#include "model/TopologyLineageBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace {

constexpr auto kPartialFaceCoverage =
    solidar::TopologyLineageCoverage::PartialFaces;

solidar::TopologyProvenanceKey makeKey(
    solidar::FeatureId featureId, solidar::TopologyKind kind,
    std::string localName) {
  return {featureId,
          kind,
          solidar::TopologyProvenanceOperation::Generated,
          kind == solidar::TopologyKind::Face
              ? solidar::TopologySemanticRole::Side
              : solidar::TopologySemanticRole::Custom,
          std::move(localName),
          {{11, kind, solidar::TopologyProvenanceOperation::Generated,
            solidar::TopologySemanticRole::Side, "profile:a"},
           {12, kind, solidar::TopologyProvenanceOperation::Modified,
            solidar::TopologySemanticRole::Side, "profile:b"}}};
}

std::vector<std::shared_ptr<const TopoDS_Shape>> subshapes(
    const TopoDS_Shape& shape, TopAbs_ShapeEnum kind, std::size_t count) {
  std::vector<std::shared_ptr<const TopoDS_Shape>> result;
  for (TopExp_Explorer explorer(shape, kind);
       explorer.More() && result.size() < count; explorer.Next())
    result.push_back(
        std::make_shared<TopoDS_Shape>(explorer.Current()));
  return result;
}

std::shared_ptr<TopoDS_Shape> faceCompound(
    const std::shared_ptr<const TopoDS_Shape>& first,
    const std::shared_ptr<const TopoDS_Shape>& second) {
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  builder.Add(compound, *first);
  builder.Add(compound, *second);
  return std::make_shared<TopoDS_Shape>(compound);
}

std::shared_ptr<const solidar::TopologyLineage> lineageFor(
    const TopoDS_Shape& shape, const solidar::TopologyProvenanceKey& key,
    std::string* error = nullptr) {
  const auto faces = subshapes(shape, TopAbs_FACE, 1);
  if (faces.empty()) return {};
  return solidar::TopologyLineage::create(
      {{key, faces.front()}}, kPartialFaceCoverage, error);
}

enum class SyntheticMode {
  ValidFirst,
  ValidNext,
  LegacyNoLineage,
  FailAfterStaging,
  ThrowAfterStaging,
  ForeignBinding,
  MalformedLineage,
  ProducerMismatch,
  EmptyLineage,
  DuplicateLocalName,
};

class SyntheticLineageFeature final : public solidar::ShapeFeature {
 public:
  explicit SyntheticLineageFeature(solidar::BodyId bodyId)
      : ShapeFeature("Synthetic lineage"), bodyId_(bodyId) {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<SyntheticLineageFeature>(*this);
  }

  [[nodiscard]] solidar::TopologyReferenceV2 reference(
      std::string localName) const {
    return {bodyId_, makeKey(id(), solidar::TopologyKind::Face,
                             std::move(localName))};
  }

  SyntheticMode mode{SyntheticMode::ValidFirst};

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    const bool next = mode != SyntheticMode::ValidFirst;
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(next ? 20.0 : 10.0, 10.0, 10.0).Shape());
    if (mode == SyntheticMode::LegacyNoLineage) {
      setShape(std::move(shape));
      markValid();
      return true;
    }
    auto key = makeKey(id(), solidar::TopologyKind::Face,
                       next ? "side:next" : "side:first");
    if (mode == SyntheticMode::MalformedLineage)
      key.operation = static_cast<solidar::TopologyProvenanceOperation>(255);
    if (mode == SyntheticMode::ProducerMismatch)
      key.featureId = id() + 1000;
    auto lineage = lineageFor(*shape, key);
    if (mode == SyntheticMode::EmptyLineage) {
      lineage = solidar::TopologyLineage::create(
          {}, solidar::TopologyLineageCoverage::None);
    }
    if (mode == SyntheticMode::DuplicateLocalName) {
      auto conflicting = key;
      conflicting.origins.push_back(
          {991, solidar::TopologyKind::Face,
           solidar::TopologyProvenanceOperation::Modified,
           solidar::TopologySemanticRole::Side, "conflicting:ancestry"});
      conflicting.canonicalizeOrigins();
      const auto faces = subshapes(*shape, TopAbs_FACE, 2);
      lineage = solidar::TopologyLineage::create(
          {{key, faces[0]}, {conflicting, faces[1]}},
          kPartialFaceCoverage);
    }
    if (mode == SyntheticMode::ForeignBinding) {
      const TopoDS_Shape foreign =
          BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0), 2.0, 2.0, 2.0)
              .Shape();
      const auto faces = subshapes(foreign, TopAbs_FACE, 1);
      lineage = solidar::TopologyLineage::create(
          {{key, faces.front()}}, kPartialFaceCoverage);
    }

    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    if (mode == SyntheticMode::ThrowAfterStaging)
      throw std::runtime_error("synthetic staged failure");
    if (mode == SyntheticMode::FailAfterStaging) {
      markError("synthetic staged failure");
      return false;
    }
    markValid();
    return true;
  }

 private:
  solidar::BodyId bodyId_;
};

class SyntheticEdgeLineageFeature final : public solidar::ShapeFeature {
 public:
  explicit SyntheticEdgeLineageFeature(bool publishLineage = true)
      : ShapeFeature("Synthetic edge lineage"),
        publishLineage_(publishLineage) {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<SyntheticEdgeLineageFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape());
    if (!publishLineage_) {
      setShape(std::move(shape));
      markValid();
      return true;
    }
    const auto faces = subshapes(*shape, TopAbs_FACE, 1);
    const auto edges = subshapes(*shape, TopAbs_EDGE, 1);
    if (faces.empty() || edges.empty()) {
      markError("synthetic shape lacks topology");
      return false;
    }
    auto lineage = solidar::TopologyLineage::create(
        {{makeKey(id(), solidar::TopologyKind::Face, "edge-owner:face"),
          faces.front()},
         {makeKey(id(), solidar::TopologyKind::Edge, "edge-owner:edge"),
          edges.front()}},
        kPartialFaceCoverage);
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  bool publishLineage_{};
};

}  // namespace

int main() {
  constexpr solidar::BodyId bodyId = 101;
  constexpr solidar::FeatureId featureId = 202;

  // Set-like origins are deterministically sorted and exactly deduplicated by
  // construction. Authored occurrence identity remains untouched.
  {
    const std::vector<solidar::TopologyProvenanceOrigin> forward{
        {11, solidar::TopologyKind::Face,
         solidar::TopologyProvenanceOperation::Generated,
         solidar::TopologySemanticRole::Side, "profile:a"},
        {12, solidar::TopologyKind::Face,
         solidar::TopologyProvenanceOperation::Modified,
         solidar::TopologySemanticRole::Cap, "profile:b"},
        {12, solidar::TopologyKind::Face,
         solidar::TopologyProvenanceOperation::Modified,
         solidar::TopologySemanticRole::Cap, "profile:b"}};
    const std::vector<solidar::TopologyProvenanceOrigin> reversed{
        forward[2], forward[1], forward[0]};
    const solidar::TopologyOccurrenceKey occurrence{featureId, 7};
    const solidar::TopologyProvenanceKey forwardKey{
        featureId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::PatternInstance, "side:0", forward,
        occurrence};
    const solidar::TopologyProvenanceKey reversedKey{
        featureId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::PatternInstance, "side:0", reversed,
        occurrence};
    std::string error;
    CHECK(forwardKey.isCanonical(&error));
    CHECK(error.empty());
    CHECK(forwardKey == reversedKey);
    CHECK(forwardKey.origins.size() == 2);
    CHECK(forwardKey.occurrence == occurrence);

    auto rawPermutation = forwardKey;
    std::swap(rawPermutation.origins[0], rawPermutation.origins[1]);
    const auto canonicalizedLineage = solidar::TopologyLineage::create(
        {{rawPermutation, std::make_shared<TopoDS_Shape>(
                               BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape())}},
        kPartialFaceCoverage, &error);
    CHECK(canonicalizedLineage);
    CHECK(canonicalizedLineage->bindings().front().provenance == forwardKey);
  }

  // Immediate origins retain every non-recursive identity-bearing field.
  // Permutation and exact duplicates canonicalize, while operation, role and
  // authored occurrence differences remain distinct.
  {
    const solidar::TopologyOccurrenceKey occurrenceA{44, 3};
    const solidar::TopologyOccurrenceKey occurrenceB{44, 4};
    const solidar::TopologyProvenanceOrigin generated{
        44, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::Side, "source:face", occurrenceA};
    auto modified = generated;
    modified.operation = solidar::TopologyProvenanceOperation::Modified;
    auto cap = generated;
    cap.role = solidar::TopologySemanticRole::Cap;
    auto otherOccurrence = generated;
    otherOccurrence.occurrence = occurrenceB;
    CHECK(generated != modified);
    CHECK(generated != cap);
    CHECK(generated != otherOccurrence);

    const solidar::TopologyProvenanceKey key{
        featureId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Merge,
        solidar::TopologySemanticRole::BooleanResult, "result:face",
        {otherOccurrence, generated, cap, modified, generated}};
    const solidar::TopologyProvenanceKey permuted{
        featureId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Merge,
        solidar::TopologySemanticRole::BooleanResult, "result:face",
        {modified, generated, otherOccurrence, cap}};
    CHECK(key == permuted);
    CHECK(key.origins.size() == 4);
    CHECK(key.origins[0] != key.origins[1]);

    const auto projected = key.immediateOrigin();
    CHECK(projected.featureId == key.featureId);
    CHECK(projected.kind == key.kind);
    CHECK(projected.operation == key.operation);
    CHECK(projected.role == key.role);
    CHECK(projected.localName == key.localName);
    CHECK(projected.occurrence == key.occurrence);
  }

  // Occurrences belong to the identity that carries them. Distinct authored
  // occurrences may reuse a readable local name, while equal immediate
  // projections with conflicting ancestry are rejected.
  {
    std::string error;
    auto occurrenceKeyA = makeKey(
        featureId, solidar::TopologyKind::Face, "pattern:side");
    occurrenceKeyA.occurrence =
        solidar::TopologyOccurrenceKey{featureId, 1};
    auto occurrenceKeyB = occurrenceKeyA;
    occurrenceKeyB.occurrence =
        solidar::TopologyOccurrenceKey{featureId, 2};
    const auto occurrenceLineage = solidar::TopologyLineage::create(
        {{occurrenceKeyA, std::make_shared<TopoDS_Shape>(
                              BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape())},
         {occurrenceKeyB, std::make_shared<TopoDS_Shape>(
                              BRepPrimAPI_MakeBox(2.0, 1.0, 1.0).Shape())}},
        kPartialFaceCoverage, &error);
    CHECK(occurrenceLineage);
    CHECK(occurrenceLineage->validateForProducer(featureId, &error));

    auto ancestryA = makeKey(featureId, solidar::TopologyKind::Face,
                             "ancestry:collision");
    auto ancestryB = ancestryA;
    ancestryB.origins.push_back(
        {99, solidar::TopologyKind::Face,
         solidar::TopologyProvenanceOperation::Modified,
         solidar::TopologySemanticRole::Side, "extra:ancestor"});
    ancestryB.canonicalizeOrigins();
    const auto ancestryCollision = solidar::TopologyLineage::create(
        {{ancestryA, std::make_shared<TopoDS_Shape>(
                         BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape())},
         {ancestryB, std::make_shared<TopoDS_Shape>(
                         BRepPrimAPI_MakeBox(2.0, 1.0, 1.0).Shape())}},
        kPartialFaceCoverage, &error);
    CHECK(ancestryCollision);
    CHECK(!ancestryCollision->validateForProducer(featureId, &error));

    auto wrongOwnerKey = occurrenceKeyA;
    wrongOwnerKey.occurrence =
        solidar::TopologyOccurrenceKey{featureId + 1, 1};
    CHECK(!solidar::TopologyLineage::create(
        {{wrongOwnerKey, std::make_shared<TopoDS_Shape>(
                              BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape())}},
        kPartialFaceCoverage, &error));
    auto wrongOwnerOrigin = occurrenceKeyA;
    wrongOwnerOrigin.origins.front().occurrence =
        solidar::TopologyOccurrenceKey{
            wrongOwnerOrigin.origins.front().featureId + 1, 1};
    CHECK(!solidar::TopologyLineage::create(
        {{wrongOwnerOrigin, std::make_shared<TopoDS_Shape>(
                                 BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape())}},
        kPartialFaceCoverage, &error));
  }

  const auto shape = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape());
  const auto faces = subshapes(*shape, TopAbs_FACE, 2);
  const auto edges = subshapes(*shape, TopAbs_EDGE, 1);
  CHECK(faces.size() == 2);
  CHECK(edges.size() == 1);

  // Coverage is immutable carrier metadata. CompleteFaces is validated by
  // TopologyIndex against the output, but no production producer claims it.
  {
    std::string error;
    const auto none = solidar::TopologyLineage::create(
        {}, solidar::TopologyLineageCoverage::None, &error);
    CHECK(none);
    CHECK(none->empty());
    CHECK(none->coverage() == solidar::TopologyLineageCoverage::None);
    CHECK(!solidar::TopologyLineage::create(
        {}, kPartialFaceCoverage, &error));
    CHECK(!solidar::TopologyLineage::create(
        {{makeKey(featureId, solidar::TopologyKind::Face, "coverage:none"),
          faces[0]}},
        solidar::TopologyLineageCoverage::None, &error));
    CHECK(!solidar::TopologyLineage::create(
        {}, static_cast<solidar::TopologyLineageCoverage>(255), &error));

    const auto incomplete = solidar::TopologyLineage::create(
        {{makeKey(featureId, solidar::TopologyKind::Face,
                  "coverage:partial"),
          faces[0]}},
        solidar::TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(incomplete);
    CHECK(!solidar::TopologyIndex::build(shape, 690, incomplete, &error));
    CHECK(error.find("every output face") != std::string::npos);

    const auto allFaces = subshapes(*shape, TopAbs_FACE, 100);
    std::vector<solidar::TopologyLineageBinding> completeBindings;
    for (std::size_t index = 0; index < allFaces.size(); ++index) {
      completeBindings.push_back(
          {makeKey(featureId, solidar::TopologyKind::Face,
                   "coverage:face:" + std::to_string(index)),
           allFaces[index]});
    }
    const auto complete = solidar::TopologyLineage::create(
        std::move(completeBindings),
        solidar::TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(complete);
    CHECK(complete->coverage() ==
          solidar::TopologyLineageCoverage::CompleteFaces);
    CHECK(solidar::TopologyIndex::build(shape, 691, complete, &error));
    CHECK(error.empty());
  }

  // The neutral history seam owns source/output shapes and copied facts after
  // producer-local objects leave scope. Validation rejects missing source
  // evidence and preserves the exact immediate-origin projection.
  {
    constexpr solidar::FeatureId sourceFeatureId = 880;
    constexpr solidar::FeatureId producerFeatureId = 881;
    std::optional<solidar::TopologyLineageBuilder> retainedBuilder;
    std::weak_ptr<const TopoDS_Shape> sourceLifetime;
    std::weak_ptr<const TopoDS_Shape> outputLifetime;
    solidar::TopologyProvenanceKey outputKey;
    {
      const auto sourceShape = std::make_shared<TopoDS_Shape>(
          BRepPrimAPI_MakeBox(3.0, 4.0, 5.0).Shape());
      const auto outputShape = std::make_shared<TopoDS_Shape>(
          BRepPrimAPI_MakeBox(6.0, 4.0, 5.0).Shape());
      const auto sourceFace = subshapes(*sourceShape, TopAbs_FACE, 1).front();
      const auto outputFace = subshapes(*outputShape, TopAbs_FACE, 1).front();
      const auto sourceKey = makeKey(
          sourceFeatureId, solidar::TopologyKind::Face, "source:owned");
      const auto sourceLineage = solidar::TopologyLineage::create(
          {{sourceKey, sourceFace}}, kPartialFaceCoverage);
      CHECK(sourceLineage);
      const auto sourceOrigin = sourceKey.immediateOrigin();
      outputKey = {producerFeatureId, solidar::TopologyKind::Face,
                   solidar::TopologyProvenanceOperation::Generated,
                   solidar::TopologySemanticRole::Side, "output:owned",
                   {sourceOrigin}};
      solidar::ShapeHistoryArtifact artifact;
      artifact.producerFeatureId = producerFeatureId;
      artifact.outputShape = outputShape;
      artifact.coverage = kPartialFaceCoverage;
      artifact.sources.push_back(
          {sourceFeatureId, sourceShape, sourceLineage});
      artifact.facts.push_back(
          {solidar::ShapeHistoryFactKind::Generated, std::nullopt,
           sourceOrigin, sourceFace, outputKey, outputFace});

      std::string error;
      CHECK(solidar::validateTopologySourceSnapshot(artifact.sources[0],
                                                     &error));
      CHECK(solidar::validateHistoryFact(
          artifact.facts[0], producerFeatureId, artifact.sources, &error));
      CHECK(solidar::validateShapeHistoryArtifact(artifact, &error));
      retainedBuilder =
          solidar::TopologyLineageBuilder::create(std::move(artifact), &error);
      CHECK(retainedBuilder);
      CHECK(error.empty());
      sourceLifetime = sourceShape;
      outputLifetime = outputShape;
    }
    CHECK(!sourceLifetime.expired());
    CHECK(!outputLifetime.expired());
    CHECK(retainedBuilder->artifact().sources.front().shape);
    CHECK(retainedBuilder->artifact().outputShape);
    CHECK(retainedBuilder->artifact().facts.front().source ==
          std::optional{outputKey.origins.front()});
    CHECK(retainedBuilder->artifact().facts.front().sourceSubshape);
    std::string error;
    const auto built = retainedBuilder->build(&error);
    CHECK(built);
    CHECK(error.empty());
    CHECK(built->coverage() == kPartialFaceCoverage);
    CHECK(built->validateForProducer(producerFeatureId, &error));

    auto missingSourceArtifact = retainedBuilder->artifact();
    missingSourceArtifact.sources.clear();
    CHECK(!solidar::validateShapeHistoryArtifact(missingSourceArtifact,
                                                  &error));
    CHECK(error.find("source snapshot") != std::string::npos);

    auto incompleteCoverageArtifact = retainedBuilder->artifact();
    incompleteCoverageArtifact.coverage =
        solidar::TopologyLineageCoverage::CompleteFaces;
    CHECK(!solidar::validateShapeHistoryArtifact(
        incompleteCoverageArtifact, &error));
    CHECK(error.find("every output face") != std::string::npos);

    solidar::ShapeHistoryArtifact deletedArtifact;
    deletedArtifact.producerFeatureId = producerFeatureId;
    deletedArtifact.outputShape = retainedBuilder->artifact().outputShape;
    deletedArtifact.coverage = solidar::TopologyLineageCoverage::None;
    deletedArtifact.sources = retainedBuilder->artifact().sources;
    deletedArtifact.facts.push_back(
        {solidar::ShapeHistoryFactKind::Deleted, std::nullopt,
         outputKey.origins.front(),
         retainedBuilder->artifact().facts.front().sourceSubshape,
         std::nullopt, {}});
    const auto deletedBuilder =
        solidar::TopologyLineageBuilder::create(
            std::move(deletedArtifact), &error);
    CHECK(!deletedBuilder);
    CHECK(!error.empty());
  }

  // RT0 validates coherent, strongly owned source evidence and exact
  // many-to-one history groups before any lineage can be published.
  {
    constexpr solidar::FeatureId sourceAId = 900;
    constexpr solidar::FeatureId sourceBId = 901;
    constexpr solidar::FeatureId producerId = 902;
    const auto sourceAShape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(3.0, 3.0, 3.0).Shape());
    const auto sourceBShape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(gp_Pnt(10.0, 0.0, 0.0), 3.0, 3.0, 3.0)
            .Shape());
    const auto outputShape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(6.0, 3.0, 3.0).Shape());
    const auto sourceAFace =
        subshapes(*sourceAShape, TopAbs_FACE, 1).front();
    const auto sourceBFace =
        subshapes(*sourceBShape, TopAbs_FACE, 1).front();
    const auto sourceAEdge =
        subshapes(*sourceAShape, TopAbs_EDGE, 1).front();
    const auto outputFaces = subshapes(*outputShape, TopAbs_FACE, 100);
    CHECK(outputFaces.size() >= 2);

    const auto sourceAKey = makeKey(
        sourceAId, solidar::TopologyKind::Face, "source:a");
    const auto sourceBKey = makeKey(
        sourceBId, solidar::TopologyKind::Face, "source:b");
    const auto sourceALineage = solidar::TopologyLineage::create(
        {{sourceAKey, sourceAFace}}, kPartialFaceCoverage);
    const auto sourceBLineage = solidar::TopologyLineage::create(
        {{sourceBKey, sourceBFace}}, kPartialFaceCoverage);
    CHECK(sourceALineage);
    CHECK(sourceBLineage);
    const solidar::TopologySourceSnapshot sourceA{
        sourceAId, sourceAShape, sourceALineage};
    const solidar::TopologySourceSnapshot sourceB{
        sourceBId, sourceBShape, sourceBLineage};
    const auto originA = sourceAKey.immediateOrigin();
    const auto originB = sourceBKey.immediateOrigin();

    std::string error;
    CHECK(solidar::validateTopologySourceSnapshot(sourceA, &error));
    const auto foreignSourceLineage = solidar::TopologyLineage::create(
        {{sourceAKey, sourceBFace}}, kPartialFaceCoverage);
    CHECK(foreignSourceLineage);
    CHECK(!solidar::validateTopologySourceSnapshot(
        {sourceAId, sourceAShape, foreignSourceLineage}, &error));
    const auto wrongKindSourceLineage = solidar::TopologyLineage::create(
        {{sourceAKey, sourceAEdge}}, kPartialFaceCoverage);
    CHECK(wrongKindSourceLineage);
    CHECK(!solidar::validateTopologySourceSnapshot(
        {sourceAId, sourceAShape, wrongKindSourceLineage}, &error));

    const solidar::TopologyProvenanceKey generatedKey{
        producerId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::Side, "output:generated", {originA}};
    solidar::ShapeHistoryArtifact generated;
    generated.producerFeatureId = producerId;
    generated.outputShape = outputShape;
    generated.coverage = kPartialFaceCoverage;
    generated.sources = {sourceA, sourceB};
    generated.facts.push_back(
        {solidar::ShapeHistoryFactKind::Generated, std::nullopt, originA,
         sourceAFace, generatedKey, outputFaces[0]});
    CHECK(solidar::validateShapeHistoryArtifact(generated, &error));
    const auto generatedBuilder =
        solidar::TopologyLineageBuilder::create(generated, &error);
    CHECK(generatedBuilder);
    CHECK(generatedBuilder->build(&error));

    auto occurrenceKeyA = generatedKey;
    occurrenceKeyA.localName = "output:occurrence";
    occurrenceKeyA.occurrence =
        solidar::TopologyOccurrenceKey{producerId, 1};
    auto occurrenceKeyB = occurrenceKeyA;
    occurrenceKeyB.occurrence =
        solidar::TopologyOccurrenceKey{producerId, 2};
    auto occurrenceArtifact = generated;
    occurrenceArtifact.facts = {
        {solidar::ShapeHistoryFactKind::Generated, std::nullopt, originA,
         sourceAFace, occurrenceKeyA, outputFaces[0]},
        {solidar::ShapeHistoryFactKind::Generated, std::nullopt, originA,
         sourceAFace, occurrenceKeyB, outputFaces[1]}};
    const auto occurrenceBuilder =
        solidar::TopologyLineageBuilder::create(occurrenceArtifact, &error);
    CHECK(occurrenceBuilder);
    CHECK(occurrenceBuilder->build(&error));

    auto ancestryKeyA = generatedKey;
    ancestryKeyA.localName = "output:ancestry";
    auto ancestryKeyB = ancestryKeyA;
    ancestryKeyB.origins = {originB};
    auto ancestryArtifact = generated;
    ancestryArtifact.facts = {
        {solidar::ShapeHistoryFactKind::Generated, std::nullopt, originA,
         sourceAFace, ancestryKeyA, outputFaces[0]},
        {solidar::ShapeHistoryFactKind::Generated, std::nullopt, originB,
         sourceBFace, ancestryKeyB, outputFaces[1]}};
    const auto ancestryBuilder =
        solidar::TopologyLineageBuilder::create(ancestryArtifact, &error);
    CHECK(ancestryBuilder);
    CHECK(!ancestryBuilder->build(&error));

    auto duplicateSource = generated;
    duplicateSource.sources.push_back(sourceA);
    CHECK(!solidar::TopologyLineageBuilder::create(
        duplicateSource, &error));

    auto foreignCandidate = generated;
    foreignCandidate.facts[0].sourceSubshape = sourceBFace;
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(foreignCandidate), &error));
    auto wrongKindCandidate = generated;
    wrongKindCandidate.facts[0].sourceSubshape = sourceAEdge;
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(wrongKindCandidate), &error));

    const solidar::TopologyProvenanceKey mergeKey{
        producerId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Merge,
        solidar::TopologySemanticRole::BooleanResult, "output:merge",
        {originB, originA}};
    solidar::ShapeHistoryArtifact merged = generated;
    merged.facts = {
        {solidar::ShapeHistoryFactKind::Generated, std::nullopt, originA,
         sourceAFace, mergeKey, outputFaces[0]},
        {solidar::ShapeHistoryFactKind::Modified, std::nullopt, originB,
         sourceBFace, mergeKey, outputFaces[0]}};
    CHECK(solidar::validateShapeHistoryArtifact(merged, &error));
    const auto forwardMerge =
        solidar::TopologyLineageBuilder::create(merged, &error);
    CHECK(forwardMerge);
    auto reversedMerge = merged;
    std::reverse(reversedMerge.sources.begin(), reversedMerge.sources.end());
    std::reverse(reversedMerge.facts.begin(), reversedMerge.facts.end());
    const auto reverseMerge =
        solidar::TopologyLineageBuilder::create(reversedMerge, &error);
    CHECK(reverseMerge);
    const auto forwardLineage = forwardMerge->build(&error);
    const auto reverseLineage = reverseMerge->build(&error);
    CHECK(forwardLineage);
    CHECK(reverseLineage);
    const auto forwardIndex = solidar::TopologyIndex::build(
        outputShape, 910, forwardLineage, &error);
    const auto reverseIndex = solidar::TopologyIndex::build(
        outputShape, 911, reverseLineage, &error);
    CHECK(forwardIndex);
    CHECK(reverseIndex);
    const auto forwardResolved = forwardIndex->resolveFace({bodyId, mergeKey});
    const auto reverseResolved = reverseIndex->resolveFace({bodyId, mergeKey});
    CHECK(forwardResolved);
    CHECK(reverseResolved);
    CHECK(forwardResolved.subshape->IsSame(*outputFaces[0]));
    CHECK(reverseResolved.subshape->IsSame(*outputFaces[0]));

    auto missingOrigin = merged;
    missingOrigin.facts.pop_back();
    CHECK(!solidar::validateShapeHistoryArtifact(missingOrigin, &error));
    auto extraOrigin = generated;
    extraOrigin.facts.push_back(
        {solidar::ShapeHistoryFactKind::Generated, std::nullopt, originB,
         sourceBFace, generatedKey, outputFaces[0]});
    CHECK(!solidar::validateShapeHistoryArtifact(extraOrigin, &error));
    auto duplicateOrigin = generated;
    duplicateOrigin.facts.push_back(duplicateOrigin.facts.front());
    CHECK(!solidar::validateShapeHistoryArtifact(duplicateOrigin, &error));
    auto singleSourceMerge = generated;
    singleSourceMerge.facts[0].outputProvenance =
        solidar::TopologyProvenanceKey{
            producerId, solidar::TopologyKind::Face,
            solidar::TopologyProvenanceOperation::Merge,
            solidar::TopologySemanticRole::BooleanResult,
            "output:single-merge", {originA}};
    CHECK(!solidar::validateShapeHistoryArtifact(singleSourceMerge, &error));
    auto splitMerge = merged;
    splitMerge.facts[1].outputSubshape = outputFaces[1];
    CHECK(!solidar::validateShapeHistoryArtifact(splitMerge, &error));

    auto inverseManyToOne = merged;
    inverseManyToOne.facts[0].outputProvenance =
        solidar::TopologyProvenanceKey{
            producerId, solidar::TopologyKind::Face,
            solidar::TopologyProvenanceOperation::Generated,
            solidar::TopologySemanticRole::Side, "output:from-a", {originA}};
    inverseManyToOne.facts[1].outputProvenance =
        solidar::TopologyProvenanceKey{
            producerId, solidar::TopologyKind::Face,
            solidar::TopologyProvenanceOperation::Modified,
            solidar::TopologySemanticRole::Side, "output:from-b", {originB}};
    CHECK(!solidar::validateShapeHistoryArtifact(inverseManyToOne, &error));

    const solidar::TopologyProvenanceKey retainedKey{
        producerId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Retained,
        solidar::TopologySemanticRole::Original, "output:retained", {originA}};
    const auto retainedOutput = faceCompound(sourceAFace, outputFaces[1]);
    const auto reversedSourceFace =
        std::make_shared<TopoDS_Shape>(sourceAFace->Reversed());
    auto retained = generated;
    retained.outputShape = retainedOutput;
    retained.facts = {
        {solidar::ShapeHistoryFactKind::Retained, std::nullopt, originA,
         sourceAFace, retainedKey, reversedSourceFace}};
    CHECK(solidar::validateShapeHistoryArtifact(retained, &error));
    const auto retainedBuilder =
        solidar::TopologyLineageBuilder::create(retained, &error);
    CHECK(retainedBuilder);
    const auto retainedLineage = retainedBuilder->build(&error);
    CHECK(retainedLineage);
    const auto retainedIndex = solidar::TopologyIndex::build(
        retainedOutput, 913, retainedLineage, &error);
    CHECK(retainedIndex);
    const auto retainedResolved =
        retainedIndex->resolveFace({bodyId, retainedKey});
    CHECK(retainedResolved);
    CHECK(retainedResolved.subshape->IsSame(*sourceAFace));
    auto invalidRetained = retained;
    invalidRetained.facts[0].outputSubshape = outputFaces[1];
    CHECK(!solidar::validateShapeHistoryArtifact(invalidRetained, &error));

    const solidar::HistoryFact deletedA{
        solidar::ShapeHistoryFactKind::Deleted, std::nullopt, originA,
        sourceAFace, std::nullopt, {}};
    auto deletedAndGenerated = generated;
    deletedAndGenerated.facts.push_back(deletedA);
    const auto deletedGeneratedBuilder =
        solidar::TopologyLineageBuilder::create(
            deletedAndGenerated, &error);
    CHECK(deletedGeneratedBuilder);
    CHECK(deletedGeneratedBuilder->build(&error));
    auto deletedAndModified = generated;
    deletedAndModified.facts[0].kind =
        solidar::ShapeHistoryFactKind::Modified;
    deletedAndModified.facts[0].outputProvenance =
        solidar::TopologyProvenanceKey{
            producerId, solidar::TopologyKind::Face,
            solidar::TopologyProvenanceOperation::Modified,
            solidar::TopologySemanticRole::Side, "output:modified", {originA}};
    deletedAndModified.facts.push_back(deletedA);
    CHECK(!solidar::TopologyLineageBuilder::create(
        deletedAndModified, &error));
    auto deletedAndRetained = retained;
    deletedAndRetained.facts.push_back(deletedA);
    CHECK(!solidar::TopologyLineageBuilder::create(
        deletedAndRetained, &error));

    solidar::ShapeHistoryArtifact seeded;
    seeded.producerFeatureId = producerId;
    seeded.outputShape = outputShape;
    seeded.coverage = kPartialFaceCoverage;
    const solidar::AuthoredTopologyToken startCap{
        solidar::AuthoredTopologySourceKind::ProfileStartCap, 777,
        solidar::sketch::kInvalidGeometryId};
    const solidar::AuthoredTopologyToken endCap{
        solidar::AuthoredTopologySourceKind::ProfileEndCap, 777,
        solidar::sketch::kInvalidGeometryId};
    const solidar::AuthoredTopologyToken geometrySide{
        solidar::AuthoredTopologySourceKind::GeometrySide, 777, 12};
    const auto startCapKey = solidar::projectAuthoredTopologyToken(
        startCap, producerId, &error);
    const auto endCapKey = solidar::projectAuthoredTopologyToken(
        endCap, producerId, &error);
    const auto geometrySideKey = solidar::projectAuthoredTopologyToken(
        geometrySide, producerId, &error);
    CHECK(startCapKey);
    CHECK(endCapKey);
    CHECK(geometrySideKey);
    CHECK(startCapKey != endCapKey);
    CHECK(startCapKey->operation ==
          solidar::TopologyProvenanceOperation::Seed);
    CHECK(startCapKey->role == solidar::TopologySemanticRole::Cap);
    CHECK(endCapKey->role == solidar::TopologySemanticRole::Cap);
    CHECK(geometrySideKey->role == solidar::TopologySemanticRole::Side);
    CHECK(startCapKey->origins.empty());
    CHECK(endCapKey->origins.empty());
    CHECK(geometrySideKey->origins.empty());
    auto changedGeometrySide = geometrySide;
    changedGeometrySide.geometryId = 13;
    CHECK(solidar::projectAuthoredTopologyToken(
              changedGeometrySide, producerId, &error) != geometrySideKey);
    changedGeometrySide = geometrySide;
    changedGeometrySide.sketchId = 778;
    CHECK(solidar::projectAuthoredTopologyToken(
              changedGeometrySide, producerId, &error) != geometrySideKey);
    seeded.facts.push_back(
        {solidar::ShapeHistoryFactKind::Seed, geometrySide, std::nullopt, {},
         std::nullopt, outputFaces[0]});
    CHECK(solidar::validateShapeHistoryArtifact(seeded, &error));
    const auto seededBuilder =
        solidar::TopologyLineageBuilder::create(seeded, &error);
    CHECK(seededBuilder);
    CHECK(seededBuilder->artifact().facts[0].outputProvenance ==
          geometrySideKey);
    CHECK(seededBuilder->build(&error));
    auto invalidSeed = seeded;
    invalidSeed.facts[0].authoredSource.reset();
    CHECK(!solidar::TopologyLineageBuilder::create(invalidSeed, &error));
    invalidSeed = seeded;
    invalidSeed.facts[0].authoredSource->kind =
        static_cast<solidar::AuthoredTopologySourceKind>(255);
    CHECK(!solidar::TopologyLineageBuilder::create(invalidSeed, &error));
    invalidSeed = seeded;
    invalidSeed.facts[0].authoredSource->sketchId =
        solidar::kInvalidSketchId;
    CHECK(!solidar::TopologyLineageBuilder::create(invalidSeed, &error));
    invalidSeed = seeded;
    invalidSeed.facts[0].authoredSource->geometryId =
        solidar::sketch::kInvalidGeometryId;
    CHECK(!solidar::validateShapeHistoryArtifact(invalidSeed, &error));
    invalidSeed = seeded;
    invalidSeed.facts[0].authoredSource = startCap;
    invalidSeed.facts[0].authoredSource->geometryId = 99;
    CHECK(!solidar::TopologyLineageBuilder::create(invalidSeed, &error));
    invalidSeed = seeded;
    auto incompatibleSeedKey = *geometrySideKey;
    incompatibleSeedKey.operation =
        solidar::TopologyProvenanceOperation::Generated;
    invalidSeed.facts[0].outputProvenance = incompatibleSeedKey;
    CHECK(!solidar::TopologyLineageBuilder::create(invalidSeed, &error));
    invalidSeed = seeded;
    incompatibleSeedKey = *geometrySideKey;
    incompatibleSeedKey.role = solidar::TopologySemanticRole::Cap;
    invalidSeed.facts[0].outputProvenance = incompatibleSeedKey;
    CHECK(!solidar::TopologyLineageBuilder::create(invalidSeed, &error));
    invalidSeed = seeded;
    incompatibleSeedKey = *geometrySideKey;
    incompatibleSeedKey.origins = {originA};
    invalidSeed.facts[0].outputProvenance = incompatibleSeedKey;
    CHECK(!solidar::TopologyLineageBuilder::create(invalidSeed, &error));

    // Complete coverage accepts orientation aliases via IsSame and rejects a
    // single missing canonical face.
    solidar::ShapeHistoryArtifact complete;
    complete.producerFeatureId = producerId;
    complete.outputShape = outputShape;
    complete.coverage = solidar::TopologyLineageCoverage::CompleteFaces;
    for (std::size_t index = 0; index < outputFaces.size(); ++index) {
      auto candidate = outputFaces[index];
      if (index == 0)
        candidate =
            std::make_shared<TopoDS_Shape>(outputFaces[index]->Reversed());
      complete.facts.push_back(
          {solidar::ShapeHistoryFactKind::Seed,
           solidar::AuthoredTopologyToken{
               solidar::AuthoredTopologySourceKind::GeometrySide, 777,
               index + 1},
           std::nullopt, {}, std::nullopt, candidate});
    }
    const auto completeBuilder =
        solidar::TopologyLineageBuilder::create(complete, &error);
    CHECK(completeBuilder);
    const auto completeLineage = completeBuilder->build(&error);
    CHECK(completeLineage);
    CHECK(solidar::TopologyIndex::build(
        outputShape, 912, completeLineage, &error));
    auto incomplete = complete;
    incomplete.facts.pop_back();
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(incomplete), &error));

    // Malformed artifacts fail at create; the valid control reaches build.
    auto producerMismatch = generated;
    producerMismatch.facts[0].outputProvenance->featureId = producerId + 1;
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(producerMismatch), &error));
    auto relationMismatch = generated;
    relationMismatch.facts[0].kind = solidar::ShapeHistoryFactKind::Modified;
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(relationMismatch), &error));
    auto kindMismatch = generated;
    kindMismatch.facts[0].outputProvenance->kind =
        solidar::TopologyKind::Edge;
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(kindMismatch), &error));
    auto foreignOutput = generated;
    foreignOutput.facts[0].outputSubshape = sourceAFace;
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(foreignOutput), &error));
    auto sourceRetention = generated;
    sourceRetention.facts[0].outputProvenance =
        solidar::TopologyProvenanceKey{
            producerId, solidar::TopologyKind::Face,
            solidar::TopologyProvenanceOperation::Generated,
            solidar::TopologySemanticRole::Side, "output:wrong-source",
            {originB}};
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(sourceRetention), &error));
    auto invalidRelation = generated;
    invalidRelation.facts[0].kind =
        static_cast<solidar::ShapeHistoryFactKind>(255);
    CHECK(!solidar::TopologyLineageBuilder::create(
        std::move(invalidRelation), &error));

    // create() is not the sole trust boundary: build() revalidates the
    // strongly held objects in case another owning alias mutated them.
    auto mutableOutput =
        std::make_shared<TopoDS_Shape>(*generated.outputShape);
    auto lateOutputArtifact = generated;
    lateOutputArtifact.outputShape = mutableOutput;
    const auto lateOutputBuilder =
        solidar::TopologyLineageBuilder::create(lateOutputArtifact, &error);
    CHECK(lateOutputBuilder);
    *mutableOutput =
        BRepPrimAPI_MakeBox(gp_Pnt(100.0, 100.0, 100.0), 1.0, 1.0, 1.0)
            .Shape();
    CHECK(!lateOutputBuilder->build(&error));

    const auto lateSourceBuilder =
        solidar::TopologyLineageBuilder::create(generated, &error);
    CHECK(lateSourceBuilder);
    *sourceAShape =
        BRepPrimAPI_MakeBox(gp_Pnt(200.0, 0.0, 0.0), 1.0, 1.0, 1.0)
            .Shape();
    CHECK(!lateSourceBuilder->build(&error));
  }

  // A unique v2 key resolves exactly. V2 cannot carry v1 evidence by type;
  // missing provenance therefore returns None/Missing. Carrier-backed v1
  // legacy, semantic-tag and geometric-signature behavior stays unchanged.
  {
    const auto faceKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:unique");
    const auto edgeKey =
        makeKey(featureId, solidar::TopologyKind::Edge, "edge:unique");
    std::string error;
    const auto lineage = solidar::TopologyLineage::create(
        {{faceKey, faces[0]}, {edgeKey, edges[0]}},
        kPartialFaceCoverage, &error);
    CHECK(lineage);
    CHECK(error.empty());
    const auto index =
        solidar::TopologyIndex::build(shape, 700, lineage, &error);
    CHECK(index);
    CHECK(error.empty());
    CHECK(index->lineage() == lineage);
    const auto baselineIndex =
        solidar::TopologyIndex::build(shape, 700, &error);
    CHECK(baselineIndex);
    CHECK(error.empty());
    CHECK(!baselineIndex->lineage());

    const solidar::TopologyReferenceV2 faceReference{bodyId, faceKey};
    const auto resolvedFace = index->resolveFace(faceReference);
    CHECK(resolvedFace);
    CHECK(resolvedFace.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolvedFace.subshape && resolvedFace.subshape->IsSame(*faces[0]));

    const solidar::TopologyReferenceV2 edgeReference{bodyId, edgeKey};
    const auto resolvedEdge = index->resolveEdge(edgeReference);
    CHECK(resolvedEdge);
    CHECK(resolvedEdge.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolvedEdge.subshape && resolvedEdge.subshape->IsSame(*edges[0]));

    auto absentKey = faceKey;
    absentKey.localName = "side:absent";
    const auto missing = index->resolveFace({bodyId, absentKey});
    CHECK(!missing);
    CHECK(missing.method == solidar::TopologyMatchMethod::None);
    CHECK(missing.failure == solidar::TopologyResolutionFailure::Missing);
    CHECK(!missing.subshape);

    const auto invalidOwner = index->resolveFace(
        {solidar::kInvalidBodyId, faceKey});
    CHECK(!invalidOwner);
    CHECK(invalidOwner.failure ==
          solidar::TopologyResolutionFailure::InvalidReference);
    const auto wrongResolver = index->resolveEdge(faceReference);
    CHECK(!wrongResolver);
    CHECK(wrongResolver.failure ==
          solidar::TopologyResolutionFailure::InvalidReference);

    solidar::TopologyReference legacy;
    legacy.bodyId = bodyId;
    legacy.featureId = featureId;
    legacy.kind = solidar::TopologyKind::Face;
    legacy.legacyIndex = 0;
    const auto tagged =
        baselineIndex->createFaceReference(bodyId, featureId, 0);
    CHECK(tagged);
    CHECK(tagged.reference.v1());
    auto signatureOnly = *tagged.reference.v1();
    signatureOnly.persistentTag.clear();

    const auto compareV1 = [&](const solidar::TopologyReference& reference,
                               solidar::TopologyMatchMethod expectedMethod) {
      const auto baseline = baselineIndex->resolveFace(reference);
      const auto carrier = index->resolveFace(reference);
      CHECK(bool(baseline) == bool(carrier));
      CHECK(baseline.failure == carrier.failure);
      CHECK(baseline.method == carrier.method);
      CHECK(baseline.method == expectedMethod);
      CHECK(baseline.subshape.has_value() == carrier.subshape.has_value());
      CHECK(baseline.subshape);
      CHECK(carrier.subshape);
      CHECK(baseline.subshape->IsSame(*carrier.subshape));
    };
    compareV1(legacy, solidar::TopologyMatchMethod::LegacyIndex);
    compareV1(*tagged.reference.v1(),
              solidar::TopologyMatchMethod::SemanticTag);
    compareV1(signatureOnly,
              solidar::TopologyMatchMethod::GeometricSignature);

    solidar::TopologyReference legacyEdge;
    legacyEdge.bodyId = bodyId;
    legacyEdge.featureId = featureId;
    legacyEdge.kind = solidar::TopologyKind::Edge;
    legacyEdge.legacyIndex = 0;
    const auto capturedEdge =
        baselineIndex->createEdgeReference(bodyId, featureId, 0);
    CHECK(capturedEdge);
    CHECK(capturedEdge.reference.v1());
    auto signatureEdge = *capturedEdge.reference.v1();
    signatureEdge.persistentTag.clear();
    const auto compareEdgeV1 = [&](const solidar::TopologyReference& reference,
                                   solidar::TopologyMatchMethod method,
                                   solidar::TopologyResolutionFailure failure) {
      const auto baseline = baselineIndex->resolveEdge(reference);
      const auto carrier = index->resolveEdge(
          solidar::EdgeReference::fromV1(reference));
      CHECK(bool(baseline) == bool(carrier));
      CHECK(baseline.failure == carrier.failure);
      CHECK(baseline.method == carrier.method);
      CHECK(baseline.index == carrier.index);
      CHECK(baseline.error == carrier.error);
      CHECK(baseline.method == method);
      CHECK(baseline.failure == failure);
      CHECK(baseline.subshape.has_value() == carrier.subshape.has_value());
      if (baseline.subshape)
        CHECK(baseline.subshape->IsSame(*carrier.subshape));
    };
    compareEdgeV1(legacyEdge, solidar::TopologyMatchMethod::LegacyIndex,
                  solidar::TopologyResolutionFailure::None);
    compareEdgeV1(signatureEdge,
                  solidar::TopologyMatchMethod::GeometricSignature,
                  solidar::TopologyResolutionFailure::None);
    auto tagOnlyEdge = legacyEdge;
    tagOnlyEdge.persistentTag = "edge:missing-tag";
    tagOnlyEdge.edgeSignature.reset();
    compareEdgeV1(tagOnlyEdge, solidar::TopologyMatchMethod::None,
                  solidar::TopologyResolutionFailure::Missing);

    auto missingEdge = signatureEdge;
    missingEdge.edgeSignature->midpoint = {10000.0, 10000.0, 10000.0};
    compareEdgeV1(missingEdge, solidar::TopologyMatchMethod::None,
                  solidar::TopologyResolutionFailure::Missing);

    std::vector<solidar::TopologyReference> capturedEdges;
    for (std::size_t raw = 0; raw < baselineIndex->rawEdgeCount(); ++raw) {
      const auto captured =
          baselineIndex->createEdgeReference(bodyId, featureId, raw);
      CHECK(captured);
      CHECK(captured.reference.v1());
      capturedEdges.push_back(*captured.reference.v1());
      capturedEdges.back().persistentTag.clear();
    }
    std::optional<std::pair<std::size_t, std::size_t>> equivalent;
    for (std::size_t first = 0;
         first < capturedEdges.size() && !equivalent; ++first)
      for (std::size_t second = first + 1; second < capturedEdges.size();
           ++second) {
        const auto& a = *capturedEdges[first].edgeSignature;
        const auto& b = *capturedEdges[second].edgeSignature;
        const double tangentDot = a.tangent.x * b.tangent.x +
                                  a.tangent.y * b.tangent.y +
                                  a.tangent.z * b.tangent.z;
        if (a.curve == b.curve && std::abs(a.length - b.length) < 1e-7 &&
            std::abs(tangentDot) > 0.999) {
          equivalent = {{first, second}};
          break;
        }
      }
    CHECK(equivalent);
    auto ambiguousEdge = capturedEdges[equivalent->first];
    const auto& other = *capturedEdges[equivalent->second].edgeSignature;
    ambiguousEdge.edgeSignature->midpoint = {
        (ambiguousEdge.edgeSignature->midpoint.x + other.midpoint.x) * 0.5,
        (ambiguousEdge.edgeSignature->midpoint.y + other.midpoint.y) * 0.5,
        (ambiguousEdge.edgeSignature->midpoint.z + other.midpoint.z) * 0.5};
    ambiguousEdge.edgeSignature->bounds.minimum = {
        (ambiguousEdge.edgeSignature->bounds.minimum.x +
         other.bounds.minimum.x) * 0.5,
        (ambiguousEdge.edgeSignature->bounds.minimum.y +
         other.bounds.minimum.y) * 0.5,
        (ambiguousEdge.edgeSignature->bounds.minimum.z +
         other.bounds.minimum.z) * 0.5};
    ambiguousEdge.edgeSignature->bounds.maximum = {
        (ambiguousEdge.edgeSignature->bounds.maximum.x +
         other.bounds.maximum.x) * 0.5,
        (ambiguousEdge.edgeSignature->bounds.maximum.y +
         other.bounds.maximum.y) * 0.5,
        (ambiguousEdge.edgeSignature->bounds.maximum.z +
         other.bounds.maximum.z) * 0.5};
    compareEdgeV1(ambiguousEdge, solidar::TopologyMatchMethod::None,
                  solidar::TopologyResolutionFailure::Ambiguous);
  }

  // IsSame aliases are idempotent. Distinct keys may name the same canonical
  // subshape, while the same key on two distinct canonical subshapes remains
  // ambiguous.
  {
    const auto aliasKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:alias");
    const auto secondAliasKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:alias-two");
    const auto reversedAlias =
        std::make_shared<TopoDS_Shape>(faces[0]->Reversed());
    CHECK(faces[0]->IsSame(*reversedAlias));
    const auto aliasLineage = solidar::TopologyLineage::create(
        {{aliasKey, faces[0]},
         {aliasKey, reversedAlias},
         {secondAliasKey, reversedAlias}},
        kPartialFaceCoverage);
    CHECK(aliasLineage);
    const auto aliasIndex =
        solidar::TopologyIndex::build(shape, 701, aliasLineage);
    CHECK(aliasIndex);
    const auto firstAlias =
        aliasIndex->resolveFace({bodyId, aliasKey});
    const auto secondAlias =
        aliasIndex->resolveFace({bodyId, secondAliasKey});
    CHECK(firstAlias);
    CHECK(secondAlias);
    CHECK(firstAlias.subshape->IsSame(*secondAlias.subshape));

    const auto duplicateKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:duplicate");
    CHECK(!faces[0]->IsSame(*faces[1]));
    const auto lineage = solidar::TopologyLineage::create(
        {{duplicateKey, faces[0]}, {duplicateKey, faces[1]}},
        kPartialFaceCoverage);
    CHECK(lineage);
    const auto index = solidar::TopologyIndex::build(shape, 702, lineage);
    CHECK(index);
    const auto ambiguous =
        index->resolveFace(solidar::TopologyReferenceV2{bodyId, duplicateKey});
    CHECK(!ambiguous);
    CHECK(!ambiguous.subshape);
    CHECK(ambiguous.method == solidar::TopologyMatchMethod::None);
    CHECK(ambiguous.failure ==
          solidar::TopologyResolutionFailure::Ambiguous);
  }

  // FaceReference is a closed v1|v2 carrier. Its active alternative is
  // preserved by copy/move and equality never collapses different versions
  // that happen to share the same owner.
  {
    const solidar::FaceReference invalid;
    CHECK(invalid.isV1());
    CHECK(invalid.bodyId() == solidar::kInvalidBodyId);
    CHECK(invalid.featureId() == solidar::kInvalidFeatureId);

    const auto key =
        makeKey(featureId, solidar::TopologyKind::Face, "carrier:face");
    const solidar::FaceReference v1(bodyId, featureId, 3, "legacy:face");
    const auto v2 = solidar::FaceReference::fromV2({bodyId, key});
    CHECK(v1.version() == solidar::FaceReferenceVersion::V1);
    CHECK(v1.isV1());
    CHECK(!v1.isV2());
    CHECK(v1.bodyId() == bodyId);
    CHECK(v1.featureId() == featureId);
    CHECK(v1.v1());
    CHECK(!v1.v2());
    CHECK(v1.faceIndex() == 3);
    CHECK(v2.version() == solidar::FaceReferenceVersion::V2);
    CHECK(v2.isV2());
    CHECK(v2.bodyId() == bodyId);
    CHECK(v2.featureId() == featureId);
    CHECK(v2.v2());
    CHECK(!v2.v1());
    CHECK(v1 != v2);

    const auto copied = v2;
    CHECK(copied == v2);
    auto moved = copied;
    const auto movedAgain = std::move(moved);
    CHECK(movedAgain == v2);
  }

  // V2 creation names the selected canonical face only when exactly one Face
  // provenance key exists, that key is owned by the expected producer, and
  // the complete key resolves globally to exactly one face. Raw IsSame
  // orientation aliases are idempotent.
  {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const auto boxFaces = subshapes(box, TopAbs_FACE, 2);
    CHECK(boxFaces.size() == 2);
    const auto uniqueKey =
        makeKey(featureId, solidar::TopologyKind::Face, "factory:unique");
    const auto lineage = solidar::TopologyLineage::create(
        {{uniqueKey, boxFaces[0]}}, kPartialFaceCoverage);
    CHECK(lineage);
    const auto index = solidar::TopologyIndex::build(
        std::make_shared<TopoDS_Shape>(box), 801, lineage);
    CHECK(index);

    const auto created =
        index->createFaceReferenceV2(bodyId, featureId, 0);
    CHECK(created);
    CHECK(created.status ==
          solidar::FaceReferenceV2CreationStatus::Created);
    CHECK(created.reference.isV2());
    CHECK(created.reference.v2()->provenance == uniqueKey);
    const auto resolved = index->resolveFace(created.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*boxFaces[0]));

    const auto unnamed =
        index->createFaceReferenceV2(bodyId, featureId, 1);
    CHECK(!unnamed);
    CHECK(unnamed.status ==
          solidar::FaceReferenceV2CreationStatus::Missing);
    const auto wrongOwner =
        index->createFaceReferenceV2(bodyId, featureId + 1, 0);
    CHECK(!wrongOwner);
    CHECK(wrongOwner.status ==
          solidar::FaceReferenceV2CreationStatus::InvalidReference);
    const auto invalidOwner = index->createFaceReferenceV2(
        solidar::kInvalidBodyId, featureId, 0);
    CHECK(!invalidOwner);
    CHECK(invalidOwner.status ==
          solidar::FaceReferenceV2CreationStatus::InvalidReference);

    auto missingKey = uniqueKey;
    missingKey.localName = "factory:missing";
    const auto missingCarrier =
        solidar::FaceReference::fromV2({bodyId, missingKey});
    const auto missing = index->resolveFace(missingCarrier);
    CHECK(!missing);
    CHECK(missing.failure == solidar::TopologyResolutionFailure::Missing);
    // A v1 geometric reference to the same face still succeeds, proving the
    // v2 visit did not fall back across alternatives.
    const auto geometric =
        index->createFaceReference(bodyId, featureId, 0);
    CHECK(geometric);
    CHECK(index->resolveFace(geometric.reference));

    auto secondKey = uniqueKey;
    secondKey.localName = "factory:second";
    const auto multipleLineage = solidar::TopologyLineage::create(
        {{uniqueKey, boxFaces[0]}, {secondKey, boxFaces[0]}},
        kPartialFaceCoverage);
    CHECK(multipleLineage);
    const auto multipleIndex = solidar::TopologyIndex::build(
        std::make_shared<TopoDS_Shape>(box), 802, multipleLineage);
    CHECK(multipleIndex);
    const auto multiple =
        multipleIndex->createFaceReferenceV2(bodyId, featureId, 0);
    CHECK(!multiple);
    CHECK(multiple.status ==
          solidar::FaceReferenceV2CreationStatus::Ambiguous);

    auto foreignKey = uniqueKey;
    foreignKey.featureId = featureId + 1;
    foreignKey.localName = "factory:foreign";
    const auto expectedFirstLineage = solidar::TopologyLineage::create(
        {{uniqueKey, boxFaces[0]}, {foreignKey, boxFaces[0]}},
        kPartialFaceCoverage);
    const auto foreignFirstLineage = solidar::TopologyLineage::create(
        {{foreignKey, boxFaces[0]}, {uniqueKey, boxFaces[0]}},
        kPartialFaceCoverage);
    CHECK(expectedFirstLineage);
    CHECK(foreignFirstLineage);
    for (const auto& mixedLineage :
         {expectedFirstLineage, foreignFirstLineage}) {
      const auto mixedIndex = solidar::TopologyIndex::build(
          std::make_shared<TopoDS_Shape>(box), 805, mixedLineage);
      CHECK(mixedIndex);
      const auto mixed =
          mixedIndex->createFaceReferenceV2(bodyId, featureId, 0);
      CHECK(!mixed);
      CHECK(mixed.status ==
            solidar::FaceReferenceV2CreationStatus::Ambiguous);
    }

    const auto duplicateLineage = solidar::TopologyLineage::create(
        {{uniqueKey, boxFaces[0]}, {uniqueKey, boxFaces[1]}},
        kPartialFaceCoverage);
    CHECK(duplicateLineage);
    const auto duplicateIndex = solidar::TopologyIndex::build(
        std::make_shared<TopoDS_Shape>(box), 803, duplicateLineage);
    CHECK(duplicateIndex);
    const auto duplicate =
        duplicateIndex->createFaceReferenceV2(bodyId, featureId, 0);
    CHECK(!duplicate);
    CHECK(duplicate.status ==
          solidar::FaceReferenceV2CreationStatus::Ambiguous);
    const auto duplicateCarrier =
        solidar::FaceReference::fromV2({bodyId, uniqueKey});
    const auto duplicateResolution =
        duplicateIndex->resolveFace(duplicateCarrier);
    CHECK(!duplicateResolution);
    CHECK(duplicateResolution.failure ==
          solidar::TopologyResolutionFailure::Ambiguous);

    const auto reversed =
        std::make_shared<TopoDS_Shape>(boxFaces[0]->Reversed());
    const auto aliasShape = faceCompound(boxFaces[0], reversed);
    const auto aliasLineage = solidar::TopologyLineage::create(
        {{uniqueKey, boxFaces[0]}}, kPartialFaceCoverage);
    CHECK(aliasLineage);
    const auto aliasIndex =
        solidar::TopologyIndex::build(aliasShape, 804, aliasLineage);
    CHECK(aliasIndex);
    CHECK(aliasIndex->rawFaceCount() == 2);
    const auto firstAlias =
        aliasIndex->createFaceReferenceV2(bodyId, featureId, 0);
    const auto secondAlias =
        aliasIndex->createFaceReferenceV2(bodyId, featureId, 1);
    CHECK(firstAlias);
    CHECK(secondAlias);
    CHECK(firstAlias.reference == secondAlias.reference);
  }

  // EdgeReference is the same closed v1|v2 carrier contract as
  // FaceReference. fromV1 normalizes the kind and removes face-only evidence;
  // fromV2 preserves the exact operation-provenance value.
  {
    solidar::EdgeReference invalid;
    CHECK(invalid.version() == solidar::EdgeReferenceVersion::V1);
    CHECK(invalid.isV1());
    CHECK(!invalid.isV2());
    CHECK(invalid.bodyId() == solidar::kInvalidBodyId);
    CHECK(invalid.featureId() == solidar::kInvalidFeatureId);
    CHECK(invalid.v1());
    CHECK(!invalid.v2());
    CHECK(invalid.v1()->kind == solidar::TopologyKind::Edge);
    CHECK(std::holds_alternative<solidar::TopologyReference>(
        invalid.storage()));

    solidar::TopologyReference rawV1;
    rawV1.bodyId = bodyId;
    rawV1.featureId = featureId;
    rawV1.kind = solidar::TopologyKind::Face;
    rawV1.legacyIndex = 7;
    rawV1.persistentTag = "legacy:edge";
    rawV1.faceSignature = solidar::FaceSignature{};
    solidar::EdgeSignature exactSignature;
    exactSignature.curve = solidar::CurveKind::Circle;
    exactSignature.length = 12.5;
    exactSignature.midpoint = {1.0, 2.0, 3.0};
    exactSignature.radius = 2.5;
    exactSignature.center = {4.0, 5.0, 6.0};
    rawV1.edgeSignature = exactSignature;
    auto v1 = solidar::EdgeReference::fromV1(rawV1);
    CHECK(v1.isV1());
    CHECK(!v1.isV2());
    CHECK(v1.bodyId() == bodyId);
    CHECK(v1.featureId() == featureId);
    CHECK(v1.v1());
    CHECK(!v1.v2());
    CHECK(std::holds_alternative<solidar::TopologyReference>(v1.storage()));
    CHECK(v1.v1()->kind == solidar::TopologyKind::Edge);
    CHECK(!v1.v1()->faceSignature);
    CHECK(v1.edgeIndex() == 7);
    CHECK(v1.persistentTag() == "legacy:edge");
    CHECK(v1.signature() == exactSignature);
    v1.edgeIndex() = 9;
    v1.persistentTag() = "legacy:edge:mutated";
    v1.signature()->length = 15.0;
    CHECK(v1.v1()->legacyIndex == 9);
    CHECK(v1.v1()->persistentTag == "legacy:edge:mutated");
    CHECK(v1.v1()->edgeSignature->length == 15.0);
    const auto v1Copy = v1;
    CHECK(v1Copy == v1);
    auto v1Moved = v1Copy;
    const auto v1Expected = v1Moved;
    auto movedCarrier = std::move(v1Moved);
    CHECK(movedCarrier == v1Expected);

    auto exactV2 = solidar::TopologyReferenceV2{
        bodyId,
        makeKey(featureId, solidar::TopologyKind::Edge, "carrier:edge")};
    const auto v2 = solidar::EdgeReference::fromV2(exactV2);
    CHECK(v2.version() == solidar::EdgeReferenceVersion::V2);
    CHECK(v2.isV2());
    CHECK(!v2.isV1());
    CHECK(v2.bodyId() == bodyId);
    CHECK(v2.featureId() == featureId);
    CHECK(!v2.v1());
    CHECK(v2.v2());
    CHECK(std::holds_alternative<solidar::TopologyReferenceV2>(v2.storage()));
    CHECK(*v2.v2() == exactV2);
    CHECK(v1 != v2);
    const auto v2Copy = v2;
    CHECK(v2Copy == v2);
    auto v2Moved = v2Copy;
    const auto v2Expected = v2Moved;
    auto movedV2 = std::move(v2Moved);
    CHECK(movedV2 == v2Expected);
    CHECK(solidar::EdgeReference::fromV2(*v2.v2()) == v2);

    exactV2.provenance.kind = solidar::TopologyKind::Face;
    const auto unnormalizedV2 =
        solidar::EdgeReference::fromV2(exactV2);
    CHECK(unnormalizedV2.v2());
    CHECK(*unnormalizedV2.v2() == exactV2);
  }

  // Exact edge-reference creation uses one Edge key on the selected canonical
  // edge, owned by the expected producer, then verifies global exact
  // resolution. It never falls back to v1 geometric evidence.
  {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const auto boxFaces = subshapes(box, TopAbs_FACE, 1);
    const auto boxEdges = subshapes(box, TopAbs_EDGE, 2);
    CHECK(boxFaces.size() == 1);
    CHECK(boxEdges.size() == 2);
    const auto faceKey =
        makeKey(featureId, solidar::TopologyKind::Face, "factory:face");
    const auto uniqueKey =
        makeKey(featureId, solidar::TopologyKind::Edge, "factory:edge");
    const auto lineage = solidar::TopologyLineage::create(
        {{faceKey, boxFaces[0]}, {uniqueKey, boxEdges[0]}},
        kPartialFaceCoverage);
    CHECK(lineage);
    const auto index = solidar::TopologyIndex::build(
        std::make_shared<TopoDS_Shape>(box), 901, lineage);
    CHECK(index);

    const auto created =
        index->createEdgeReferenceV2(bodyId, featureId, 0);
    CHECK(created);
    CHECK(created.status == solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(created.reference.isV2());
    CHECK(created.reference.v2()->provenance == uniqueKey);
    const auto resolved = index->resolveEdge(created.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*boxEdges[0]));

    const auto absent =
        index->createEdgeReferenceV2(bodyId, featureId, 1);
    CHECK(!absent);
    CHECK(absent.status == solidar::EdgeReferenceV2CreationStatus::Missing);
    const auto outOfRange =
        index->createEdgeReferenceV2(bodyId, featureId, 9999);
    CHECK(!outOfRange);
    CHECK(outOfRange.status ==
          solidar::EdgeReferenceV2CreationStatus::Missing);
    const auto invalidOwner = index->createEdgeReferenceV2(
        solidar::kInvalidBodyId, featureId, 0);
    CHECK(!invalidOwner);
    CHECK(invalidOwner.status ==
          solidar::EdgeReferenceV2CreationStatus::InvalidReference);
    const auto wrongProducer =
        index->createEdgeReferenceV2(bodyId, featureId + 1, 0);
    CHECK(!wrongProducer);
    CHECK(wrongProducer.status ==
          solidar::EdgeReferenceV2CreationStatus::InvalidReference);

    auto wrongKindReference = *created.reference.v2();
    wrongKindReference.provenance.kind = solidar::TopologyKind::Face;
    const auto wrongKind = index->resolveEdge(
        solidar::EdgeReference::fromV2(wrongKindReference));
    CHECK(!wrongKind);
    CHECK(wrongKind.failure ==
          solidar::TopologyResolutionFailure::InvalidReference);

    auto missingKey = uniqueKey;
    missingKey.localName = "factory:edge-missing";
    const auto missingCarrier =
        solidar::EdgeReference::fromV2({bodyId, missingKey});
    const auto missing = index->resolveEdge(missingCarrier);
    CHECK(!missing);
    CHECK(missing.failure == solidar::TopologyResolutionFailure::Missing);
    const auto legacy = index->createEdgeReference(bodyId, featureId, 0);
    CHECK(legacy);
    CHECK(legacy.reference.isV1());
    CHECK(index->resolveEdge(legacy.reference));

    auto secondKey = uniqueKey;
    secondKey.localName = "factory:edge-second";
    const auto multipleLineage = solidar::TopologyLineage::create(
        {{faceKey, boxFaces[0]},
         {uniqueKey, boxEdges[0]},
         {secondKey, boxEdges[0]}},
        kPartialFaceCoverage);
    CHECK(multipleLineage);
    const auto multipleIndex = solidar::TopologyIndex::build(
        std::make_shared<TopoDS_Shape>(box), 902, multipleLineage);
    CHECK(multipleIndex);
    const auto multiple =
        multipleIndex->createEdgeReferenceV2(bodyId, featureId, 0);
    CHECK(!multiple);
    CHECK(multiple.status ==
          solidar::EdgeReferenceV2CreationStatus::Ambiguous);

    const auto duplicateLineage = solidar::TopologyLineage::create(
        {{faceKey, boxFaces[0]},
         {uniqueKey, boxEdges[0]},
         {uniqueKey, boxEdges[1]}},
        kPartialFaceCoverage);
    CHECK(duplicateLineage);
    const auto duplicateIndex = solidar::TopologyIndex::build(
        std::make_shared<TopoDS_Shape>(box), 903, duplicateLineage);
    CHECK(duplicateIndex);
    const auto duplicate =
        duplicateIndex->createEdgeReferenceV2(bodyId, featureId, 0);
    CHECK(!duplicate);
    CHECK(duplicate.status ==
          solidar::EdgeReferenceV2CreationStatus::Ambiguous);

    const auto reversed =
        std::make_shared<TopoDS_Shape>(boxEdges[0]->Reversed());
    CHECK(boxEdges[0]->IsSame(*reversed));
    TopoDS_Compound aliasCompound;
    BRep_Builder aliasBuilder;
    aliasBuilder.MakeCompound(aliasCompound);
    aliasBuilder.Add(aliasCompound, *boxFaces[0]);
    aliasBuilder.Add(aliasCompound, *boxEdges[0]);
    aliasBuilder.Add(aliasCompound, *reversed);
    const auto aliasShape = std::make_shared<TopoDS_Shape>(aliasCompound);
    const auto aliasLineage = solidar::TopologyLineage::create(
        {{faceKey, boxFaces[0]}, {uniqueKey, boxEdges[0]}},
        kPartialFaceCoverage);
    CHECK(aliasLineage);
    const auto aliasIndex =
        solidar::TopologyIndex::build(aliasShape, 904, aliasLineage);
    CHECK(aliasIndex);
    std::vector<solidar::EdgeReference> aliases;
    for (std::size_t raw = 0; raw < aliasIndex->rawEdgeCount(); ++raw) {
      const auto candidate =
          aliasIndex->createEdgeReferenceV2(bodyId, featureId, raw);
      if (candidate) aliases.push_back(candidate.reference);
    }
    CHECK(aliases.size() >= 2);
    for (const auto& alias : aliases) CHECK(alias == aliases.front());

    const solidar::EdgeReferenceV2Creation sentinel;
    CHECK(!sentinel);
    CHECK(sentinel.status ==
          solidar::EdgeReferenceV2CreationStatus::Unexpected);
  }

  // Candidate traversal and binding insertion order do not change exact
  // Resolved/Missing/Ambiguous outcomes.
  {
    const auto forwardShape = faceCompound(faces[0], faces[1]);
    const auto reverseShape = faceCompound(faces[1], faces[0]);
    const auto uniqueKey =
        makeKey(featureId, solidar::TopologyKind::Face, "order:unique");
    const auto duplicateKey =
        makeKey(featureId, solidar::TopologyKind::Face, "order:duplicate");
    const auto missingKey =
        makeKey(featureId, solidar::TopologyKind::Face, "order:missing");
    const auto forwardLineage = solidar::TopologyLineage::create(
        {{uniqueKey, faces[0]},
         {duplicateKey, faces[0]},
         {duplicateKey, faces[1]}},
        kPartialFaceCoverage);
    const auto reverseLineage = solidar::TopologyLineage::create(
        {{duplicateKey, faces[1]},
         {duplicateKey, faces[0]},
         {uniqueKey, faces[0]}},
        kPartialFaceCoverage);
    CHECK(forwardLineage);
    CHECK(reverseLineage);
    const auto forwardIndex =
        solidar::TopologyIndex::build(forwardShape, 703, forwardLineage);
    const auto reverseIndex =
        solidar::TopologyIndex::build(reverseShape, 704, reverseLineage);
    CHECK(forwardIndex);
    CHECK(reverseIndex);
    const auto forwardResolved =
        forwardIndex->resolveFace({bodyId, uniqueKey});
    const auto reverseResolved =
        reverseIndex->resolveFace({bodyId, uniqueKey});
    CHECK(forwardResolved);
    CHECK(reverseResolved);
    CHECK(forwardResolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(reverseResolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(forwardResolved.subshape);
    CHECK(reverseResolved.subshape);
    CHECK(forwardResolved.subshape->IsSame(*faces[0]));
    CHECK(reverseResolved.subshape->IsSame(*faces[0]));
    CHECK(forwardResolved.subshape->IsSame(*reverseResolved.subshape));

    const auto forwardMissing =
        forwardIndex->resolveFace({bodyId, missingKey});
    const auto reverseMissing =
        reverseIndex->resolveFace({bodyId, missingKey});
    for (const auto* missing : {&forwardMissing, &reverseMissing}) {
      CHECK(!*missing);
      CHECK(!missing->subshape);
      CHECK(missing->method == solidar::TopologyMatchMethod::None);
      CHECK(missing->failure == solidar::TopologyResolutionFailure::Missing);
    }

    const auto forwardAmbiguous =
        forwardIndex->resolveFace({bodyId, duplicateKey});
    const auto reverseAmbiguous =
        reverseIndex->resolveFace({bodyId, duplicateKey});
    for (const auto* ambiguous : {&forwardAmbiguous, &reverseAmbiguous}) {
      CHECK(!*ambiguous);
      CHECK(!ambiguous->subshape);
      CHECK(ambiguous->method == solidar::TopologyMatchMethod::None);
      CHECK(ambiguous->failure ==
            solidar::TopologyResolutionFailure::Ambiguous);
    }
  }

  // Sidecar bindings are validated against the committed output and kind.
  {
    const auto faceKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:foreign");
    const TopoDS_Shape foreign =
        BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0), 1.0, 1.0, 1.0)
            .Shape();
    const auto foreignFace = subshapes(foreign, TopAbs_FACE, 1).front();
    std::string error;
    const auto foreignLineage =
        solidar::TopologyLineage::create(
            {{faceKey, foreignFace}}, kPartialFaceCoverage);
    CHECK(foreignLineage);
    CHECK(!solidar::TopologyIndex::build(shape, 705, foreignLineage, &error));
    CHECK(error.find("not part") != std::string::npos);

    const auto wrongKindLineage =
        solidar::TopologyLineage::create(
            {{faceKey, edges.front()}}, kPartialFaceCoverage);
    CHECK(wrongKindLineage);
    CHECK(!solidar::TopologyIndex::build(shape, 706, wrongKindLineage,
                                         &error));
    CHECK(error.find("non-face") != std::string::npos);

    for (int invalidField = 0; invalidField < 4; ++invalidField) {
      auto invalidKey = faceKey;
      if (invalidField == 0)
        invalidKey.kind = static_cast<solidar::TopologyKind>(255);
      else if (invalidField == 1)
        invalidKey.operation =
            static_cast<solidar::TopologyProvenanceOperation>(255);
      else if (invalidField == 2)
        invalidKey.role = static_cast<solidar::TopologySemanticRole>(255);
      else
        invalidKey.origins.front().kind =
            static_cast<solidar::TopologyKind>(255);
      CHECK(!solidar::TopologyLineage::create(
          {{invalidKey, faces.front()}}, kPartialFaceCoverage, &error));
      CHECK(!error.empty());
    }
    for (int invalidOriginField = 0; invalidOriginField < 4;
         ++invalidOriginField) {
      auto invalidKey = faceKey;
      auto& origin = invalidKey.origins.front();
      if (invalidOriginField == 0)
        origin.kind = static_cast<solidar::TopologyKind>(255);
      else if (invalidOriginField == 1)
        origin.operation =
            static_cast<solidar::TopologyProvenanceOperation>(255);
      else if (invalidOriginField == 2)
        origin.role = static_cast<solidar::TopologySemanticRole>(255);
      else
        origin.occurrence = solidar::TopologyOccurrenceKey{
            solidar::kInvalidFeatureId, 1};
      CHECK(!solidar::TopologyLineage::create(
          {{invalidKey, faces.front()}}, kPartialFaceCoverage, &error));
      CHECK(!error.empty());
    }
  }

  // ShapeFeature commits B-Rep, lineage, revision and index together. Failed,
  // exceptional and invalid-lineage rebuilds retain the complete last-valid
  // snapshot; invalidation clears the complete snapshot.
  {
    solidar::Document document;
    SyntheticLineageFeature feature(bodyId);
    solidar::RebuildContext context{document, nullptr, nullptr, nullptr};
    CHECK(feature.rebuild(context));
    const auto firstShape = feature.shape();
    const auto firstLineage = feature.topologyLineage();
    const auto firstRevision = feature.shapeRevision();
    const auto firstIndex = feature.topologyIndex();
    CHECK(firstShape);
    CHECK(firstLineage);
    CHECK(firstLineage->coverage() == kPartialFaceCoverage);
    CHECK(firstRevision != solidar::kInvalidShapeRevision);
    CHECK(firstIndex);
    CHECK(firstIndex->revision() == firstRevision);
    CHECK(firstIndex->lineage() == firstLineage);
    CHECK(firstIndex->resolveFace(feature.reference("side:first")));
    const auto ownedReference = feature.createFaceReferenceV2(bodyId, 0);
    CHECK(ownedReference);
    CHECK(ownedReference.reference.isV2());
    CHECK(ownedReference.reference.featureId() == feature.id());

    for (const SyntheticMode failureMode : {
             SyntheticMode::FailAfterStaging,
             SyntheticMode::ThrowAfterStaging,
             SyntheticMode::ForeignBinding,
             SyntheticMode::MalformedLineage,
             SyntheticMode::ProducerMismatch,
             SyntheticMode::EmptyLineage,
             SyntheticMode::DuplicateLocalName}) {
      feature.mode = failureMode;
      feature.setDirty();
      CHECK(!feature.rebuild(context));
      CHECK(feature.isFailed());
      CHECK(!feature.shape());
      CHECK(!feature.topologyLineage());
      std::string currentError;
      CHECK(!feature.topologyIndex(&currentError));
      CHECK(!currentError.empty());
      CHECK(feature.lastValidShape() == firstShape);
      CHECK(feature.lastValidTopologyLineage() == firstLineage);
      CHECK(feature.shapeRevision() == firstRevision);
      CHECK(feature.lastValidTopologyIndex() == firstIndex);
      CHECK(firstIndex->resolveFace(feature.reference("side:first")));
      CHECK(!firstIndex->resolveFace(feature.reference("side:next")));
    }

    // Recovery can publish the same key that was present in the failed
    // attempt, without retaining any half-staged state.
    feature.mode = SyntheticMode::ValidNext;
    feature.setDirty();
    CHECK(feature.rebuild(context));
    CHECK(feature.shape() != firstShape);
    CHECK(feature.topologyLineage() != firstLineage);
    CHECK(feature.shapeRevision() != firstRevision);
    const auto secondIndex = feature.topologyIndex();
    CHECK(secondIndex);
    CHECK(secondIndex->resolveFace(feature.reference("side:next")));
    CHECK(!secondIndex->resolveFace(feature.reference("side:first")));

    feature.prepareForHistory();
    CHECK(feature.isDirty());
    CHECK(!feature.lastValidShape());
    CHECK(!feature.lastValidTopologyLineage());
    CHECK(feature.rebuild(context));
    CHECK(feature.topologyIndex()->resolveFace(
        feature.reference("side:next")));

    feature.discardResult();
    CHECK(!feature.lastValidShape());
    CHECK(!feature.lastValidTopologyLineage());
    CHECK(feature.shapeRevision() == solidar::kInvalidShapeRevision);
    std::string error;
    CHECK(!feature.lastValidTopologyIndex(&error));
    CHECK(!error.empty());
    feature.setDirty();
    CHECK(feature.rebuild(context));
    CHECK(feature.topologyIndex()->resolveFace(
        feature.reference("side:next")));

    SyntheticLineageFeature legacyFeature(bodyId);
    legacyFeature.mode = SyntheticMode::LegacyNoLineage;
    CHECK(legacyFeature.rebuild(context));
    CHECK(legacyFeature.shape());
    CHECK(!legacyFeature.topologyLineage());
    CHECK(legacyFeature.topologyIndex());
  }

  // The ShapeFeature facade preserves exact edge-factory status and resolves
  // the created operation-provenance carrier back to the selected edge.
  {
    solidar::Document document;
    solidar::RebuildContext context{document, nullptr, nullptr, nullptr};
    SyntheticEdgeLineageFeature feature;
    const auto unavailable = feature.createEdgeReferenceV2(bodyId, 0);
    CHECK(!unavailable);
    CHECK(unavailable.status ==
          solidar::EdgeReferenceV2CreationStatus::Missing);
    CHECK(!unavailable.error.empty());

    CHECK(feature.rebuild(context));
    const auto selected = subshapes(*feature.shape(), TopAbs_EDGE, 1);
    CHECK(selected.size() == 1);
    const auto created = feature.createEdgeReferenceV2(bodyId, 0);
    CHECK(created);
    CHECK(created.status ==
          solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(created.reference.isV2());
    CHECK(created.reference.bodyId() == bodyId);
    CHECK(created.reference.featureId() == feature.id());
    const auto resolved = feature.topologyIndex()->resolveEdge(
        created.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*selected.front()));

    const auto invalidOwner = feature.createEdgeReferenceV2(
        solidar::kInvalidBodyId, 0);
    CHECK(!invalidOwner);
    CHECK(invalidOwner.status ==
          solidar::EdgeReferenceV2CreationStatus::InvalidReference);
    const auto absent = feature.createEdgeReferenceV2(bodyId, 9999);
    CHECK(!absent);
    CHECK(absent.status ==
          solidar::EdgeReferenceV2CreationStatus::Missing);

    SyntheticEdgeLineageFeature legacyFeature(false);
    CHECK(legacyFeature.rebuild(context));
    const auto noLineage = legacyFeature.createEdgeReferenceV2(bodyId, 0);
    CHECK(!noLineage);
    CHECK(noLineage.status ==
          solidar::EdgeReferenceV2CreationStatus::Missing);
  }

  // Standard_Failure and unknown-exception rollback remain covered by the
  // existing document_tests guarded ShapeFeature boundary matrix.

  return EXIT_SUCCESS;
}

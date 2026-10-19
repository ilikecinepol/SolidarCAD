#include "TestAssertions.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <NCollection_List.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/OcctBooleanTopologyHistoryCapture.h"

namespace {

struct SourceFixture {
  solidar::BooleanTopologySource source;
  std::vector<std::pair<std::shared_ptr<const TopoDS_Face>,
                        solidar::TopologyProvenanceOrigin>> faces;
};

class ScriptedCut final : public BRepAlgoAPI_Cut {
 public:
  ScriptedCut(const TopoDS_Shape& object, const TopoDS_Shape& tool,
              const TopoDS_Shape& output, const TopoDS_Shape& liveSource,
              const TopoDS_Shape& liveOutput, bool emitGenerated,
              bool emitModified, bool deleteLiveSource = false)
      : tool_(tool),
        liveSource_(liveSource),
        deleteLiveSource_(deleteLiveSource) {
    NCollection_List<TopoDS_Shape> arguments;
    arguments.Append(object);
    SetArguments(arguments);
    NCollection_List<TopoDS_Shape> tools;
    tools.Append(tool);
    SetTools(tools);
    SetOperation(BOPAlgo_CUT);
    SetToFillHistory(true);
    if (emitGenerated) generated_.Append(liveOutput);
    if (emitModified) modified_.Append(liveOutput);
    myShape = output;
    Done();
  }

  const NCollection_List<TopoDS_Shape>& Generated(
      const TopoDS_Shape& source) override {
    return source.IsSame(liveSource_) ? generated_ : empty_;
  }

  const NCollection_List<TopoDS_Shape>& Modified(
      const TopoDS_Shape& source) override {
    return source.IsSame(liveSource_) ? modified_ : empty_;
  }

  bool IsDeleted(const TopoDS_Shape& source) override {
    return source.IsSame(tool_) ||
           (deleteLiveSource_ && source.IsSame(liveSource_));
  }

 private:
  TopoDS_Shape tool_;
  TopoDS_Shape liveSource_;
  bool deleteLiveSource_{};
  NCollection_List<TopoDS_Shape> generated_;
  NCollection_List<TopoDS_Shape> modified_;
  NCollection_List<TopoDS_Shape> empty_;
};

class ManySourceScriptedCut final : public BRepAlgoAPI_Cut {
 public:
  ManySourceScriptedCut(std::span<const TopoDS_Shape> objects,
                        const TopoDS_Shape& tool,
                        const TopoDS_Shape& output)
      : objects_(objects.begin(), objects.end()), tool_(tool) {
    NCollection_List<TopoDS_Shape> arguments;
    for (const auto& object : objects_) arguments.Append(object);
    SetArguments(arguments);
    NCollection_List<TopoDS_Shape> tools;
    tools.Append(tool);
    SetTools(tools);
    SetOperation(BOPAlgo_CUT);
    SetToFillHistory(true);
    modified_.Append(output);
    myShape = output;
    Done();
  }

  const NCollection_List<TopoDS_Shape>& Generated(
      const TopoDS_Shape&) override {
    return empty_;
  }

  const NCollection_List<TopoDS_Shape>& Modified(
      const TopoDS_Shape& source) override {
    return std::any_of(objects_.begin(), objects_.end(), [&](const auto& item) {
             return item.IsSame(source);
           })
               ? modified_
               : empty_;
  }

  bool IsDeleted(const TopoDS_Shape& source) override {
    return source.IsSame(tool_);
  }

 private:
  std::vector<TopoDS_Shape> objects_;
  TopoDS_Shape tool_;
  NCollection_List<TopoDS_Shape> modified_;
  NCollection_List<TopoDS_Shape> empty_;
};

std::vector<TopoDS_Face> facesOf(const TopoDS_Shape& shape) {
  std::vector<TopoDS_Face> faces;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const auto face = TopoDS::Face(explorer.Current());
    if (std::none_of(faces.begin(), faces.end(), [&](const auto& existing) {
          return existing.IsSame(face);
        }))
      faces.push_back(face);
  }
  return faces;
}

bool containsFace(const TopoDS_Shape& shape, const TopoDS_Shape& face) {
  const auto faces = facesOf(shape);
  return std::any_of(faces.begin(), faces.end(),
                     [&](const auto& item) { return item.IsSame(face); });
}

std::string boundsToken(const TopoDS_Shape& shape) {
  Bnd_Box bounds;
  BRepBndLib::Add(shape, bounds);
  double xMin = 0.0;
  double yMin = 0.0;
  double zMin = 0.0;
  double xMax = 0.0;
  double yMax = 0.0;
  double zMax = 0.0;
  bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
  const auto q = [](double value) {
    return std::to_string(static_cast<long long>(std::llround(value * 1000.0)));
  };
  return q(xMin) + "," + q(yMin) + "," + q(zMin) + ":" + q(xMax) +
         "," + q(yMax) + "," + q(zMax);
}

SourceFixture sourceFixture(const TopoDS_Shape& shape,
                            solidar::FeatureId featureId,
                            solidar::BooleanOperandRole role,
                            std::string fixedLocalName = {}) {
  SourceFixture fixture;
  auto ownedShape = std::make_shared<TopoDS_Shape>(shape);
  std::vector<solidar::TopologyLineageBinding> bindings;
  const auto faces = facesOf(shape);
  for (std::size_t index = 0; index < faces.size(); ++index) {
    solidar::TopologyProvenanceKey key{
        featureId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Seed,
        solidar::TopologySemanticRole::Custom,
        fixedLocalName.empty()
            ? "test-source-face:" + boundsToken(faces[index])
            : fixedLocalName};
    auto face = std::make_shared<TopoDS_Face>(faces[index]);
    fixture.faces.emplace_back(face, key.immediateOrigin());
    bindings.push_back({std::move(key), face});
  }
  std::string error;
  auto lineage = solidar::TopologyLineage::create(
      std::move(bindings), solidar::TopologyLineageCoverage::CompleteFaces,
      &error);
  CHECK(lineage);
  CHECK(error.empty());
  fixture.source = {role, {featureId, ownedShape, std::move(lineage)}};
  return fixture;
}

std::vector<solidar::BooleanFaceBranchSeed> splitSeeds(
    BRepAlgoAPI_Cut& maker, const std::vector<SourceFixture>& fixtures) {
  std::vector<solidar::BooleanFaceBranchSeed> seeds;
  const auto output = maker.Shape();
  for (const auto& fixture : fixtures) {
    for (const auto& [face, origin] : fixture.faces) {
      std::vector<TopoDS_Face> related;
      if (containsFace(output, *face)) related.push_back(*face);
      const auto& modified = maker.Modified(*face);
      for (NCollection_List<TopoDS_Shape>::Iterator iterator(modified);
           iterator.More(); iterator.Next()) {
        if (iterator.Value().ShapeType() != TopAbs_FACE) continue;
        const auto candidate = TopoDS::Face(iterator.Value());
        if (std::none_of(related.begin(), related.end(), [&](const auto& item) {
              return item.IsSame(candidate);
            }))
          related.push_back(candidate);
      }
      const auto& generated = maker.Generated(*face);
      for (NCollection_List<TopoDS_Shape>::Iterator iterator(generated);
           iterator.More(); iterator.Next()) {
        if (iterator.Value().ShapeType() != TopAbs_FACE) continue;
        const auto candidate = TopoDS::Face(iterator.Value());
        if (std::none_of(related.begin(), related.end(), [&](const auto& item) {
              return item.IsSame(candidate);
            }))
          related.push_back(candidate);
      }
      if (related.size() <= 1) continue;
      for (const auto& outputFace : related) {
        // These are authored fixture semantics based on known box regions;
        // neither maker/source traversal nor production identity uses bounds.
        seeds.push_back({{origin}, "authored-split-branch:" +
                                      boundsToken(*face) + ":" +
                                      boundsToken(outputFace),
                         std::make_shared<TopoDS_Face>(outputFace)});
      }
    }
  }
  return seeds;
}

std::vector<std::pair<std::string, std::string>> identityToSemanticOutput(
    const solidar::TopologyLineage& lineage) {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& binding : lineage.bindings())
    result.emplace_back(binding.provenance.localName,
                        boundsToken(*binding.subshape));
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<solidar::TopologyProvenanceKey> keys(
    const solidar::TopologyLineage& lineage) {
  std::vector<solidar::TopologyProvenanceKey> result;
  for (const auto& binding : lineage.bindings())
    result.push_back(binding.provenance);
  std::sort(result.begin(), result.end(), [](const auto& left,
                                             const auto& right) {
    return left.localName < right.localName;
  });
  return result;
}

std::shared_ptr<const solidar::TopologyLineage> build(
    const solidar::BooleanTopologyCaptureResult& result) {
  std::string error;
  auto lineage = result.builder.build(&error);
  CHECK(lineage);
  CHECK(error.empty());
  CHECK(lineage->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(lineage->bindings().size() == facesOf(*result.outputShape).size());
  return lineage;
}

}  // namespace

int main() {
  constexpr solidar::FeatureId kObjectId = 8101;
  constexpr solidar::FeatureId kToolId = 8102;
  constexpr solidar::FeatureId kBooleanId = 8201;

  // A corner cut provides retained, modified and deleted source evidence.
  const TopoDS_Shape object = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
  const TopoDS_Shape tool =
      BRepPrimAPI_MakeBox(gp_Pnt(5.0, 5.0, 5.0), 8.0, 8.0, 8.0).Shape();
  auto objectSource = sourceFixture(
      object, kObjectId, solidar::BooleanOperandRole::Object);
  auto toolSource =
      sourceFixture(tool, kToolId, solidar::BooleanOperandRole::Tool);
  std::vector<SourceFixture> cutFixtures{objectSource, toolSource};
  std::vector<solidar::BooleanTopologySource> cutSources{
      objectSource.source, toolSource.source};
  std::optional<solidar::BooleanTopologyCaptureResult> retainedResult;
  {
    BRepAlgoAPI_Cut maker(object, tool);
    auto seeds = splitSeeds(maker, cutFixtures);
    solidar::BooleanTopologyCaptureFailure failure{
        solidar::BooleanTopologyCaptureFailureCode::InvalidOutput, "stale"};
    retainedResult = solidar::captureBooleanFaceHistory(
        maker, kBooleanId, cutSources, seeds, {}, &failure);
    CHECK(retainedResult);
    CHECK(failure.code ==
          solidar::BooleanTopologyCaptureFailureCode::None);
    CHECK(failure.detail.empty());
    const auto& facts = retainedResult->builder.artifact().facts;
    CHECK(std::any_of(facts.begin(), facts.end(), [](const auto& fact) {
      return fact.kind == solidar::ShapeHistoryFactKind::Retained;
    }));
    CHECK(std::any_of(facts.begin(), facts.end(), [](const auto& fact) {
      return fact.kind == solidar::ShapeHistoryFactKind::Modified;
    }));
    CHECK(std::any_of(facts.begin(), facts.end(), [](const auto& fact) {
      return fact.kind == solidar::ShapeHistoryFactKind::Deleted;
    }));
  }
  // The owned result and copied history remain valid after maker destruction.
  CHECK(build(*retainedResult));

  // Cutting through a solid splits source faces. Capture fails closed until
  // every branch has an authored seed; IsSame accepts orientation aliases.
  const TopoDS_Shape splitter =
      BRepPrimAPI_MakeBox(gp_Pnt(4.0, -1.0, -1.0), 2.0, 12.0, 12.0)
          .Shape();
  auto splitObject = sourceFixture(
      object, 8301, solidar::BooleanOperandRole::Object);
  auto splitTool = sourceFixture(
      splitter, 8302, solidar::BooleanOperandRole::Tool);
  std::vector<SourceFixture> splitFixtures{splitObject, splitTool};
  std::vector<solidar::BooleanTopologySource> splitSources{
      splitObject.source, splitTool.source};
  BRepAlgoAPI_Cut splitMaker(object, splitter);
  solidar::BooleanTopologyCaptureFailure failure;
  auto missingSeeds = solidar::captureBooleanFaceHistory(
      splitMaker, 8303, splitSources, {}, {}, &failure);
  CHECK(!missingSeeds);
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::SplitSeedRequired);
  auto seeds = splitSeeds(splitMaker, splitFixtures);
  CHECK(!seeds.empty());
  seeds.front().outputFace = std::make_shared<TopoDS_Face>(
      TopoDS::Face(seeds.front().outputFace->Reversed()));
  auto splitResult = solidar::captureBooleanFaceHistory(
      splitMaker, 8303, splitSources, seeds, {}, &failure);
  CHECK(splitResult);
  const auto splitLineage = build(*splitResult);
  std::reverse(splitSources.begin(), splitSources.end());
  BRepAlgoAPI_Cut freshSplitMaker(object, splitter);
  auto freshSeeds = splitSeeds(freshSplitMaker, splitFixtures);
  std::reverse(freshSeeds.begin(), freshSeeds.end());
  auto permutedSplitResult = solidar::captureBooleanFaceHistory(
      freshSplitMaker, 8303, splitSources, freshSeeds, {}, &failure);
  CHECK(permutedSplitResult);
  CHECK(identityToSemanticOutput(*build(*permutedSplitResult)) ==
        identityToSemanticOutput(*splitLineage));
  auto collisionSeeds = seeds;
  collisionSeeds.back().stableBranchId =
      collisionSeeds.front().stableBranchId;
  CHECK(!solidar::captureBooleanFaceHistory(
      splitMaker, 8303, splitSources, collisionSeeds, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::SplitSeedCollision);
  auto oversizedSeeds = seeds;
  oversizedSeeds.front().stableBranchId = std::string(17000, 'x');
  CHECK(!solidar::captureBooleanFaceHistory(
      splitMaker, 8303, splitSources, oversizedSeeds, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::LocalNameTooLong);
  auto nonCanonicalSeeds = seeds;
  nonCanonicalSeeds.front().origins.push_back(
      nonCanonicalSeeds.front().origins.front());
  CHECK(!solidar::captureBooleanFaceHistory(
      splitMaker, 8303, splitSources, nonCanonicalSeeds, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidSplitSeed);
  auto foreignSeeds = seeds;
  foreignSeeds.front().outputFace = std::make_shared<TopoDS_Face>(
      facesOf(BRepPrimAPI_MakeBox(gp_Pnt(30.0, 0.0, 0.0), 1.0, 1.0, 1.0)
                  .Shape())
          .front());
  CHECK(!solidar::captureBooleanFaceHistory(
      splitMaker, 8303, splitSources, foreignSeeds, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidSplitSeed);

  // Simplification is applied to the same live Fuse maker. Coplanar halves
  // merge with canonical origins, independent of argument permutation.
  const TopoDS_Shape left = BRepPrimAPI_MakeBox(5.0, 10.0, 10.0).Shape();
  const TopoDS_Shape right =
      BRepPrimAPI_MakeBox(gp_Pnt(5.0, 0.0, 0.0), 5.0, 10.0, 10.0).Shape();
  auto leftSource = sourceFixture(
      left, 8401, solidar::BooleanOperandRole::Object);
  auto rightSource = sourceFixture(
      right, 8402, solidar::BooleanOperandRole::Tool);
  std::vector<solidar::BooleanTopologySource> fuseSources{
      leftSource.source, rightSource.source};
  BRepAlgoAPI_Fuse fuse(left, right);
  auto fused = solidar::captureBooleanFaceHistory(
      fuse, 8403, fuseSources, {}, {.simplifyResult = true}, &failure);
  CHECK(fused);
  const auto fusedLineage = build(*fused);
  CHECK(std::any_of(fusedLineage->bindings().begin(),
                    fusedLineage->bindings().end(), [](const auto& binding) {
    return binding.provenance.operation ==
           solidar::TopologyProvenanceOperation::Merge;
  }));

  auto rightObject = rightSource.source;
  rightObject.role = solidar::BooleanOperandRole::Object;
  auto leftTool = leftSource.source;
  leftTool.role = solidar::BooleanOperandRole::Tool;
  std::vector<solidar::BooleanTopologySource> reversedSources{rightObject,
                                                              leftTool};
  BRepAlgoAPI_Fuse reversedFuse(right, left);
  auto reversed = solidar::captureBooleanFaceHistory(
      reversedFuse, 8403, reversedSources, {}, {.simplifyResult = true},
      &failure);
  CHECK(reversed);
  CHECK(keys(*build(*reversed)) == keys(*fusedLineage));

  // Failures are typed, clear stale details, and reject weakened evidence.
  BRepAlgoAPI_Fuse notDone;
  failure.detail = "stale";
  CHECK(!solidar::captureBooleanFaceHistory(notDone, 8501, fuseSources, {},
                                             {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::BuilderNotDone);
  CHECK(failure.detail != "stale");

  NCollection_List<TopoDS_Shape> arguments;
  arguments.Append(left);
  NCollection_List<TopoDS_Shape> tools;
  tools.Append(right);
  BRepAlgoAPI_Fuse historyDisabled;
  historyDisabled.SetArguments(arguments);
  historyDisabled.SetTools(tools);
  historyDisabled.SetToFillHistory(false);
  historyDisabled.Build();
  CHECK(!solidar::captureBooleanFaceHistory(
      historyDisabled, 8502, fuseSources, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::HistoryUnavailable);

  CHECK(!solidar::captureBooleanFaceHistory(
      historyDisabled, solidar::kInvalidFeatureId, fuseSources, {}, {},
      &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidProducer);

  auto unknownRole = fuseSources;
  unknownRole.front().role =
      static_cast<solidar::BooleanOperandRole>(255);
  BRepAlgoAPI_Fuse unknownRoleMaker(left, right);
  CHECK(!solidar::captureBooleanFaceHistory(
      unknownRoleMaker, 8503, unknownRole, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidSource);

  auto mismatched = fuseSources;
  mismatched.front().role = solidar::BooleanOperandRole::Tool;
  BRepAlgoAPI_Fuse mismatchMaker(left, right);
  CHECK(!solidar::captureBooleanFaceHistory(
      mismatchMaker, 8503, mismatched, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::SourceMismatch);

  auto duplicate = fuseSources;
  duplicate.back().snapshot.featureId = duplicate.front().snapshot.featureId;
  CHECK(!solidar::captureBooleanFaceHistory(
      mismatchMaker, 8503, duplicate, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::DuplicateSourceFeature);

  auto partial = fuseSources;
  std::string lineageError;
  partial.front().snapshot.lineage = solidar::TopologyLineage::create(
      leftSource.source.snapshot.lineage->bindings(),
      solidar::TopologyLineageCoverage::PartialFaces, &lineageError);
  CHECK(partial.front().snapshot.lineage);
  CHECK(!solidar::captureBooleanFaceHistory(
      mismatchMaker, 8503, partial, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidSource);

  auto missingIdentity = fuseSources;
  auto incompleteBindings =
      leftSource.source.snapshot.lineage->bindings();
  incompleteBindings.pop_back();
  missingIdentity.front().snapshot.lineage = solidar::TopologyLineage::create(
      std::move(incompleteBindings),
      solidar::TopologyLineageCoverage::CompleteFaces, &lineageError);
  CHECK(missingIdentity.front().snapshot.lineage);
  CHECK(!solidar::captureBooleanFaceHistory(
      mismatchMaker, 8503, missingIdentity, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::MissingSourceIdentity);

  auto duplicateIdentity = fuseSources;
  auto duplicatedBindings =
      leftSource.source.snapshot.lineage->bindings();
  auto duplicateKey = duplicatedBindings.front().provenance;
  duplicateKey.localName += ":duplicate";
  duplicatedBindings.push_back(
      {std::move(duplicateKey), duplicatedBindings.front().subshape});
  duplicateIdentity.front().snapshot.lineage =
      solidar::TopologyLineage::create(
          std::move(duplicatedBindings),
          solidar::TopologyLineageCoverage::CompleteFaces, &lineageError);
  CHECK(duplicateIdentity.front().snapshot.lineage);
  CHECK(!solidar::captureBooleanFaceHistory(
      mismatchMaker, 8503, duplicateIdentity, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::DuplicateSourceIdentity);

  // Script only the OCCT history interface for relations that are not
  // deterministic across kernels: face-valued Generated, contradictory
  // evidence and an uncovered final face. The production adapter has no test
  // seam and still validates real Arguments/Tools and owned TopoDS values.
  const TopoDS_Face scriptedObject = facesOf(object).front();
  const TopoDS_Face scriptedTool = facesOf(tool).front();
  const TopoDS_Face scriptedOutput = facesOf(right).front();
  auto scriptedObjectSource = sourceFixture(
      scriptedObject, 8601, solidar::BooleanOperandRole::Object);
  auto scriptedToolSource = sourceFixture(
      scriptedTool, 8602, solidar::BooleanOperandRole::Tool);
  std::vector<solidar::BooleanTopologySource> scriptedSources{
      scriptedObjectSource.source, scriptedToolSource.source};
  ScriptedCut generatedFaceMaker(scriptedObject, scriptedTool,
                                 scriptedOutput, scriptedObject,
                                 scriptedOutput, true, false);
  CHECK(!solidar::captureBooleanFaceHistory(
      generatedFaceMaker, 8603, scriptedSources, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidHistory);

  TopoDS_Edge generatedEdge;
  for (TopExp_Explorer explorer(object, TopAbs_EDGE); explorer.More();
       explorer.Next()) {
    generatedEdge = TopoDS::Edge(explorer.Current());
    break;
  }
  CHECK(!generatedEdge.IsNull());
  ScriptedCut generatedEdgeMaker(scriptedObject, scriptedTool,
                                 scriptedObject, scriptedObject,
                                 generatedEdge, true, false);
  CHECK(solidar::captureBooleanFaceHistory(
      generatedEdgeMaker, 8603, scriptedSources, {}, {}, &failure));

  ScriptedCut nullGeneratedMaker(scriptedObject, scriptedTool,
                                 scriptedObject, scriptedObject,
                                 TopoDS_Shape{}, true, false);
  CHECK(!solidar::captureBooleanFaceHistory(
      nullGeneratedMaker, 8603, scriptedSources, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidHistory);

  ScriptedCut nullModifiedMaker(scriptedObject, scriptedTool,
                                scriptedOutput, scriptedObject,
                                TopoDS_Shape{}, false, true);
  CHECK(!solidar::captureBooleanFaceHistory(
      nullModifiedMaker, 8603, scriptedSources, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidHistory);

  ScriptedCut inconsistentMaker(scriptedObject, scriptedTool, scriptedOutput,
                                scriptedObject, scriptedOutput, false, true,
                                true);
  CHECK(!solidar::captureBooleanFaceHistory(
      inconsistentMaker, 8603, scriptedSources, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::InvalidHistory);

  TopoDS_Compound compound;
  BRep_Builder compoundBuilder;
  compoundBuilder.MakeCompound(compound);
  compoundBuilder.Add(compound, scriptedOutput);
  const TopoDS_Face extraOutput = facesOf(left).front();
  CHECK(!extraOutput.IsSame(scriptedOutput));
  compoundBuilder.Add(compound, extraOutput);
  ScriptedCut uncoveredMaker(scriptedObject, scriptedTool, compound,
                             scriptedObject, scriptedOutput, false, true);
  CHECK(!solidar::captureBooleanFaceHistory(
      uncoveredMaker, 8603, scriptedSources, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::UncoveredOutput);

  ScriptedCut missingHistoryMaker(scriptedObject, scriptedTool,
                                  scriptedOutput, scriptedObject,
                                  scriptedOutput, false, false);
  CHECK(!solidar::captureBooleanFaceHistory(
      missingHistoryMaker, 8603, scriptedSources, {}, {}, &failure));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::MissingHistory);

  const auto captureManyOrigins = [&](std::size_t originCount) {
    std::vector<TopoDS_Shape> objectFaces;
    std::vector<solidar::BooleanTopologySource> manySources;
    objectFaces.reserve(originCount);
    manySources.reserve(originCount + 1);
    for (std::size_t index = 0; index < originCount; ++index) {
      const auto box = BRepPrimAPI_MakeBox(
                           gp_Pnt(100.0 + static_cast<double>(index) * 3.0,
                                  0.0, 0.0),
                           1.0, 1.0, 1.0)
                           .Shape();
      objectFaces.push_back(facesOf(box).front());
      manySources.push_back(
          sourceFixture(objectFaces.back(),
                        9000 + static_cast<solidar::FeatureId>(index),
                        solidar::BooleanOperandRole::Object, "face")
              .source);
    }
    const TopoDS_Face manyTool =
        facesOf(BRepPrimAPI_MakeBox(gp_Pnt(-20.0, 0.0, 0.0), 1.0, 1.0,
                                    1.0)
                    .Shape())
            .front();
    manySources.push_back(
        sourceFixture(manyTool, 9900, solidar::BooleanOperandRole::Tool,
                      "tool-face")
            .source);
    const TopoDS_Face manyOutput =
        facesOf(BRepPrimAPI_MakeBox(gp_Pnt(-10.0, 0.0, 0.0), 1.0, 1.0,
                                    1.0)
                    .Shape())
            .front();
    ManySourceScriptedCut maker(objectFaces, manyTool, manyOutput);
    return solidar::captureBooleanFaceHistory(
        maker, 9950, manySources, {}, {}, &failure);
  };
  auto maximumOrigins = captureManyOrigins(
      solidar::kMaximumTopologyImmediateOrigins);
  CHECK(maximumOrigins);
  CHECK(build(*maximumOrigins));
  CHECK(!captureManyOrigins(
      solidar::kMaximumTopologyImmediateOrigins + 1));
  CHECK(failure.code ==
        solidar::BooleanTopologyCaptureFailureCode::ResourceLimit);

  return EXIT_SUCCESS;
}
